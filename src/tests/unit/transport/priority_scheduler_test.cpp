#include "transport/priority_scheduler.h"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

using kvcache::node::transport::Priority;
using kvcache::node::transport::PriorityScheduler;
using kvcache::node::transport::WorkItem;
using kvcache::node::transport::kSystemTenantHash;

TEST(PrioritySchedulerTest, PrioritiesAreServedInOrder) {
    PriorityScheduler::Options o;
    o.total_window_bytes = 1000;
    o.p0_pct = 20; o.p1_pct = 75; o.p2_pct = 5;
    PriorityScheduler s(o);
    s.Submit(Priority::P2, 1, nullptr);
    s.Submit(Priority::P1, 1, nullptr);
    s.Submit(Priority::P0, 1, nullptr);

    auto a = s.TryNext();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->priority, Priority::P0);
    auto b = s.TryNext();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->priority, Priority::P1);
    auto c = s.TryNext();
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->priority, Priority::P2);
    EXPECT_FALSE(s.TryNext().has_value());
}

TEST(PrioritySchedulerTest, ReservationIsRespected) {
    PriorityScheduler::Options o;
    o.total_window_bytes = 1000;
    o.p0_pct = 20; o.p1_pct = 75; o.p2_pct = 5;
    PriorityScheduler s(o);
    // Reservations: P0=200, P1=750, P2=50.
    // Two 100-byte P0 items fit exactly into the P0 reservation.
    s.Submit(Priority::P0, 100, nullptr);
    s.Submit(Priority::P0, 100, nullptr);
    s.Submit(Priority::P0, 100, nullptr);  // would push P0 over

    auto a = s.TryNext();
    auto b = s.TryNext();
    ASSERT_TRUE(a && b);
    // The third item exceeds P0's 200B reservation and there's no idle credit
    // to loan (no other classes have data). The loan window is "upward only",
    // so we expect the third item to be deferred.
    auto c = s.TryNext();
    EXPECT_FALSE(c.has_value()) << "P0 should not borrow from below";
}

TEST(PrioritySchedulerTest, IdleCreditLoansDownward) {
    PriorityScheduler::Options o;
    o.total_window_bytes = 1000;
    o.p0_pct = 20; o.p1_pct = 75; o.p2_pct = 5;
    PriorityScheduler s(o);

    // P2 reservation is 50B; the work item is 600B. P0 + P1 are idle, so the
    // 50B reservation + 950B loan should cover it.
    s.Submit(Priority::P2, 600, nullptr);
    auto w = s.TryNext();
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->priority, Priority::P2);
}

TEST(PrioritySchedulerTest, OnCompleteFreesCredit) {
    PriorityScheduler::Options o;
    o.total_window_bytes = 200;
    o.p0_pct = 50; o.p1_pct = 40; o.p2_pct = 10;
    PriorityScheduler s(o);
    // P0 reservation = 100 bytes. Three 40-byte items: first two fit
    // (40+40=80 ≤ 100), the third would overflow (120 > 100).
    auto id1 = s.Submit(Priority::P0, 40, nullptr);
    s.Submit(Priority::P0, 40, nullptr);
    s.Submit(Priority::P0, 40, nullptr);

    auto a = s.TryNext();
    auto b = s.TryNext();
    ASSERT_TRUE(a && b);
    // 180/100 — third blocked (no idle credit since only P0 has work).
    EXPECT_FALSE(s.TryNext().has_value());

    EXPECT_TRUE(s.OnComplete(id1));
    auto c = s.TryNext();
    ASSERT_TRUE(c.has_value());
}

