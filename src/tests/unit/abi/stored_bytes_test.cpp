// Phase KVZ-3 — kv_lookup_stored_bytes C ABI test.
//
// Reserve → write → publish → seal a chunk of a known byte size, then look it
// up and assert kv_lookup_stored_bytes reports exactly that size (so a caller
// can size a fetch buffer for a variable-size / compressed payload). Plus
// argument + handle guards.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "kvcache/kv_abi.h"
#include "kvcache/kv_errors.h"

namespace {
kv_locator_t TinyLocator() {
    kv_locator_t loc{};
    std::memset(&loc, 0, sizeof(loc));
    loc.range.token_count = 16;
    loc.version = 1;
    return loc;
}
}  // namespace

TEST(KvStoredBytes, ReportsSealedChunkSize) {
    kv_ctx_config_t cfg{};
    cfg.abi_version = KVCACHE_ABI_VERSION;
    cfg.tenant_id = "kvz3-tenant";
    cfg.model_id = "kvz3-demo";
    kv_ctx_t* ctx = nullptr;
    ASSERT_EQ(kv_ctx_open(&cfg, &ctx), KV_OK);

    const size_t N = 7777;  // deliberately not a round number
    kv_locator_t loc = TinyLocator();
    kv_handle_t wh = 0;
    kv_buffer_desc_t slot{};
    ASSERT_EQ(kv_reserve(ctx, &loc, N, &wh, &slot), KV_OK);
    ASSERT_GE(slot.len, N);
    std::memset(slot.addr, 0xAB, N);
    ASSERT_EQ(kv_publish(ctx, wh, slot, N), KV_OK);

    std::vector<uint32_t> tokens(16);
    for (uint32_t i = 0; i < 16; ++i) tokens[i] = 1000 + i;
    ASSERT_EQ(kv_seal(ctx, wh, tokens.data(), tokens.size()), KV_OK);

    kv_locator_t meta{};
    kv_handle_t rh = 0;
    uint32_t matched = 0;
    ASSERT_EQ(kv_lookup(ctx, tokens.data(), tokens.size(), &meta, &rh, &matched), KV_OK);
    EXPECT_EQ(matched, 16u);

    size_t stored = 0;
    ASSERT_EQ(kv_lookup_stored_bytes(ctx, rh, &stored), KV_OK);
    EXPECT_EQ(stored, N) << "stored bytes must equal the sealed content size";

    kv_release(ctx, rh);
    kv_ctx_close(ctx);
}

TEST(KvStoredBytes, Guards) {
    kv_ctx_config_t cfg{};
    cfg.abi_version = KVCACHE_ABI_VERSION;
    cfg.tenant_id = "kvz3-tenant";
    cfg.model_id = "kvz3-guard";
    kv_ctx_t* ctx = nullptr;
    ASSERT_EQ(kv_ctx_open(&cfg, &ctx), KV_OK);

    size_t out = 0;
    EXPECT_EQ(kv_lookup_stored_bytes(nullptr, 1, &out), KV_E_INVAL);
    EXPECT_EQ(kv_lookup_stored_bytes(ctx, 1, nullptr), KV_E_INVAL);
    EXPECT_EQ(kv_lookup_stored_bytes(ctx, 999999, &out), KV_E_NOT_FOUND);
    kv_ctx_close(ctx);
}
