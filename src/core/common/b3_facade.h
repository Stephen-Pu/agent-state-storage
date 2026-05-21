// BLAKE3 facade — used for:
//   - 128-bit prefix_hash inside Locator (LLD §2.1)
//   - 64-bit model_id canonical hash       (LLD §3.1)
//   - 64-bit content-addressed chunk keys  (LLD §3.4 / §3.2)
//
// Implementation: the official BLAKE3-team reference C library (1.5.x),
// linked via the `BLAKE3::blake3` CMake target. This facade keeps the
// downstream codebase free of the BLAKE3 C header.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace kvcache::hash {

using Digest128 = std::array<uint8_t, 16>;
using Digest256 = std::array<uint8_t, 32>;

Digest128 Blake3_128(std::span<const uint8_t> data) noexcept;
Digest256 Blake3_256(std::span<const uint8_t> data) noexcept;
uint64_t  Blake3_64 (std::span<const uint8_t> data) noexcept;

// Streaming variant for hashing token sequences without an extra copy.
// Uses pImpl because the size of a `blake3_hasher` is large (~1.9 KiB) and
// would pull the BLAKE3 C header into this public C++ header otherwise.
class Hasher {
   public:
    Hasher();
    ~Hasher();
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    void Update(std::span<const uint8_t> data) noexcept;
    Digest128 Finalize128() noexcept;
    Digest256 Finalize256() noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kvcache::hash
