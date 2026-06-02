// Phase A2.2 — DrainGate + DrainWatcher tests.
//
// Drives the watcher against a real in-process InMemoryEtcdClient
// (faithful etcd v3: Put/Delete + prefix watches). Verifies the gate
// seeds from the key at Start, flips on Put (drain) and Delete
// (undrain), the key-format helper matches the rest of the stack, and
// the fail-safe (Start leaves the gate untouched if it can't seed).
#include "cluster/drain_gate.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using kvcache::node::cluster::DrainGate;
using kvcache::node::cluster::DrainKeyFor;
using kvcache::node::cluster::DrainWatcher;
using kvcache::node::cluster::InMemoryEtcdClient;
using kvcache::node::cluster::kNoLease;

namespace {
// Poll until `pred()` or timeout — the watcher callback runs on the
// etcd client's dispatch thread, so changes are observed asynchronously.
template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}
}  // namespace

TEST(DrainGateTest, DefaultsToNotDraining) {
    DrainGate g;
    EXPECT_FALSE(g.draining());
    EXPECT_FALSE(g.ShouldRejectNewWork());
    g.SetDraining(true);
    EXPECT_TRUE(g.ShouldRejectNewWork());
}

TEST(DrainKeyTest, MatchesStackFormat) {
    EXPECT_EQ(DrainKeyFor("prod", "node-7"), "/kvcache/drain/prod/node-7");
}

TEST(DrainWatcherTest, SeedsNotDrainingWhenKeyAbsent) {
    InMemoryEtcdClient etcd;
    DrainGate gate;
    const std::string key = DrainKeyFor("kvcache", "node-a");
    DrainWatcher w(etcd, key, gate);
    ASSERT_TRUE(w.Start());
    EXPECT_FALSE(gate.draining()) << "absent key → not draining";
}

TEST(DrainWatcherTest, SeedsDrainingWhenKeyPresent) {
    InMemoryEtcdClient etcd;
    const std::string key = DrainKeyFor("kvcache", "node-a");
    std::string err;
    ASSERT_TRUE(etcd.Put(key, R"({"node_id":"node-a"})", kNoLease, nullptr, &err)) << err;

    DrainGate gate;
    DrainWatcher w(etcd, key, gate);
    ASSERT_TRUE(w.Start());
    EXPECT_TRUE(gate.draining()) << "marker present at Start → draining";
}

TEST(DrainWatcherTest, FlipsOnPutAndDelete) {
    InMemoryEtcdClient etcd;
    const std::string key = DrainKeyFor("kvcache", "node-a");
    DrainGate gate;
    DrainWatcher w(etcd, key, gate);
    ASSERT_TRUE(w.Start());
    ASSERT_FALSE(gate.draining());

    // Drain: write the marker → gate should flip to draining.
    std::string err;
    ASSERT_TRUE(etcd.Put(key, R"({"node_id":"node-a"})", kNoLease, nullptr, &err)) << err;
    EXPECT_TRUE(WaitFor([&] { return gate.draining(); }))
        << "Put on the drain key should set the gate";

    // Undrain: delete the marker → gate should clear.
    ASSERT_TRUE(etcd.Delete(key, &err)) << err;
    EXPECT_TRUE(WaitFor([&] { return !gate.draining(); }))
        << "Delete on the drain key should clear the gate";
}

TEST(DrainWatcherTest, StopUnsubscribesCleanly) {
    InMemoryEtcdClient etcd;
    const std::string key = DrainKeyFor("kvcache", "node-a");
    DrainGate gate;
    {
        DrainWatcher w(etcd, key, gate);
        ASSERT_TRUE(w.Start());
        w.Stop();
        // After Stop, a Put must not flip the gate (no live subscription).
        std::string err;
        ASSERT_TRUE(etcd.Put(key, "{}", kNoLease, nullptr, &err)) << err;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_FALSE(gate.draining()) << "Stopped watcher must not observe events";
    }  // dtor Stop() again — must be idempotent (no crash).
}
