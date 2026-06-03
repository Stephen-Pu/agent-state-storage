// LLD §5.2 — IdentityWatcher implementation.
#include "cluster/identity_watcher.h"

#include <nlohmann/json.hpp>

namespace kvcache::node::cluster {

IdentityWatcher::IdentityWatcher(IEtcdClient& etcd,
                                 security::MtlsRegistry& registry,
                                 std::string prefix)
    : etcd_(etcd), registry_(registry), prefix_(std::move(prefix)) {}

IdentityWatcher::~IdentityWatcher() { Stop(); }

std::optional<IdentityWatcher::Entry> IdentityWatcher::ParseEntry(
    const std::string& json) {
    nlohmann::json j = nlohmann::json::parse(json, /*cb=*/nullptr,
                                             /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    Entry e;
    if (auto it = j.find("spiffe_id"); it != j.end() && it->is_string()) {
        e.spiffe_id = it->get<std::string>();
    }
    if (auto it = j.find("cn"); it != j.end() && it->is_string()) {
        e.cn = it->get<std::string>();
    }
    // An entry with neither a spiffe_id nor a CN can't be keyed → reject.
    if (e.spiffe_id.empty() && e.cn.empty()) return std::nullopt;

    e.identity.cn = e.cn;
    if (auto it = j.find("tenant"); it != j.end() && it->is_string()) {
        e.identity.tenant = it->get<std::string>();
    }
    if (auto it = j.find("kind"); it != j.end() && it->is_string()) {
        const std::string k = it->get<std::string>();
        if (k == "tenant")        e.identity.kind = security::IdentityKind::kTenant;
        else if (k == "internal") e.identity.kind = security::IdentityKind::kInternal;
        else if (k == "admin")    e.identity.kind = security::IdentityKind::kAdmin;
    }
    return e;
}

void IdentityWatcher::Apply(const Entry& e) {
    if (!e.spiffe_id.empty()) registry_.UpsertSpiffeMapping(e.spiffe_id, e.identity);
    if (!e.cn.empty())        registry_.UpsertMapping(e.cn, e.identity);
}

void IdentityWatcher::Remove(const Entry& e) {
    if (!e.spiffe_id.empty()) registry_.RemoveSpiffeMapping(e.spiffe_id);
    if (!e.cn.empty())        registry_.RemoveMapping(e.cn);
}

bool IdentityWatcher::Start() {
    std::string err;
    auto kvs = etcd_.GetPrefix(prefix_, &err);
    if (!err.empty()) return false;  // fail safe — keep existing mappings
    for (const auto& kv : kvs) {
        if (auto e = ParseEntry(kv.value)) Apply(*e);
    }

    handle_ = etcd_.WatchPrefix(prefix_, [this](const WatchEvent& ev) {
        if (ev.type == WatchEventType::kPut) {
            if (auto e = ParseEntry(ev.kv.value)) Apply(*e);
        } else {  // kDelete — use prev_kv to know what to remove
            if (auto e = ParseEntry(ev.prev_kv.value)) Remove(*e);
        }
    });
    watching_ = true;
    return true;
}

void IdentityWatcher::Stop() {
    if (watching_) {
        etcd_.Unwatch(handle_);
        watching_ = false;
    }
}

}  // namespace kvcache::node::cluster
