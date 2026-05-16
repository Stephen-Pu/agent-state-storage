// Package membership manages the cluster member registry and leader election.
//
// Public surface: see registry.go. The leader-election loop runs from
// cmd/cp/main.go using etcd's concurrency.Election.
//
// LLD §4.1 — registry keyspace, lease TTLs, and 5-state node FSM (mirrored
// on the node side by kvstore-node/src/cluster/membership_fsm.h).
package membership
