// LLD §4.2 — Bloom-sketch view of cluster prefix presence.
//
// Each node maintains a local Bloom filter over the chunk-keys it holds.
// Every 30 s the local sketch is shipped to the CP (LLD §4.2), and a merged
// per-tenant aggregated sketch is pushed back to every node.
//
// This module provides:
//   * `LocalBloom`   — the per-node, locally-updated filter.
//   * `AggregatedBloom` — the cluster view (bit-wise OR of peer sketches).
//   * Helpers to serialize / deserialize the wire form (matches the
//     `BloomSketchRequest`/`BloomSketchResponse` proto in node.proto).
//
// The actual periodic ship/refresh loop is driven by the cluster layer
// (membership/CP sync); this module is purely the data structure and math.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace kvcache::node::routing {

// Compute (m, k) for target false-positive rate p with capacity n.
// Closed-form: m = -n*ln(p) / (ln 2)^2,  k = (m/n)*ln 2.
struct BloomParams {
    uint32_t m_bits;
    uint32_t k_hashes;
    static BloomParams ForCapacity(uint64_t expected_n, double target_fpr);
};

class LocalBloom {
   public:
    explicit LocalBloom(BloomParams params);

    BloomParams Params() const noexcept { return params_; }
    uint64_t    InsertCount() const noexcept {
        return inserts_.load(std::memory_order_relaxed);
    }

    // Insert / lookup keyed on a raw byte sequence (typically the 40B
    // "identity" of a sealed chunk: tenant|model|prefix).
    void Add(std::span<const uint8_t> key);
    bool MaybeContains(std::span<const uint8_t> key) const;

    // Snapshot for shipping to the CP. The returned bytes are owned by the
    // caller; the snapshot is internally consistent (single atomic copy).
    std::vector<uint8_t> Snapshot() const;

    // Reset (used after CP confirms the sketch has been collected and a new
    // 30s window starts).
    void Reset();

   private:
    void SetBit(uint64_t idx);
    bool TestBit(uint64_t idx) const;

    BloomParams                    params_;
    mutable std::mutex             mu_;
    std::vector<uint64_t>          words_;  // m_bits / 64 words
    std::atomic<uint64_t>          inserts_{0};
};

// AggregatedBloom is the merged view across N peer sketches. The merge is a
// pure bit-wise OR, so an "I might have it" answer from any peer surfaces in
// the aggregate. False positives compound; the CP is expected to keep the
// per-tenant aggregate within its sizing budget.
class AggregatedBloom {
   public:
    AggregatedBloom();
    explicit AggregatedBloom(BloomParams params);

    // Replace the underlying bits / params. Thread-safe.
    void Set(BloomParams params, std::vector<uint8_t> raw);

    bool MaybeContains(std::span<const uint8_t> key) const;
    BloomParams Params() const;

   private:
    mutable std::mutex     mu_;
    BloomParams            params_{0, 0};
    std::vector<uint64_t>  words_;
};

}  // namespace kvcache::node::routing
