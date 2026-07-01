//! Safe Rust bindings for the KV Cache Core ABI (`libkvcache`).
//!
//! This is the Rust surface of the multi-language API story
//! (integration-stack.md A4 — a Phase-2 nice-to-have). It wraps the stable C
//! ABI (LLD §6.1.2) the same way the Python `kvcache_core` connector does, but
//! with RAII + `Result` instead of manual refcounting + status ints.
//!
//! ```no_run
//! use kvcache::{Context, Locator};
//! let cx = Context::open("tenant-a", "llama-3-70b").unwrap();
//! let tokens: Vec<u32> = (0..32).collect();
//! let payload = vec![7u8; tokens.len() * 64];
//! let loc = Locator::for_tokens("tenant-a", "llama-3-70b", &tokens);
//! let mut rsv = cx.reserve(&loc, payload.len()).unwrap();
//! rsv.write(&payload).unwrap();
//! cx.publish(rsv.handle(), payload.len() as u64).unwrap();
//! cx.seal(rsv.handle(), &tokens).unwrap();
//! if let Some(hit) = cx.lookup(&tokens).unwrap() {
//!     let mut dst = vec![0u8; hit.matched_tokens as usize * 64];
//!     let c = cx.fetch(hit.handle, &mut dst).unwrap();
//!     cx.wait(c, 5000).unwrap();
//!     cx.release(hit.handle).unwrap();
//! }
//! ```
//!
//! The MVP loopback backend completes fetches inline, so `wait` is a
//! formality; the async shape is preserved for the RDMA backends.

use std::ffi::{CStr, CString};
use std::fmt;
use std::os::raw::c_char;
use std::ptr;

mod ffi;

pub use ffi::KV_LOCATOR_SIZE;

/// A non-OK Core ABI status, carrying the human-readable string from
/// `kv_status_str` so `Display` matches what the C side reports.
#[derive(Debug, Clone)]
pub struct Error {
    pub op: &'static str,
    pub code: i32,
    pub message: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {} (status={})", self.op, self.message, self.code)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

fn status_message(code: i32) -> String {
    // SAFETY: kv_status_str returns a static NUL-terminated string for any int.
    unsafe {
        let p = ffi::kv_status_str(code);
        if p.is_null() {
            format!("status {code}")
        } else {
            CStr::from_ptr(p).to_string_lossy().into_owned()
        }
    }
}

fn check(op: &'static str, code: i32) -> Result<()> {
    if code == ffi::KV_OK {
        Ok(())
    } else {
        Err(Error { op, code, message: status_message(code) })
    }
}

/// Canonical FNV-1a-64 — matches `kvcache::Fnv1a64` (src/core/common/hashing.h)
/// and the Python connector's `fnv1a64`, so `model_id_hash` on the wire agrees
/// across languages.
pub fn fnv1a64(data: &[u8]) -> u64 {
    let mut h: u64 = 0xCBF2_9CE4_8422_2325;
    for &b in data {
        h ^= b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01B3);
    }
    h
}

/// A 64-byte KV chunk Locator (LLD §2.1).
///
/// The in-process backend keys its ART path on the ctx's tenant/model hashes +
/// the token sequence (see `HeadlessNode::Seal`/`Lookup`), so `prefix_hash`
/// here is not what a local round-trip matches on. [`Locator::for_tokens`]
/// fills `model_id_hash` with the wire-canonical FNV-1a-64 and derives the
/// other fields deterministically from the inputs — sufficient for the
/// in-process backend. Matching a remote cluster's exact tenant (SHA-1) /
/// prefix (BLAKE2b) derivation over the gRPC path is a follow-up for a real
/// Rust deployment; construct a [`Locator`] directly with those bytes when
/// that day comes.
#[derive(Clone)]
pub struct Locator(ffi::KvLocator);

impl Locator {
    /// Construct from explicit fields (use this to match a remote cluster's
    /// exact hashing when driving the gRPC path).
    pub fn new(tenant_id: [u8; 16], model_id_hash: u64, prefix_hash: [u8; 16]) -> Self {
        Locator(ffi::KvLocator {
            tenant_id,
            model_id_hash,
            prefix_hash,
            range: ffi::KvRange::default(),
            version: 1,
            flags: 0,
        })
    }

