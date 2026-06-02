// LLD §4.1 — node-side drain reaction.
//
// Phase A2.2 — the node half of the drain workflow. `kvctl drain` writes
// a marker to etcd /kvcache/drain/<cluster>/<node_id> and the CP overlays
// DRAINING onto the cluster view so routers stop sending NEW prefixes
// (A2.1). But a node also wants to refuse new *writes* it still receives
// in the brief window before the view propagates, and to make its
// draining state authoritative locally rather than inferred.
//
// DrainGate is that local authority: an atomic flag the data path checks
// before admitting new work (Reserve). DrainWatcher drives the flag from
// etcd — it watches the node's own drain key and flips the gate on
// Put / Delete, with an initial Get to seed state at startup.
//
// What draining does NOT block: reads (Lookup/Fetch) and the completion
// of in-flight writes (a handle already Reserved can still Seal). Only
// *new* Reserves are rejected, so a draining node finishes what it
// started and bleeds out cleanly.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "cluster/etcd_client.h"

namespace kvcache::node::cluster {

// The drain-marker key for (cluster, node_id) — must match
// control-plane/internal/membership.DrainKey and kvctl's drainKey.
std::string DrainKeyFor(const std::string& cluster, const std::string& node_id);

// Thread-safe boolean the data path consults. Cheap to read (relaxed
// atomic) so it's fine on the Reserve hot path.
class DrainGate {
   public:
    bool draining() const noexcept {
        return draining_.load(std::memory_order_relaxed);
    }
    // True when the node should refuse NEW work (new Reserves). Today
    // that's exactly `draining()`; kept as a named predicate so the
    // call sites read intent and a future "soft drain" can refine it.
    bool ShouldRejectNewWork() const noexcept { return draining(); }

    void SetDraining(bool v) noexcept {
        draining_.store(v, std::memory_order_relaxed);
    }

   private:
    std::atomic<bool> draining_{false};
};

// Watches a node's drain key via IEtcdClient and keeps a DrainGate in
// sync. Construct, then Start(); Stop() (or the dtor) unsubscribes.
class DrainWatcher {
   public:
    DrainWatcher(IEtcdClient& etcd, std::string drain_key, DrainGate& gate);
    ~DrainWatcher();

    DrainWatcher(const DrainWatcher&)            = delete;
    DrainWatcher& operator=(const DrainWatcher&) = delete;

    // Seed the gate from the key's current value (Get), then subscribe
    // to changes. Returns false on the initial Get error (the gate is
    // left as-is — fail safe: a node that can't read its drain state
    // keeps serving rather than wedging).
    bool Start();
    void Stop();

   private:
    IEtcdClient& etcd_;
    std::string  key_;
    DrainGate&   gate_;
    WatchHandle  handle_ = 0;
    bool         watching_ = false;
};

}  // namespace kvcache::node::cluster
