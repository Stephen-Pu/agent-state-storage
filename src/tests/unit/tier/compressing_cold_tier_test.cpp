// Phase B3.1 — CompressingColdTier + block-codec tests.
//
// The decorator + header logic is exercised deterministically against an
// in-memory FakeColdTier with the always-available IdentityCodec; a
// KVCACHE_HAVE_ZSTD-gated test covers the real zstd round-trip + ratio.
#include "tier/compressing_cold_tier.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "tier/block_codec.h"
#include "tier/cold_tier.h"

using kvcache::node::tier::ColdTierOptions;
using kvcache::node::tier::CompressingColdTier;
using kvcache::node::tier::CreateColdTier;
using kvcache::node::tier::DramKey;
using kvcache::node::tier::IBlockCodec;
using kvcache::node::tier::IColdTier;
using kvcache::node::tier::IdentityCodec;
using kvcache::node::tier::MakeCodec;

namespace {

DramKey Key(uint8_t b) {
    DramKey k{};
    k.bytes[0] = b;
    k.bytes[15] = b;
    return k;
}

// Minimal in-memory IColdTier so the decorator's behaviour is fully
// observable (what bytes hit the backend, miss semantics, delete).
class FakeColdTier final : public IColdTier {
   public:
    std::string Name() const override { return "fake"; }
    bool Put(const DramKey& k, const uint8_t* d, std::size_t n, std::string*) override {
        store_[KeyStr(k)] = std::string(reinterpret_cast<const char*>(d), n);
        return true;
    }
    bool Get(const DramKey& k, std::vector<uint8_t>* out, std::string* err) override {
        auto it = store_.find(KeyStr(k));
        if (it == store_.end()) {
            if (err) err->clear();  // miss, not an error
            return false;
        }
        out->assign(it->second.begin(), it->second.end());
        return true;
    }
    bool Delete(const DramKey& k, std::string*) override {
        store_.erase(KeyStr(k));
        return true;
    }
    bool Exists(const DramKey& k) const override {
        return store_.count(KeyStr(k)) != 0;
    }
    // Test introspection: the raw stored blob for a key.
    const std::string* Raw(const DramKey& k) const {
        auto it = store_.find(KeyStr(k));
        return it == store_.end() ? nullptr : &it->second;
    }

   private:
    static std::string KeyStr(const DramKey& k) {
        return std::string(reinterpret_cast<const char*>(k.bytes.data()),
                           k.bytes.size());
    }
    std::map<std::string, std::string> store_;
};

// Build a CompressingColdTier over a FakeColdTier we keep a borrowed
// pointer to for introspection.
std::unique_ptr<CompressingColdTier> MakeTier(FakeColdTier** borrow,
                                              std::unique_ptr<IBlockCodec> codec) {
    auto fake = std::make_unique<FakeColdTier>();
    *borrow = fake.get();
    std::string err;
    auto t = CompressingColdTier::Create(std::move(fake), std::move(codec), &err);
    EXPECT_NE(t, nullptr) << err;
    return t;
}

}  // namespace

TEST(CompressingColdTierTest, RejectsNullInnerOrCodec) {
    std::string err;
    EXPECT_EQ(CompressingColdTier::Create(nullptr,
                  std::make_unique<IdentityCodec>(), &err), nullptr);
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(CompressingColdTier::Create(
                  std::make_unique<FakeColdTier>(), nullptr, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(CompressingColdTierTest, IdentityRoundTripAndHeader) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, std::make_unique<IdentityCodec>());
    std::string err;

    std::vector<uint8_t> data{1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(t->Put(Key(1), data.data(), data.size(), &err)) << err;

    // The stored blob carries the 16-byte header + identity payload.
    const std::string* raw = fake->Raw(Key(1));
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->size(), CompressingColdTier::kHeaderSize + data.size());
    EXPECT_EQ(raw->substr(0, 4), "KVB1");

    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(1), &out, &err)) << err;
    EXPECT_EQ(out, data);
}

TEST(CompressingColdTierTest, EmptyPayloadRoundTrips) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, std::make_unique<IdentityCodec>());
    std::string err;
    ASSERT_TRUE(t->Put(Key(2), nullptr, 0, &err)) << err;
    std::vector<uint8_t> out{0xFF};
    ASSERT_TRUE(t->Get(Key(2), &out, &err)) << err;
    EXPECT_TRUE(out.empty());
}

TEST(CompressingColdTierTest, GetMissingDelegatesAndIsNotAnError) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, std::make_unique<IdentityCodec>());
    std::vector<uint8_t> out;
    std::string err = "preexisting";
    EXPECT_FALSE(t->Get(Key(9), &out, &err));
    EXPECT_TRUE(err.empty());
}

TEST(CompressingColdTierTest, DeleteAndExistsDelegate) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, std::make_unique<IdentityCodec>());
    std::string err;
    std::vector<uint8_t> data(32, 0xAB);
    ASSERT_TRUE(t->Put(Key(3), data.data(), data.size(), &err)) << err;
    EXPECT_TRUE(t->Exists(Key(3)));
    EXPECT_TRUE(t->Delete(Key(3), &err)) << err;
    EXPECT_FALSE(t->Exists(Key(3)));
}

