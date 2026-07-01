# `kvcache` — Rust bindings for the Core ABI

Safe Rust surface over the stable C ABI (`libkvcache`, LLD §6.1.2). This is the
Rust leg of the multi-language API story (integration-stack.md **A4**, a Phase-2
nice-to-have) — the fourth language binding after C, C++, and Python.

## What it wraps

RAII + `Result` over the C ABI, matching the Python `kvcache_core` connector's
shape:

| Rust | C ABI |
|------|-------|
| `Context::open` / `Drop` | `kv_ctx_open` / `kv_ctx_close` |
| `Context::lookup` → `Option<LookupHit>` | `kv_lookup` (miss → `Ok(None)`) |
| `Context::reserve` → `Reservation` | `kv_reserve` |
| `Reservation::write` | copy into the pinned slot |
| `Context::publish` / `seal` / `release` | `kv_publish` / `kv_seal` / `kv_release` |
| `Context::fetch` → `Completion` / `wait` | `kv_fetch` / `kv_wait` |
| `Context::stored_bytes` | `kv_lookup_stored_bytes` (KVZ-3) |
| `kvtensor_encode` / `kvtensor_decode` | `kv_kvtensor_encode` / `_decode` (KVZ-2) |
| `metrics_scrape` | `kv_metrics_scrape` (G-3) |

`Locator::for_tokens` fills `model_id_hash` with the wire-canonical FNV-1a-64
(matches `kvcache::Fnv1a64` and the Python `fnv1a64`). The in-process backend
keys its ART path on the ctx hashes + tokens, so a local round-trip needs no
SHA-1/BLAKE2b — the crate is **dependency-free**. Matching a remote cluster's
exact tenant (SHA-1) / prefix (BLAKE2b) hashing over the gRPC path is a follow-up;
construct a `Locator` directly with those bytes when a real Rust deployment
needs it.

## Build & test

`build.rs` locates the sibling dylib (via `$KVCACHE_LIB`, else by walking up to
`build/core-abi/`) and emits an rpath so test binaries resolve it at runtime.

```bash
# Build the C/C++ tree first so libkvcache exists:
cmake --build build --target kvcache

cd src/adapters/rust
cargo test          # auto-discovers ../../build/core-abi/libkvcache.dylib
# or point explicitly:
KVCACHE_LIB=/abs/path/to/libkvcache.dylib cargo test
```

## Status

MVP binding — verified end-to-end against the loopback backend (full
reserve→seal→lookup→fetch round-trip + codec + metrics). Async is inline on
loopback; the async shape (`Completion` + `wait`) is preserved for the RDMA
backends. Not published to crates.io (`publish = false`) — in-tree only.
