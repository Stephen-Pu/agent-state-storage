# Helm chart: kvcache

Deploys a full KV Cache cluster on Kubernetes. LLD §7 / §8.

Components installed:
- `control-plane` (StatefulSet, 3 replicas)
- `etcd`         (StatefulSet, 3 replicas; skipped if `byoEtcd: true`)
- `kvstore-node` (DaemonSet, labeled GPU hosts)
- `kvagent`      (sidecar in engine pods — typically injected by mutating webhook)
- `kvcache-operator` (Deployment; manages `KVCacheCluster` / `KVCacheTenant` CRDs)
- ServiceMonitors, Grafana dashboards (5 panels per LLD §6.2)

TODO(stephen):
- `Chart.yaml`, `values.yaml`
- templates: `cp-sts.yaml`, `etcd-sts.yaml`, `node-ds.yaml`, `operator-dep.yaml`
- mTLS material via cert-manager `Certificate` resources
- 5 default `PrometheusRule` alerts (LLD §6.2)
