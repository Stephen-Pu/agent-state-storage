// Phase Q-1 — NodeRegistrar + NodeDirectory tests against the
// in-memory etcd implementation.
//
// These tests verify the cluster-membership wiring end-to-end without
// touching the network: a registrar PUTs a leased key, a directory
// Watching the same prefix observes the upsert and refreshes the
// HrwRing; lease revoke / Stop() removes the entry; HRW Primary
// converges on the surviving node.
#include "cluster/etcd_client.h"
#include "cluster/node_directory.h"
#include "cluster/node_registrar.h"
#include "routing/hrw.h"

#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <thread>

using namespace kvcache::node::cluster;
using kvcache::node::routing::HrwRing;

namespace {

bool WaitFor(std::function<bool()> pred,
             std::chrono::milliseconds budget = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

}  // namespace

// ---------------------------------------------------------------------------
// Value codec
// ---------------------------------------------------------------------------

TEST(NodeRegistrarCodec, RoundTrip) {
    const std::string encoded =
        EncodeNodeValue("node-a", "10.0.0.7", 7777);
    std::string host;
    uint16_t    port = 0;
    ASSERT_TRUE(DecodeNodeValue(encoded, &host, &port));
    EXPECT_EQ(host, "10.0.0.7");
    EXPECT_EQ(port, 7777u);
}

TEST(NodeRegistrarCodec, RejectsMalformed) {
    std::string host;
    uint16_t    port = 0;
    EXPECT_FALSE(DecodeNodeValue("", &host, &port));
    EXPECT_FALSE(DecodeNodeValue(R"({"node_id":"x"})", &host, &port));
    EXPECT_FALSE(DecodeNodeValue(R"({"host":"h","grpc_port":99999})",
                                  &host, &port));
}

// ---------------------------------------------------------------------------
// Registrar
// ---------------------------------------------------------------------------

TEST(NodeRegistrarTest, StartPutsLeasedKey) {
    InMemoryEtcdClient etcd;

    NodeRegistrar::Options o{};
    o.node_id        = "node-a";
    o.advertise_host = "127.0.0.1";
    o.grpc_port      = 7100;
    o.lease_ttl_seconds = 5;
    NodeRegistrar r(&etcd, o);

    std::string err;
    ASSERT_TRUE(r.Start(&err)) << err;
    EXPECT_TRUE(r.Running());
    EXPECT_NE(r.Lease(), kNoLease);

    auto kv = etcd.Get(r.Key(), &err);
    ASSERT_TRUE(kv.has_value()) << err;
    EXPECT_EQ(kv->lease, r.Lease());

    std::string host;
    uint16_t    port = 0;
    ASSERT_TRUE(DecodeNodeValue(kv->value, &host, &port));
    EXPECT_EQ(host, "127.0.0.1");
    EXPECT_EQ(port, 7100u);

    r.Stop();
    EXPECT_FALSE(r.Running());
    // After revoke the key is gone.
    auto after = etcd.Get(r.Key(), &err);
    EXPECT_FALSE(after.has_value());
}

TEST(NodeRegistrarTest, StartFailsOnIncompleteIdentity) {
    InMemoryEtcdClient etcd;
    NodeRegistrar::Options o{};  // node_id / host / port all default
    NodeRegistrar r(&etcd, o);
    std::string err;
    EXPECT_FALSE(r.Start(&err));
    EXPECT_NE(err.find("identity"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Directory
// ---------------------------------------------------------------------------

TEST(NodeDirectoryTest, SeedsThenObservesPuts) {
    InMemoryEtcdClient etcd;
    HrwRing ring;
    NodeDirectory dir(&etcd, &ring);
    std::string err;
    ASSERT_TRUE(dir.Start(&err)) << err;
    EXPECT_EQ(dir.NodeCount(), 0u);

    // Add node-a via a registrar — directory should observe via Watch.
    NodeRegistrar::Options oa{};
    oa.node_id = "node-a"; oa.advertise_host = "10.0.0.1"; oa.grpc_port = 7000;
    NodeRegistrar ra(&etcd, oa);
    ASSERT_TRUE(ra.Start(&err)) << err;

    ASSERT_TRUE(WaitFor([&] { return dir.NodeCount() == 1; }));
    auto ep = dir.Resolve("node-a");
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(ep->host, "10.0.0.1");
    EXPECT_EQ(ep->grpc_port, 7000u);
    EXPECT_EQ(ep->dial_target, "10.0.0.1:7000");
    EXPECT_EQ(ring.NodeCount(), 1u);

    // Add a second node, watch the ring expand.
    NodeRegistrar::Options ob{};
    ob.node_id = "node-b"; ob.advertise_host = "10.0.0.2"; ob.grpc_port = 7000;
    NodeRegistrar rb(&etcd, ob);
    ASSERT_TRUE(rb.Start(&err)) << err;
    ASSERT_TRUE(WaitFor([&] { return dir.NodeCount() == 2; }));
    EXPECT_EQ(ring.NodeCount(), 2u);

    // Removing one drops it from the ring.
    rb.Stop();
    ASSERT_TRUE(WaitFor([&] { return dir.NodeCount() == 1; }));
    EXPECT_EQ(ring.NodeCount(), 1u);
    EXPECT_FALSE(dir.Resolve("node-b").has_value());

    ra.Stop();
    dir.Stop();
}

// Phase K-3 — directory adopts the CP-published ClusterView snapshot
// in one go. PUTting a view with two nodes makes the table reflect
// those nodes BEFORE either registrar-published key exists; later
// putting a smaller view replaces (not merges) the table. A stale-
// epoch view from the same leader is dropped.
TEST(NodeDirectoryTest, AdoptsClusterViewSnapshot) {
    InMemoryEtcdClient etcd;
    HrwRing ring;
    NodeDirectory dir(&etcd, &ring);
    std::string err;
    ASSERT_TRUE(dir.Start(&err)) << err;
    EXPECT_EQ(dir.NodeCount(), 0u);

    // PUT a ClusterView with 2 nodes — the directory should adopt
    // it even though /kvcache/nodes/ has no registrar entries.
    const std::string v1 = R"({
        "epoch": 1,
        "leader_id": "cp-test",
        "published_at_ns": 0,
        "nodes": [
          {"node_id":"alpha","host":"10.0.0.1","grpc_port":7000},
          {"node_id":"beta", "host":"10.0.0.2","grpc_port":7001}
        ]
    })";
    ASSERT_TRUE(etcd.Put("/kvcache/cluster/view", v1, kNoLease,
                           nullptr, &err)) << err;
    ASSERT_TRUE(WaitFor([&] { return dir.NodeCount() == 2; }));
    {
        auto a = dir.Resolve("alpha");
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->dial_target, "10.0.0.1:7000");
    }

    // Stale-epoch view from the same leader: must be ignored.
    const std::string v_stale = R"({
        "epoch": 1, "leader_id": "cp-test",
        "nodes": [{"node_id":"ghost","host":"x","grpc_port":1}]
    })";
    ASSERT_TRUE(etcd.Put("/kvcache/cluster/view", v_stale, kNoLease,
                           nullptr, &err));
    // Negative check: NodeCount should NOT briefly flip to 1.
    // Sleep a beat so the watcher dispatched (in-memory etcd is
    // synchronous to PUT but the callback runs on a dispatch
    // thread).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(dir.NodeCount(), 2u)
        << "stale-epoch view from same leader must be ignored";
    EXPECT_FALSE(dir.Resolve("ghost").has_value());

    // Fresh epoch with one node: table replaces wholesale.
    const std::string v2 = R"({
        "epoch": 7, "leader_id": "cp-test",
        "nodes": [{"node_id":"gamma","host":"10.0.0.3","grpc_port":7002}]
    })";
    ASSERT_TRUE(etcd.Put("/kvcache/cluster/view", v2, kNoLease,
                           nullptr, &err));
    ASSERT_TRUE(WaitFor([&] {
        auto ids = dir.NodeIds();
        return ids.size() == 1 && ids[0] == "gamma";
    }));
    EXPECT_FALSE(dir.Resolve("alpha").has_value());

    // New leader's epoch=1 must be accepted (epoch threshold resets
    // when leader_id changes).
    const std::string v_new_leader = R"({
        "epoch": 1, "leader_id": "cp-2",
        "nodes": [{"node_id":"delta","host":"10.0.0.4","grpc_port":7003}]
    })";
    ASSERT_TRUE(etcd.Put("/kvcache/cluster/view", v_new_leader, kNoLease,
                           nullptr, &err));
    ASSERT_TRUE(WaitFor([&] {
        auto ids = dir.NodeIds();
        return ids.size() == 1 && ids[0] == "delta";
    }));
    dir.Stop();
}

