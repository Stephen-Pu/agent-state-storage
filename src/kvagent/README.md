# kvagent

Sidecar daemon co-located with the inference engine. LLD §6.1.5.

Responsibilities:
- Owns the **Shmem Ring** end of the Core ABI (SQ / CQ + doorbell, LLD §6.1.3).
- Maintains a **local routing cache** (per-prefix → primary node) and a
  **bloom-sketch view** of cluster presence (refreshed 30 s from peers).
- Forwards `kv_lookup` / `kv_fetch` / `kv_publish` to the right
  `kvstore-node` via gRPC + NIXL.
- Holds an 8–32 GB local KV cache (≈10–20 % of node DRAM).

Multi-engine: one `kvagent` per host serves all co-located engines via
independent SQ/CQ ring pairs over `/dev/shm`.
