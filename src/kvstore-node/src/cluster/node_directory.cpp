// Phase Q-1 — NodeDirectory implementation.
#include "cluster/node_directory.h"

#include <nlohmann/json.hpp>

#include <algorithm>

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

    // Seed from a one-shot GetPrefix so a freshly-started node sees its
    // peers immediately — Watch alone would only catch CHANGES from the
    // current revision forward.
    auto seed = etcd_->GetPrefix(opts_.key_prefix, err);
    {
        std::lock_guard lk(mu_);
        for (const auto& kv : seed) {
            const std::string node_id = KeyToNodeId(kv.key, opts_.key_prefix);
            if (node_id.empty()) continue;
            std::string host;
            uint16_t    port = 0;
            if (!DecodeNodeValue(kv.value, &host, &port)) continue;
            table_[node_id] = NodeEndpoint{host, port, DialTarget(host, port)};
        }
        RebuildRingLocked();
    }

    watch_handle_ = etcd_->WatchPrefix(opts_.key_prefix,
        [this](const WatchEvent& ev) { OnWatch(ev); });
    if (watch_handle_ == 0) {
        if (err) *err = "node_directory: WatchPrefix failed";
        running_.store(false);
        return false;
    }

    // Phase K-3 — also watch the CP's ClusterView snapshot.
    if (!opts_.view_key.empty()) {
        // Seed from any view the leader already published. Missing
        // key is OK — fall back to the prefix-watch path until the
        // first publish lands.
        std::string vget_err;
        if (auto kv = etcd_->Get(opts_.view_key, &vget_err); kv.has_value()) {
            ApplyClusterViewJson(kv->value);
        }
        // Single-key watch via WatchPrefix on the exact key works
        // because prefix matches the key itself and excludes any
        // sibling keys (none exist under /kvcache/cluster/ today).
        view_watch_handle_ = etcd_->WatchPrefix(opts_.view_key,
            [this](const WatchEvent& ev) { OnClusterViewWatch(ev); });
        // Watch failure on the view path is non-fatal: the prefix
        // watch above keeps the table converging. Log via err only
        // if it's still null (don't overwrite the more important
        // primary-watch err).
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
}

// Phase K-3 — incoming events on /kvcache/cluster/view. Delete is the
// signal that the leader's lease expired (or someone explicitly
// dropped it). Treat that as "view is gone; the prefix watch is the
// source of truth again" by resetting the epoch tracking — a fresh
// leader's epoch=1 will be accepted next.
void NodeDirectory::OnClusterViewWatch(const WatchEvent& ev) {
    if (ev.type == WatchEventType::kDelete) {
        std::lock_guard lk(mu_);
        last_view_leader_.clear();
        last_view_epoch_ = 0;
        return;
    }
    ApplyClusterViewJson(ev.kv.value);
}

void NodeDirectory::ApplyClusterViewJson(const std::string& json) {
    if (json.empty()) return;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (...) {
        // Bad JSON shouldn't sink the directory — the prefix-watch
        // path keeps the table fresh on its own.
        return;
    }
    const auto leader_id = doc.value("leader_id", std::string{});
    const uint64_t epoch = doc.value("epoch", uint64_t{0});

    std::lock_guard lk(mu_);
    // Drop stale / out-of-order events from the same leader. A
    // different leader ID always resets the epoch threshold (a new
    // leader's epoch=1 must be accepted).
    if (leader_id == last_view_leader_ && epoch <= last_view_epoch_) {
        return;
    }
    last_view_leader_ = leader_id;
    last_view_epoch_  = epoch;

    // Adopt the view's membership wholesale. The view IS authoritative
    // by construction (CP derived it from /kvcache/nodes/ at publish
    // time), so we can replace the table without losing data — at
    // worst we briefly drop a node the prefix watch will re-add.
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
}

void NodeDirectory::OnWatch(const WatchEvent& ev) {
    const std::string node_id =
        KeyToNodeId(ev.kv.key, opts_.key_prefix);
    if (node_id.empty()) return;

    std::lock_guard lk(mu_);
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
