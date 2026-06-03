// LLD §3.3 T4 — Compression middleware for the cold tier (Phase B3.1).
//
// `CompressingColdTier` is a decorator that wraps ANY `IColdTier`
// (FilesystemColdTier or RestColdTier) and runs a block codec on the data
// path: compress on Put, decompress on Get. Delete / Exists / key layout are
// delegated unchanged, so the decorator is transparent to callers and to the
// inner tier's keying.
//
// On-blob format — a small self-describing header lets a Get pick the right
// codec regardless of which codec wrote the blob (so flipping the configured
// codec doesn't strand previously-written data):
//
//   offset  size  field
//   0       4     magic "KVB1"
//   4       1     codec id (CodecId)
//   5       3     reserved (zero)
//   8       8     original (uncompressed) size, little-endian u64
//   16      ...   codec payload
//
// The header is 16 bytes — negligible against KV-cache chunk sizes. A blob
// whose codec id isn't compiled into the reader fails the Get with a clear
// error rather than returning garbage.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tier/block_codec.h"
#include "tier/cold_tier.h"

namespace kvcache::node::tier {

class CompressingColdTier final : public IColdTier {
   public:
    // Takes ownership of the inner tier and the codec. Neither may be null.
    static std::unique_ptr<CompressingColdTier> Create(
        std::unique_ptr<IColdTier> inner, std::unique_ptr<IBlockCodec> codec,
        std::string* err);

    ~CompressingColdTier() override = default;

    std::string Name() const override;

    bool Put   (const DramKey&, const uint8_t* data, std::size_t n, std::string* err) override;
    bool Get   (const DramKey&, std::vector<uint8_t>* out, std::string* err) override;
    bool Delete(const DramKey&, std::string* err) override;
    bool Exists(const DramKey&) const override;

    // Header size in bytes (exposed for tests).
    static constexpr std::size_t kHeaderSize = 16;

   private:
    CompressingColdTier() = default;

    // Decode the codec id for a stored blob's header byte. Returns nullptr +
    // sets *err if the header is malformed or names an uncompiled codec.
    IBlockCodec* CodecForId(CodecId id, std::string* err);

    std::unique_ptr<IColdTier>   inner_;
    std::unique_ptr<IBlockCodec> write_codec_;   // used on Put
    // Read-side codecs by id. Always holds an IdentityCodec; holds the write
    // codec too (so its blobs are readable). Populated in Create.
    std::unique_ptr<IBlockCodec> identity_;      // always present for reads
};

}  // namespace kvcache::node::tier
