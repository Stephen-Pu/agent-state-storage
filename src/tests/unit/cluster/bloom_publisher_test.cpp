// Phase K-5 — BloomPublisher + NodeDirectory sketch fan-out tests.
//
// Two distinct nodes share an in-memory etcd. Node A publishes a
// sketch containing three chunk-keys; node B's NodeDirectory watches
// the sketch prefix and learns of A's sketch. We then assert:
//
//   * NodeDirectory.PeerMaybeHas("node-a", k) returns true for each
//     of the three keys A inserted.
//   * NodeDirectory.PeerMaybeHas("node-a", random_key) is false for a
//     handful of unrelated keys (within the Bloom false-positive
//     budget).
//   * Deleting the sketch entry drops it from the directory.
#include "cluster/bloom_publisher.h"
#include "cluster/etcd_client.h"
#include "cluster/node_directory.h"
#include "routing/hrw.h"

#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace kvcache::node::cluster;
using kvcache::node::routing::BloomParams;
using kvcache::node::routing::HrwRing;

namespace {

bool WaitFor(std::function<bool()> pred,
             std::chrono::milliseconds budget = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

std::vector<uint8_t> Key(uint8_t seed) {
    // 40-byte chunk identity shape — same width as a real
    // tenant|model|prefix triple.
    std::vector<uint8_t> k(40);
    for (std::size_t i = 0; i < k.size(); ++i) {
        k[i] = static_cast<uint8_t>(seed ^ (i * 17 + 3));
    }
    return k;
}

}  // namespace

TEST(BloomSnapshotCodec, RoundTrip) {
    InMemoryEtcdClient etcd;
    BloomPublisher::Options o{};
    o.node_id        = "node-a";
    o.expected_chunks = 1024;
    o.target_fpr      = 0.01;
    BloomPublisher pub(&etcd, o);
    const auto k1 = Key(1);
    pub.Add(k1);

    const auto encoded = pub.EncodeSnapshot();
    BloomParams         decoded{0, 0};
    std::vector<uint8_t> raw;
    ASSERT_TRUE(DecodeBloomSnapshot(
        std::string(encoded.begin(), encoded.end()), &decoded, &raw));
    EXPECT_GT(decoded.m_bits, 0u);
    EXPECT_GT(decoded.k_hashes, 0u);
    EXPECT_EQ(raw.size(), (decoded.m_bits + 7) / 8);
}

TEST(BloomSnapshotCodec, RejectsMalformed) {
    BloomParams          p{0, 0};
    std::vector<uint8_t> r;
    EXPECT_FALSE(DecodeBloomSnapshot("", &p, &r));
    EXPECT_FALSE(DecodeBloomSnapshot("short", &p, &r));
    // 8 bytes of zero header (m=0, k=0) — invalid params.
    std::string zeros(8, '\0');
    EXPECT_FALSE(DecodeBloomSnapshot(zeros, &p, &r));
}

TEST(BloomPublisherTest, StartPublishesSnapshot) {
    InMemoryEtcdClient etcd;
    BloomPublisher::Options o{};
    o.node_id        = "node-a";
    o.expected_chunks = 1024;
    o.publish_period = std::chrono::milliseconds(50);
    BloomPublisher pub(&etcd, o);

    pub.Add(Key(1));
    pub.Add(Key(2));
    pub.Add(Key(3));

    std::string err;
    ASSERT_TRUE(pub.Start(&err)) << err;
    EXPECT_GE(pub.PublishCount(), 1u);

    auto kv = etcd.Get(pub.Key(), &err);
    ASSERT_TRUE(kv.has_value()) << err;
    EXPECT_GE(kv->value.size(), 8u);

    pub.Stop();
}

TEST(NodeDirectorySketchTest, AdoptsPeerSketchAndAnswersMaybeHas) {
    InMemoryEtcdClient etcd;

    // Publisher for node-a.
    BloomPublisher::Options pub_opts{};
    pub_opts.node_id        = "node-a";
    pub_opts.expected_chunks = 4096;
    pub_opts.target_fpr      = 0.005;
    pub_opts.publish_period = std::chrono::milliseconds(50);
    BloomPublisher pub(&etcd, pub_opts);
    pub.Add(Key(11));
    pub.Add(Key(22));
    pub.Add(Key(33));
    std::string err;
    ASSERT_TRUE(pub.Start(&err)) << err;

    // Directory on node-b (etcd is shared).
    HrwRing ring;
    NodeDirectory dir(&etcd, &ring);
    ASSERT_TRUE(dir.Start(&err)) << err;

    ASSERT_TRUE(WaitFor([&] { return dir.SketchCount() == 1; }));

    EXPECT_TRUE(dir.PeerMaybeHas("node-a", Key(11)));
    EXPECT_TRUE(dir.PeerMaybeHas("node-a", Key(22)));
    EXPECT_TRUE(dir.PeerMaybeHas("node-a", Key(33)));

    // Five "unrelated" keys — at 0.5% FPR collisions should be rare.
    // We don't require zero (bloom is probabilistic) — just that
    // most are negative, which proves the sketch is actually being
    // consulted and not returning a blanket true.
    int positives = 0;
    for (uint8_t seed = 100; seed < 105; ++seed) {
        if (dir.PeerMaybeHas("node-a", Key(seed))) positives++;
    }
    EXPECT_LE(positives, 1)
        << "too many false positives on unrelated keys (got "
        << positives << "/5) — sketch likely not being honoured";

    // Unknown peer always returns false.
    EXPECT_FALSE(dir.PeerMaybeHas("node-c", Key(11)));

    // Delete the sketch key in etcd — directory should drop it.
    ASSERT_TRUE(etcd.Delete(pub.Key(), &err));
    ASSERT_TRUE(WaitFor([&] { return dir.SketchCount() == 0; }));
    EXPECT_FALSE(dir.PeerMaybeHas("node-a", Key(11)));

    pub.Stop();
    dir.Stop();
}

// Directory started AFTER a sketch is already present must seed it
// off the initial GetPrefix — not wait for a future Watch event.
TEST(NodeDirectorySketchTest, SeedsExistingSketchOnStart) {
    InMemoryEtcdClient etcd;

    BloomPublisher::Options pub_opts{};
    pub_opts.node_id         = "early-node";
    pub_opts.expected_chunks = 1024;
    pub_opts.publish_period  = std::chrono::seconds(60);  // no-op
    BloomPublisher pub(&etcd, pub_opts);
    pub.Add(Key(42));
    std::string err;
    ASSERT_TRUE(pub.Start(&err)) << err;

    // Directory comes up LATE.
    HrwRing ring;
    NodeDirectory dir(&etcd, &ring);
    ASSERT_TRUE(dir.Start(&err)) << err;
    EXPECT_EQ(dir.SketchCount(), 1u)
        << "sketch already in etcd should be seeded before Start returns";
    EXPECT_TRUE(dir.PeerMaybeHas("early-node", Key(42)));

    pub.Stop();
    dir.Stop();
}
