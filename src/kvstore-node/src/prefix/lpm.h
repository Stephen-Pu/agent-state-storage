// LLD §3.2 — Longest-prefix-match over 16-token chunks.
//
// This module bridges the engine-facing API (lookup over a raw token vector)
// and the ART (which is keyed on ChunkHash sequences).
//
//   tokens  (size N)
//     │
//     ▼ Chunkify  (16 tokens per chunk; tail < 16 tokens is dropped — only
//                  whole sealed chunks participate in LPM)
//   chunks (size ⌊N/16⌋)
//     │
//     ▼ Hash each chunk: ChunkHash = Blake3_128(chunk)[0..8]
//   chunk_hashes (size ⌊N/16⌋)
//     │
//     ▼ ArtIndex::Lookup
//   { matched_chunks, leaf* }
//
// Caller converts matched_chunks → matched_tokens = matched_chunks * 16.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "prefix/art_index.h"

namespace kvcache::node::prefix {

inline constexpr uint32_t kChunkTokens = 16;  // LLD §3.2

// Split a token sequence into 16-token chunks and hash each. Any tail < 16 is
// discarded — partial chunks are never sealed and therefore never indexed.
std::vector<ChunkHash> Chunkify(std::span<const uint32_t> tokens);

struct LpmOutcome {
    uint32_t matched_tokens = 0;   // = matched_chunks * 16
    uint32_t matched_chunks = 0;
    LeafData* leaf          = nullptr;  // valid while `guard` is held
};

// Convenience wrapper: builds the ChunkHash vector and runs ArtIndex::Lookup.
// The caller is responsible for holding `guard` for the entire time it
// dereferences `leaf`.
LpmOutcome LongestPrefixMatch(const ArtIndex& art,
                               std::span<const uint32_t> tokens,
                               const ArtIndex::ReaderGuard& guard);

}  // namespace kvcache::node::prefix
