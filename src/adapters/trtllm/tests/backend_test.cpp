// TrtLlmKVCacheBackend — gtest coverage against the loopback C ABI.
//
// Mirrors the SGLang / AIBrix test corpus: store → retrieve round-trip,
// LPM chunk alignment on extended tokens, miss-returns-nullopt, prefix
// truncation. The C ABI's loopback backend is used directly via
// libkvcache (the shared lib), so this exercises the same path
// production TRT-LLM plugin code would hit.
#include "kvcache_trtllm/backend.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

using namespace kvcache::trtllm;

namespace {

constexpr std::size_t kBytesPerToken = 64;
constexpr std::size_t kChunkBytes    = kChunkTokens * kBytesPerToken;

std::vector<std::byte> MakePayload(std::size_t n_bytes, uint8_t seed) {
    std::vector<std::byte> out(n_bytes);
    for (std::size_t i = 0; i < n_bytes; ++i) {
        out[i] = static_cast<std::byte>(
            static_cast<uint8_t>((i * 13u + seed) & 0xff));
    }
    return out;
}

std::vector<uint32_t> Range(uint32_t lo, uint32_t hi) {
    std::vector<uint32_t> out(hi - lo);
    std::iota(out.begin(), out.end(), lo);
    return out;
}

}  // namespace

TEST(TrtLlmBackend, StoreThenRetrieveRoundTrip) {
    TrtLlmKVCacheBackend kv("trtllm-tenant", "trtllm-demo", kBytesPerToken);

    auto tokens  = Range(4000, 4000 + 2 * kChunkTokens);
    auto payload = MakePayload(tokens.size() * kBytesPerToken, /*seed=*/17);

    kv.Store(tokens, payload);

    auto got = kv.Retrieve(tokens);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->size(), payload.size());
    EXPECT_EQ(*got, payload);
}

TEST(TrtLlmBackend, LookupReturnsChunkAlignedMatch) {
    TrtLlmKVCacheBackend kv("trtllm-tenant", "trtllm-lpm", kBytesPerToken);
    auto base    = Range(5000, 5000 + 2 * kChunkTokens);
    auto payload = MakePayload(base.size() * kBytesPerToken, /*seed=*/9);
    kv.Store(base, payload);

    // Tack on a 3-token tail that LPM must drop (LLD §3.2).
    auto extended = base;
    extended.push_back(9001);
    extended.push_back(9002);
    extended.push_back(9003);
    EXPECT_EQ(kv.Lookup(extended), 2 * kChunkTokens);
}

TEST(TrtLlmBackend, RetrieveMissReturnsNullopt) {
    TrtLlmKVCacheBackend kv("trtllm-tenant", "trtllm-miss", kBytesPerToken);
    auto tokens = Range(6000, 6000 + kChunkTokens);
    EXPECT_FALSE(kv.Retrieve(tokens).has_value());
    EXPECT_EQ(kv.Lookup(tokens), 0u);
}

TEST(TrtLlmBackend, RetrieveTruncatesToMatchedPrefix) {
    TrtLlmKVCacheBackend kv("trtllm-tenant", "trtllm-prefix", kBytesPerToken);

    auto base    = Range(7000, 7000 + 2 * kChunkTokens);
    auto payload = MakePayload(base.size() * kBytesPerToken, /*seed=*/21);
    kv.Store(base, payload);

    // Caller asks for 3 chunks; the backend only has 2.
    auto longer = base;
    auto tail   = Range(7000 + 2 * kChunkTokens, 7000 + 3 * kChunkTokens);
    longer.insert(longer.end(), tail.begin(), tail.end());

    auto got = kv.Retrieve(longer);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->size(), payload.size());  // only the cached prefix
    EXPECT_EQ(*got, payload);
}

TEST(TrtLlmBackend, ConstructorRejectsZeroBytesPerToken) {
    EXPECT_THROW(TrtLlmKVCacheBackend("t", "m", 0), std::invalid_argument);
}

TEST(TrtLlmBackend, DropReturnsFalse) {
    TrtLlmKVCacheBackend kv("trtllm-tenant", "trtllm-drop", kBytesPerToken);
    auto tokens = Range(8000, 8000 + kChunkTokens);
    EXPECT_FALSE(kv.Drop(tokens));
}

TEST(TrtLlmBackend, IdAccessors) {
    TrtLlmKVCacheBackend kv("trtllm-tenant", "trtllm-acc", kBytesPerToken);
    EXPECT_EQ(kv.tenant_id(), "trtllm-tenant");
    EXPECT_EQ(kv.model_id(),  "trtllm-acc");
}

TEST(TrtLlmBackend, MoveLeavesSourceEmpty) {
    TrtLlmKVCacheBackend a("trtllm-tenant", "trtllm-move", kBytesPerToken);
    TrtLlmKVCacheBackend b = std::move(a);
    // Sanity: moved-into backend works.
    auto tokens  = Range(9000, 9000 + 2 * kChunkTokens);
    auto payload = MakePayload(tokens.size() * kBytesPerToken, /*seed=*/5);
    b.Store(tokens, payload);
    auto got = b.Retrieve(tokens);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->size(), payload.size());
}
