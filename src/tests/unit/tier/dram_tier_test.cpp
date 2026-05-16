#include "tier/dram_tier.h"

#include <gtest/gtest.h>
#include <vector>

using kvcache::node::tier::DramKey;
using kvcache::node::tier::DramTier;

namespace {
DramKey Key(uint8_t b) {
    DramKey k{};
    k.bytes[0] = b;
    return k;
}
std::vector<uint8_t> Bytes(uint8_t b, std::size_t n) {
    return std::vector<uint8_t>(n, b);
}
}  // namespace

TEST(DramTierTest, InsertAndLookup_A1in) {
    DramTier::Options o;
    o.capacity_bytes    = 1024;
    o.a1out_max_entries = 16;
    DramTier t(o);

    auto data = Bytes(1, 64);
    t.Insert(Key(1), data.data(), data.size());
    auto r = t.Lookup(Key(1));
    EXPECT_EQ(r.where, DramTier::HitWhere::kA1in);
    ASSERT_NE(r.data, nullptr);
    EXPECT_EQ(r.data_bytes, 64u);
}

TEST(DramTierTest, GhostHitPromotesToAm) {
    DramTier::Options o;
    o.capacity_bytes    = 256;
    o.a1out_max_entries = 16;
    DramTier t(o);
    // a1in capacity = 64 → second 64B insert evicts the first from A1in.
    t.Insert(Key(1), Bytes(1, 64).data(), 64);
    t.Insert(Key(2), Bytes(2, 64).data(), 64);
    // Key 1 should now be in the ghost queue.
    auto r = t.Lookup(Key(1));
    EXPECT_EQ(r.where, DramTier::HitWhere::kGhost);
    // Re-insert; should land in Am.
    t.Insert(Key(1), Bytes(1, 64).data(), 64);
    r = t.Lookup(Key(1));
    EXPECT_EQ(r.where, DramTier::HitWhere::kAm);
}

TEST(DramTierTest, CapacityIsEnforced) {
    DramTier::Options o;
    o.capacity_bytes    = 1024;
    o.a1out_max_entries = 64;
    DramTier t(o);
    for (int i = 0; i < 100; ++i) {
        t.Insert(Key(static_cast<uint8_t>(i)), Bytes(i, 64).data(), 64);
    }
    EXPECT_LE(t.UsedBytes(), 1024u);
}

TEST(DramTierTest, EraseHits) {
    DramTier::Options o;
    o.capacity_bytes    = 1024;
    o.a1out_max_entries = 16;
    DramTier t(o);
    t.Insert(Key(7), Bytes(7, 64).data(), 64);
    EXPECT_TRUE(t.Erase(Key(7)));
    auto r = t.Lookup(Key(7));
    EXPECT_EQ(r.where, DramTier::HitWhere::kMiss);
}
