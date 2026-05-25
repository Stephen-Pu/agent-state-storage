// Phase Q-1 — etcd-driven view of the cluster's live nodes.
//
// On Start the directory does a one-shot GetPrefix to seed the table,
// then opens a Watch on the same prefix. Every PUT becomes an upsert,
// every DELETE (or lease-expiry, which etcd surfaces as DELETE) drops
// the entry. The HrwRing is refreshed from the table after each mutation.
//
// Lookup paths (and any other RPC handler that needs to know "who owns
// this key?") query Primary() / endpoint_for(). Threading model:
//
//   * Mutations land on the Watch dispatcher thread inside the etcd
//     client. We serialise table updates under `mu_` and rebuild the
//     HrwRing's node set inline.
//   * Reads hold `mu_` briefly to copy out the snapshot they need;
//     because the table is tiny (a handful of nodes) this is cheap.
//
// The directory does NOT itself initiate any gRPC connections — that
// is the forwarding layer's job (see node_data_service.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cluster/etcd_client.h"
#include "routing/bloom_sketch.h"
#include "routing/hrw.h"

namespace kvcache::node::cluster {

struct NodeEndpoint {
    std::string host;
    uint16_t    grpc_port = 0;
    // "host:port" — precomputed because the forwarding layer reads it
    // on every fan-out hop.
    std::string dial_target;
};

class NodeDirectory {
   public:
    struct Options {
        std::string key_prefix = "/kvcache/nodes/";
        // Phase K-3 — the etcd key the CP leader writes the
        // ClusterView snapshot to. When this key is present, the
        // directory adopts the snapshot wholesale (atomic table
        // replace). The /kvcache/nodes/ prefix watch stays armed in
        // parallel so the table keeps converging when no leader has
        // published yet (or the leader's lease expired mid-failover).
        // Empty disables the ClusterView path entirely.
        std::string view_key = "/kvcache/cluster/view";
        // Phase K-5 — prefix the directory watches for peer
        // bloom-sketch publications. Each entry is a BloomPublisher
        // blob (header + bit array). Empty disables the sketch path.
        std::string sketch_prefix = "/kvcache/sketches/";
    };

    NodeDirectory(IEtcdClient* etcd, routing::HrwRing* ring);
    NodeDirectory(IEtcdClient* etcd, routing::HrwRing* ring, Options opts);
    ~NodeDirectory();

    NodeDirectory(const NodeDirectory&)            = delete;
    NodeDirectory& operator=(const NodeDirectory&) = delete;

    // Seed + start Watch. Returns false on failure.
    bool Start(std::string* err);
    void Stop();

    // Snapshot of current node ids — sorted for deterministic iteration.
    std::vector<std::string> NodeIds() const;

    // Resolve a node id to its dial target. Returns nullopt if unknown.
    std::optional<NodeEndpoint> Resolve(const std::string& node_id) const;

    std::size_t NodeCount() const noexcept;

    // Phase K-5 — query a peer's published bloom sketch. Returns
    // false when (a) we have no sketch from `node_id` (the peer
    // hasn't published yet, or its lease expired), or (b) the
    // sketch says it doesn't have the key. Returns true if the
    // peer's sketch says "I MIGHT have it" — false positives are
    // tolerable; the caller routes a probe to that peer and
    // verifies with a real Lookup.
    bool PeerMaybeHas(const std::string& node_id,
                       std::span<const uint8_t> key) const;

    // Number of peers whose sketch we have observed. Useful for
    // tests + metrics.
    std::size_t SketchCount() const noexcept;

   private:
    void OnWatch(const WatchEvent& ev);
    // Phase K-3 — single-key watcher on the CP's ClusterView snapshot.
    // Replaces the entire table in one go from the JSON payload.
    void OnClusterViewWatch(const WatchEvent& ev);
    // Phase K-5 — sketch watcher: PUT installs a peer's sketch,
    // DELETE drops it.
    void OnSketchWatch(const WatchEvent& ev);
    // Apply a wire-encoded sketch blob to the in-memory map.
    void ApplyPeerSketch(const std::string& node_id, const std::string& blob);
    // Apply a serialized ClusterView JSON to the in-memory table.
    // Caller does NOT hold `mu_`; the function takes it internally.
    // Returns true if the view was accepted (epoch fresh enough).
    bool ApplyClusterViewJson(const std::string& json);
    // Phase K-4 — prefix-watch lifecycle. Opening seeds from
    // GetPrefix first then arms WatchPrefix. The *Locked variant
    // assumes `mu_` is held; the bare variant takes it. Closing
    // (when transitioning into view mode) hands the handle out
    // through the caller-side WatchHandle plumbing in
    // ApplyClusterViewJson — there's no closer here because
    // calling etcd_->Unwatch under mu_ deadlocks against the etcd
    // dispatcher's callback path.
    void OpenPrefixWatchLocked();
    void OpenPrefixWatch();
    // Re-build the HrwRing's node set from the current table. Caller
    // holds `mu_`.
    void RebuildRingLocked();

    IEtcdClient*           etcd_;
    routing::HrwRing*      ring_;        // not owned
    Options                opts_;
    WatchHandle            watch_handle_       = 0;
    WatchHandle            view_watch_handle_  = 0;
    WatchHandle            sketch_watch_handle_ = 0;
    // Phase K-3 — last ClusterView epoch we've applied. Stale or
    // re-ordered events (from leader churn) are dropped if their
    // epoch isn't greater than this. Reset to 0 on leader_id
    // change so a brand-new leader's epoch=1 always wins.
    std::string            last_view_leader_;
    uint64_t               last_view_epoch_ = 0;
    // Phase K-4 — when true, the directory is being driven by
    // ClusterView events alone and the prefix watch has been
    // un-subscribed to save etcd traffic. Flips back to false on
    // view-key delete (leader lease expiry) so the directory
    // re-opens the prefix watch and keeps converging.
    bool                   view_active_ = false;

    mutable std::mutex                                mu_;
    std::unordered_map<std::string, NodeEndpoint>     table_;
    // Phase K-5 — per-peer sketches keyed by node_id. The
    // AggregatedBloom inside owns its own mutex so MaybeContains
    // doesn't need our `mu_`; we hold `mu_` only for the map
    // lookup/insert/erase.
    std::unordered_map<std::string,
                       std::unique_ptr<routing::AggregatedBloom>> sketches_;
    std::atomic<bool>                                 running_{false};
};

}  // namespace kvcache::node::cluster
