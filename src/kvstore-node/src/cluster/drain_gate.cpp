// LLD §4.1 — DrainGate + DrainWatcher implementation.
#include "cluster/drain_gate.h"

namespace kvcache::node::cluster {

std::string DrainKeyFor(const std::string& cluster, const std::string& node_id) {
    return "/kvcache/drain/" + cluster + "/" + node_id;
}

DrainWatcher::DrainWatcher(IEtcdClient& etcd, std::string drain_key,
                           DrainGate& gate)
    : etcd_(etcd), key_(std::move(drain_key)), gate_(gate) {}

DrainWatcher::~DrainWatcher() { Stop(); }

bool DrainWatcher::Start() {
    // Seed: the marker's presence == draining. Get error → leave the
    // gate as-is (fail safe — keep serving rather than wedge on an
    // etcd blip).
    std::string err;
    auto kv = etcd_.Get(key_, &err);
    if (!err.empty()) {
        return false;
    }
    gate_.SetDraining(kv.has_value());

    // Subscribe. WatchPrefix on the exact key (a key is a prefix of
    // itself) delivers Put (drain) and Delete (undrain) events. The
    // callback runs on the etcd client's dispatch thread; flipping an
    // atomic is all we do there.
    handle_ = etcd_.WatchPrefix(key_, [this](const WatchEvent& ev) {
        gate_.SetDraining(ev.type == WatchEventType::kPut);
    });
    watching_ = true;
    return true;
}

void DrainWatcher::Stop() {
    if (watching_) {
        etcd_.Unwatch(handle_);
        watching_ = false;
    }
}

}  // namespace kvcache::node::cluster