    /// Deterministic locator for `tokens` under `(tenant, model)`. `model_id_hash`
    /// is the wire-canonical FNV-1a-64 of the model string; `tenant_id` and
    /// `prefix_hash` are deterministic, backend-local derivations (see the
    /// type-level note).
    pub fn for_tokens(tenant: &str, model: &str, tokens: &[u32]) -> Self {
        let mut tenant_id = [0u8; 16];
        let tb = tenant.as_bytes();
        let n = tb.len().min(16);
        tenant_id[..n].copy_from_slice(&tb[..n]);

        // 16-byte prefix hash: two FNV-1a-64 passes over the LE token bytes
        // (second pass salted) so distinct token prefixes get distinct keys.
        let mut tok_bytes = Vec::with_capacity(tokens.len() * 4);
        for &t in tokens {
            tok_bytes.extend_from_slice(&t.to_le_bytes());
        }
        let lo = fnv1a64(&tok_bytes);
        tok_bytes.push(0x9e);
        let hi = fnv1a64(&tok_bytes);
        let mut prefix_hash = [0u8; 16];
        prefix_hash[..8].copy_from_slice(&lo.to_le_bytes());
        prefix_hash[8..].copy_from_slice(&hi.to_le_bytes());

        Locator::new(tenant_id, fnv1a64(model.as_bytes()), prefix_hash)
    }
}

/// Result of a successful [`Context::lookup`].
#[derive(Debug, Clone, Copy)]
pub struct LookupHit {
    pub matched_tokens: u32,
    pub handle: u64,
}

/// A reserved write slot (T1 pinned memory) from [`Context::reserve`].
pub struct Reservation {
    handle: u64,
    slot_addr: *mut u8,
    slot_len: usize,
}

impl Reservation {
    pub fn handle(&self) -> u64 {
        self.handle
    }

    /// Bytes available in the reserved slot.
    pub fn capacity(&self) -> usize {
        self.slot_len
    }

    /// Copy `data` into the server-allocated slot (the zero-copy ABI; this
    /// stands in for an engine's device→host copy). Errors if `data` exceeds
    /// the reserved capacity.
    pub fn write(&mut self, data: &[u8]) -> Result<()> {
        if data.len() > self.slot_len {
            return Err(Error {
                op: "reservation.write",
                code: ffi::KV_OK, // client-side guard, not an ABI code
                message: format!(
                    "data {} exceeds reserved slot {}",
                    data.len(),
                    self.slot_len
                ),
            });
        }
        // SAFETY: slot_addr points at slot_len bytes of live pinned memory the
        // node returned from Reserve and holds until Seal/Release; we bound the
        // copy to data.len() <= slot_len.
        unsafe {
            ptr::copy_nonoverlapping(data.as_ptr(), self.slot_addr, data.len());
        }
        Ok(())
    }
}

/// A completion token from [`Context::fetch`]; block on it with
/// [`Context::wait`].
#[derive(Debug, Clone, Copy)]
pub struct Completion(pub u64);

/// An engine-agnostic handle to a KV cache context (one per (tenant, model)).
///
/// Not `Send`/`Sync`: the underlying `kv_ctx_t*` is bound to the process
/// backend and must be driven from a consistent owner. Open one per worker.
pub struct Context {
    ctx: *mut ffi::KvCtx,
    // Keep the CStrings alive for the ctx's lifetime — kv_ctx_open copies, but
    // holding them is cheap insurance against any impl that retains the ptrs.
    _tenant: CString,
    _model: CString,
}

impl Context {
    /// Open a context for `(tenant, model)`. The first open in the process
    /// creates the singleton in-process backend (loopback by default).
    pub fn open(tenant: &str, model: &str) -> Result<Context> {
        let tenant_c = CString::new(tenant).map_err(|_| Error {
            op: "Context::open",
            code: ffi::KV_OK,
            message: "tenant contains NUL".into(),
        })?;
        let model_c = CString::new(model).map_err(|_| Error {
            op: "Context::open",
            code: ffi::KV_OK,
            message: "model contains NUL".into(),
        })?;

        let cfg = ffi::KvCtxConfig {
            abi_version: ffi::KVCACHE_ABI_VERSION,
            agent_endpoint: ptr::null(),
            tenant_id: tenant_c.as_ptr(),
            model_id: model_c.as_ptr(),
            flags: 0,
            tuning: ptr::null(),
        };
        let mut ctx: *mut ffi::KvCtx = ptr::null_mut();
        // SAFETY: cfg outlives the call; out_ctx is a valid slot.
        let rc = unsafe { ffi::kv_ctx_open(&cfg, &mut ctx) };
        check("kv_ctx_open", rc)?;
        Ok(Context { ctx, _tenant: tenant_c, _model: model_c })
    }

