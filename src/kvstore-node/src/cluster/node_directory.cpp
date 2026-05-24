// Phase Q-1 — NodeDirectory implementation.
#include "cluster/node_directory.h"

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
    return true;
}

void NodeDirectory::Stop() {
    if (!running_.exchange(false)) return;
    if (watch_handle_ != 0 && etcd_) {
        etcd_->Unwatch(watch_handle_);
        watch_handle_ = 0;
    }
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
