# control-plane

3-replica single-binary Control Plane. LLD §4.1.

Responsibilities:
- Member registry (joining / active / draining / unreachable / terminated)
- Quota authority (per-tenant 3D quotas; reconciles with node-local caches)
- Routing fan-out (collects per-node bloom sketches, redistributes every 30 s)
- Config push (per-tenant settings, tier capacities, priority weights)
- Leader election via Etcd lease

Etcd is brought as a dependency (3-replica StatefulSet in MVP). LLD §7.2.

Data plane is CP-independent (LLD §4.1) — CP outages do not break lookup /
fetch / publish. Quota inflates 1.5× after CP unreachable > 1 h.

## Build

```bash
cd src/control-plane
go build ./...
```
