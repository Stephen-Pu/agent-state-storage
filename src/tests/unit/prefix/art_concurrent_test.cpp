// Concurrent stress test for ArtIndex epoch-based lock-free reads.
//
// Spawns one writer thread that continuously Insert/Remove leaves on a
// rotating set of paths, plus N reader threads that continuously Lookup
// random paths. The test passes if:
//   * Readers never crash (no UAF, no nullptr-deref on retired leaves)
//   * Writer never crashes (no concurrent-modification corruption)
//   * Eventual reclamation frees retired nodes (PendingRetires eventually
//     drops near zero after writers stop)
//
// This validates the epoch contract: a reader's leaf pointer stays valid
// for the lifetime of its EpochGuard, even if the writer concurrently
// removes / replaces that leaf.
#include "prefix/art_index.h"
#include "prefix/lpm.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

using kvcache::node::prefix::ArtIndex;
using kvcache::node::prefix::ChunkHash;
using kvcache::node::prefix::LeafData;

namespace {
ChunkHash MakeHash(uint8_t a, uint8_t b = 0) {
    ChunkHash h{};
    h[0] = a; h[1] = b;
    return h;
}
std::unique_ptr<LeafData> MakeLeaf(uint64_t bytes) {
    auto l = std::make_unique<LeafData>();
    l->bytes_total = bytes;
    return l;
}
}  // namespace

TEST(ArtConcurrentTest, ReadersAndWriterDoNotCrash) {
    ArtIndex art;

    // Pre-populate so readers have something to find from t=0.
    for (uint8_t i = 0; i < 32; ++i) {
        std::vector<ChunkHash> p{MakeHash(i)};
        art.Insert({p.data(), p.size()}, MakeLeaf(static_cast<uint64_t>(i) + 1));
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reader_ops{0};
    std::atomic<uint64_t> writer_ops{0};
    std::atomic<uint64_t> hits{0};

    // 4 reader threads, doing constant lookups.
    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&, t] {
            std::mt19937 rng(static_cast<uint32_t>(t) * 7919u + 1);
            while (!stop.load(std::memory_order_acquire)) {
                const uint8_t k = static_cast<uint8_t>(rng() & 0x1F);  // 0..31
                std::vector<ChunkHash> path{MakeHash(k)};
                auto g = art.EnterRead();
                auto r = art.Lookup({path.data(), path.size()}, g);
                if (r.leaf) {
                    // Read a field — if the leaf was freed, ASAN/MSAN would
                    // crash here. Without those, dereferencing a retired
                    // pointer is still UB, so we touch it to maximise the
                    // chance of catching breakage in stress.
                    (void)r.leaf->bytes_total;
                    hits.fetch_add(1, std::memory_order_relaxed);
                }
                reader_ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 1 writer thread doing Insert/Remove on the same key range.
    std::thread writer([&] {
        std::mt19937 rng(424242u);
        while (!stop.load(std::memory_order_acquire)) {
            const uint8_t k = static_cast<uint8_t>(rng() & 0x1F);
            std::vector<ChunkHash> path{MakeHash(k)};
            if (rng() & 1) {
                art.Insert({path.data(), path.size()}, MakeLeaf(rng()));
            } else {
                art.Remove({path.data(), path.size()});
            }
            writer_ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Let it run for 300 ms — that's plenty for any UAF / data race to
    // surface in a stress run.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_release);

    for (auto& r : readers) r.join();
    writer.join();

    // Sanity: each role did real work.
    EXPECT_GT(reader_ops.load(), 1000u);
    EXPECT_GT(writer_ops.load(), 100u);
    EXPECT_GT(hits.load(), 0u);

    // After workers stop, retired items should eventually drain. Force a
    // few reclaim passes — at this point all readers have exited their
    // guards so MinActiveEpoch is kQuiescent, every retired tag is
    // strictly less, and reclamation should empty the list.
    art.RunReclaim();
    art.RunReclaim();
    EXPECT_EQ(art.epoch_manager().PendingRetires(), 0u);
}

TEST(ArtConcurrentTest, ReaderHoldingLeafIsSafeAcrossWriterRemove) {
    // Targeted test: reader Lookup-and-Hold, writer concurrently Remove
    // (same path), reader continues to dereference the leaf pointer
    // safely until its guard expires.
    ArtIndex art;
    std::vector<ChunkHash> path{MakeHash(7)};
    art.Insert({path.data(), path.size()}, MakeLeaf(123));

    // Reader: enter guard, lookup, capture pointer.
    auto g = art.EnterRead();
    auto r = art.Lookup({path.data(), path.size()}, g);
    ASSERT_NE(r.leaf, nullptr);
    LeafData* held = r.leaf;
    EXPECT_EQ(held->bytes_total, 123u);

    // Writer: removes the path. The leaf is retired.
    EXPECT_TRUE(art.Remove({path.data(), path.size()}));
    EXPECT_EQ(art.LeafCount(), 0u);

    // Reader can still dereference `held` because the EpochGuard is alive.
    EXPECT_EQ(held->bytes_total, 123u);
    EXPECT_EQ(art.epoch_manager().PendingRetires(), 1u);

    // Reclaim attempt while guard still alive must NOT free.
    EXPECT_EQ(art.RunReclaim(), 0u);
    EXPECT_EQ(held->bytes_total, 123u);  // still valid

    // After guard goes out of scope (end of test), reclaim will succeed.
    // We test that explicitly here by tearing down the guard.
    g = ArtIndex::ReaderGuard{};  // assign default-constructed — releases epoch
    EXPECT_EQ(art.RunReclaim(), 1u);
}
