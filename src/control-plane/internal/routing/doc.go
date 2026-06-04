// Package routing was scaffolded for a CP-side routing reconciler under the
// original LLD §4.1 "push via the Sync stream" design. That design was
// superseded: routing state is published to etcd and consumed by node-side
// watchers, so this package is intentionally empty. The routing subsystem
// actually lives in:
//
//   - membership.ViewPublisher (Phase K-2) — the leader writes a coherent
//     ClusterView{epoch, leader_id, nodes[]} to /kvcache/cluster/view.
//   - membership.SketchAggregator (Phase K-7) — ORs per-node bloom sketches
//     into /kvcache/cluster/sketch for prefix-presence-aware routing.
//   - node-side NodeDirectory (kvstore-node, Phases K-3/K-4) — watches those
//     keys + /kvcache/nodes/ and feeds HrwRing for cross-node Lookup fan-out.
//
// There is deliberately no CP-side routing reconciler. Kept as a doc marker;
// safe to delete if the empty package ever bothers a linter.
package routing
