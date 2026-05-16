#include "tier/cold_tier.h"

#include <gtest/gtest.h>
#include <filesystem>

using kvcache::node::tier::ColdTierOptions;
using kvcache::node::tier::CreateColdTier;
using kvcache::node::tier::DramKey;
using kvcache::node::tier::FilesystemColdTier;

namespace {
DramKey Key(uint8_t b) {
    DramKey k{};
    k.bytes[0] = b; k.bytes[15] = b;
    return k;
}
std::filesystem::path TmpRoot(const char* tag) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("kvcache_cold_") + tag);
    std::filesystem::remove_all(p);
    return p;
}
}  // namespace

TEST(FilesystemColdTierTest, EmptyRootRejected) {
    FilesystemColdTier::Options o;
    o.root = "";
    std::string err;
    EXPECT_EQ(FilesystemColdTier::Create(o, &err), nullptr);
}

TEST(FilesystemColdTierTest, PutGetExistsDelete) {
    auto root = TmpRoot("flow");
    FilesystemColdTier::Options o;
    o.root = root.string();
    std::string err;
    auto t = FilesystemColdTier::Create(o, &err);
    ASSERT_NE(t, nullptr) << err;

    std::vector<uint8_t> data(1024, 0xAA);
    EXPECT_TRUE(t->Put(Key(1), data.data(), data.size(), &err)) << err;
    EXPECT_TRUE(t->Exists(Key(1)));

    std::vector<uint8_t> out;
    EXPECT_TRUE(t->Get(Key(1), &out, &err)) << err;
    EXPECT_EQ(out, data);

    EXPECT_TRUE(t->Delete(Key(1), &err));
    EXPECT_FALSE(t->Exists(Key(1)));
    std::filesystem::remove_all(root);
}

TEST(FilesystemColdTierTest, OverwriteIsAtomic) {
    auto root = TmpRoot("over");
    FilesystemColdTier::Options o; o.root = root.string();
    std::string err;
    auto t = FilesystemColdTier::Create(o, &err);
    ASSERT_NE(t, nullptr);
    std::vector<uint8_t> a(10, 1), b(10, 2);
    EXPECT_TRUE(t->Put(Key(1), a.data(), a.size(), &err));
    EXPECT_TRUE(t->Put(Key(1), b.data(), b.size(), &err));
    std::vector<uint8_t> out;
    EXPECT_TRUE(t->Get(Key(1), &out, &err));
    EXPECT_EQ(out, b);
    std::filesystem::remove_all(root);
}

TEST(FilesystemColdTierTest, GetMissingFails) {
    auto root = TmpRoot("miss");
    FilesystemColdTier::Options o; o.root = root.string();
    std::string err;
    auto t = FilesystemColdTier::Create(o, &err);
    ASSERT_NE(t, nullptr);
    std::vector<uint8_t> out;
    EXPECT_FALSE(t->Get(Key(99), &out, &err));
    EXPECT_FALSE(err.empty());
    std::filesystem::remove_all(root);
}

TEST(ColdTierFactoryTest, FsAliasWorks) {
    auto root = TmpRoot("alias");
    ColdTierOptions o;
    o.type    = "alluxio-fuse";  // alias for fs in MVP
    o.fs.root = root.string();
    std::string err;
    auto t = CreateColdTier(o, &err);
    ASSERT_NE(t, nullptr) << err;
    EXPECT_EQ(t->Name(), "filesystem");
    std::filesystem::remove_all(root);
}

TEST(ColdTierFactoryTest, NativeBackendNotImplemented) {
    ColdTierOptions o; o.type = "alluxio-native";
    std::string err;
    EXPECT_EQ(CreateColdTier(o, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(ColdTierFactoryTest, UnknownBackend) {
    ColdTierOptions o; o.type = "azure-blob-direct";
    std::string err;
    EXPECT_EQ(CreateColdTier(o, &err), nullptr);
}
