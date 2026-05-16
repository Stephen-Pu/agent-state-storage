#include "qos/quota.h"

#include <gtest/gtest.h>
#include <thread>

using namespace kvcache::node::qos;

namespace {
constexpr uint64_t kT = 0x1234'5678'9abc'def0ULL;
}

TEST(QuotaTest, UnknownTenantRejected) {
    QuotaManager q;
    ReserveRequest r; r.bytes = 1; r.requests = 1;
    EXPECT_EQ(q.Reserve(kT, r), QuotaResult::kUnknownTenant);
}

TEST(QuotaTest, CapacityEnforced) {
    QuotaManager q;
    q.SetLimits(kT, {100, 1000, 1ull << 30, false});
    EXPECT_EQ(q.Reserve(kT, {60, 1, 0}), QuotaResult::kOk);
    EXPECT_EQ(q.Reserve(kT, {40, 1, 0}), QuotaResult::kOk);
    EXPECT_EQ(q.Reserve(kT, {1,  1, 0}), QuotaResult::kCapacityExceeded);
    EXPECT_EQ(q.CapacityUsed(kT), 100u);
}

TEST(QuotaTest, ReleaseFreesCapacity) {
    QuotaManager q;
    q.SetLimits(kT, {100, 1000, 1ull << 30, false});
    q.Reserve(kT, {80, 1, 0});
    q.Release(kT, 80);
    EXPECT_EQ(q.CapacityUsed(kT), 0u);
    EXPECT_EQ(q.Reserve(kT, {100, 1, 0}), QuotaResult::kOk);
}

TEST(QuotaTest, QpsEnforcedWithinWindow) {
    QuotaManager q;
    q.SetLimits(kT, {1000, 3, 1ull << 30, false});  // 3 QPS
    EXPECT_EQ(q.Reserve(kT, {1, 1, 0}), QuotaResult::kOk);
    EXPECT_EQ(q.Reserve(kT, {1, 1, 0}), QuotaResult::kOk);
    EXPECT_EQ(q.Reserve(kT, {1, 1, 0}), QuotaResult::kOk);
    EXPECT_EQ(q.Reserve(kT, {1, 1, 0}), QuotaResult::kQpsExceeded);
}

TEST(QuotaTest, BandwidthEnforcedWithinWindow) {
    QuotaManager q;
    q.SetLimits(kT, {1ull << 30, 1000, 100, false});  // 100 B/s
    EXPECT_EQ(q.Reserve(kT, {1, 1, 80}), QuotaResult::kOk);
    EXPECT_EQ(q.Reserve(kT, {1, 1, 25}), QuotaResult::kBandwidthExceeded);
}

TEST(QuotaTest, QpsWindowResetsAfterOneSecond) {
    QuotaManager::Options o; o.window = std::chrono::milliseconds(50);
    QuotaManager q(o);
    q.SetLimits(kT, {1ull << 30, 2, 1ull << 30, false});
    EXPECT_EQ(q.Reserve(kT, {0, 1, 0}), QuotaResult::kOk);
    EXPECT_EQ(q.Reserve(kT, {0, 1, 0}), QuotaResult::kOk);
    EXPECT_EQ(q.Reserve(kT, {0, 1, 0}), QuotaResult::kQpsExceeded);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(q.Reserve(kT, {0, 1, 0}), QuotaResult::kOk);
}

TEST(QuotaTest, InflationGives1_5x) {
    QuotaManager q;
    q.SetLimits(kT, {100, 1000, 1ull << 30, /*inflated=*/true});
    EXPECT_EQ(q.Reserve(kT, {150, 1, 0}), QuotaResult::kOk);
    EXPECT_EQ(q.Reserve(kT, {1,   1, 0}), QuotaResult::kCapacityExceeded);
}
