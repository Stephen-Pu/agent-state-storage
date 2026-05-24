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
#include <unordered_map>
#include <utility>
#include <vector>

#include "cluster/etcd_client.h"
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

   private:
    void OnWatch(const WatchEvent& ev);
    // Re-build the HrwRing's node set from the current table. Caller
    // holds `mu_`.
    void RebuildRingLocked();

    IEtcdClient*           etcd_;
    routing::HrwRing*      ring_;        // not owned
    Options                opts_;
    WatchHandle            watch_handle_ = 0;

    mutable std::mutex                                mu_;
    std::unordered_map<std::string, NodeEndpoint>     table_;
    std::atomic<bool>                                 running_{false};
};

}  // namespace kvcache::node::cluster
