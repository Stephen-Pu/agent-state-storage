# operator

Kubernetes operator. LLD §8.2.

CRDs (MVP):
- `KVCacheCluster` — declares a cluster: node count, tier capacities, tenants,
  storage class, NIXL transport selection, Alluxio binding.
- `KVCacheTenant`  — declares a tenant: namespace, 3D quota, priority class.

Operator reconciles:
- StatefulSet for `kvstore-node` (DaemonSet on labeled GPU hosts).
- StatefulSet for `control-plane` (3 replicas).
- StatefulSet for `etcd` (3 replicas, unless `byoEtcd: true`).
- ConfigMaps / Secrets for mTLS material (cert-manager integration).

## Build

```bash
cd src/operator
go build ./...
```

TODO(stephen): scaffold with `operator-sdk init` once Go module is in place.