TEST(PrioritySchedulerTest, StarvationOverrideKicksIn) {
    PriorityScheduler::Options o;
    o.total_window_bytes      = 200;
    o.p0_pct = 100; o.p1_pct = 0; o.p2_pct = 0;
    o.max_starvation_skips    = 3;
    PriorityScheduler s(o);
    // P2 has 0B reservation and there are no higher classes with idle work to
    // loan from … but P0 is empty, so loan is the entire 200B. Force the
    // opposite scenario: queue a P0 item that consumes all P0 budget, then a
    // P2 item that's too big for its 0B reservation AND the loan is zero
    // because P0 has data in flight.
    s.Submit(Priority::P0, 200, nullptr);
    auto a = s.TryNext();
    ASSERT_TRUE(a);
    s.Submit(Priority::P2, 50, nullptr);
    // First three TryNext: P2 has nothing admissible (P0's 200B reservation
    // is fully in_flight, so no loan). Each call increments skip counter.
    EXPECT_FALSE(s.TryNext().has_value());
    EXPECT_FALSE(s.TryNext().has_value());
    EXPECT_FALSE(s.TryNext().has_value());
    // On the fourth attempt the starvation watchdog forces admission.
    auto forced = s.TryNext();
    ASSERT_TRUE(forced.has_value());
    EXPECT_EQ(forced->priority, Priority::P2);
}

TEST(PrioritySchedulerTest, OnCompleteUnknownIdReturnsFalse) {
    PriorityScheduler s({});
    EXPECT_FALSE(s.OnComplete(99999));
}

// ---- Phase E-1: per-tenant fairness ---------------------------------------

TEST(PrioritySchedulerTest, TenantsRoundRobinWithinPriority) {
    PriorityScheduler::Options o;
    o.total_window_bytes = 100'000;
    o.p0_pct = 10; o.p1_pct = 80; o.p2_pct = 10;
    PriorityScheduler s(o);

    // Two tenants, three items each — all at P1, all small enough to admit.
    const uint64_t tA = 0xAAAA'AAAA'AAAA'AAAAull;
    const uint64_t tB = 0xBBBB'BBBB'BBBB'BBBBull;
    for (int i = 0; i < 3; ++i) {
        s.Submit(Priority::P1, tA, 10, reinterpret_cast<void*>(uintptr_t{1}));
        s.Submit(Priority::P1, tB, 10, reinterpret_cast<void*>(uintptr_t{2}));
    }
    EXPECT_EQ(s.TenantCount(Priority::P1), 2u);

    // Expected dispatch order: A,B,A,B,A,B (tA submitted first → first slot).
    std::vector<uintptr_t> got;
    for (int i = 0; i < 6; ++i) {
        auto w = s.TryNext();
        ASSERT_TRUE(w.has_value()) << "step " << i;
        got.push_back(reinterpret_cast<uintptr_t>(w->user));
    }
    EXPECT_EQ(got, (std::vector<uintptr_t>{1, 2, 1, 2, 1, 2}));
    EXPECT_FALSE(s.TryNext().has_value());
    EXPECT_EQ(s.TenantCount(Priority::P1), 0u);
}

TEST(PrioritySchedulerTest, BlockedTenantHeadDoesNotStarveSiblings) {
    // Tenant A's head item is too big to admit. Tenant B's head fits. The
    // scheduler should serve B first instead of stalling the whole class.
    PriorityScheduler::Options o;
    o.total_window_bytes = 1000;
    o.p0_pct = 100; o.p1_pct = 0; o.p2_pct = 0;
    PriorityScheduler s(o);
    // P0 reservation = 1000B. No higher class to lend.

    // Soak up the link with an unrelated P0 transfer.
    auto soaker = s.Submit(Priority::P0, kSystemTenantHash, 900, nullptr);
    ASSERT_TRUE(s.TryNext().has_value());  // takes the 900B soaker

    // Remaining P0 budget = 100B.
    const uint64_t tA = 0xA;
    const uint64_t tB = 0xB;
    s.Submit(Priority::P0, tA, 500, reinterpret_cast<void*>(uintptr_t{1}));
    s.Submit(Priority::P0, tB,  50, reinterpret_cast<void*>(uintptr_t{2}));

    auto w = s.TryNext();
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->tenant_hash, tB) << "B's small item should preempt A's blocked head";

    EXPECT_TRUE(s.OnComplete(soaker));
    // Now A's 500B item fits in the freed budget.
    auto w2 = s.TryNext();
    ASSERT_TRUE(w2.has_value());
    EXPECT_EQ(w2->tenant_hash, tA);
}

