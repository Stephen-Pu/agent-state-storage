# kvstore-node

Data-plane node process. One per GPU host (D-DEPLOY-1, co-located default).

## Subsystems

| Dir          | Subsystem                               | LLD §  |
| ------------ | --------------------------------------- | ------ |
| `tier/`      | ③ 5-tier storage (HBM/Pinned/DRAM/NVMe/Cold) | §3.3 |
| `prefix/`    | ② ART prefix-reuse engine               | §3.2   |
| `ingest/`    | ④ Streaming-write engine                | §3.4   |
| `transport/` | ⑤ NIXL data plane + Priority Scheduler  | §3.5   |
| `meta/`      | Local metadata (RocksDB)                | §2.3   |
| `qos/`       | ⑨ Tenant / quota / priority enforcement | §5.1   |
| `security/`  | ⑩ mTLS + RBAC + audit                   | §5.2   |
| `cluster/`   | ⑦ Etcd client, membership FSM           | §4.1   |
| `routing/`   | ⑥ HRW + Bloom sketch view               | §4.2   |
| `obs/`       | ⑫ Prometheus metrics + logs             | §6.2   |

## Build

```bash
cmake --build build --target kvstore-node
```

## Runtime layout

- cgroup: 4–8 vCPU, 100–300 GB DRAM, 1–2 dedicated NVMe (LLD §7.1)
- NUMA pinning: avoid cross-socket DRAM ↔ NVMe traffic
- Exposes:
    - UNIX socket / shmem to KVAgent
    - gRPC to peer nodes (NIXL Pull control + cluster RPCs)
    - gRPC to Control Plane (`control-plane/`)
