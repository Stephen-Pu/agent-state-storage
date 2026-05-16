#include "prefix/lpm.h"

#include <gtest/gtest.h>
#include <vector>

using kvcache::node::prefix::ArtIndex;
using kvcache::node::prefix::Chunkify;
using kvcache::node::prefix::ChunkHash;
using kvcache::node::prefix::kChunkTokens;
using kvcache::node::prefix::LeafData;
using kvcache::node::prefix::LongestPrefixMatch;

TEST(LpmTest, ChunkifyDiscardsTail) {
    std::vector<uint32_t> tokens(40, 1);  // 2 full chunks + 8 token tail
    auto chunks = Chunkify({tokens.data(), tokens.size()});
    EXPECT_EQ(chunks.size(), 2u);
}

TEST(LpmTest, ChunkifyEmpty) {
    std::vector<uint32_t> empty;
    auto chunks = Chunkify({empty.data(), empty.size()});
    EXPECT_TRUE(chunks.empty());
}

TEST(LpmTest, ChunkifyDeterministic) {
    std::vector<uint32_t> tokens(kChunkTokens, 7);
    auto a = Chunkify({tokens.data(), tokens.size()});
    auto b = Chunkify({tokens.data(), tokens.size()});
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a, b);
}

TEST(LpmTest, EndToEndLookup) {
    // Insert a 2-chunk prefix; query with 4-chunk extension; expect 2 chunks
    // (32 tokens) matched.
    std::vector<uint32_t> sys_prompt(kChunkTokens * 2, 0xC0DE);
    auto sys_chunks = Chunkify({sys_prompt.data(), sys_prompt.size()});
    ASSERT_EQ(sys_chunks.size(), 2u);

    ArtIndex art;
    auto leaf = std::make_unique<LeafData>();
    leaf->bytes_total = 1024;
    art.Insert({sys_chunks.data(), sys_chunks.size()}, std::move(leaf));

    std::vector<uint32_t> full = sys_prompt;
    full.insert(full.end(), kChunkTokens * 2, 0xBEEF);  // 2 extra chunks

    auto g  = art.EnterRead();
    auto r  = LongestPrefixMatch(art, {full.data(), full.size()}, g);
    EXPECT_EQ(r.matched_chunks, 2u);
    EXPECT_EQ(r.matched_tokens, 32u);
    ASSERT_NE(r.leaf, nullptr);
    EXPECT_EQ(r.leaf->bytes_total, 1024u);
}
