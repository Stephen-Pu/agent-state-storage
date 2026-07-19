// SS-2 spine spike, Task 5 — hot-path seam test.
//
// Proves the store seam wired into HeadlessNode::SealCommit is
// behavior-preserving: a normal Reserve -> Publish -> Seal still stores the
// chunk (the zeroed CostModel{} the hot path supplies makes
// ValuePolicyKv::shouldStore return true, so the seam's gate never declines),
// and a subsequent Lookup still hits with a full token match.
//
// Strategy mirrors test_seal_by_chunk_path / test_replica_fetch: compile
// headless_node.cpp directly into this test binary (no kvcache dylib) so the
// non-ABI C++ symbols are reachable, and share one HeadlessNode singleton
// across tests in this binary using non-overlapping (tenant, model, token)
// triples to avoid ART path conflicts.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "ctx_options.h"
#include "headless_node.h"
#include "kvcache/kv_errors.h"

using kvcache::abi::HeadlessNode;

namespace {

// FNV-1a 64-bit — mirrors kvcache::Fnv1a64 in hashing.h.
uint64_t Fnv1a64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

// Build a kv_locator_t for (model, tokens).
kv_locator_t MakeLocator(const std::string& model,
                          const std::vector<uint32_t>& tokens) {
    kv_locator_t loc{};
    std::memset(&loc, 0, sizeof(loc));
    loc.model_id_hash     = Fnv1a64(model);
    loc.range.token_count = static_cast<uint32_t>(tokens.size());
    loc.version           = 1;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        loc.prefix_hash[(i * 7) % 16] ^= static_cast<uint8_t>(tokens[i] & 0xff);
        loc.prefix_hash[(i * 3) % 16] ^= static_cast<uint8_t>((tokens[i] >> 8) & 0xff);
    }
    return loc;
}

// Initialise the HeadlessNode singleton once for the whole test binary.
HeadlessNode* GetNode() {
    static HeadlessNode* node = [] {
        auto opts = kvcache::abi::BuildCtxOptions(nullptr);
        std::string err;
        auto* n = HeadlessNode::GetOrCreate(opts, &err);
        return n;
    }();
    return node;
}

}  // namespace

class HotPathSeamTest : public ::testing::Test {
   protected:
    HeadlessNode* node_ = nullptr;
    void SetUp() override {
        node_ = GetNode();
        ASSERT_NE(node_, nullptr) << "HeadlessNode::GetOrCreate failed";
    }
};

// ---------------------------------------------------------------------------
// Reserve -> Publish -> Seal still stores, and Lookup still hits with a full
// match, proving the ValuePolicy store/miss seams inserted into SealCommit /
// Lookup / FetchWithPriority did not change observable behavior.
// ---------------------------------------------------------------------------
TEST_F(HotPathSeamTest, SealStillStoresAndLookupHitsThroughPolicySeam) {
    constexpr std::size_t kNTokens       = 16;
    constexpr std::size_t kBytesPerToken = 64;
    constexpr std::size_t kPayloadBytes  = kNTokens * kBytesPerToken;

    // Unique token range so this test doesn't collide with other test
    // binaries' ART entries (each test binary gets its own process/singleton,
    // but keep the range distinctive for readability/debuggability anyway).
    std::vector<uint32_t> tokens(kNTokens);
    for (uint32_t i = 0; i < kNTokens; ++i) tokens[i] = 9000u + i;

    const std::string  model       = "hot-path-seam-model";
    const uint64_t     tenant_hash = 0;
    const uint64_t     model_hash  = Fnv1a64(model);
    const kv_locator_t loc         = MakeLocator(model, tokens);
    const std::vector<uint8_t> payload(kPayloadBytes, 0xEF);

    // Reserve.
    kv_handle_t      h    = 0;
    kv_buffer_desc_t slot{};
    ASSERT_EQ(node_->Reserve(&loc, payload.size(),
                              tenant_hash, model_hash,
                              &h, &slot),
              KV_OK);
    if (slot.addr) std::memcpy(slot.addr, payload.data(), payload.size());

    // Publish.
    kv_buffer_desc_t empty{};
    ASSERT_EQ(node_->Publish(h, empty, payload.size()), KV_OK);

    // Seal — routes through the store seam (StateIdentityFromLocator +
    // zeroed CostModel{} -> ValuePolicyKv::shouldStore == true -> unchanged
    // unconditional-store behavior).
    ASSERT_EQ(node_->Seal(h, tokens.data(), tokens.size()), KV_OK);

    // Lookup — routes through the miss seam only on a miss; here it must be
    // a full-match hit.
    kv_locator_t out_meta{};
    kv_handle_t  lh      = 0;
    uint32_t     matched = 0;
    ASSERT_EQ(node_->Lookup(
                  /*tenant_id=*/"",
                  tenant_hash,
                  model_hash,
                  tokens.data(), tokens.size(),
                  &out_meta, &lh, &matched),
              KV_OK);
    EXPECT_EQ(matched, static_cast<uint32_t>(kNTokens));

    node_->Release(lh);
}

// ---------------------------------------------------------------------------
// A genuine miss (tokens never sealed) still returns KV_E_NOT_FOUND — the
// miss seam's onMiss() call is consult-only and does not change the return
// code.
// ---------------------------------------------------------------------------
TEST_F(HotPathSeamTest, LookupMissStillReturnsNotFoundThroughPolicySeam) {
    std::vector<uint32_t> tokens(4);
    for (uint32_t i = 0; i < 4; ++i) tokens[i] = 9500u + i;

    const uint64_t tenant_hash = 0;
    const uint64_t model_hash  = Fnv1a64("hot-path-seam-miss-model");

    kv_locator_t out_meta{};
    kv_handle_t  lh      = 0;
    uint32_t     matched = 0;
    EXPECT_EQ(node_->Lookup(
                  /*tenant_id=*/"",
                  tenant_hash,
                  model_hash,
                  tokens.data(), tokens.size(),
                  &out_meta, &lh, &matched),
              KV_E_NOT_FOUND);
}
