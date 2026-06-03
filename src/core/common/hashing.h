// Canonical FNV-1a 64-bit hash (Phase W-2).
//
// This is the single source of truth for the 64-bit *non-cryptographic*
// identifiers the data path derives from strings/bytes:
//   * model_id_hash   (kv_abi.cpp, trtllm adapter)
//   * tenant_id_hash  (kv_abi.cpp; node service over the SHA-1 tenant digest)
//
// It is deliberately FNV-1a, not BLAKE3 — the scheduler / namespace routing
// only needs a stable, distinct-per-input id, and keeping it a tiny pure
// function means the Python connector (connector.py) can mirror it exactly
// without a BLAKE3 dependency. The wire contract REQUIRES C++ and Python to
// agree byte-for-byte; cross_language consistency is locked by golden-vector
// tests on both sides (test_hashing.cpp / test_connector_hash.py).
//
// Header-only + `inline` so every consumer just #includes this with no new
// link dependency. The standard FNV-1a-64 constants (offset basis +
// prime) are used; e.g. Fnv1a64("a") == 0xaf63dc4c8601ec8c.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kvcache {

inline uint64_t Fnv1a64(const void* data, std::size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;           // FNV prime
    }
    return h;
}

inline uint64_t Fnv1a64(std::string_view s) {
    return Fnv1a64(s.data(), s.size());
}

}  // namespace kvcache
