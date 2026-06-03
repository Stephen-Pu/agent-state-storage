// LLD §3.3 T4 — EncryptingColdTier implementation (Phase B3.2).
#include "tier/encrypting_cold_tier.h"

#include <cstring>

#if KVCACHE_HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace kvcache::node::tier {

namespace {

constexpr char    kMagic[4] = {'K', 'V', 'E', '1'};
constexpr uint8_t kAlgAes256Gcm = 1;

}  // namespace

std::unique_ptr<EncryptingColdTier> EncryptingColdTier::Create(
    std::unique_ptr<IColdTier> inner, const Options& opts, std::string* err) {
#if !KVCACHE_HAVE_OPENSSL
    (void)inner;
    (void)opts;
    if (err) *err = "cold_tier/encrypt: OpenSSL not compiled in "
                    "(KVCACHE_HAVE_OPENSSL undefined)";
    return nullptr;
#else
    if (!inner) {
        if (err) *err = "cold_tier/encrypt: inner tier is null";
        return nullptr;
    }
    if (opts.key.size() != kKeySize) {
        if (err) *err = "cold_tier/encrypt: key must be 32 bytes (AES-256)";
        return nullptr;
    }
    auto t = std::unique_ptr<EncryptingColdTier>(new EncryptingColdTier());
    t->inner_ = std::move(inner);
    t->key_   = opts.key;
    return t;
#endif
}

std::string EncryptingColdTier::Name() const {
    return inner_->Name() + "+aes256gcm";
}

#if KVCACHE_HAVE_OPENSSL

bool EncryptingColdTier::Put(const DramKey& key, const uint8_t* data,
                             std::size_t n, std::string* err) {
    std::vector<uint8_t> blob(kHeaderSize + n);
    std::memcpy(blob.data(), kMagic, 4);
    blob[4] = kAlgAes256Gcm;
    blob[5] = blob[6] = blob[7] = 0;
    uint8_t* nonce  = blob.data() + 8;
    uint8_t* tag    = blob.data() + 8 + kNonceSize;
    uint8_t* ct     = blob.data() + kHeaderSize;

    if (RAND_bytes(nonce, kNonceSize) != 1) {
        if (err) *err = "encrypt: RAND_bytes failed";
        return false;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        if (err) *err = "encrypt: EVP_CIPHER_CTX_new failed";
        return false;
    }
    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1) break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce) != 1) break;
        int outl = 0;
        if (n > 0) {
            if (EVP_EncryptUpdate(ctx, ct, &outl, data, static_cast<int>(n)) != 1) break;
        }
        int finl = 0;
        if (EVP_EncryptFinal_ex(ctx, ct + outl, &finl) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagSize, tag) != 1) break;
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        if (err) *err = "encrypt: AES-256-GCM seal failed";
        return false;
    }
    return inner_->Put(key, blob.data(), blob.size(), err);
}

bool EncryptingColdTier::Get(const DramKey& key, std::vector<uint8_t>* out,
                             std::string* err) {
    std::vector<uint8_t> blob;
    if (!inner_->Get(key, &blob, err)) return false;  // miss / inner error

    if (blob.size() < kHeaderSize || std::memcmp(blob.data(), kMagic, 4) != 0) {
        if (err) *err = "decrypt: bad magic / short blob (not a KVE1 blob)";
        return false;
    }
    if (blob[4] != kAlgAes256Gcm) {
        if (err) *err = "decrypt: unknown alg id";
        return false;
    }
    const uint8_t* nonce = blob.data() + 8;
    uint8_t*       tag   = blob.data() + 8 + kNonceSize;  // EVP wants non-const
    const uint8_t* ct    = blob.data() + kHeaderSize;
    const std::size_t ct_n = blob.size() - kHeaderSize;

    out->resize(ct_n);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        if (err) *err = "decrypt: EVP_CIPHER_CTX_new failed";
        return false;
    }
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1) break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce) != 1) break;
        int outl = 0;
        if (ct_n > 0) {
            if (EVP_DecryptUpdate(ctx, out->data(), &outl, ct, static_cast<int>(ct_n)) != 1) break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagSize, tag) != 1) break;
        int finl = 0;
        // DecryptFinal returns 0 on tag mismatch — the tamper / wrong-key check.
        if (EVP_DecryptFinal_ex(ctx, out->data() + outl, &finl) != 1) break;
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        out->clear();
        if (err) *err = "decrypt: AES-256-GCM auth failed (tampered or wrong key)";
        return false;
    }
    return true;
}

#else  // !KVCACHE_HAVE_OPENSSL — Create already rejects, these never run.

bool EncryptingColdTier::Put(const DramKey&, const uint8_t*, std::size_t,
                             std::string* err) {
    if (err) *err = "encrypt: OpenSSL not compiled in";
    return false;
}
bool EncryptingColdTier::Get(const DramKey&, std::vector<uint8_t>*,
                             std::string* err) {
    if (err) *err = "decrypt: OpenSSL not compiled in";
    return false;
}

#endif  // KVCACHE_HAVE_OPENSSL

bool EncryptingColdTier::Delete(const DramKey& key, std::string* err) {
    return inner_->Delete(key, err);
}

bool EncryptingColdTier::Exists(const DramKey& key) const {
    return inner_->Exists(key);
}

}  // namespace kvcache::node::tier
