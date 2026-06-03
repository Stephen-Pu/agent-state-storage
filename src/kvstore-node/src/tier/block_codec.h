// LLD §3.3 T4 — Cold-tier block codecs (Phase B3.1).
//
// A codec compresses / decompresses a single cold-tier blob. The cold tier
// stores KV-cache chunks that are written once and read on a cache miss, so
// the trade we want is "spend CPU on the cold path to shrink bytes-at-rest
// and egress" — a textbook fit for zstd at a low level.
//
// The codec is a tiny seam (mirrors B3's IHttpTransport):
//   * IdentityCodec — always compiled; a no-op passthrough.
//   * ZstdCodec     — compiled only under KVCACHE_HAVE_ZSTD.
//
// CompressingColdTier (compressing_cold_tier.h) wraps any IColdTier and runs
// the configured codec on Put, dispatching the right codec on Get via a
// self-describing header — so a blob written by one codec is always readable
// as long as that codec is compiled in.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kvcache::node::tier {

// Stable codec ids — persisted in the on-blob header, so values must never
// be reused for a different algorithm.
enum class CodecId : uint8_t {
    kIdentity = 0,
    kZstd     = 1,
};

class IBlockCodec {
   public:
    virtual ~IBlockCodec() = default;

    virtual CodecId     Id()   const = 0;
    virtual std::string Name() const = 0;

    // Compress `n` bytes at `in` into `*out`. Returns false + sets `*err` on
    // failure. `out` is replaced (not appended).
    virtual bool Compress(const uint8_t* in, std::size_t n,
                          std::vector<uint8_t>* out, std::string* err) = 0;

    // Decompress `n` bytes at `in` (known to expand to exactly `orig_size`
    // bytes — carried in the blob header) into `*out`.
    virtual bool Decompress(const uint8_t* in, std::size_t n,
                            std::size_t orig_size,
                            std::vector<uint8_t>* out, std::string* err) = 0;
};

// No-op passthrough. Always available; used when compression is disabled or
// to read identity-written blobs on any build.
class IdentityCodec final : public IBlockCodec {
   public:
    CodecId     Id()   const override { return CodecId::kIdentity; }
    std::string Name() const override { return "identity"; }
    bool Compress(const uint8_t* in, std::size_t n,
                  std::vector<uint8_t>* out, std::string* err) override;
    bool Decompress(const uint8_t* in, std::size_t n, std::size_t orig_size,
                    std::vector<uint8_t>* out, std::string* err) override;
};

#if KVCACHE_HAVE_ZSTD
// zstd-backed codec. `level` is the zstd compression level (1..22; 3 is the
// library default and a good speed/ratio balance for cold-path writes).
class ZstdCodec final : public IBlockCodec {
   public:
    explicit ZstdCodec(int level = 3) : level_(level) {}
    CodecId     Id()   const override { return CodecId::kZstd; }
    std::string Name() const override { return "zstd"; }
    bool Compress(const uint8_t* in, std::size_t n,
                  std::vector<uint8_t>* out, std::string* err) override;
    bool Decompress(const uint8_t* in, std::size_t n, std::size_t orig_size,
                    std::vector<uint8_t>* out, std::string* err) override;

   private:
    int level_;
};
#endif  // KVCACHE_HAVE_ZSTD

// MakeCodec returns the codec for `name` ("none"/"identity" | "zstd"), or
// nullptr + sets `*err` if the name is unknown or the codec isn't compiled
// in (e.g. "zstd" on a build without KVCACHE_HAVE_ZSTD).
std::unique_ptr<IBlockCodec> MakeCodec(const std::string& name, int level,
                                       std::string* err);

}  // namespace kvcache::node::tier
