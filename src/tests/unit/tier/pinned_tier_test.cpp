#include "tier/pinned_tier.h"

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using kvcache::node::tier::PinnedTier;
using kvcache::node::tier::SlotDesc;

TEST(PinnedTierTest, RejectsBadOptions) {
    PinnedTier::Options bad;
    bad.pool_bytes = 100;
    bad.slot_bytes = 64;  // not a divisor of 100
    std::string err;
    EXPECT_EQ(PinnedTier::Create(bad, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(PinnedTierTest, AcquireReleaseRoundTrip) {
    PinnedTier::Options o;
    o.pool_bytes = 4 * 4096;
    o.slot_bytes = 4096;
    o.use_mlock  = false;
    std::string err;
    auto t = PinnedTier::Create(o, &err);
    ASSERT_NE(t, nullptr) << err;
    EXPECT_EQ(t->SlotCount(), 4u);

    std::vector<SlotDesc> held;
    for (int i = 0; i < 4; ++i) {
        auto s = t->Acquire();
        ASSERT_TRUE(s.has_value());
        EXPECT_EQ(s->bytes, 4096u);
        held.push_back(*s);
    }
    EXPECT_FALSE(t->Acquire().has_value());

    for (auto& s : held) t->Release(s.id);
    EXPECT_TRUE(t->Acquire().has_value());
}

TEST(PinnedTierTest, SlotsAreWritable) {
    PinnedTier::Options o;
    o.pool_bytes = 4096;
    o.slot_bytes = 4096;
    o.use_mlock  = false;
    std::string err;
    auto t = PinnedTier::Create(o, &err);
    ASSERT_NE(t, nullptr);
    auto s = t->Acquire();
    ASSERT_TRUE(s.has_value());
    std::memset(s->addr, 0xAB, s->bytes);
    EXPECT_EQ(*static_cast<uint8_t*>(s->addr), 0xABu);
    t->Release(s->id);
}

TEST(PinnedTierTest, MrKeyComesFromCallback) {
    PinnedTier::Options o;
    o.pool_bytes = 4096;
    o.slot_bytes = 4096;
    o.use_mlock  = false;
    o.register_region = [](void*, uint64_t) -> uint32_t { return 0xDEADBEEF; };
    std::string err;
    auto t = PinnedTier::Create(o, &err);
    ASSERT_NE(t, nullptr);
    auto s = t->Acquire();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->mr_key, 0xDEADBEEFu);
}
