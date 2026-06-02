// LLD §4.2 — kvagent slow-path resolver backed by HRW.
//
// Phase A1.7 — the NodeResolver the RequestRouter calls on a cache
// miss. Wraps kvcache::node::routing::HrwRing: given (tenant, prefix)
// it computes the rendezvous-hash primary over the current node set
// and returns that node_id. This is the in-cluster resolution path —
// the agent already holds the cluster's node list (refreshed from
// etcd's /kvcache/cluster/view, the K-2 publisher key), so it can pick
// the primary locally without a CP round-trip.
//
// The node set is injected via SetNodes (production wires a 30s etcd
// refresh, same shape as BloomView's Loader; tests set a fixed list).
// HRW makes the choice deterministic + minimal-disruption on
// membership change — exactly matching what the kvstore-node side
// computes, so the agent and node agree on the primary without
// coordination.
//
// What this does NOT do: confirm the prefix actually lives on the
// chosen node (the bloom view already gated that as "maybe"), or
// return the matched-token count (only the node knows the real LPM
// length — the engine learns it when it issues the server-pull). So
// ResolveResult::matched_tokens is left 0; the router/engine treat 0
// as "ask the node".
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "router/router.h"            // ResolveResult, NodeResolver
#include "routing/hrw.h"              // kvcache::node::routing::HrwRing

namespace kvcache::agent::router {

class HrwResolver {
   public:
    HrwResolver() = default;

    // Replace the node set (copy-on-write inside HrwRing). Thread-safe.
    // node_ids are the kvstore-node ids the agent learned from the
    // cluster view; traffic_weight defaults to 1.0 (uniform) until the
    // agent also ingests per-node weights.
    void SetNodes(const std::vector<std::string>& node_ids);

    std::size_t NodeCount() const noexcept { return ring_.NodeCount(); }

    // Resolve (tenant, prefix) → primary node_id. Returns nullopt when
    // the node set is empty (no cluster view yet → the engine should
    // recompute rather than pull from nowhere).
    std::optional<ResolveResult> Resolve(
        const std::array<uint8_t, 16>& tenant_id,
        const std::array<uint8_t, 16>& prefix_hash) const;

    // Adapt to the router's NodeResolver std::function shape so it can
    // be handed straight to RequestRouter's ctor.
    NodeResolver AsCallback();

   private:
    kvcache::node::routing::HrwRing ring_;
};

}  // namespace kvcache::agent::router
