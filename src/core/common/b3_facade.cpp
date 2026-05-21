// BLAKE3 facade implementation — real BLAKE3 (BLAKE3-team reference C lib,
// 1.5.x), pulled in via the `BLAKE3::blake3` CMake target.
//
// This replaced the FNV-1a + SplitMix64 placeholder. The placeholder was
// adequate for unit tests but failed two production-relevance criteria:
//   (1) the 64-bit output had measurable bias for short, similar inputs
//       (the kind HRW feeds it: "node_id || key"), which made HRW node
//       distribution drift from uniform;
//   (2) the 128-bit "BLAKE3_128" was just a layered FNV-1a, with no real
//       avalanche — wholly inappropriate as a content-address.
//
// With the real BLAKE3 in place, all hash quality assumptions in the rest
// of the codebase (HRW uniformity, Bloom-filter FPR, chunk-content keys)
// are met.
#include "b3_facade.h"

#include <cstring>

extern "C" {
#include <blake3.h>  // the official BLAKE3 C header
}

namespace kvcache::hash {

namespace {

inline void HashOnce(std::span<const uint8_t> data,
                     uint8_t* out, std::size_t out_len) noexcept {
    blake3_hasher h;
    blake3_hasher_init(&h);
    if (!data.empty()) {
        blake3_hasher_update(&h, data.data(), data.size());
    }
    blake3_hasher_finalize(&h, out, out_len);
}

}  // namespace

Digest128 Blake3_128(std::span<const uint8_t> data) noexcept {
    Digest128 out{};
    HashOnce(data, out.data(), out.size());
    return out;
}

Digest256 Blake3_256(std::span<const uint8_t> data) noexcept {
    Digest256 out{};
    HashOnce(data, out.data(), out.size());
    return out;
}

uint64_t Blake3_64(std::span<const uint8_t> data) noexcept {
    // BLAKE3's output is uniformly distributed; the first 8 bytes are as
    // strong as any other 8-byte slice. We don't need the SplitMix64
    // finalizer the placeholder relied on.
    uint8_t buf[8];
    HashOnce(data, buf, sizeof(buf));
    uint64_t v;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// Hasher (streaming)
// ---------------------------------------------------------------------------

struct Hasher::Impl {
    blake3_hasher h;
};

Hasher::Hasher() : impl_(std::make_unique<Impl>()) {
    blake3_hasher_init(&impl_->h);
}

Hasher::~Hasher() = default;

void Hasher::Update(std::span<const uint8_t> data) noexcept {
    if (data.empty() || !impl_) return;
    blake3_hasher_update(&impl_->h, data.data(), data.size());
}

Digest128 Hasher::Finalize128() noexcept {
    Digest128 out{};
    if (impl_) blake3_hasher_finalize(&impl_->h, out.data(), out.size());
    return out;
}

Digest256 Hasher::Finalize256() noexcept {
    Digest256 out{};
    if (impl_) blake3_hasher_finalize(&impl_->h, out.data(), out.size());
    return out;
}

}  // namespace kvcache::hash
