// SS-2 B-plane spike, Task 1 — Entry carries state_kind; Insert threads it.
#include "tier/dram_tier.h"

#include <gtest/gtest.h>
#include <vector>

using kvcache::common::SK_KV;
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
