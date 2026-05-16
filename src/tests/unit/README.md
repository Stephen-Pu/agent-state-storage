# Unit tests (C++ / gtest)

Layout mirrors the source tree:
- `tier/`     — tier_manager, NVMe backends, 2Q + Ghost Cache
- `prefix/`   — ART correctness, LPM, epoch reclamation
- `ingest/`   — mutable buffer slot lifetime, watermark advance, seal atomicity
- `transport/`— Priority Scheduler ordering, NIXL backend selection
- `qos/`      — quota math, priority-context propagation
- `security/` — RBAC matrix, audit ring buffer

Hot-path microbenchmarks (LLD §9.1: lookup p99 ≤ 10µs) live under `bench/`.