TEST(CompressingColdTierTest, CorruptHeaderFailsGet) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, std::make_unique<IdentityCodec>());
    std::string err;
    // Inject a too-short blob directly into the backend.
    std::vector<uint8_t> junk{'x', 'y'};
    ASSERT_TRUE(fake->Put(Key(4), junk.data(), junk.size(), &err));
    std::vector<uint8_t> out;
    EXPECT_FALSE(t->Get(Key(4), &out, &err));
    EXPECT_FALSE(err.empty());

    // Right length, wrong magic.
    std::vector<uint8_t> badmagic(CompressingColdTier::kHeaderSize + 2, 0);
    ASSERT_TRUE(fake->Put(Key(5), badmagic.data(), badmagic.size(), &err));
    EXPECT_FALSE(t->Get(Key(5), &out, &err));
    EXPECT_FALSE(err.empty());
}

TEST(CompressingColdTierTest, NameComposesInnerAndCodec) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, std::make_unique<IdentityCodec>());
    EXPECT_EQ(t->Name(), "fake+identity");
}

// ---- MakeCodec ------------------------------------------------------------

TEST(BlockCodecTest, MakeCodecNoneAndIdentity) {
    std::string err;
    EXPECT_NE(MakeCodec("none", 3, &err), nullptr);
    EXPECT_NE(MakeCodec("identity", 3, &err), nullptr);
    EXPECT_EQ(MakeCodec("bogus", 3, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(BlockCodecTest, MakeCodecZstdGated) {
    std::string err;
    auto z = MakeCodec("zstd", 3, &err);
#if KVCACHE_HAVE_ZSTD
    EXPECT_NE(z, nullptr) << err;
#else
    EXPECT_EQ(z, nullptr);  // not compiled in → error
    EXPECT_FALSE(err.empty());
#endif
}

// ---- factory wiring -------------------------------------------------------

TEST(ColdTierFactoryTest, CompressionNoneLeavesBaseUnwrapped) {
    ColdTierOptions o;
    o.type = "fs";
    auto root = std::string(::testing::TempDir()) + "/kvc_b31_none";
    o.fs.root = root;
    o.compression.codec = "none";
    std::string err;
    auto t = CreateColdTier(o, &err);
    ASSERT_NE(t, nullptr) << err;
    EXPECT_EQ(t->Name(), "filesystem");  // not wrapped
}

TEST(ColdTierFactoryTest, CompressionIdentityWrapsBase) {
    ColdTierOptions o;
    o.type = "fs";
    o.fs.root = std::string(::testing::TempDir()) + "/kvc_b31_id";
    o.compression.codec = "identity";
    std::string err;
    auto t = CreateColdTier(o, &err);
    ASSERT_NE(t, nullptr) << err;
    EXPECT_EQ(t->Name(), "filesystem+identity");

    // End-to-end through the real filesystem backend.
    std::vector<uint8_t> data(100, 0x5A);
    ASSERT_TRUE(t->Put(Key(7), data.data(), data.size(), &err)) << err;
    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(7), &out, &err)) << err;
    EXPECT_EQ(out, data);
}

TEST(ColdTierFactoryTest, UnknownCodecFails) {
    ColdTierOptions o;
    o.type = "fs";
    o.fs.root = std::string(::testing::TempDir()) + "/kvc_b31_bad";
    o.compression.codec = "lz4-not-supported";
    std::string err;
    EXPECT_EQ(CreateColdTier(o, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

#if KVCACHE_HAVE_ZSTD
TEST(CompressingColdTierTest, ZstdRoundTripAndCompresses) {
    std::string err;
    auto codec = MakeCodec("zstd", 3, &err);
    ASSERT_NE(codec, nullptr) << err;
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, std::move(codec));

    // Highly compressible payload (all zeros) so the stored blob — header
    // included — is meaningfully smaller than the original.
    std::vector<uint8_t> data(64 * 1024, 0);
    ASSERT_TRUE(t->Put(Key(1), data.data(), data.size(), &err)) << err;

    const std::string* raw = fake->Raw(Key(1));
    ASSERT_NE(raw, nullptr);
    EXPECT_LT(raw->size(), data.size())
        << "zstd should shrink an all-zero 64 KiB blob";
    EXPECT_EQ(raw->substr(0, 4), "KVB1");

    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(1), &out, &err)) << err;
    EXPECT_EQ(out, data);
}

TEST(CompressingColdTierTest, ZstdRoundTripsIncompressibleData) {
    std::string err;
    auto codec = MakeCodec("zstd", 3, &err);
    ASSERT_NE(codec, nullptr) << err;
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, std::move(codec));

    // Pseudo-random-ish bytes — round-trip must still be exact even when
    // compression can't shrink (zstd stores a near-verbatim frame).
    std::vector<uint8_t> data(4096);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 1103515245u + 12345u) >> 7);
    }
    ASSERT_TRUE(t->Put(Key(2), data.data(), data.size(), &err)) << err;
    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(2), &out, &err)) << err;
    EXPECT_EQ(out, data);
}
#endif  // KVCACHE_HAVE_ZSTD
