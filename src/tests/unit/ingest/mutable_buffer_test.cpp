#include "ingest/mutable_buffer.h"

#include <gtest/gtest.h>
#include <thread>

#include "tier/tier_manager.h"

using namespace kvcache::node;

namespace {

std::unique_ptr<tier::TierManager> MakeTm(uint32_t slots = 4) {
    tier::TierManager::Options o;
    o.pinned.pool_bytes = 4096ull * slots;
    o.pinned.slot_bytes = 4096;
    o.pinned.use_mlock  = false;
    o.dram.capacity_bytes    = 1 << 20;
    o.dram.a1out_max_entries = 8;
    std::string err;
    auto t = tier::TierManager::Create(o, &err);
    if (!t) ADD_FAILURE() << err;
    return t;
}

}  // namespace

TEST(MutableBufferPoolTest, ReserveReleaseRoundTrip) {
    auto tm = MakeTm(4);
    ingest::MutableBufferPool::Options o;
    o.start_sweeper = false;
    ingest::MutableBufferPool pool(tm.get(), o);

    auto h1 = pool.Reserve();
    auto h2 = pool.Reserve();
    ASSERT_NE(h1, ingest::kInvalidIngestHandle);
    ASSERT_NE(h2, ingest::kInvalidIngestHandle);
    EXPECT_EQ(pool.ActiveCount(), 2u);
    EXPECT_TRUE(pool.GetSlot(h1).has_value());

    EXPECT_TRUE(pool.Release(h1));
    EXPECT_FALSE(pool.GetSlot(h1).has_value());
    EXPECT_TRUE(pool.Release(h2));
    EXPECT_EQ(pool.ActiveCount(), 0u);
}

TEST(MutableBufferPoolTest, ExhaustionReturnsInvalid) {
    auto tm = MakeTm(2);
    ingest::MutableBufferPool::Options o;
    o.start_sweeper = false;
    ingest::MutableBufferPool pool(tm.get(), o);
    auto h1 = pool.Reserve();
    auto h2 = pool.Reserve();
    auto h3 = pool.Reserve();
    EXPECT_NE(h1, ingest::kInvalidIngestHandle);
    EXPECT_NE(h2, ingest::kInvalidIngestHandle);
    EXPECT_EQ(h3, ingest::kInvalidIngestHandle);
    pool.Release(h1);
    pool.Release(h2);
}

TEST(MutableBufferPoolTest, TtlSweeperReclaimsExpired) {
    auto tm = MakeTm(2);
    ingest::MutableBufferPool::Options o;
    o.start_sweeper = false;
    o.default_ttl   = std::chrono::seconds(0);  // immediate expiry
    ingest::MutableBufferPool pool(tm.get(), o);
    auto h = pool.Reserve();
    ASSERT_NE(h, ingest::kInvalidIngestHandle);
    EXPECT_EQ(pool.ActiveCount(), 1u);
    pool.SweepOnce();
    EXPECT_EQ(pool.ActiveCount(), 0u);
}
