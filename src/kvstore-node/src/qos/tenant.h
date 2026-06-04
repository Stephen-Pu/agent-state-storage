// LLD §5.1 — Tenant registry. Flat model.
//
// One TenantRegistry per node. Owns the canonical map from tenant_id
// (16-byte UUID) → TenantConfig. The registry is updated:
//   * At node bring-up, from the local RocksDB cache (TenantQuotaSnapshot).
//   * At runtime, by watching the etcd prefix /kvcache/tenants/<cluster>/
//     that the operator's EtcdTenantPublisher (Phase H-4) writes. (The
//     original LLD §4.1 design pushed quota over the CP Sync stream; that
//     was superseded by the etcd-watch path.)
//
// All quota / priority lookups in hot path go through this registry's
// O(1) hash; for cross-subsystem propagation we hash the 16-byte tenant_id
// into a uint64 and pass that around inside PriorityContext.
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "qos/priority_ctx.h"
#include "qos/quota.h"

namespace kvcache::node::qos {

using TenantUuid = std::array<uint8_t, 16>;

struct TenantConfig {
    TenantUuid   tenant_id;
    std::string  display_name;
    QuotaLimits  quota;
    Priority     default_priority = kDefaultPriority;
    bool         deletion_pending = false;  // right-to-erase in progress
};

// FNV-1a 64-bit over the 16-byte UUID. Placeholder until BLAKE3-64 is wired.
uint64_t TenantHash(const TenantUuid& id);

class TenantRegistry {
   public:
    TenantRegistry() = default;
    ~TenantRegistry() = default;

    void Upsert(const TenantConfig& cfg);
    bool Remove(const TenantUuid& id);

    std::optional<TenantConfig> Get(const TenantUuid& id) const;
    std::optional<TenantConfig> GetByHash(uint64_t hash) const;

    std::size_t Size() const noexcept;

   private:
    mutable std::mutex mu_;
    // Two maps for two lookup styles. Memory cost is trivial.
    std::unordered_map<uint64_t, TenantConfig> by_hash_;
};

}  // namespace kvcache::node::qos
