// Package config was scaffolded for a CP-side config reconciler under the
// original LLD §4.1 "push via the Sync stream" design. That design was
// superseded: node configuration is delivered out-of-band, so this package
// is intentionally empty. Node config actually comes from:
//
//   - operator DesiredConfigMap (controller/resources.go) — renders the
//     per-cluster node config (cluster name, NIXL backend, tier sizes, etcd
//     endpoints) into a K8s ConfigMap mounted by each kvstore-node pod.
//   - kv_ctx_tuning_t (Phase ABI-1, include/kvcache/kv_abi.h) + KVCACHE_*
//     env vars — per-process backend knobs at kv_ctx_open time.
//
// There is deliberately no CP-side config reconciler. Kept as a doc marker;
// safe to delete if the empty package ever bothers a linter.
package config
