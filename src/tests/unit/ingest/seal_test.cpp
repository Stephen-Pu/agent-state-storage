// SealCommitter end-to-end test. RocksDB is required for the durable side, so
// this test is conditional on KVCACHE_HAVE_ROCKSDB. The non-rocksdb path
// asserts the early-error contract.
#include "ingest/seal.h"

#include <gtest/gtest.h>

#include "ingest/mutable_buffer.h"
#include "ingest/watermark.h"
#include "prefix/art_index.h"
#include "prefix/kv_event_stream.h"
#include "prefix/lpm.h"
#include "tier/tier_manager.h"

using namespace kvcache::node;

#if defined(KVCACHE_HAVE_ROCKSDB)
#include <filesystem>

TEST(SealCommitterTest, EndToEndCommit) {
    // Set up dependencies.
    auto path = std::filesystem::temp_directory_path() / "kvcache_seal_test";
    std::filesystem::remove_all(path);
    meta::RocksdbStore::Options ro; ro.path = path.string();
    std::string err;
    auto rocks = meta::RocksdbStore::Open(ro, &err);
    ASSERT_NE(rocks, nullptr) << err;

    tier::TierManager::Options tmo;
    tmo.pinned.pool_bytes = 4096;
    tmo.pinned.slot_bytes = 4096;
    tmo.pinned.use_mlock  = false;
    tmo.dram.capacity_bytes    = 1 << 20;
    tmo.dram.a1out_max_entries = 8;
    auto tm = tier::TierManager::Create(tmo, &err);
    ASSERT_NE(tm, nullptr) << err;

    ingest::MutableBufferPool::Options bo; bo.start_sweeper = false;
    ingest::MutableBufferPool buffers(tm.get(), bo);
    ingest::WatermarkTracker wm;
    prefix::ArtIndex   art;
    prefix::EventStream events;

    auto handle = buffers.Reserve();
    ASSERT_NE(handle, ingest::kInvalidIngestHandle);
    wm.Track(handle);
    wm.Publish(handle, 1024);

    auto sub = events.Subscribe(64);

    ingest::SealCommitter::Deps deps{rocks.get(), &art, &events, &buffers, &wm};
    ingest::SealCommitter committer(deps);

    ingest::SealCommitter::Request req{};
    req.handle  = handle;
    for (int i = 0; i < 16; ++i) req.locator.tenant_id[i] = static_cast<uint8_t>(i);
    req.locator.model_id_hash = 7;
    for (int i = 0; i < 16; ++i) req.locator.prefix_hash[i] = static_cast<uint8_t>(0xA0 + i);
    req.chunk_path = {prefix::ChunkHash{1, 2, 3, 4, 5, 6, 7, 8}};
    req.tier_residency_bitmap = 0b10;  // pinned only

    auto r = committer.Commit(req);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.sealed_bytes, 1024u);
    EXPECT_EQ(r.new_epoch, 1u);

    // ART now has the leaf.
    auto g = art.EnterRead();
    auto lk = art.Lookup({req.chunk_path.data(), req.chunk_path.size()}, g);
    EXPECT_EQ(lk.matched_chunks, 1u);
    ASSERT_NE(lk.leaf, nullptr);
    EXPECT_EQ(lk.leaf->bytes_total, 1024u);

    // Add event was published.
    prefix::Event ev{};
    ASSERT_TRUE(events.Poll(sub, &ev));
    EXPECT_EQ(ev.type, prefix::EventType::Add);

    // Handle resources were released.
    EXPECT_FALSE(buffers.GetSlot(handle).has_value());
    EXPECT_EQ(wm.Read(handle), 0u);
    events.Unsubscribe(sub);
}

#else

TEST(SealCommitterTest, FailsCleanlyWithoutRocksDB) {
    // Build dependencies that we can — the seal path will fail when it tries
    // to touch rocksdb. We assert the early error rather than a crash.
    tier::TierManager::Options tmo;
    tmo.pinned.pool_bytes = 4096;
    tmo.pinned.slot_bytes = 4096;
    tmo.pinned.use_mlock  = false;
    tmo.dram.capacity_bytes    = 1 << 20;
    tmo.dram.a1out_max_entries = 8;
    std::string err;
    auto tm = tier::TierManager::Create(tmo, &err);
    ASSERT_NE(tm, nullptr);

    ingest::MutableBufferPool::Options bo; bo.start_sweeper = false;
    ingest::MutableBufferPool buffers(tm.get(), bo);
    ingest::WatermarkTracker wm;
    prefix::ArtIndex   art;
    prefix::EventStream events;

    auto handle = buffers.Reserve();
    wm.Track(handle);
    wm.Publish(handle, 100);

    ingest::SealCommitter::Deps deps{nullptr, &art, &events, &buffers, &wm};
    ingest::SealCommitter committer(deps);

    ingest::SealCommitter::Request req{};
    req.handle = handle;
    req.chunk_path = {prefix::ChunkHash{}};
    auto r = committer.Commit(req);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

#endif
