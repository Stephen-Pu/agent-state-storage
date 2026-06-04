// Phase B3.2 — EncryptingColdTier (AES-256-GCM) tests.
//
// Exercised against an in-memory FakeColdTier so the sealed blob is fully
// observable: round-trip, header shape, tamper-detection (flip a ciphertext
// byte → auth failure), wrong-key failure, key-length validation, delegation,
// and factory wiring (encryption alone + composed with compression).
#include "tier/encrypting_cold_tier.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "tier/cold_tier.h"

using kvcache::node::tier::ColdTierOptions;
using kvcache::node::tier::CreateColdTier;
using kvcache::node::tier::DramKey;
using kvcache::node::tier::EncryptingColdTier;
using kvcache::node::tier::IColdTier;

namespace {

DramKey Key(uint8_t b) {
    DramKey k{};
    k.bytes[0] = b;
    k.bytes[15] = b;
    return k;
}

std::vector<uint8_t> Key32(uint8_t fill) { return std::vector<uint8_t>(32, fill); }

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
            if (err) err->clear();
            return false;
        }
        out->assign(it->second.begin(), it->second.end());
        return true;
    }
    bool Delete(const DramKey& k, std::string*) override { store_.erase(KeyStr(k)); return true; }
    bool Exists(const DramKey& k) const override { return store_.count(KeyStr(k)) != 0; }

    std::string* Raw(const DramKey& k) {
        auto it = store_.find(KeyStr(k));
        return it == store_.end() ? nullptr : &it->second;
    }

   private:
    static std::string KeyStr(const DramKey& k) {
        return std::string(reinterpret_cast<const char*>(k.bytes.data()), k.bytes.size());
    }
    std::map<std::string, std::string> store_;
};

std::unique_ptr<EncryptingColdTier> MakeTier(FakeColdTier** borrow,
                                             const std::vector<uint8_t>& key) {
    auto fake = std::make_unique<FakeColdTier>();
    *borrow = fake.get();
    EncryptingColdTier::Options o;
    o.key = key;
    std::string err;
    auto t = EncryptingColdTier::Create(std::move(fake), o, &err);
    return t;  // may be null on a no-OpenSSL build — caller checks
}

}  // namespace

TEST(EncryptingColdTierTest, RejectsBadKeyLength) {
    auto fake = std::make_unique<FakeColdTier>();
    EncryptingColdTier::Options o;
    o.key = std::vector<uint8_t>(16, 0);  // too short
    std::string err;
    EXPECT_EQ(EncryptingColdTier::Create(std::move(fake), o, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

#if KVCACHE_HAVE_OPENSSL

TEST(EncryptingColdTierTest, RoundTripAndHeaderShape) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, Key32(0x11));
    ASSERT_NE(t, nullptr);
    std::string err;

    std::vector<uint8_t> data{10, 20, 30, 40, 50, 60};
    ASSERT_TRUE(t->Put(Key(1), data.data(), data.size(), &err)) << err;

    // Stored blob = header + ciphertext; ciphertext len == plaintext len.
    std::string* raw = fake->Raw(Key(1));
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->size(), EncryptingColdTier::kHeaderSize + data.size());
    EXPECT_EQ(raw->substr(0, 4), "KVE1");
    // Ciphertext must not equal plaintext (it's encrypted).
    EXPECT_NE(raw->substr(EncryptingColdTier::kHeaderSize),
              std::string(data.begin(), data.end()));

    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(1), &out, &err)) << err;
    EXPECT_EQ(out, data);
}

TEST(EncryptingColdTierTest, EmptyPayloadRoundTrips) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, Key32(0x22));
    ASSERT_NE(t, nullptr);
    std::string err;
    ASSERT_TRUE(t->Put(Key(2), nullptr, 0, &err)) << err;
    std::vector<uint8_t> out{0xFF};
    ASSERT_TRUE(t->Get(Key(2), &out, &err)) << err;
    EXPECT_TRUE(out.empty());
}

TEST(EncryptingColdTierTest, DistinctNoncePerPut) {
    // Same key + same plaintext → different stored blobs (fresh random nonce).
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, Key32(0x33));
    ASSERT_NE(t, nullptr);
    std::string err;
    std::vector<uint8_t> data(64, 0xAB);
    ASSERT_TRUE(t->Put(Key(1), data.data(), data.size(), &err)) << err;
    std::string first = *fake->Raw(Key(1));
    ASSERT_TRUE(t->Put(Key(2), data.data(), data.size(), &err)) << err;
    std::string second = *fake->Raw(Key(2));
    EXPECT_NE(first, second) << "nonce reuse — identical sealed blobs";
}

