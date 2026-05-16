// LLD §3.2 — Adaptive Radix Tree, in-memory prefix index.
//
// Keys are sequences of ChunkHash values. A ChunkHash is the first 8 bytes of
// BLAKE3-128(chunk_tokens), where each chunk is 16 tokens (LLD §3.2).
//
//   token stream  : t_0  t_1 ... t_{N-1}
//   chunk i       : tokens [i*16, (i+1)*16)
//   ChunkHash_i   : Blake3_128(chunk_i)[0..8]
//   ART key       : (ChunkHash_0, ChunkHash_1, ..., ChunkHash_{K-1})
//
// LPM semantics: Lookup(path) returns the deepest leaf such that the leaf's
// chunk-path is a prefix of `path`.
//
// Tree shape (this reference implementation):
//   * Every inner node has 256 child slots indexed by the FIRST byte of the
//     next ChunkHash on the path. This is the "Node256" specialization.
//   * The remaining 7 bytes of that ChunkHash live on the edge to the child as
//     an "edge label" (path compression).
//   * MVP: Node256-only — uses ~2 KiB per inner node. The Node4 / Node16 /
//     Node48 specializations from Leis et al. 2013 are deferred — see
//     TODO(perf-art-adaptive). Functional correctness is unchanged.
//
// Concurrency:
//   * The reference impl uses a single shared_mutex: writers (Insert / Remove
//     / Replace) take it exclusively, readers (Lookup) take it shared. This is
//     intentionally simple and correct; it does NOT meet the LLD §9.1
//     "lookup p99 ≤ 10µs" target under write contention. The production
//     replacement is epoch-based reclamation (TODO(perf-art-epoch)). The
//     public interface is designed so this change is purely internal.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <vector>

#include "kvcache/kv_types.h"
#include "prefix/refcount.h"

namespace kvcache::node::prefix {

// 8-byte chunk hash — see header docblock for derivation.
using ChunkHash = std::array<uint8_t, 8>;

// LeafData is what the prefix engine attaches to each sealed prefix entry.
// Pointers handed out by Lookup are valid for the lifetime of the reader's
// ReaderGuard (see below).
struct LeafData {
    kv_locator_t                locator;
    uint32_t                    tier_residency_bitmap;
    Refcount                    refcount;
    uint64_t                    sealed_at_nanos;
    std::atomic<uint64_t>       last_access_nanos;
    uint64_t                    bytes_total;

    LeafData() noexcept : locator{}, tier_residency_bitmap{0},
                          sealed_at_nanos{0}, last_access_nanos{0},
                          bytes_total{0} {}
};

// ---------------------------------------------------------------------------
// Forward declarations for the internal node types.
// ---------------------------------------------------------------------------
struct ArtNode;       // tagged union: inner or leaf
struct ArtInner256;   // MVP: only inner-node specialization
struct ArtLeaf;

// ---------------------------------------------------------------------------
// ArtIndex
// ---------------------------------------------------------------------------
class ArtIndex {
   public:
    ArtIndex();
    ~ArtIndex();

    ArtIndex(const ArtIndex&)            = delete;
    ArtIndex& operator=(const ArtIndex&) = delete;

    // ---- writer API (exclusive lock internally) ----

    enum class InsertResult {
        kInserted,        // new leaf created
        kReplaced,        // existing leaf at this exact path replaced
        kPathConflict,    // edge-label mismatch (should never happen with hashes)
    };

    // Insert or replace a sealed leaf at `path`. Ownership of the supplied
    // LeafData moves into the tree.
    InsertResult Insert(std::span<const ChunkHash> path,
                        std::unique_ptr<LeafData> leaf);

    // Remove a leaf at exactly `path`. Returns true if a leaf was removed.
    // The leaf is destroyed only after the writer lock is released — this is
    // safe because Lookup holds the shared lock while reading the pointer.
    bool Remove(std::span<const ChunkHash> path);

    // ---- reader API ----

    class ReaderGuard {
       public:
        ReaderGuard() = default;
        explicit ReaderGuard(std::shared_mutex& m) : lock_(m) {}
        // Non-copyable; movable.
        ReaderGuard(const ReaderGuard&)            = delete;
        ReaderGuard& operator=(const ReaderGuard&) = delete;
        ReaderGuard(ReaderGuard&&) noexcept            = default;
        ReaderGuard& operator=(ReaderGuard&&) noexcept = default;

       private:
        std::shared_lock<std::shared_mutex> lock_;
    };

    // Acquire a reader guard. Must outlive every pointer returned by Lookup
    // on this guard.
    ReaderGuard EnterRead() const;

    struct LookupResult {
        std::size_t matched_chunks{0};
        // Pointer to in-tree storage. Valid until the corresponding ReaderGuard
        // is destroyed. Caller MUST call leaf->refcount.Acquire() before
        // releasing the guard if they intend to hold across reads.
        LeafData*   leaf{nullptr};
    };

    // Longest-prefix-match over `path`. Returns the deepest leaf whose
    // chunk-path is a prefix of `path`, or `{0, nullptr}` if none.
    LookupResult Lookup(std::span<const ChunkHash> path,
                        const ReaderGuard&) const;

    // ---- stats ----
    std::size_t LeafCount() const noexcept;
    std::size_t NodeCount() const noexcept;

   private:
    mutable std::shared_mutex mu_;

    // Root is always an inner node. Path from root → leaf:
    //   level i:  edge labeled by ChunkHash_i (8 bytes)
    // The first byte selects the child slot; the remaining 7 are the edge
    // label stored in the child's `edge_tail` field.
    std::unique_ptr<ArtInner256> root_;

    std::size_t leaf_count_ = 0;
    std::size_t node_count_ = 0;  // inner nodes only
};

}  // namespace kvcache::node::prefix
