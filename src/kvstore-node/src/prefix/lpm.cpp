// LLD §3.2 — Longest-prefix-match.
#include "prefix/lpm.h"

#include <cstring>

#include "blake3.h"

namespace kvcache::node::prefix {

std::vector<ChunkHash> Chunkify(std::span<const uint32_t> tokens) {
    const std::size_t whole = tokens.size() / kChunkTokens;
    std::vector<ChunkHash> out;
    out.reserve(whole);

    // Each chunk hash = first 8 bytes of BLAKE3-128 over the chunk's raw token
    // bytes. We hash the tokens as little-endian 4-byte words (the wire layout
    // we use everywhere); this MUST stay consistent with seal-side hashing.
    for (std::size_t i = 0; i < whole; ++i) {
        const uint32_t* p = tokens.data() + i * kChunkTokens;
        const auto* raw = reinterpret_cast<const uint8_t*>(p);
        const auto digest = hash::Blake3_128({raw, kChunkTokens * sizeof(uint32_t)});
        ChunkHash h{};
        std::memcpy(h.data(), digest.data(), 8);
        out.push_back(h);
    }
    return out;
}

LpmOutcome LongestPrefixMatch(const ArtIndex& art,
                              std::span<const uint32_t> tokens,
                              const ArtIndex::ReaderGuard& guard) {
    LpmOutcome r{};
    const auto chunks = Chunkify(tokens);
    if (chunks.empty()) return r;
    const auto res = art.Lookup({chunks.data(), chunks.size()}, guard);
    r.matched_chunks = static_cast<uint32_t>(res.matched_chunks);
    r.matched_tokens = r.matched_chunks * kChunkTokens;
    r.leaf           = res.leaf;
    return r;
}

}  // namespace kvcache::node::prefix
