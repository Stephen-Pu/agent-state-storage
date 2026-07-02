// Task 4 — A9 DR warm-standby: ReplicationCursor unit test.
//
// ReplicationCursor tracks the last-applied epoch and decides whether a new
// event should be applied or skipped (dedup on reconnect). Monotonic advance:
// Advance() always moves forward (max(last, epoch)), never backwards.
#include "replication/replication_cursor.h"

#include <gtest/gtest.h>

using kvcache::node::replication::ReplicationCursor;

TEST(ReplicationCursor, AdvancesMonotonicallyAndDedups) {
    ReplicationCursor c;
    EXPECT_TRUE(c.ShouldApply(5));  c.Advance(5);
    EXPECT_FALSE(c.ShouldApply(5)) << "already applied";
    EXPECT_FALSE(c.ShouldApply(3)) << "older event on reconnect";
    EXPECT_TRUE(c.ShouldApply(6));  c.Advance(6);
    EXPECT_EQ(c.Last(), 6u);
}
