// LLD §2.1 — Locator implementation.
#include "locator.h"

#include <cstring>

#include "b3_facade.h"

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

namespace {

// Explicit little-endian scalar (de)serialization — independent of host
// byte order, so the 64-byte wire format is identical on x86-64, arm64, and
// any future big-endian peer. Byte arrays (tenant_id, prefix_hash) are
// endian-neutral and copied verbatim.
inline void PutU16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void PutU32LE(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void PutU64LE(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint16_t GetU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t GetU32LE(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
inline uint64_t GetU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

}  // namespace

bool SerializeLocator(const kv_locator_t& loc, std::span<uint8_t, 64> out) {
    static_assert(sizeof(kv_locator_t) == 64);
    uint8_t* p = out.data();
    // Field offsets match the kv_locator_t struct layout (LLD §2.1), so on a
    // little-endian host the encoded bytes are byte-identical to the old raw
    // memcpy — backward compatible — while a big-endian host now emits the
    // same canonical layout instead of a byte-swapped one.
    std::memcpy(p + 0, loc.tenant_id, 16);
    PutU64LE(p + 16, loc.model_id_hash);
    std::memcpy(p + 24, loc.prefix_hash, 16);
    PutU16LE(p + 40, loc.range.layer_start);
    PutU16LE(p + 42, loc.range.layer_count);
    PutU16LE(p + 44, loc.range.head_start);
    PutU16LE(p + 46, loc.range.head_count);
    PutU32LE(p + 48, loc.range.token_start);
    PutU32LE(p + 52, loc.range.token_count);
    PutU32LE(p + 56, loc.version);
    PutU32LE(p + 60, loc.flags);
    return true;
}

bool DeserializeLocator(std::span<const uint8_t, 64> in, kv_locator_t* out) {
    const uint8_t* p = in.data();
    kv_locator_t loc{};
    std::memcpy(loc.tenant_id, p + 0, 16);
    loc.model_id_hash = GetU64LE(p + 16);
    std::memcpy(loc.prefix_hash, p + 24, 16);
    loc.range.layer_start = GetU16LE(p + 40);
    loc.range.layer_count = GetU16LE(p + 42);
    loc.range.head_start  = GetU16LE(p + 44);
    loc.range.head_count  = GetU16LE(p + 46);
    loc.range.token_start = GetU32LE(p + 48);
    loc.range.token_count = GetU32LE(p + 52);
    loc.version = GetU32LE(p + 56);
    loc.flags   = GetU32LE(p + 60);
    if (loc.version != 1) return false;
    *out = loc;
    return true;
}

uint64_t ComputeModelIdHash(std::string_view canonical_model_id) {
    return hash::Blake3_64({
        reinterpret_cast<const uint8_t*>(canonical_model_id.data()),
        canonical_model_id.size()});
}

}  // namespace kvcache
