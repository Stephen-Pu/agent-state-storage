#include "prefix/art_index.h"

#include <gtest/gtest.h>
#include <memory>

using kvcache::node::prefix::ArtIndex;
using kvcache::node::prefix::ChunkHash;
using kvcache::node::prefix::LeafData;

namespace {

ChunkHash H(uint8_t a, uint8_t b = 0) {
    ChunkHash h{};
    h[0] = a;
    h[1] = b;
    return h;
}

std::unique_ptr<LeafData> MakeLeaf(uint64_t bytes) {
    auto l = std::make_unique<LeafData>();
    l->bytes_total = bytes;
    return l;
}

}  // namespace

TEST(ArtIndexTest, EmptyLookupReturnsZero) {
    ArtIndex art;
    auto g = art.EnterRead();
    std::vector<ChunkHash> path{H(1), H(2)};
    auto r = art.Lookup({path.data(), path.size()}, g);
    EXPECT_EQ(r.matched_chunks, 0u);
    EXPECT_EQ(r.leaf, nullptr);
}

TEST(ArtIndexTest, InsertThenExactLookup) {
    ArtIndex art;
    std::vector<ChunkHash> path{H(1), H(2), H(3)};
    auto res = art.Insert({path.data(), path.size()}, MakeLeaf(42));
    EXPECT_EQ(res, ArtIndex::InsertResult::kInserted);
    EXPECT_EQ(art.LeafCount(), 1u);

    auto g = art.EnterRead();
    auto r = art.Lookup({path.data(), path.size()}, g);
    EXPECT_EQ(r.matched_chunks, 3u);
    ASSERT_NE(r.leaf, nullptr);
    EXPECT_EQ(r.leaf->bytes_total, 42u);
}

TEST(ArtIndexTest, LongestPrefixMatch) {
    ArtIndex art;
    std::vector<ChunkHash> short_p{H(1), H(2)};
    art.Insert({short_p.data(), short_p.size()}, MakeLeaf(100));

    std::vector<ChunkHash> query{H(1), H(2), H(3), H(4)};
    auto g = art.EnterRead();
    auto r = art.Lookup({query.data(), query.size()}, g);
    EXPECT_EQ(r.matched_chunks, 2u);
    ASSERT_NE(r.leaf, nullptr);
    EXPECT_EQ(r.leaf->bytes_total, 100u);
}

TEST(ArtIndexTest, NoMatchOnDivergentFirstChunk) {
    ArtIndex art;
    std::vector<ChunkHash> stored{H(1), H(2)};
    art.Insert({stored.data(), stored.size()}, MakeLeaf(10));

    std::vector<ChunkHash> query{H(9), H(2)};
    auto g = art.EnterRead();
    auto r = art.Lookup({query.data(), query.size()}, g);
    EXPECT_EQ(r.matched_chunks, 0u);
}

TEST(ArtIndexTest, ReplaceUpdatesLeaf) {
    ArtIndex art;
    std::vector<ChunkHash> p{H(1)};
    art.Insert({p.data(), p.size()}, MakeLeaf(10));
    auto res = art.Insert({p.data(), p.size()}, MakeLeaf(20));
    EXPECT_EQ(res, ArtIndex::InsertResult::kReplaced);
    EXPECT_EQ(art.LeafCount(), 1u);

    auto g = art.EnterRead();
    auto r = art.Lookup({p.data(), p.size()}, g);
    ASSERT_NE(r.leaf, nullptr);
    EXPECT_EQ(r.leaf->bytes_total, 20u);
}

TEST(ArtIndexTest, RemoveLeaf) {
    ArtIndex art;
    std::vector<ChunkHash> p{H(1), H(2)};
    art.Insert({p.data(), p.size()}, MakeLeaf(10));
    EXPECT_TRUE(art.Remove({p.data(), p.size()}));
    EXPECT_EQ(art.LeafCount(), 0u);

    auto g = art.EnterRead();
    auto r = art.Lookup({p.data(), p.size()}, g);
    EXPECT_EQ(r.matched_chunks, 0u);
}

TEST(ArtIndexTest, RejectInsertOnTopOfDeeperPath) {
    // MVP limitation: leaves are terminal. Inserting "1,2" then "1" must fail
    // because that would require splitting an inner node back into a leaf.
    ArtIndex art;
    std::vector<ChunkHash> deep{H(1), H(2)};
    art.Insert({deep.data(), deep.size()}, MakeLeaf(10));
    std::vector<ChunkHash> shallow{H(1)};
    auto res = art.Insert({shallow.data(), shallow.size()}, MakeLeaf(20));
    EXPECT_EQ(res, ArtIndex::InsertResult::kPathConflict);
}
