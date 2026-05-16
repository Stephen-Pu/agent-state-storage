// LLD §5.1 — TenantRegistry.
#include "qos/tenant.h"

namespace kvcache::node::qos {

uint64_t TenantHash(const TenantUuid& id) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (auto b : id) {
        h ^= b;
        h *= 0x100000001b3ULL;
    }
    return h;
}

void TenantRegistry::Upsert(const TenantConfig& cfg) {
    std::lock_guard lk(mu_);
    by_hash_[TenantHash(cfg.tenant_id)] = cfg;
}

bool TenantRegistry::Remove(const TenantUuid& id) {
    std::lock_guard lk(mu_);
    return by_hash_.erase(TenantHash(id)) > 0;
}

std::optional<TenantConfig> TenantRegistry::Get(const TenantUuid& id) const {
    std::lock_guard lk(mu_);
    auto it = by_hash_.find(TenantHash(id));
    if (it == by_hash_.end()) return std::nullopt;
    return it->second;
}

std::optional<TenantConfig> TenantRegistry::GetByHash(uint64_t hash) const {
    std::lock_guard lk(mu_);
    auto it = by_hash_.find(hash);
    if (it == by_hash_.end()) return std::nullopt;
    return it->second;
}

std::size_t TenantRegistry::Size() const noexcept {
    std::lock_guard lk(mu_);
    return by_hash_.size();
}

}  // namespace kvcache::node::qos
