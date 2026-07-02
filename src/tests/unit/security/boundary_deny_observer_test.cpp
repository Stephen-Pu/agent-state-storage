// src/tests/unit/security/boundary_deny_observer_test.cpp
//
// A10 Regulated Mode — BoundaryDenyObserver unit tests.
//
// Tests:
//   1. IncrementsMetricAndToleratesNullAudit — the metric lambda fires once per
//      deny call; a null audit pointer does not crash.
//   2. AppendsAuditRecordWithDenyDecision — when an AuditLog is supplied the
//      observer appends one record with Decision::kDeny and the reason in
//      message.

#include "security/boundary_deny_observer.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "security/audit.h"
#include "security/boundary_guard.h"

using namespace kvcache::node::security;

// -------------------------------------------------------------------
// Test 1: metric fires; null AuditLog is safe.
// -------------------------------------------------------------------
TEST(BoundaryDenyObserver, IncrementsMetricAndToleratesNullAudit) {
    int metric = 0;
    auto obs = MakeBoundaryDenyObserver([&] { ++metric; }, /*audit=*/nullptr);
    obs(Endpoint{.host = "evil.com", .port = 443, .purpose = Purpose::kColdTier},
        "out of boundary");
    EXPECT_EQ(metric, 1) << "deny must bump the counter";
    // null audit must not crash — if we get here, we're good.
}

// -------------------------------------------------------------------
// Test 2: AuditLog receives a record with kDeny + the reason message.
// -------------------------------------------------------------------
TEST(BoundaryDenyObserver, AppendsAuditRecordWithDenyDecision) {
    // Capture records via the consumer callback.
    std::vector<AuditRecord> captured;
    AuditLog::Options opts;
    opts.ring_capacity  = 32;
    opts.drain_interval = std::chrono::milliseconds(10);
    AuditLog log(opts, [&](const AuditRecord& r) { captured.push_back(r); });

    int metric = 0;
    auto obs = MakeBoundaryDenyObserver([&] { ++metric; }, &log);

    const std::string reason = "host not in allowlist";
    obs(Endpoint{.host = "rogue.example.com", .port = 80, .purpose = Purpose::kTelemetry},
        reason);

    // Drain the ring (AuditLog drains asynchronously).
    for (int i = 0; i < 100 && captured.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    log.Stop();

    EXPECT_EQ(metric, 1);
    ASSERT_EQ(captured.size(), 1u) << "exactly one AuditRecord expected";

    const AuditRecord& rec = captured.front();
    EXPECT_EQ(rec.decision, Decision::kDeny);
    EXPECT_NE(rec.message.find(reason), std::string::npos)
        << "reason must appear in message; got: " << rec.message;
    EXPECT_EQ(rec.cn, "boundary-guard");
}
