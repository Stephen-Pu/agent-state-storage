// Task 3 — StatePut/StateGet refuse to operate without a durable WAL, and
// never create a DRAM-only B entry.
#include "headless_node.h"

#include <gtest/gtest.h>
#include <vector>

#include "kvcache/kv_errors.h"
#include "state_identity.h"

using kvcache::abi::HeadlessNode;
using kvcache::common::SK_MEMORY;
using kvcache::common::StateIdentity;

TEST(StateIngestNoWal, RefusesWithoutWalAndStagesNothing) {
    HeadlessNode::Options o{};
    o.tier.dram.capacity_bytes = 1u << 20;
    // state_wal_path deliberately left empty → B ingest disabled.
    std::string err;
    auto* node = HeadlessNode::GetOrCreate(o, &err);
    ASSERT_NE(node, nullptr) << err;

    StateIdentity id{};
    id.state_kind = SK_MEMORY;
    id.content_hash[0] = 7;
    std::vector<uint8_t> blob(64, 0xAB);

    EXPECT_EQ(node->StatePut(id, blob.data(), blob.size()), KV_E_TIER_DOWN);
    std::vector<uint8_t> out;
    EXPECT_EQ(node->StateGet(id, &out), KV_E_TIER_DOWN);  // nothing to read; no DRAM-only entry
}
