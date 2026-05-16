#include "routing/hrw.h"

#include <gtest/gtest.h>
#include <set>
#include <vector>

using namespace kvcache::node::routing;

namespace {
std::vector<uint8_t> Key(const std::string& s) {
    return {s.begin(), s.end()};
}
}  // namespace

TEST(HrwTest, EmptyRingReturnsEmpty) {
    HrwRing r;
    auto k = Key("anything");
    EXPECT_TRUE(r.TopK({k.data(), k.size()}, 3).empty());
}

TEST(HrwTest, DeterministicForSameInput) {
    HrwRing r;
    r.SetNodes({{"n1", 1}, {"n2", 1}, {"n3", 1}, {"n4", 1}});
    auto k = Key("/prefix/abc");
    auto a = r.TopK({k.data(), k.size()}, 4);
    auto b = r.TopK({k.data(), k.size()}, 4);
    EXPECT_EQ(a, b);
}

TEST(HrwTest, PrimaryAndTopKConsistent) {
    HrwRing r;
    r.SetNodes({{"n1", 1}, {"n2", 1}, {"n3", 1}});
    auto k = Key("/prefix/xyz");
    auto top = r.TopK({k.data(), k.size()}, 3);
    ASSERT_EQ(top.size(), 3u);
    EXPECT_EQ(r.Primary({k.data(), k.size()}), top.front());
}

TEST(HrwTest, RoughlyBalancedAcrossManyKeys) {
    HrwRing r;
    r.SetNodes({{"n1", 1}, {"n2", 1}, {"n3", 1}, {"n4", 1}});
    std::map<NodeId, int> counts;
    for (int i = 0; i < 4000; ++i) {
        auto k = Key("k-" + std::to_string(i));
        counts[r.Primary({k.data(), k.size()})]++;
    }
    // Each node should be primary roughly 25% of the time; allow ±5%.
    for (const auto& [_, c] : counts) {
        EXPECT_GT(c, 700);
        EXPECT_LT(c, 1300);
    }
}

TEST(HrwTest, TrafficWeightZeroExcludes) {
    HrwRing r;
    r.SetNodes({{"n1", 1.0}, {"n2", 0.0}, {"n3", 1.0}});
    int picks_n2 = 0;
    for (int i = 0; i < 200; ++i) {
        auto k = Key("k-" + std::to_string(i));
        if (r.Primary({k.data(), k.size()}) == "n2") ++picks_n2;
    }
    EXPECT_EQ(picks_n2, 0);
}

TEST(HrwTest, OverlapBiasShiftsRouting) {
    HrwRing::Options o; o.overlap_alpha = 10.0;  // very strong bias
    HrwRing r(o);
    r.SetNodes({{"n1", 1.0}, {"n2", 1.0}, {"n3", 1.0}});

    // Find a key for which n2 is NOT the primary under raw HRW; then apply an
    // overlap function that gives n2 a 100% overlap and verify it wins.
    auto k = Key("overlap-test-key");
    NodeId raw_primary = r.Primary({k.data(), k.size()});
    NodeId other       = raw_primary == "n2" ? "n1" : "n2";
    OverlapScoreFn fn = [&other](const NodeId& id, std::span<const uint8_t>) {
        return id == other ? 1.0 : 0.0;
    };
    EXPECT_EQ(r.Primary({k.data(), k.size()}, fn), other);
}
