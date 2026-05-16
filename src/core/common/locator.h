// LLD §2.1 — Locator construction and wire serialization helpers.
//
// kv_locator_t is the C wire struct (see include/kvcache/kv_types.h).
// This header provides the C++ side: hashing, comparison, and (de)serialization.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "kvcache/kv_types.h"

namespace kvcache {

// Strongly-typed view of the 16-byte tenant UUID.
using TenantId = std::array<uint8_t, 16>;

// Strongly-typed view of the 16-byte prefix hash (BLAKE3-128).
using PrefixHash = std::array<uint8_t, 16>;

// Build a Locator from its component parts. The 'range' field defaults to
// covering all layers/heads/tokens (zeroed counts mean "unspecified" —
// callers fill in before transfer).
kv_locator_t MakeLocator(TenantId tenant,
                         uint64_t model_id_hash,
                         PrefixHash prefix,
                         kv_range_t range);

// Deterministic 64-bit hash over a Locator's identity fields (excludes
// 'range' so that all ranges of the same prefix collide into the same
// routing bucket — see LLD §4.2 HRW).
uint64_t LocatorIdentityHash(const kv_locator_t& loc);

// Equality on identity (tenant + model + prefix). Range-insensitive.
bool LocatorIdentityEq(const kv_locator_t& a, const kv_locator_t& b);

// Wire (de)serialization — little-endian, packed, 64 bytes.
// Returns false on size mismatch or version field unsupported.
bool SerializeLocator(const kv_locator_t& loc, std::span<uint8_t, 64> out);
bool DeserializeLocator(std::span<const uint8_t, 64> in, kv_locator_t* out);

// Canonical model_id_hash: hash of "<name>|<weights_sha>|<quant>|<tokenizer_sha>".
// See LLD §3.1 strong model isolation.
uint64_t ComputeModelIdHash(std::string_view canonical_model_id);

}  // namespace kvcache
