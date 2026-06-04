// Package quota was scaffolded for a CP-side quota reconciler under the
// original LLD §4.1 "push via the Sync stream" design. That design was
// superseded: quota distribution moved operator-side + etcd-watch, so this
// package is intentionally empty. The quota subsystem actually lives in:
//
//   - operator EtcdTenantPublisher (Phase H-4) — reconciles each validated
//     KVCacheTenant CR to /kvcache/tenants/<cluster>/<tenantID> in etcd.
//   - node-side TenantRegistry (kvstore-node, qos/tenant.h) — watches that
//     prefix and applies quota/priority on the hot path.
//
// There is deliberately no CP-side quota reconciler. Kept as a doc marker;
// safe to delete if the empty package ever bothers a linter.
package quota
