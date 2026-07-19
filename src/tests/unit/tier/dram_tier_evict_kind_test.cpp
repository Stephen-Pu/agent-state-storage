// SS-2 B-plane spike, Task 1 — Entry carries state_kind; Insert threads it.
// SS-2 B-plane spike, Task 2 — DramTier dispatches eviction by each entry's
// real state_kind via the ValuePolicyRegistry (no more synthetic SK_KV).
#include "tier/dram_tier.h"

#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "value_policy.h"
#include "value_policy_kv.h"
#include "value_policy_persistent_stub.h"

using kvcache::common::SK_KV;
using kvcache::common::SK_MEMORY;
using kvcache::common::ValuePolicyKv;
using kvcache::common::ValuePolicyPersistentStub;
using kvcache::common::ValuePolicyRegistry;
using kvcache::node::tier::DramKey;
using kvcache::node::tier::DramTier;

namespace {
DramTier::Options SmallOpts() {
    DramTier::Options o;
    o.capacity_bytes    = 1024;
    o.a1out_max_entries = 16;
    return o;
}
}  // namespace

TEST(DramTierKind, InsertRecordsKindDefaultKv) {
    DramTier t(SmallOpts());
    DramKey k{};
    k.bytes[0] = 1;
    std::vector<uint8_t> v(10, 0xAA);
    t.Insert(k, v.data(), v.size());  // default kind
    ASSERT_TRUE(t.KindOf(k).has_value());
    EXPECT_EQ(*t.KindOf(k), static_cast<uint16_t>(SK_KV));

    DramKey k2{};
    k2.bytes[0] = 2;
    t.Insert(k2, v.data(), v.size(), /*state_kind=*/16);  // e.g. SK_MEMORY
    ASSERT_TRUE(t.KindOf(k2).has_value());
    EXPECT_EQ(*t.KindOf(k2), 16u);
}

TEST(DramTierKind, KindOfMissingKeyIsNullopt) {
    DramTier t(SmallOpts());
    DramKey missing{};
    missing.bytes[0] = 99;
    EXPECT_FALSE(t.KindOf(missing).has_value());
}

TEST(DramTierKind, InPlaceReplaceUpdatesKind) {
    DramTier t(SmallOpts());
    DramKey k{};
    k.bytes[0] = 3;
    std::vector<uint8_t> v(10, 0xBB);
    t.Insert(k, v.data(), v.size());  // default SK_KV
    ASSERT_TRUE(t.KindOf(k).has_value());
    EXPECT_EQ(*t.KindOf(k), static_cast<uint16_t>(SK_KV));

    // Replace in place (same key, still resident) with a new kind.
    t.Insert(k, v.data(), v.size(), /*state_kind=*/16);
    ASSERT_TRUE(t.KindOf(k).has_value());
    EXPECT_EQ(*t.KindOf(k), 16u);
}

// SS-2 B-plane spike, Task 2 — the tier now dispatches shouldEvict by each
// entry's own state_kind through the registry, instead of the earlier
// spike's synthetic SK_KV-only projection. IsNotEvictable is private, so we
// assert through eviction behavior (mirrors DramTierTest.GhostHitPromotesToAm
// in dram_tier_test.cpp): with a 2-policy registry wired in
// (SK_KV -> ValuePolicyKv, SK_MEMORY -> ValuePolicyPersistentStub), a KV
// entry must still evict exactly as it did with no registry at all —
// ValuePolicyKv::shouldEvict is unconditionally kEvictable, so the
// registry's presence (and the unrelated SK_MEMORY policy sharing it) must
// not change the KV path's victims. Task 3 exercises the SK_MEMORY
// not-evictable path; this only proves the registry wiring compiles and
// dispatches correctly for the kind that's actually present.
TEST(DramTierKind, KvEntryStillEvictsNormallyWithRegistryWired) {
    ValuePolicyRegistry reg;
    reg.registerPolicy(SK_KV, std::make_unique<ValuePolicyKv>());
    reg.registerPolicy(SK_MEMORY, std::make_unique<ValuePolicyPersistentStub>());

    DramTier::Options o;
    o.capacity_bytes    = 256;   // a1in capacity = 64
    o.a1out_max_entries = 16;
    o.policy_registry   = &reg;
    DramTier t(o);

    DramKey k1{};
    k1.bytes[0] = 1;
    DramKey k2{};
    k2.bytes[0] = 2;
    std::vector<uint8_t> v(64, 0xCC);

    // Default state_kind on Insert() is SK_KV. First insert fits within the
    // 64-byte A1in budget; the second forces the first out into the ghost
    // queue — same victim as the no-registry case (registry-gated
    // shouldEvict for SK_KV is always kEvictable).
    t.Insert(k1, v.data(), v.size());
    t.Insert(k2, v.data(), v.size());

    EXPECT_FALSE(t.KindOf(k1).has_value());  // evicted out of A1in/Am
    ASSERT_TRUE(t.KindOf(k2).has_value());
    EXPECT_EQ(*t.KindOf(k2), static_cast<uint16_t>(SK_KV));

    auto r1 = t.Lookup(k1);
    EXPECT_EQ(r1.where, DramTier::HitWhere::kGhost);
}
