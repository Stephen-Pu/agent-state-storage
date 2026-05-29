// Phase Q-1 — NodeDirectory implementation.
#include "cluster/node_directory.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <thread>

#include "cluster/bloom_publisher.h"  // DecodeBloomSnapshot
#include "cluster/node_registrar.h"  // for Decode/EncodeNodeValue

namespace kvcache::node::cluster {

namespace {

std::string DialTarget(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

std::string KeyToNodeId(const std::string& key, const std::string& prefix) {
    if (key.size() <= prefix.size() ||
        key.compare(0, prefix.size(), prefix) != 0) {
        return {};
    }
    return key.substr(prefix.size());
}

}  // namespace

NodeDirectory::NodeDirectory(IEtcdClient* etcd, routing::HrwRing* ring)
    : NodeDirectory(etcd, ring, Options{}) {}

NodeDirectory::NodeDirectory(IEtcdClient* etcd, routing::HrwRing* ring,
                              Options opts)
    : etcd_(etcd), ring_(ring), opts_(std::move(opts)) {}

NodeDirectory::~NodeDirectory() { Stop(); }

bool NodeDirectory::Start(std::string* err) {
    if (running_.exchange(true)) return true;
    if (!etcd_ || !ring_) {
        if (err) *err = "node_directory: null etcd or ring";
        running_.store(false);
        return false;
    }

    // Phase K-4 — startup decides ONE primary source of truth.
    //
    //   * If the CP leader already published a ClusterView, adopt it
    //     and run "lean" — view watch only, no prefix watch.
    //   * Otherwise, seed from the /kvcache/nodes/ prefix and arm the
    //     prefix watch. The view watch is opened regardless so the
    //     first leader publish promotes us to lean mode.
    //
    // The prefix path is the recovery mode; under steady-state we
    // expect ~1 RPC/event (the view) instead of one event per node.
    bool view_seeded = false;
    if (!opts_.view_key.empty()) {
        std::string vget_err;
        if (auto kv = etcd_->Get(opts_.view_key, &vget_err); kv.has_value()) {
            view_seeded = ApplyClusterViewJson(kv->value);
        }
    }

    if (!view_seeded) {
        // No view yet — start in prefix-driven mode.
        std::lock_guard lk(mu_);
        OpenPrefixWatchLocked();
        if (watch_handle_ == 0) {
            if (err) *err = "node_directory: WatchPrefix failed";
            running_.store(false);
            return false;
        }
    }

    if (!opts_.view_key.empty()) {
        // Always arm the view watch — even when we just seeded from
        // it, a future PUT (next epoch) needs to land somewhere.
        view_watch_handle_ = etcd_->WatchPrefix(opts_.view_key,
            [this](const WatchEvent& ev) { OnClusterViewWatch(ev); });
        // View-watch failure is non-fatal: prefix mode keeps working.
    }

    // Phase K-5 — seed + watch peer bloom sketches. Independent of
    // both the prefix and view paths; sketches are routing hints,
    // not membership data. A failure here is benign (router falls
    // back to pure HRW with no overlap bias).
    if (!opts_.sketch_prefix.empty()) {
        std::string sget_err;
        auto sk_seed = etcd_->GetPrefix(opts_.sketch_prefix, &sget_err);
        for (const auto& kv : sk_seed) {
            const std::string node_id =
                KeyToNodeId(kv.key, opts_.sketch_prefix);
            if (node_id.empty()) continue;
            ApplyPeerSketch(node_id, kv.value);
        }
        sketch_watch_handle_ = etcd_->WatchPrefix(opts_.sketch_prefix,
            [this](const WatchEvent& ev) { OnSketchWatch(ev); });
    }
    return true;
}

void NodeDirectory::Stop() {
    if (!running_.exchange(false)) return;
    if (watch_handle_ != 0 && etcd_) {
        etcd_->Unwatch(watch_handle_);
        watch_handle_ = 0;
    }
    if (view_watch_handle_ != 0 && etcd_) {
        etcd_->Unwatch(view_watch_handle_);
        view_watch_handle_ = 0;
    }
    if (sketch_watch_handle_ != 0 && etcd_) {
        etcd_->Unwatch(sketch_watch_handle_);
        sketch_watch_handle_ = 0;
    }
}

// Phase K-5 — peer sketch handlers.
void NodeDirectory::OnSketchWatch(const WatchEvent& ev) {
    const std::string node_id =
        KeyToNodeId(ev.kv.key, opts_.sketch_prefix);
    if (node_id.empty()) return;
    if (ev.type == WatchEventType::kDelete) {
        std::lock_guard lk(mu_);
        sketches_.erase(node_id);
        return;
    }
    ApplyPeerSketch(node_id, ev.kv.value);
}

void NodeDirectory::ApplyPeerSketch(const std::string& node_id,
                                      const std::string& blob) {
    routing::BloomParams params{0, 0};
    std::vector<uint8_t> bytes;
    if (!DecodeBloomSnapshot(blob, &params, &bytes)) return;
    std::lock_guard lk(mu_);
    auto it = sketches_.find(node_id);
    if (it == sketches_.end()) {
        auto fresh = std::make_unique<routing::AggregatedBloom>(params);
        fresh->Set(params, std::move(bytes));
        sketches_.emplace(node_id, std::move(fresh));
    } else {
        it->second->Set(params, std::move(bytes));
    }
}

bool NodeDirectory::PeerMaybeHas(const std::string& node_id,
                                   std::span<const uint8_t> key) const {
    // Snapshot the pointer under our mutex; AggregatedBloom's own
    // mutex handles the read.
    routing::AggregatedBloom* sketch = nullptr;
    {
        std::lock_guard lk(mu_);
        auto it = sketches_.find(node_id);
        if (it == sketches_.end()) return false;
        sketch = it->second.get();
    }
    return sketch->MaybeContains(key);
}

std::size_t NodeDirectory::SketchCount() const noexcept {
    std::lock_guard lk(mu_);
    return sketches_.size();
}

// Phase K-3 — incoming events on /kvcache/cluster/view. Delete is the
// signal that the leader's lease expired (or someone explicitly
// dropped it). Treat that as "view is gone; the prefix watch is the
// source of truth again" by resetting the epoch tracking — a fresh
// leader's epoch=1 will be accepted next.
void NodeDirectory::OnClusterViewWatch(const WatchEvent& ev) {
    if (ev.type == WatchEventType::kDelete) {
        bool need_reopen = false;
        {
            std::lock_guard lk(mu_);
            last_view_leader_.clear();
            last_view_epoch_ = 0;
            if (view_active_) {
                view_active_  = false;
                need_reopen   = true;
            }
        }
        // Phase K-4 — leader's view is gone; promote the prefix
        // watch back to primary so the table keeps converging.
        // We're inside the etcd dispatcher's mutex right now;
        // OpenPrefixWatch calls back INTO etcd (GetPrefix +
        // WatchPrefix), which would re-acquire the same mutex →
        // self-deadlock. Detach a thread to break the call chain.
        if (need_reopen) {
            std::thread([this] { OpenPrefixWatch(); }).detach();
        }
        return;
    }
    ApplyClusterViewJson(ev.kv.value);
}

bool NodeDirectory::ApplyClusterViewJson(const std::string& json) {
    if (json.empty()) return false;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (...) {
        // Bad JSON shouldn't sink the directory — the prefix-watch
        // path keeps the table fresh on its own.
        return false;
    }
    const auto leader_id = doc.value("leader_id", std::string{});
    const uint64_t epoch = doc.value("epoch", uint64_t{0});

    // Phase K-4 — track the prefix-watch handle to close OUTSIDE the
    // lock to avoid invert-order deadlock with the etcd dispatcher.
    WatchHandle prefix_handle_to_close = 0;
    bool accepted = false;
    {
        std::lock_guard lk(mu_);
        if (leader_id == last_view_leader_ && epoch <= last_view_epoch_) {
            return false;  // stale
        }
        last_view_leader_ = leader_id;
        last_view_epoch_  = epoch;
        accepted = true;

        // Adopt the view's membership wholesale.
        table_.clear();
        const auto& nodes = doc.value("nodes", nlohmann::json::array());
        for (const auto& n : nodes) {
            const auto id   = n.value("node_id", std::string{});
            const auto host = n.value("host",    std::string{});
            const uint16_t port = static_cast<uint16_t>(
                n.value("grpc_port", uint32_t{0}));
            if (id.empty() || host.empty() || port == 0) continue;
            table_[id] = NodeEndpoint{host, port, DialTarget(host, port)};
        }
        RebuildRingLocked();

        if (!view_active_) {
            view_active_ = true;
            // Hand off the prefix-watch handle for closure below.
            prefix_handle_to_close = watch_handle_;
            watch_handle_          = 0;
        }
    }
    if (prefix_handle_to_close != 0 && etcd_) {
        // Phase K-4 — Unwatch from a detached thread.
        //
        // ApplyClusterViewJson is invoked synchronously from the
        // etcd watcher callback path, and the InMemoryEtcdClient (+
        // some real backends) holds its dispatcher mutex during
        // callback delivery. Calling Unwatch directly here would
        // re-enter that same mutex → self-deadlock. The detached
        // thread breaks the call chain so Unwatch sees the
        // dispatcher mutex released and completes cleanly.
        IEtcdClient* e = etcd_;
        const WatchHandle h = prefix_handle_to_close;
        std::thread([e, h] { e->Unwatch(h); }).detach();
    }
    return accepted;
}

// Open the prefix watch under mu_, having re-seeded from the live
// /kvcache/nodes/ prefix so any deltas we missed while in view-mode
// are reflected in the table immediately. Safe to call from
// OnClusterViewWatch's delete handler because OnClusterViewWatch
// releases its own lock_guard before invoking us — see the comment
// at the call site.
void NodeDirectory::OpenPrefixWatchLocked() {
    if (watch_handle_ != 0 || !etcd_) return;
    std::string ignore;
    auto seed = etcd_->GetPrefix(opts_.key_prefix, &ignore);
    table_.clear();
    for (const auto& kv : seed) {
        const std::string id = KeyToNodeId(kv.key, opts_.key_prefix);
        if (id.empty()) continue;
        std::string host;
        uint16_t    port = 0;
        if (!DecodeNodeValue(kv.value, &host, &port)) continue;
        table_[id] = NodeEndpoint{host, port, DialTarget(host, port)};
    }
    RebuildRingLocked();
    watch_handle_ = etcd_->WatchPrefix(opts_.key_prefix,
        [this](const WatchEvent& ev) { OnWatch(ev); });
}

// Public-shaped helper used by OnClusterViewWatch's delete branch.
// Takes mu_, does the re-seed + Watch, releases.
void NodeDirectory::OpenPrefixWatch() {
    std::lock_guard lk(mu_);
    OpenPrefixWatchLocked();
}

void NodeDirectory::OnWatch(const WatchEvent& ev) {
    const std::string node_id =
        KeyToNodeId(ev.kv.key, opts_.key_prefix);
    if (node_id.empty()) return;

    std::lock_guard lk(mu_);
    // Phase K-6 — view-mode wins. When ApplyClusterViewJson flips
    // view_active_=true it also fires off a detached thread to call
    // etcd_->Unwatch on the prefix handle (the detach is mandatory
    // because Unwatch would otherwise self-deadlock on the etcd
    // dispatcher's mutex). On Linux runners under load that thread
    // can be scheduled tens-to-hundreds of milliseconds later, and
    // any /kvcache/nodes/ PUT racing in that window otherwise
    // mutates the table behind the view's back — corrupting
    // membership and breaking the K-3 invariant "the CP-published
    // ClusterView is the SOLE source of truth while it's live."
    // Drop the event at the entry point; the detached Unwatch will
    // catch up eventually and the source dries up at that point.
    if (view_active_) return;
    if (ev.type == WatchEventType::kDelete) {
        table_.erase(node_id);
    } else {
        std::string host;
        uint16_t    port = 0;
        if (!DecodeNodeValue(ev.kv.value, &host, &port)) return;
        table_[node_id] = NodeEndpoint{host, port, DialTarget(host, port)};
    }
    RebuildRingLocked();
}

void NodeDirectory::RebuildRingLocked() {
    std::vector<routing::NodeEntry> entries;
    entries.reserve(table_.size());
    for (const auto& [id, ep] : table_) {
        entries.push_back(routing::NodeEntry{id, /*traffic_weight=*/1.0});
    }
    ring_->SetNodes(std::move(entries));
}

std::vector<std::string> NodeDirectory::NodeIds() const {
    std::lock_guard lk(mu_);
    std::vector<std::string> ids;
    ids.reserve(table_.size());
    for (const auto& [id, _] : table_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::optional<NodeEndpoint> NodeDirectory::Resolve(
    const std::string& node_id) const {
    std::lock_guard lk(mu_);
    auto it = table_.find(node_id);
    if (it == table_.end()) return std::nullopt;
    return it->second;
}

std::size_t NodeDirectory::NodeCount() const noexcept {
    std::lock_guard lk(mu_);
    return table_.size();
}

}  // namespace kvcache::node::cluster