    /// Longest-prefix-match over the in-memory ART. `Ok(None)` on miss.
    pub fn lookup(&self, tokens: &[u32]) -> Result<Option<LookupHit>> {
        let mut meta = std::mem::MaybeUninit::<ffi::KvLocator>::uninit();
        let mut handle: u64 = 0;
        let mut matched: u32 = 0;
        // SAFETY: tokens is a valid slice; out params are live locals.
        let rc = unsafe {
            ffi::kv_lookup(
                self.ctx,
                tokens.as_ptr(),
                tokens.len(),
                meta.as_mut_ptr(),
                &mut handle,
                &mut matched,
            )
        };
        if rc == ffi::KV_E_NOT_FOUND {
            return Ok(None);
        }
        check("kv_lookup", rc)?;
        Ok(Some(LookupHit { matched_tokens: matched, handle }))
    }

    /// Reserve a mutable write slot of `bytes` for `locator`.
    pub fn reserve(&self, locator: &Locator, bytes: usize) -> Result<Reservation> {
        let mut handle: u64 = 0;
        let mut slot = ffi::KvBufferDesc { addr: ptr::null_mut(), len: 0, mem_type: 0, mr_key: 0 };
        // SAFETY: locator + out params are live for the call.
        let rc = unsafe { ffi::kv_reserve(self.ctx, &locator.0, bytes, &mut handle, &mut slot) };
        check("kv_reserve", rc)?;
        Ok(Reservation { handle, slot_addr: slot.addr as *mut u8, slot_len: slot.len })
    }

    /// Advance the streaming-write watermark to `watermark` bytes.
    pub fn publish(&self, handle: u64, watermark: u64) -> Result<()> {
        let empty = ffi::KvBufferDesc { addr: ptr::null_mut(), len: 0, mem_type: 0, mr_key: 0 };
        // SAFETY: ctx valid; empty desc passed by value like the Python path.
        let rc = unsafe { ffi::kv_publish(self.ctx, handle, empty, watermark) };
        check("kv_publish", rc)
    }

    /// Atomically seal a write handle under `tokens`, making it lookup-visible.
    pub fn seal(&self, handle: u64, tokens: &[u32]) -> Result<()> {
        // SAFETY: tokens is a valid slice.
        let rc = unsafe { ffi::kv_seal(self.ctx, handle, tokens.as_ptr(), tokens.len()) };
        check("kv_seal", rc)
    }

    /// Async-fetch the cached bytes for `handle` into `dst`. Returns the
    /// completion to [`wait`](Self::wait) on.
    pub fn fetch(&self, handle: u64, dst: &mut [u8]) -> Result<Completion> {
        let desc = ffi::KvBufferDesc {
            addr: dst.as_mut_ptr() as *mut _,
            len: dst.len(),
            mem_type: 0, // KV_MEM_HOST
            mr_key: 0,
        };
        let mut completion: u64 = 0;
        // SAFETY: dst outlives the call; ranges=NULL/0 means "the whole chunk".
        let rc = unsafe {
            ffi::kv_fetch(self.ctx, handle, ptr::null(), 0, desc, &mut completion)
        };
        check("kv_fetch", rc)?;
        Ok(Completion(completion))
    }

    /// Block until `completion` finishes or `timeout_ms` elapses.
    pub fn wait(&self, completion: Completion, timeout_ms: u32) -> Result<()> {
        // SAFETY: ctx valid; completion is a plain u64 token.
        let rc = unsafe { ffi::kv_wait(self.ctx, completion.0, timeout_ms) };
        check("kv_wait", rc)
    }

    /// Drop the caller's reference to `handle`.
    pub fn release(&self, handle: u64) -> Result<()> {
        // SAFETY: ctx valid; handle is a plain u64 token.
        let rc = unsafe { ffi::kv_release(self.ctx, handle) };
        check("kv_release", rc)
    }