TEST(PrioritySchedulerTest, MetricsAccountNormalAndForcedAdmissions) {
    PriorityScheduler::Options o;
    o.total_window_bytes = 200;
    o.p0_pct = 100; o.p1_pct = 0; o.p2_pct = 0;
    o.max_starvation_skips = 3;
    PriorityScheduler s(o);

    // One normal P0 admission, then a P2 item that has to wait out the
    // starvation watchdog and get force-admitted.
    s.Submit(Priority::P0, 200, nullptr);
    ASSERT_TRUE(s.TryNext().has_value());
    EXPECT_EQ(s.NormalAdmissions(), 1u);
    EXPECT_EQ(s.ForcedAdmissions(), 0u);

    s.Submit(Priority::P2, 50, nullptr);
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(s.TryNext().has_value());
    auto w = s.TryNext();
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->priority, Priority::P2);
    EXPECT_EQ(s.ForcedAdmissions(), 1u);
    EXPECT_EQ(s.NormalAdmissions(), 1u);
}

TEST(PrioritySchedulerTest, OldTwoArgSubmitCompatibility) {
    // The pre-Phase-E-1 3-arg Submit still works and lands all traffic in
    // the system-tenant bucket — verified by single-tenant TenantCount.
    PriorityScheduler s({});
    s.Submit(Priority::P0, 1, nullptr);
    s.Submit(Priority::P0, 1, nullptr);
    EXPECT_EQ(s.TenantCount(Priority::P0), 1u);
    ASSERT_TRUE(s.TryNext().has_value());
    ASSERT_TRUE(s.TryNext().has_value());
    EXPECT_EQ(s.QueueDepth(Priority::P0), 0u);
}

TEST(PrioritySchedulerTest, ConcurrentProducersAndConsumer) {
    // Spawn N producer threads that each submit M items across all 3
    // priorities and 4 synthetic tenants. One consumer thread drives
    // TryNext + OnComplete. Verify: every submitted id is admitted exactly
    // once, OnComplete returns true for every id, and the scheduler ends
    // up with zero in-flight bytes / zero queue depth.
    constexpr int kProducers   = 4;
    constexpr int kPerProducer = 200;
    constexpr int kTotal       = kProducers * kPerProducer;

    PriorityScheduler::Options o;
    o.total_window_bytes = 1'000'000;
    PriorityScheduler s(o);

    std::atomic<int> next_id_check{0};
    std::atomic<int> admitted{0};

    auto producer = [&](int pid) {
        for (int i = 0; i < kPerProducer; ++i) {
            const auto p   = static_cast<Priority>(i % 3);
            const uint64_t t = static_cast<uint64_t>(pid + 1);
            s.Submit(p, t, 100, reinterpret_cast<void*>(uintptr_t{1}));
        }
    };

    std::vector<std::thread> ts;
    for (int i = 0; i < kProducers; ++i) ts.emplace_back(producer, i);

    // Consumer runs in the main thread, drains until kTotal items admitted.
    while (admitted.load(std::memory_order_relaxed) < kTotal) {
        auto w = s.TryNext();
        if (!w) {
            std::this_thread::yield();
            continue;
        }
        ++admitted;
        // Free the credit so subsequent items can admit.
        EXPECT_TRUE(s.OnComplete(w->id));
        ++next_id_check;
    }
    for (auto& t : ts) t.join();

    EXPECT_EQ(admitted.load(), kTotal);
    EXPECT_EQ(s.QueueDepth(Priority::P0), 0u);
    EXPECT_EQ(s.QueueDepth(Priority::P1), 0u);
    EXPECT_EQ(s.QueueDepth(Priority::P2), 0u);
    EXPECT_EQ(s.InFlightBytes(Priority::P0), 0u);
    EXPECT_EQ(s.InFlightBytes(Priority::P1), 0u);
    EXPECT_EQ(s.InFlightBytes(Priority::P2), 0u);
}