TEST(NodeDirectoryTest, PrimaryConvergesOnSurvivingNode) {
    InMemoryEtcdClient etcd;
    HrwRing ring;
    NodeDirectory dir(&etcd, &ring);
    std::string err;
    ASSERT_TRUE(dir.Start(&err)) << err;

    NodeRegistrar::Options oa{};
    oa.node_id = "node-a"; oa.advertise_host = "10.0.0.1"; oa.grpc_port = 7000;
    NodeRegistrar ra(&etcd, oa);
    NodeRegistrar::Options ob{};
    ob.node_id = "node-b"; ob.advertise_host = "10.0.0.2"; ob.grpc_port = 7000;
    NodeRegistrar rb(&etcd, ob);
    ASSERT_TRUE(ra.Start(&err)) << err;
    ASSERT_TRUE(rb.Start(&err)) << err;
    ASSERT_TRUE(WaitFor([&] { return dir.NodeCount() == 2; }));

    const std::vector<uint8_t> key{1, 2, 3, 4, 5, 6, 7, 8};
    const std::string primary_two_nodes = ring.Primary(key);
    EXPECT_TRUE(primary_two_nodes == "node-a" || primary_two_nodes == "node-b");

    // Drop whichever one HRW picked; the survivor MUST become primary.
    if (primary_two_nodes == "node-a") {
        ra.Stop();
    } else {
        rb.Stop();
    }
    ASSERT_TRUE(WaitFor([&] { return dir.NodeCount() == 1; }));
    const std::string primary_one_node = ring.Primary(key);
    EXPECT_NE(primary_one_node, primary_two_nodes);
    EXPECT_FALSE(primary_one_node.empty());
}