    /// Total stored byte length of a read `handle`'s cached chunk (KVZ-3) —
    /// lets a caller size a [`fetch`](Self::fetch) buffer for a variable-size
    /// (e.g. compressed) payload.
    pub fn stored_bytes(&self, handle: u64) -> Result<usize> {
        let mut out: usize = 0;
        // SAFETY: ctx valid; out is a live local.
        let rc = unsafe { ffi::kv_lookup_stored_bytes(self.ctx, handle, &mut out) };
        check("kv_lookup_stored_bytes", rc)?;
        Ok(out)
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        if !self.ctx.is_null() {
            // SAFETY: ctx was minted by kv_ctx_open and is closed exactly once.
            unsafe { ffi::kv_ctx_close(self.ctx) };
            self.ctx = ptr::null_mut();
        }
    }
}

// ---------------------------------------------------------------------------
// KV-tensor codec (KVZ) — stateless free functions, no Context needed.
// ---------------------------------------------------------------------------

/// Compress fp32 KV laid out `[n_tokens][elems_per_token]` via the
/// CacheGen-class codec. Lossy: `bits` is 8 or 4; `delta` enables token-axis
/// DPCM. Two-pass sizing under the hood.
pub fn kvtensor_encode(
    data: &[f32],
    n_tokens: u32,
    elems_per_token: u32,
    bits: i32,
    delta: bool,
) -> Result<Vec<u8>> {
    if data.len() != (n_tokens as usize) * (elems_per_token as usize) {
        return Err(Error {
            op: "kvtensor_encode",
            code: ffi::KV_OK,
            message: "data.len() != n_tokens * elems_per_token".into(),
        });
    }
    let d = if delta { 1 } else { 0 };
    let mut need: usize = 0;
    // Size probe.
    // SAFETY: data valid; out=NULL/cap=0 requests the length in `need`.
    let rc = unsafe {
        ffi::kv_kvtensor_encode(
            data.as_ptr(), n_tokens, elems_per_token, bits, d, ptr::null_mut(), 0, &mut need,
        )
    };
    check("kv_kvtensor_encode(size)", rc)?;
    let mut out = vec![0u8; need];
    // SAFETY: out has `need` bytes; data valid.
    let rc = unsafe {
        ffi::kv_kvtensor_encode(
            data.as_ptr(), n_tokens, elems_per_token, bits, d, out.as_mut_ptr(), need, &mut need,
        )
    };
    check("kv_kvtensor_encode", rc)?;
    out.truncate(need);
    Ok(out)
}

/// Decode a [`kvtensor_encode`] blob → `(floats, (n_tokens, elems_per_token))`.
/// Lossy reconstruction within the per-token quantization step.
pub fn kvtensor_decode(blob: &[u8]) -> Result<(Vec<f32>, (u32, u32))> {
    let mut nt: u32 = 0;
    let mut ne: u32 = 0;
    // Shape peek (out=NULL).
    // SAFETY: blob valid; out=NULL fills only the shape.
    let rc = unsafe {
        ffi::kv_kvtensor_decode(blob.as_ptr(), blob.len(), ptr::null_mut(), 0, &mut nt, &mut ne)
    };
    check("kv_kvtensor_decode(shape)", rc)?;
    let total = (nt as usize) * (ne as usize);
    let mut out = vec![0f32; total];
    // SAFETY: out has `total` floats; blob valid.
    let rc = unsafe {
        ffi::kv_kvtensor_decode(
            blob.as_ptr(), blob.len(), out.as_mut_ptr(), total, &mut nt, &mut ne,
        )
    };
    check("kv_kvtensor_decode", rc)?;
    Ok((out, (nt, ne)))
}

/// Scrape the dylib's Prometheus-format metrics (two-pass sizing).
pub fn metrics_scrape() -> Result<String> {
    let mut need: usize = 0;
    // SAFETY: buf=NULL/cap=0 requests the length.
    let rc = unsafe { ffi::kv_metrics_scrape(ptr::null_mut(), 0, &mut need) };
    check("kv_metrics_scrape(size)", rc)?;
    let mut buf = vec![0u8; need + 1]; // room for NUL
    // SAFETY: buf has need+1 bytes.
    let rc = unsafe {
        ffi::kv_metrics_scrape(buf.as_mut_ptr() as *mut c_char, buf.len(), &mut need)
    };
    check("kv_metrics_scrape", rc)?;
    buf.truncate(need);
    Ok(String::from_utf8_lossy(&buf).into_owned())
}

#[cfg(test)]
mod layout {
    use super::ffi;
    #[test]
    fn locator_is_64_bytes() {
        assert_eq!(std::mem::size_of::<ffi::KvLocator>(), ffi::KV_LOCATOR_SIZE);
    }
}
