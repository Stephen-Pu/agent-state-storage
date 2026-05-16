// Unified Fetch path: DRAM → NVMe → Cold with promotion.
#include "tier/tier_manager.h"

#include <gtest/gtest.h>
#include <filesystem>

using namespace kvcache::node::tier;

namespace {
DramKey Key(uint8_t b) { DramKey k{}; k.bytes[0] = b; return k; }

TierManager::Options Defaults(const std::string& nvme_path,
                              const std::string& cold_root) {
    TierManager::Options o;
    o.pinned.pool_bytes = 4096; o.pinned.slot_bytes = 4096; o.pinned.use_mlock = false;
    o.dram.capacity_bytes    = 1 << 20;
    o.dram.a1out_max_entries = 16;
    o.enable_nvme = true;
    o.nvme.path        = nvme_path;
    o.nvme.pool_bytes  = 8 * 4096;
    o.nvme.slot_bytes  = 4096;
    o.enable_cold = true;
    o.cold.type       = "fs";
    o.cold.fs.root    = cold_root;
    return o;
}

std::filesystem::path Tmp(const char* tag) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("kvcache_tm_") + tag);
    std::filesystem::remove_all(p);
    return p;
}
}  // namespace

TEST(TierManagerTest, ColdHitPromotesToDramAndNvme) {
    auto root  = Tmp("promote");
    auto nvme  = root / "nvme.bin";
    std::filesystem::create_directories(root);
    std::string err;

    auto tm = TierManager::Create(Defaults(nvme.string(), root.string()), &err);
    ASSERT_NE(tm, nullptr) << err;

    std::vector<uint8_t> data(256, 0x77);
    ASSERT_TRUE(tm->PutCold(Key(1), data.data(), data.size(), &err));

    auto r = tm->Fetch(Key(1), &err);
    EXPECT_EQ(r.hit, TierManager::FetchHit::kCold);
    EXPECT_EQ(r.data, data);
    // Promotion side effects:
    auto lk = tm->LookupDram(Key(1));
    EXPECT_NE(lk.where, DramTier::HitWhere::kMiss);
    std::vector<uint8_t> nvme_out;
    EXPECT_TRUE(tm->GetNvme(Key(1), &nvme_out, &err));
    EXPECT_EQ(nvme_out, data);

    // Second fetch now hits DRAM.
    auto r2 = tm->Fetch(Key(1), &err);
    EXPECT_EQ(r2.hit, TierManager::FetchHit::kDram);
    std::filesystem::remove_all(root);
}

TEST(TierManagerTest, NvmeHitPromotesToDram) {
    auto root = Tmp("nvme_promote");
    auto nvme = root / "nvme.bin";
    std::filesystem::create_directories(root);
    std::string err;
    auto tm = TierManager::Create(Defaults(nvme.string(), root.string()), &err);
    ASSERT_NE(tm, nullptr) << err;
    std::vector<uint8_t> data(64, 0x55);
    ASSERT_TRUE(tm->PutNvme(Key(2), data.data(), data.size(), &err));

    auto r = tm->Fetch(Key(2), &err);
    EXPECT_EQ(r.hit, TierManager::FetchHit::kNvme);
    auto lk = tm->LookupDram(Key(2));
    EXPECT_NE(lk.where, DramTier::HitWhere::kMiss);
    std::filesystem::remove_all(root);
}

TEST(TierManagerTest, MissReturnsCleanly) {
    auto root = Tmp("miss");
    auto nvme = root / "nvme.bin";
    std::filesystem::create_directories(root);
    std::string err;
    auto tm = TierManager::Create(Defaults(nvme.string(), root.string()), &err);
    ASSERT_NE(tm, nullptr);
    auto r = tm->Fetch(Key(42), &err);
    EXPECT_EQ(r.hit, TierManager::FetchHit::kMiss);
    EXPECT_TRUE(r.data.empty());
    std::filesystem::remove_all(root);
}

TEST(TierManagerTest, OptionalTiersCanBeDisabled) {
    TierManager::Options o;
    o.pinned.pool_bytes = 4096; o.pinned.slot_bytes = 4096; o.pinned.use_mlock = false;
    o.dram.capacity_bytes    = 1 << 20;
    o.dram.a1out_max_entries = 16;
    // No NVMe, no Cold.
    std::string err;
    auto tm = TierManager::Create(o, &err);
    ASSERT_NE(tm, nullptr) << err;
    EXPECT_FALSE(tm->HasNvme());
    EXPECT_FALSE(tm->HasCold());

    std::vector<uint8_t> v(8, 1);
    std::string put_err;
    EXPECT_FALSE(tm->PutNvme(Key(1), v.data(), v.size(), &put_err));
    EXPECT_FALSE(put_err.empty());
}
