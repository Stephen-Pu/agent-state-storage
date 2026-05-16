// LLD §2.1 — Locator implementation.
#include "locator.h"

#include <cstring>

#include "blake3.h"

namespace kvcache {

kv_locator_t MakeLocator(TenantId tenant, uint64_t model_id_hash,
                         PrefixHash prefix, kv_range_t range) {
    kv_locator_t loc{};
    std::memcpy(loc.tenant_id, tenant.data(), tenant.size());
    loc.model_id_hash = model_id_hash;
    std::memcpy(loc.prefix_hash, prefix.data(), prefix.size());
    loc.range = range;
    loc.version = 1;
    loc.flags = 0;
    return loc;
}

uint64_t LocatorIdentityHash(const kv_locator_t& loc) {
    // Identity = tenant_id (16B) | model_id_hash (8B) | prefix_hash (16B) = 40B.
    // Range is intentionally excluded so all sub-ranges of one prefix hash
    // collide into the same HRW bucket (LLD §4.2).
    uint8_t buf[40];
    std::memcpy(buf,      loc.tenant_id,           16);
    std::memcpy(buf + 16, &loc.model_id_hash,       8);
    std::memcpy(buf + 24, loc.prefix_hash,         16);
    return hash::Blake3_64({buf, sizeof(buf)});
}

bool LocatorIdentityEq(const kv_locator_t& a, const kv_locator_t& b) {
    return std::memcmp(a.tenant_id, b.tenant_id, sizeof(a.tenant_id)) == 0 &&
           a.model_id_hash == b.model_id_hash &&
           std::memcmp(a.prefix_hash, b.prefix_hash, sizeof(a.prefix_hash)) == 0;
}

bool SerializeLocator(const kv_locator_t& loc, std::span<uint8_t, 64> out) {
    // TODO(stephen): explicit little-endian encoding once we add big-endian CI.
    static_assert(sizeof(kv_locator_t) == 64);
    std::memcpy(out.data(), &loc, 64);
    return true;
}

bool DeserializeLocator(std::span<const uint8_t, 64> in, kv_locator_t* out) {
    std::memcpy(out, in.data(), 64);
    return out->version == 1;
}

uint64_t ComputeModelIdHash(std::string_view canonical_model_id) {
    return hash::Blake3_64({
        reinterpret_cast<const uint8_t*>(canonical_model_id.data()),
        canonical_model_id.size()});
}

}  // namespace kvcache
