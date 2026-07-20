// Task 3 — B-plane ingest end-to-end through HeadlessNode: StatePut/StateGet,
// WAL-first durability, DRAM demotion + replay.
#include "headless_node.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <vector>

#include "kvcache/kv_errors.h"
#include "state_identity.h"

using kvcache::abi::HeadlessNode;
using kvcache::common::SK_MEMORY;
using kvcache::common::StateIdentity;

namespace {
StateIdentity BId(uint8_t seed) {
    StateIdentity id{};
    id.version    = 2;
    id.state_kind = SK_MEMORY;
    for (int i = 0; i < 16; ++i) id.content_hash[i] = static_cast<uint8_t>(seed + i);
    return id;
}

// Build an isolated node (its own singleton reset is not available; each test
// process gets one). Small DRAM capacity so we can force demotion.
HeadlessNode::Options MakeOpts(const std::string& wal_path, uint64_t dram_cap) {
    HeadlessNode::Options o{};
    o.tier.dram.capacity_bytes = dram_cap;
    o.state_wal_path           = wal_path;
    return o;
}
}  // namespace

TEST(StateIngest, PutThenGetServesFromDram) {
    const std::string wal = "state_ingest_put.wal";
    std::remove(wal.c_str());
    std::string err;
    auto* node = HeadlessNode::GetOrCreate(MakeOpts(wal, 1u << 20), &err);
    ASSERT_NE(node, nullptr) << err;

    std::vector<uint8_t> blob(128, 0xB1);
    ASSERT_EQ(node->StatePut(BId(1), blob.data(), blob.size()), KV_OK);

    std::vector<uint8_t> out;
    ASSERT_EQ(node->StateGet(BId(1), &out), KV_OK);
    EXPECT_EQ(out, blob);
    EXPECT_EQ(node->StateGet(BId(99), &out), KV_E_NOT_FOUND);
    std::remove(wal.c_str());
}

TEST(StateIngest, EvictedBEntryReplaysFromWal) {
    const std::string wal = "state_ingest_demote.wal";
    std::remove(wal.c_str());
    std::string err;
    // Reuse the process singleton; use a fresh key set. Force demotion by
    // putting many B entries so DRAM capacity (set by whichever test ran
    // first) is exceeded and the LRU tail (our first key) is evicted.
    auto* node = HeadlessNode::GetOrCreate(MakeOpts(wal, 1u << 20), &err);
    ASSERT_NE(node, nullptr) << err;

    std::vector<uint8_t> first(4096, 0xEE);
    ASSERT_EQ(node->StatePut(BId(200), first.data(), first.size()), KV_OK);

    // Flood DRAM to force the first entry out (values are large; count is
    // high enough to exceed any reasonable test DRAM capacity). Filler keys
    // use distinct content_hash prefixes (content_hash[0] = 0xF0, index in
    // content_hash[1..4]) so none of them ever collides with BId(200) — a
    // collision would just refresh the target entry instead of evicting it.
    std::vector<uint8_t> filler(4096, 0x11);
    for (int i = 0; i < 4096; ++i) {
        StateIdentity fid{};
        fid.version    = 2;
        fid.state_kind = SK_MEMORY;
        fid.content_hash[0] = 0xF0;
        fid.content_hash[1] = static_cast<uint8_t>(i);
        fid.content_hash[2] = static_cast<uint8_t>(i >> 8);
        fid.content_hash[3] = static_cast<uint8_t>(i >> 16);
        fid.content_hash[4] = static_cast<uint8_t>(i >> 24);
        ASSERT_EQ(node->StatePut(fid, filler.data(), filler.size()), KV_OK) << i;
    }

    // First entry is (almost certainly) no longer in DRAM, but StateGet must
    // still return it byte-identical by replaying from the WAL.
    std::vector<uint8_t> out;
    ASSERT_EQ(node->StateGet(BId(200), &out), KV_OK);
    EXPECT_EQ(out, first);
    std::remove(wal.c_str());
}
