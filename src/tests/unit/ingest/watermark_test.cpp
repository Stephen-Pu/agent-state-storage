#include "ingest/watermark.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using kvcache::node::ingest::WatermarkTracker;

TEST(WatermarkTest, UntrackedReadsZero) {
    WatermarkTracker w;
    EXPECT_EQ(w.Read(42), 0u);
    EXPECT_EQ(w.Publish(42, 100), 0u);
}

TEST(WatermarkTest, MonotonicProgression) {
    WatermarkTracker w;
    w.Track(1);
    EXPECT_EQ(w.Publish(1, 100), 100u);
    EXPECT_EQ(w.Publish(1, 200), 200u);
    // Regress attempts are refused; current value is returned.
    EXPECT_EQ(w.Publish(1, 150), 200u);
    EXPECT_EQ(w.Read(1), 200u);
}

TEST(WatermarkTest, DropReturnsFinal) {
    WatermarkTracker w;
    w.Track(7);
    w.Publish(7, 50);
    EXPECT_EQ(w.Drop(7), 50u);
    EXPECT_EQ(w.Read(7), 0u);  // no longer tracked
}

TEST(WatermarkTest, ConcurrentPublishOnlyAdvances) {
    WatermarkTracker w;
    w.Track(1);
    constexpr int kThreads = 4;
    constexpr int kSteps   = 10000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 1; i <= kSteps; ++i) {
                w.Publish(1, t * kSteps + i);
            }
        });
    }
    for (auto& th : ts) th.join();
    // Final must be >= the global max set by any thread.
    EXPECT_GE(w.Read(1), static_cast<uint64_t>((kThreads - 1) * kSteps + kSteps));
}
