#include "prefix/refcount.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using kvcache::node::prefix::Refcount;

TEST(RefcountTest, StartsAtZero) {
    Refcount rc;
    EXPECT_TRUE(rc.IsZero());
    EXPECT_EQ(rc.Load(), 0u);
}

TEST(RefcountTest, AcquireRelease) {
    Refcount rc;
    EXPECT_EQ(rc.Acquire(), 1u);
    EXPECT_EQ(rc.Acquire(), 2u);
    EXPECT_EQ(rc.Release(), 1u);
    EXPECT_EQ(rc.Release(), 0u);
    EXPECT_TRUE(rc.IsZero());
}

TEST(RefcountTest, TryAcquireIfNonZero_FailsAtZero) {
    Refcount rc;
    EXPECT_FALSE(rc.TryAcquireIfNonZero());
    EXPECT_TRUE(rc.IsZero());
}

TEST(RefcountTest, TryAcquireIfNonZero_SucceedsWhenHeld) {
    Refcount rc(1);
    EXPECT_TRUE(rc.TryAcquireIfNonZero());
    EXPECT_EQ(rc.Load(), 2u);
}

TEST(RefcountTest, ConcurrentAcquireRelease) {
    Refcount rc;
    constexpr int kThreads = 8;
    constexpr int kIters   = 10000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&]{
            for (int i = 0; i < kIters; ++i) {
                rc.Acquire();
                rc.Release();
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_TRUE(rc.IsZero());
}
