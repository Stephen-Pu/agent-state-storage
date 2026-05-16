# End-to-end tests (Python / pytest)

Scenarios (LLD §9.5):
- `test_100k_rag.py`         — 100K-token RAG long-prompt demo, asserts ≥17× speedup.
- `test_multi_tenant.py`     — hard isolation, quota enforcement, priority preemption.
- `test_cross_node_share.py` — system-prompt reuse across two GPU nodes.
- `test_safety_net.py`       — verify D-PERF-1 trip falls back to recompute.
- `test_crash_recovery.py`   — kill kvstore-node, verify RocksDB rebuild + ART replay.

Requires:
- A live KV Cache cluster (kind / minikube / EKS).
- One of the supported engines (vLLM by default).
- The corresponding adapter package installed.

TODO(stephen): add `conftest.py` with cluster bring-up fixtures.
