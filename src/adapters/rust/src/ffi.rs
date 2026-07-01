//! Raw `extern "C"` declarations + `#[repr(C)]` structs mirroring
//! `kvcache/kv_abi.h`, `kv_types.h`, `kv_errors.h`. This module is the
//! unsafe seam; the safe idiomatic surface lives in `lib.rs`.
//!
//! Every struct here is byte-compatible with the C header — the round-trip
//! test asserts `size_of::<KvLocator>() == 64` to catch any layout drift.

use std::os::raw::{c_char, c_int, c_void};

pub const KVCACHE_ABI_VERSION: i32 = 2;
pub const KV_LOCATOR_SIZE: usize = 64;

// Error codes (kv_errors.h). Only the ones the safe layer branches on are
// named; the rest travel through as raw ints + kv_status_str.
pub const KV_OK: c_int = 0;
pub const KV_E_NOT_FOUND: c_int = -3;

/// Opaque `kv_ctx_t`.
#[repr(C)]
pub struct KvCtx {
    _private: [u8; 0],
}

#[repr(C)]
pub struct KvCtxTuning {
    pub nixl_backend: *const c_char,
    pub nixl_bind_host: *const c_char,
    pub nixl_bind_port: u32,
    pub nixl_segment_bytes: u64,
    pub nixl_segment_bytes_set: i32,
    pub pinned_pool_bytes: u64,
    pub pinned_slot_bytes: u64,
    pub dram_capacity_bytes: u64,
}

#[repr(C)]
pub struct KvCtxConfig {
    pub abi_version: i32,
    pub agent_endpoint: *const c_char,
    pub tenant_id: *const c_char,
    pub model_id: *const c_char,
    pub flags: u32,
    pub tuning: *const KvCtxTuning,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KvBufferDesc {
    pub addr: *mut c_void,
    pub len: usize,
    pub mem_type: i32,
    pub mr_key: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct KvRange {
    pub layer_start: u16,
    pub layer_count: u16,
    pub head_start: u16,
    pub head_count: u16,
    pub token_start: u32,
    pub token_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KvLocator {
    pub tenant_id: [u8; 16],
    pub model_id_hash: u64,
    pub prefix_hash: [u8; 16],
    pub range: KvRange,
    pub version: u32,
    pub flags: u32,
}

pub type KvHandle = u64;
pub type KvCompletion = u64;

extern "C" {
    pub fn kv_ctx_open(cfg: *const KvCtxConfig, out_ctx: *mut *mut KvCtx) -> c_int;
    pub fn kv_ctx_close(ctx: *mut KvCtx) -> c_int;

    pub fn kv_lookup(
        ctx: *mut KvCtx,
        tokens: *const u32,
        n: usize,
        meta: *mut KvLocator,
        handle: *mut KvHandle,
        matched_tokens: *mut u32,
    ) -> c_int;

    pub fn kv_reserve(
        ctx: *mut KvCtx,
        locator: *const KvLocator,
        bytes: usize,
        handle: *mut KvHandle,
        slot: *mut KvBufferDesc,
    ) -> c_int;

    pub fn kv_publish(
        ctx: *mut KvCtx,
        handle: KvHandle,
        src: KvBufferDesc,
        watermark: u64,
    ) -> c_int;

    pub fn kv_fetch(
        ctx: *mut KvCtx,
        handle: KvHandle,
        ranges: *const KvRange,
        n: usize,
        dst: KvBufferDesc,
        completion: *mut KvCompletion,
    ) -> c_int;

    pub fn kv_wait(ctx: *mut KvCtx, completion: KvCompletion, timeout_ms: u32) -> c_int;

    pub fn kv_seal(ctx: *mut KvCtx, handle: KvHandle, tokens: *const u32, n_tokens: usize)
        -> c_int;

    pub fn kv_release(ctx: *mut KvCtx, handle: KvHandle) -> c_int;

    pub fn kv_lookup_stored_bytes(
        ctx: *mut KvCtx,
        handle: KvHandle,
        out_bytes: *mut usize,
    ) -> c_int;

    pub fn kv_kvtensor_encode(
        data: *const f32,
        n_tokens: u32,
        elems_per_token: u32,
        bits: i32,
        delta: i32,
        out: *mut u8,
        out_cap: usize,
        out_len: *mut usize,
    ) -> c_int;

    pub fn kv_kvtensor_decode(
        blob: *const u8,
        blob_len: usize,
        out: *mut f32,
        out_cap_elems: usize,
        out_n_tokens: *mut u32,
        out_elems_per_token: *mut u32,
    ) -> c_int;

    pub fn kv_metrics_scrape(buf: *mut c_char, cap: usize, out_len: *mut usize) -> c_int;

    pub fn kv_status_str(status: c_int) -> *const c_char;
}
