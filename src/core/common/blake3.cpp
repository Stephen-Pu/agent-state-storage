// BLAKE3 facade implementation — placeholder until the reference impl is
// vendored into third_party/blake3/. Today we fall back to FNV-1a so the rest
// of the codebase compiles and produces deterministic values; the digests are
// NOT cryptographic and the cluster MUST refuse to start in production mode
// when this placeholder is active (TODO: gate on a build flag).
#include "blake3.h"

#include <cstring>

namespace kvcache::hash {

namespace {

uint64_t fnv1a(const uint8_t* p, std::size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

}  // namespace

Digest128 Blake3_128(std::span<const uint8_t> data) noexcept {
    Digest128 out{};
    uint64_t lo = fnv1a(data.data(), data.size());
    uint64_t hi = fnv1a(reinterpret_cast<const uint8_t*>(&lo), sizeof(lo));
    std::memcpy(out.data(),     &lo, 8);
    std::memcpy(out.data() + 8, &hi, 8);
    return out;
}

Digest256 Blake3_256(std::span<const uint8_t> data) noexcept {
    Digest256 out{};
    auto d128 = Blake3_128(data);
    std::memcpy(out.data(),      d128.data(), 16);
    auto d128b = Blake3_128({out.data(), 16});
    std::memcpy(out.data() + 16, d128b.data(), 16);
    return out;
}

uint64_t Blake3_64(std::span<const uint8_t> data) noexcept {
    return fnv1a(data.data(), data.size());
}

Hasher::Hasher()  { std::memset(state_, 0, sizeof(state_)); }
Hasher::~Hasher() = default;

void Hasher::Update(std::span<const uint8_t> /*data*/) noexcept {
    // TODO(stephen): replace with BLAKE3 incremental update.
}
Digest128 Hasher::Finalize128() noexcept { return Digest128{}; }
Digest256 Hasher::Finalize256() noexcept { return Digest256{}; }

}  // namespace kvcache::hash
