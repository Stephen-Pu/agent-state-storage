#include "cluster/etcd_client.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace kvcache::node::cluster;

TEST(InMemoryEtcdTest, PutGetDeleteRoundTrip) {
    InMemoryEtcdClient c;
    std::string err;
    Revision rev = 0;
    EXPECT_TRUE(c.Put("k", "v", kNoLease, &rev, &err)) << err;
    EXPECT_GT(rev, 0u);

    auto got = c.Get("k", &err);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->value, "v");
    EXPECT_EQ(got->mod_revision, rev);

    EXPECT_TRUE(c.Delete("k", &err));
    EXPECT_FALSE(c.Get("k", &err).has_value());
}

TEST(InMemoryEtcdTest, PrefixListSortedAndScoped) {
    InMemoryEtcdClient c;
    std::string err;
    c.Put("/a/1", "x", kNoLease, nullptr, &err);
    c.Put("/a/3", "z", kNoLease, nullptr, &err);
    c.Put("/a/2", "y", kNoLease, nullptr, &err);
    c.Put("/b/9", "q", kNoLease, nullptr, &err);
    auto vs = c.GetPrefix("/a/", &err);
    ASSERT_EQ(vs.size(), 3u);
    EXPECT_EQ(vs[0].key, "/a/1");
    EXPECT_EQ(vs[1].key, "/a/2");
    EXPECT_EQ(vs[2].key, "/a/3");
}

TEST(InMemoryEtcdTest, PutIfRevisionRefusesOnMismatch) {
    InMemoryEtcdClient c;
    std::string err;
    Revision rev = 0;
    ASSERT_TRUE(c.PutIfRevision("k", "v0", 0, kNoLease, &rev, &err));
    // Wrong expected_rev → reject.
    EXPECT_FALSE(c.PutIfRevision("k", "v1", 0, kNoLease, nullptr, &err));
    // Correct expected_rev → accept.
    EXPECT_TRUE(c.PutIfRevision("k", "v1", rev, kNoLease, nullptr, &err));
    auto got = c.Get("k", &err);
    EXPECT_EQ(got->value, "v1");
}

TEST(InMemoryEtcdTest, LeaseExpiryDropsKey) {
    InMemoryEtcdClient::Options o;
    o.lease_sweep_interval = std::chrono::milliseconds(20);
    InMemoryEtcdClient c(o);
    std::string err;
    auto lease = c.LeaseGrant(1, &err);
    ASSERT_NE(lease, kNoLease);
    ASSERT_TRUE(c.Put("/n/1", "alive", lease, nullptr, &err));
    EXPECT_TRUE(c.Get("/n/1", &err).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    EXPECT_FALSE(c.Get("/n/1", &err).has_value());
    EXPECT_EQ(c.LeaseTTLRemaining(lease), 0u);
}

TEST(InMemoryEtcdTest, LeaseKeepAliveRefreshes) {
    InMemoryEtcdClient::Options o;
    o.lease_sweep_interval = std::chrono::milliseconds(20);
    InMemoryEtcdClient c(o);
    std::string err;
    auto lease = c.LeaseGrant(1, &err);
    c.Put("/n/1", "alive", lease, nullptr, &err);
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT_TRUE(c.LeaseKeepAlive(lease, &err));
    }
    EXPECT_TRUE(c.Get("/n/1", &err).has_value());
}

namespace {
template <typename Pred>
bool WaitFor(Pred p, std::chrono::milliseconds t = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + t;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return p();
}
}  // namespace

TEST(InMemoryEtcdTest, WatchPrefixDeliversEvents) {
    InMemoryEtcdClient c;
    std::atomic<int> puts{0}, deletes{0};
    auto h = c.WatchPrefix("/n/", [&](const WatchEvent& e) {
        if (e.type == WatchEventType::kPut)    puts++;
        if (e.type == WatchEventType::kDelete) deletes++;
    });
    std::string err;
    c.Put("/n/1", "v", kNoLease, nullptr, &err);
    c.Put("/x/1", "v", kNoLease, nullptr, &err);  // outside prefix
    c.Delete("/n/1", &err);
    // Delivery is asynchronous (dispatcher thread), so poll for convergence
    // BEFORE Unwatch — otherwise the cancel could race ahead of delivery.
    EXPECT_TRUE(WaitFor([&] { return puts.load() == 1 && deletes.load() == 1; }));
    c.Unwatch(h);
    EXPECT_EQ(puts.load(), 1);
    EXPECT_EQ(deletes.load(), 1);
}

// Phase C2 — the headline guarantee: a watch callback may re-enter the client
// (Get / Put / Unwatch) without deadlocking, because callbacks run on the
// dispatcher thread with the client's mutex released. Before C2 this
// synchronous-under-lock dispatch was the A1.11 / K-4 deadlock footgun.
TEST(InMemoryEtcdTest, CallbackMayReenterClientWithoutDeadlock) {
    InMemoryEtcdClient c;
    std::string err;
    std::atomic<bool> reentered_ok{false};
    std::atomic<int>  fired{0};

    WatchHandle h = 0;
    h = c.WatchPrefix("/n/", [&](const WatchEvent& e) {
        if (++fired != 1) return;  // act only on the first event
        // Re-enter the client from inside the callback — each of these would
        // have self-deadlocked under the old synchronous-under-lock dispatch.
        std::string e2;
        auto got = c.Get(e.kv.key, &e2);                 // re-entrant read
        c.Put("/n/echo", "x", kNoLease, nullptr, &e2);   // re-entrant write
        c.Unwatch(h);                                    // cancel self
        reentered_ok = got.has_value() && got->value == "v";
    });

    ASSERT_TRUE(c.Put("/n/1", "v", kNoLease, nullptr, &err)) << err;
    EXPECT_TRUE(WaitFor([&] { return reentered_ok.load(); }))
        << "callback never completed — re-entrant call deadlocked";
    // The re-entrant Put landed, and self-Unwatch stopped further delivery.
    EXPECT_TRUE(WaitFor([&] { return c.Get("/n/echo", &err).has_value(); }));
}

TEST(GrpcEtcdClientTest, ReportsBuildState) {
    std::string err;
    GrpcEtcdClient::Options o; o.endpoints = {"127.0.0.1:2379"};
    auto c = GrpcEtcdClient::Create(o, &err);
    // Without KVCACHE_ENABLE_ETCD, Create returns nullptr with a clear error.
    EXPECT_EQ(c, nullptr);
    EXPECT_FALSE(err.empty());
}
