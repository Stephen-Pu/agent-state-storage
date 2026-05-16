#include "security/audit.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace kvcache::node::security;

TEST(AuditLogTest, AppendThenDrain) {
    std::atomic<int> count{0};
    AuditLog::Options o;
    o.ring_capacity = 32;
    o.drain_interval = std::chrono::milliseconds(20);
    AuditLog log(o, [&](const AuditRecord&) { count.fetch_add(1); });

    for (int i = 0; i < 16; ++i) {
        AuditRecord r{};
        r.at = std::chrono::system_clock::now();
        r.kind = IdentityKind::kTenant;
        r.cn = "tenant-" + std::to_string(i);
        r.action = Action::kLookup;
        r.decision = Decision::kAllow;
        r.tenant_hash = i;
        EXPECT_TRUE(log.Append(r));
    }
    // Wait for drain.
    for (int i = 0; i < 50 && count.load() < 16; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    log.Stop();
    EXPECT_EQ(count.load(), 16);
    EXPECT_EQ(log.DroppedCount(), 0u);
    EXPECT_EQ(log.DeliveredCount(), 16u);
}

TEST(AuditLogTest, OverflowIncrementsDropped) {
    AuditLog::Options o;
    o.ring_capacity = 4;
    o.drain_interval = std::chrono::milliseconds(500);  // delay drain
    AuditLog log(o, [&](const AuditRecord&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
    int ok = 0, dropped = 0;
    for (int i = 0; i < 20; ++i) {
        AuditRecord r{};
        if (log.Append(r)) ++ok;
        else                ++dropped;
    }
    EXPECT_GE(dropped, 1);
    EXPECT_LE(ok, 4 + 1);
    log.Stop();
}

TEST(SerializeAuditTest, ProducesValidJson) {
    AuditRecord r{};
    r.at = std::chrono::system_clock::time_point{};
    r.kind = IdentityKind::kTenant;
    r.cn = "tenant-A";
    r.action = Action::kFetch;
    r.decision = Decision::kAllow;
    r.tenant_hash = 42;
    r.message = "ok";
    auto s = SerializeAudit(r);
    EXPECT_NE(s.find("\"action\":\"fetch\""), std::string::npos);
    EXPECT_NE(s.find("\"decision\":\"allow\""), std::string::npos);
    EXPECT_NE(s.find("\"tenant\":42"), std::string::npos);
    EXPECT_NE(s.find("\"msg\":\"ok\""), std::string::npos);
}
