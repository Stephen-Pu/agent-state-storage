// BLAKE3 facade — used for:
//   - 128-bit prefix_hash inside Locator (LLD §2.1)
//   - 64-bit model_id canonical hash       (LLD §3.1)
//   - 64-bit content-addressed chunk keys  (LLD §3.4 / §3.2)
//
// Implementation: vendored upstream `BLAKE3` C reference (TODO: import as
// third_party/blake3/). This facade keeps the rest of the codebase
// implementation-agnostic.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace kvcache::hash {

using Digest128 = std::array<uint8_t, 16>;
using Digest256 = std::array<uint8_t, 32>;

Digest128 Blake3_128(std::span<const uint8_t> data) noexcept;
Digest256 Blake3_256(std::span<const uint8_t> data) noexcept;
uint64_t  Blake3_64 (std::span<const uint8_t> data) noexcept;

// Streaming variant for hashing token sequences without an extra copy.
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
    alignas(8) unsigned char state_[1920]; // sized to fit BLAKE3 reference state
};

}  // namespace kvcache::hash
