// LLD §3.3 T4 — CompressingColdTier implementation (Phase B3.1).
#include "tier/compressing_cold_tier.h"

#include <cstring>

namespace kvcache::node::tier {

namespace {

constexpr char kMagic[4] = {'K', 'V', 'B', '1'};

void WriteHeader(std::vector<uint8_t>* buf, CodecId id, uint64_t orig_size) {
    buf->resize(CompressingColdTier::kHeaderSize);
    std::memcpy(buf->data(), kMagic, 4);
    (*buf)[4] = static_cast<uint8_t>(id);
    (*buf)[5] = 0;
    (*buf)[6] = 0;
    (*buf)[7] = 0;
    for (int i = 0; i < 8; ++i) {
        (*buf)[8 + i] = static_cast<uint8_t>((orig_size >> (8 * i)) & 0xff);
    }
}

bool ReadHeader(const std::vector<uint8_t>& blob, CodecId* id,
                uint64_t* orig_size, std::string* err) {
    if (blob.size() < CompressingColdTier::kHeaderSize) {
        if (err) *err = "compressing: blob shorter than header";
        return false;
    }
    if (std::memcmp(blob.data(), kMagic, 4) != 0) {
        if (err) *err = "compressing: bad magic (not a KVB1 blob)";
        return false;
    }
    *id = static_cast<CodecId>(blob[4]);
    uint64_t sz = 0;
    for (int i = 0; i < 8; ++i) {
        sz |= static_cast<uint64_t>(blob[8 + i]) << (8 * i);
    }
    *orig_size = sz;
    return true;
}

}  // namespace

std::unique_ptr<CompressingColdTier> CompressingColdTier::Create(
    std::unique_ptr<IColdTier> inner, std::unique_ptr<IBlockCodec> codec,
    std::string* err) {
    if (!inner) {
        if (err) *err = "compressing: inner tier is null";
        return nullptr;
    }
    if (!codec) {
        if (err) *err = "compressing: codec is null";
        return nullptr;
    }
    auto t = std::unique_ptr<CompressingColdTier>(new CompressingColdTier());
    t->inner_       = std::move(inner);
    t->write_codec_ = std::move(codec);
    t->identity_    = std::make_unique<IdentityCodec>();
    return t;
}

std::string CompressingColdTier::Name() const {
    return inner_->Name() + "+" + write_codec_->Name();
}

IBlockCodec* CompressingColdTier::CodecForId(CodecId id, std::string* err) {
    if (id == CodecId::kIdentity) return identity_.get();
    if (id == write_codec_->Id()) return write_codec_.get();
    // The blob was written by a codec this build can't decode.
    if (err) {
        *err = "compressing: blob codec id " +
               std::to_string(static_cast<int>(id)) +
               " not available in this build";
    }
    return nullptr;
}

bool CompressingColdTier::Put(const DramKey& key, const uint8_t* data,
                              std::size_t n, std::string* err) {
    std::vector<uint8_t> payload;
    if (!write_codec_->Compress(data, n, &payload, err)) return false;

    std::vector<uint8_t> blob;
    WriteHeader(&blob, write_codec_->Id(), static_cast<uint64_t>(n));
    blob.insert(blob.end(), payload.begin(), payload.end());

    return inner_->Put(key, blob.data(), blob.size(), err);
}

bool CompressingColdTier::Get(const DramKey& key, std::vector<uint8_t>* out,
                              std::string* err) {
    std::vector<uint8_t> blob;
    if (!inner_->Get(key, &blob, err)) return false;  // miss / inner error

    CodecId  id;
    uint64_t orig_size;
    if (!ReadHeader(blob, &id, &orig_size, err)) return false;

    IBlockCodec* codec = CodecForId(id, err);
    if (!codec) return false;

    const uint8_t*    payload = blob.data() + kHeaderSize;
    const std::size_t payload_n = blob.size() - kHeaderSize;
    return codec->Decompress(payload, payload_n,
                             static_cast<std::size_t>(orig_size), out, err);
}

bool CompressingColdTier::Delete(const DramKey& key, std::string* err) {
    return inner_->Delete(key, err);
}

bool CompressingColdTier::Exists(const DramKey& key) const {
    return inner_->Exists(key);
}

}  // namespace kvcache::node::tier
