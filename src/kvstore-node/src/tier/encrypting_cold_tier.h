// LLD §3.3 T4 — Encryption middleware for the cold tier (Phase B3.2).
//
// `EncryptingColdTier` is the second cold-tier decorator (after B3.1's
// CompressingColdTier). It wraps ANY `IColdTier` and applies authenticated
// encryption — AES-256-GCM — on the data path: encrypt on Put, decrypt +
// verify on Get. Delete / Exists / key layout delegate unchanged.
//
// On-blob format (self-describing, like the compression header):
//
//   offset  size  field
//   0       4     magic "KVE1"
//   4       1     alg id (1 = AES-256-GCM)
//   5       3     reserved (zero)
//   8       12    GCM nonce (random per Put)
//   20      16    GCM auth tag
//   36      ...   ciphertext (same length as plaintext for GCM)
//
// Authentication: GCM's tag makes decryption tamper-evident — a flipped
// ciphertext or header byte fails `Get` with an auth error rather than
// returning corrupted plaintext.
//
// Key management is out of scope: the 32-byte key is supplied via Options
// (in production from a mounted Secret / KMS-unwrapped DEK). Nonce reuse
// under one key is catastrophic for GCM; we draw a fresh random 96-bit nonce
// per Put — safe up to the ~2^32-message birthday bound, comfortably beyond a
// cold tier's blob count. Per-blob key rotation / a KMS envelope is a future
// cut and slots in behind this same interface.
//
// Compiled only under KVCACHE_HAVE_OPENSSL; without it Create returns an
// error (the rest of the cold-tier code still builds).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tier/cold_tier.h"

namespace kvcache::node::tier {

class EncryptingColdTier final : public IColdTier {
   public:
    struct Options {
        std::vector<uint8_t> key;  // required, exactly 32 bytes (AES-256)
    };

    // Takes ownership of the inner tier. Fails (nullptr + *err) if the key is
    // not 32 bytes or OpenSSL isn't compiled in.
    static std::unique_ptr<EncryptingColdTier> Create(
        std::unique_ptr<IColdTier> inner, const Options& opts,
        std::string* err);

    ~EncryptingColdTier() override = default;

    std::string Name() const override;

    bool Put   (const DramKey&, const uint8_t* data, std::size_t n, std::string* err) override;
    bool Get   (const DramKey&, std::vector<uint8_t>* out, std::string* err) override;
    bool Delete(const DramKey&, std::string* err) override;
    bool Exists(const DramKey&) const override;

    static constexpr std::size_t kNonceSize  = 12;
    static constexpr std::size_t kTagSize    = 16;
    static constexpr std::size_t kHeaderSize = 36;  // 8 + nonce + tag
    static constexpr std::size_t kKeySize    = 32;  // AES-256

   private:
    EncryptingColdTier() = default;

    std::unique_ptr<IColdTier> inner_;
    std::vector<uint8_t>       key_;
};

}  // namespace kvcache::node::tier
