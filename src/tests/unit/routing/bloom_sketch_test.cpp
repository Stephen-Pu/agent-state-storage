#include "routing/bloom_sketch.h"

#include <gtest/gtest.h>
#include <vector>

using namespace kvcache::node::routing;

namespace {
std::vector<uint8_t> Key(int i) {
    std::vector<uint8_t> k(16, 0);
    for (int b = 0; b < 4; ++b) k[b] = static_cast<uint8_t>((i >> (b * 8)) & 0xff);
    return k;
}
}  // namespace

TEST(BloomParamsTest, FormulaIsSensible) {
    auto p = BloomParams::ForCapacity(10000, 0.01);
    EXPECT_GT(p.m_bits, 0u);
    EXPECT_GE(p.k_hashes, 5u);
    EXPECT_LE(p.k_hashes, 10u);
}

TEST(LocalBloomTest, AddThenContainsAlwaysTrue) {
    auto p = BloomParams::ForCapacity(1024, 0.01);
    LocalBloom b(p);
    for (int i = 0; i < 100; ++i) {
        auto k = Key(i);
        b.Add({k.data(), k.size()});
    }
    for (int i = 0; i < 100; ++i) {
        auto k = Key(i);
        EXPECT_TRUE(b.MaybeContains({k.data(), k.size()}));
    }
    EXPECT_EQ(b.InsertCount(), 100u);
}

TEST(LocalBloomTest, FprApproximatelyMeetsTarget) {
    auto p = BloomParams::ForCapacity(2000, 0.01);
    LocalBloom b(p);
    for (int i = 0; i < 2000; ++i) {
        auto k = Key(i);
        b.Add({k.data(), k.size()});
    }
    int fp = 0;
    constexpr int kTrials = 5000;
    for (int i = 100000; i < 100000 + kTrials; ++i) {  // disjoint set
        auto k = Key(i);
        if (b.MaybeContains({k.data(), k.size()})) ++fp;
    }
    // Expect ≤ 5% even with placeholder hashing (true FPR with real BLAKE3 ~ 1%).
    EXPECT_LT(fp, kTrials * 5 / 100);
}

TEST(LocalBloomTest, ResetClears) {
    auto p = BloomParams::ForCapacity(1024, 0.01);
    LocalBloom b(p);
    auto k = Key(7);
    b.Add({k.data(), k.size()});
    EXPECT_TRUE(b.MaybeContains({k.data(), k.size()}));
    b.Reset();
    EXPECT_FALSE(b.MaybeContains({k.data(), k.size()}));
    EXPECT_EQ(b.InsertCount(), 0u);
}

TEST(AggregatedBloomTest, MirrorsLocalAfterSerializeDeserialize) {
    auto p = BloomParams::ForCapacity(1024, 0.01);
    LocalBloom local(p);
    for (int i = 0; i < 50; ++i) {
        auto k = Key(i);
        local.Add({k.data(), k.size()});
    }
    AggregatedBloom agg;
    agg.Set(p, local.Snapshot());
    for (int i = 0; i < 50; ++i) {
        auto k = Key(i);
        EXPECT_TRUE(agg.MaybeContains({k.data(), k.size()}));
    }
}

TEST(AggregatedBloomTest, EmptyReturnsFalse) {
    AggregatedBloom agg;
    auto k = Key(1);
    EXPECT_FALSE(agg.MaybeContains({k.data(), k.size()}));
}
