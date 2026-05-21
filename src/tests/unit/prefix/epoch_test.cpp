// Unit tests for the EpochManager primitive.
#include "prefix/epoch.h"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

using kvcache::node::prefix::EpochGuard;
using kvcache::node::prefix::EpochManager;

TEST(EpochManagerTest, GlobalEpochAdvancesOnRetire) {
    EpochManager mgr;
    const auto e0 = mgr.GlobalEpoch();
    mgr.Retire([] {});
    const auto e1 = mgr.GlobalEpoch();
    EXPECT_GT(e1, e0);
}

TEST(EpochManagerTest, ReclaimRunsDeleterWhenNoActiveReaders) {
    EpochManager mgr;
    bool deleted = false;
    mgr.Retire([&deleted] { deleted = true; });
    EXPECT_EQ(mgr.PendingRetires(), 1u);

    const auto freed = mgr.Reclaim();
    EXPECT_EQ(freed, 1u);
    EXPECT_TRUE(deleted);
    EXPECT_EQ(mgr.PendingRetires(), 0u);
}

TEST(EpochManagerTest, ActiveReaderBlocksReclamation) {
    EpochManager mgr;
    bool deleted = false;

    // Reader enters BEFORE retire — witnesses an older epoch than the
    // retire's tag, so reclamation must NOT free this item until the
    // reader exits.
    EpochGuard guard(mgr);

    mgr.Retire([&deleted] { deleted = true; });
    EXPECT_EQ(mgr.Reclaim(), 0u);
    EXPECT_FALSE(deleted);
    EXPECT_EQ(mgr.PendingRetires(), 1u);

    // Need to end the EpochGuard's lifetime explicitly so the destructor
    // runs *before* the assertions below. Use an inner scope.
    {
        EpochGuard moved = std::move(guard);  // moves ownership
        // Original guard is now inert; moved still active.
        EXPECT_EQ(mgr.Reclaim(), 0u);
        EXPECT_FALSE(deleted);
        // moved goes out of scope here, ExitRead fires.
    }

    EXPECT_EQ(mgr.Reclaim(), 1u);
    EXPECT_TRUE(deleted);
}

TEST(EpochManagerTest, ReaderEnteringAfterRetireDoesNotBlockReclamation) {
    EpochManager mgr;
    bool deleted = false;
    mgr.Retire([&deleted] { deleted = true; });

    // Reader enters AFTER retire — its witness is at or above the retire
    // epoch, so reclamation can proceed.
    EpochGuard guard(mgr);
    EXPECT_EQ(mgr.Reclaim(), 1u);
    EXPECT_TRUE(deleted);
}

TEST(EpochManagerTest, ForceReclaimAllAtShutdown) {
    EpochManager mgr;
    int run_count = 0;

    // Enter epoch FIRST (witness = pre-retire epoch). All subsequent retires
    // tag epochs strictly greater than this witness, so the active reader
    // blocks them from regular Reclaim().
    auto guard = std::make_unique<EpochGuard>(mgr);

    for (int i = 0; i < 5; ++i) {
        mgr.Retire([&run_count] { run_count++; });
    }
    EXPECT_EQ(mgr.Reclaim(), 0u);

    // ForceReclaimAll bypasses the epoch check — the right tool to use at
    // structure-destruction time when we know readers have wound down.
    EXPECT_EQ(mgr.ForceReclaimAll(), 5u);
    EXPECT_EQ(run_count, 5);

    guard.reset();
}

TEST(EpochManagerTest, ActiveReadersCount) {
    EpochManager mgr;
    EXPECT_EQ(mgr.ActiveReaders(), 0u);
    {
        EpochGuard g1(mgr);
        EXPECT_EQ(mgr.ActiveReaders(), 1u);
        // Spawning a second thread to register a second reader slot.
        std::thread t([&mgr] {
            EpochGuard g2(mgr);
            // Spin briefly so the main thread can observe both readers.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_EQ(mgr.ActiveReaders(), 2u);
        t.join();
        EXPECT_EQ(mgr.ActiveReaders(), 1u);
    }
    EXPECT_EQ(mgr.ActiveReaders(), 0u);
}
