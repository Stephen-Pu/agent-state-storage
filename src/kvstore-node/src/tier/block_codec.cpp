// LLD §3.3 T4 — Cold-tier block codecs implementation (Phase B3.1).
#include "tier/block_codec.h"

#if KVCACHE_HAVE_ZSTD
#include <zstd.h>
#endif

namespace kvcache::node::tier {

bool IdentityCodec::Compress(const uint8_t* in, std::size_t n,
                             std::vector<uint8_t>* out, std::string*) {
    out->assign(in, in + n);
    return true;
}

bool IdentityCodec::Decompress(const uint8_t* in, std::size_t n,
                               std::size_t orig_size,
                               std::vector<uint8_t>* out, std::string* err) {
    if (n != orig_size) {
        if (err) *err = "identity: size mismatch (corrupt header?)";
        return false;
    }
    out->assign(in, in + n);
    return true;
}

#if KVCACHE_HAVE_ZSTD

bool ZstdCodec::Compress(const uint8_t* in, std::size_t n,
                         std::vector<uint8_t>* out, std::string* err) {
    const std::size_t bound = ZSTD_compressBound(n);
    out->resize(bound);
    const std::size_t r = ZSTD_compress(out->data(), bound, in, n, level_);
    if (ZSTD_isError(r)) {
        if (err) *err = std::string("zstd compress: ") + ZSTD_getErrorName(r);
        return false;
    }
    out->resize(r);
    return true;
}

bool ZstdCodec::Decompress(const uint8_t* in, std::size_t n,
                           std::size_t orig_size,
                           std::vector<uint8_t>* out, std::string* err) {
    out->resize(orig_size);
    const std::size_t r = ZSTD_decompress(out->data(), orig_size, in, n);
    if (ZSTD_isError(r)) {
        if (err) *err = std::string("zstd decompress: ") + ZSTD_getErrorName(r);
        return false;
    }
    if (r != orig_size) {
        if (err) *err = "zstd decompress: size mismatch";
        return false;
    }
    return true;
}

#endif  // KVCACHE_HAVE_ZSTD

std::unique_ptr<IBlockCodec> MakeCodec(const std::string& name, int level,
                                       std::string* err) {
    if (name == "none" || name == "identity") {
        return std::make_unique<IdentityCodec>();
    }
    if (name == "zstd") {
#if KVCACHE_HAVE_ZSTD
        return std::make_unique<ZstdCodec>(level);
#else
        if (err) *err = "codec 'zstd' not compiled in (build with "
                        "KVCACHE_ENABLE_ZSTD=ON)";
        return nullptr;
#endif
    }
    (void)level;
    if (err) *err = "unknown codec '" + name + "'";
    return nullptr;
}

}  // namespace kvcache::node::tier