TEST(EncryptingColdTierTest, TamperedCiphertextFailsAuth) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, Key32(0x44));
    ASSERT_NE(t, nullptr);
    std::string err;
    std::vector<uint8_t> data(48, 0x5A);
    ASSERT_TRUE(t->Put(Key(1), data.data(), data.size(), &err)) << err;

    // Flip one ciphertext byte → GCM tag check must fail on Get.
    std::string* raw = fake->Raw(Key(1));
    ASSERT_NE(raw, nullptr);
    (*raw)[EncryptingColdTier::kHeaderSize] ^= 0x01;

    std::vector<uint8_t> out;
    EXPECT_FALSE(t->Get(Key(1), &out, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(out.empty());
}

TEST(EncryptingColdTierTest, WrongKeyFailsAuth) {
    // Encrypt under one key; decrypt with a second tier holding a different
    // key, primed with the same sealed blob.
    FakeColdTier* fakeA = nullptr;
    auto encTier = MakeTier(&fakeA, Key32(0x55));
    ASSERT_NE(encTier, nullptr);
    std::string err;
    std::vector<uint8_t> data(32, 0x77);
    ASSERT_TRUE(encTier->Put(Key(1), data.data(), data.size(), &err)) << err;
    std::string sealed = *fakeA->Raw(Key(1));

    // New tier, different key, prime its backend with the sealed blob.
    FakeColdTier* fakeB = nullptr;
    auto decTier = MakeTier(&fakeB, Key32(0x66));
    ASSERT_NE(decTier, nullptr);
    ASSERT_TRUE(fakeB->Put(Key(1),
        reinterpret_cast<const uint8_t*>(sealed.data()), sealed.size(), &err));
    std::vector<uint8_t> out;
    EXPECT_FALSE(decTier->Get(Key(1), &out, &err));  // wrong key → auth fail
    EXPECT_FALSE(err.empty());
}

TEST(EncryptingColdTierTest, ShortBlobBelowHeaderFails) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, Key32(0xC1));
    ASSERT_NE(t, nullptr);
    std::string err;
    // Inject a blob shorter than the 36-byte header directly into the backend.
    std::vector<uint8_t> tiny{'K', 'V', 'E', '1', 1};
    ASSERT_TRUE(fake->Put(Key(1), tiny.data(), tiny.size(), &err));
    std::vector<uint8_t> out;
    EXPECT_FALSE(t->Get(Key(1), &out, &err));
    EXPECT_FALSE(err.empty());
}

TEST(EncryptingColdTierTest, UnknownAlgIdFails) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, Key32(0xC2));
    ASSERT_NE(t, nullptr);
    std::string err;
    // Valid magic + header length, but an alg id this build doesn't know.
    std::vector<uint8_t> blob(EncryptingColdTier::kHeaderSize + 8, 0);
    blob[0] = 'K'; blob[1] = 'V'; blob[2] = 'E'; blob[3] = '1';
    blob[4] = 0x09;  // unknown alg
    ASSERT_TRUE(fake->Put(Key(1), blob.data(), blob.size(), &err));
    std::vector<uint8_t> out;
    EXPECT_FALSE(t->Get(Key(1), &out, &err));
    EXPECT_FALSE(err.empty());
}

TEST(EncryptingColdTierTest, GetMissingDelegatesCleanly) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, Key32(0x88));
    ASSERT_NE(t, nullptr);
    std::vector<uint8_t> out;
    std::string err = "pre";
    EXPECT_FALSE(t->Get(Key(9), &out, &err));
    EXPECT_TRUE(err.empty());
}

TEST(EncryptingColdTierTest, DeleteExistsDelegateAndName) {
    FakeColdTier* fake = nullptr;
    auto t = MakeTier(&fake, Key32(0x99));
    ASSERT_NE(t, nullptr);
    std::string err;
    std::vector<uint8_t> data(16, 1);
    ASSERT_TRUE(t->Put(Key(3), data.data(), data.size(), &err)) << err;
    EXPECT_TRUE(t->Exists(Key(3)));
    EXPECT_TRUE(t->Delete(Key(3), &err)) << err;
    EXPECT_FALSE(t->Exists(Key(3)));
    EXPECT_EQ(t->Name(), "fake+aes256gcm");
}

// ---- factory wiring -------------------------------------------------------

TEST(ColdTierFactoryTest, EncryptionWrapsBaseRoundTrip) {
    ColdTierOptions o;
    o.type = "fs";
    o.fs.root = std::string(::testing::TempDir()) + "/kvc_b32_enc";
    o.encryption.enabled = true;
    o.encryption.key = Key32(0xAA);
    std::string err;
    auto t = CreateColdTier(o, &err);
    ASSERT_NE(t, nullptr) << err;
    EXPECT_EQ(t->Name(), "filesystem+aes256gcm");

    std::vector<uint8_t> data(200, 0x42);
    ASSERT_TRUE(t->Put(Key(7), data.data(), data.size(), &err)) << err;
    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(7), &out, &err)) << err;
    EXPECT_EQ(out, data);
}

TEST(ColdTierFactoryTest, EncryptionBadKeyFails) {
    ColdTierOptions o;
    o.type = "fs";
    o.fs.root = std::string(::testing::TempDir()) + "/kvc_b32_badkey";
    o.encryption.enabled = true;
    o.encryption.key = std::vector<uint8_t>(8, 0);  // wrong length
    std::string err;
    EXPECT_EQ(CreateColdTier(o, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(ColdTierFactoryTest, CompressThenEncryptComposes) {
    // Both layers on: Name reflects compress-outer / encrypt-inner; a
    // round-trip through the real FS backend still recovers the bytes.
    ColdTierOptions o;
    o.type = "fs";
    o.fs.root = std::string(::testing::TempDir()) + "/kvc_b32_both";
    o.compression.codec = "identity";  // always available
    o.encryption.enabled = true;
    o.encryption.key = Key32(0xBB);
    std::string err;
    auto t = CreateColdTier(o, &err);
    ASSERT_NE(t, nullptr) << err;
    // Outermost = compression wraps the encryptor which wraps the fs tier.
    EXPECT_EQ(t->Name(), "filesystem+aes256gcm+identity");

    std::vector<uint8_t> data(512, 0xCD);
    ASSERT_TRUE(t->Put(Key(8), data.data(), data.size(), &err)) << err;
    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(8), &out, &err)) << err;
    EXPECT_EQ(out, data);
}

#endif  // KVCACHE_HAVE_OPENSSL
