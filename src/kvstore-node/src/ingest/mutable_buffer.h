// LLD §3.4 — Mutable buffer pool for streaming write.
//
// One `IngestHandle` per in-flight streaming write. Each handle owns:
//   * a Pinned tier slot (acquired at Reserve)
//   * a watermark (managed by ingest/watermark.h)
//   * a TTL deadline; the sweeper drops unsealed handles after the TTL
//
// Lifecycle:
//
//   Reserve  ──▶ active ──Seal──▶ sealed (handle remains valid until Release)
//                  │
//                  └──TTL expiry──▶ aborted (slot released; bytes discarded)
//
// MVP simplifications:
//   * No CrossNUMA placement — one PinnedTier pool, one NUMA domain.
//   * No partial-seal — each handle seals exactly one ART leaf.
//   * The sweeper is a thread on the manager; it scans active handles every
//     `sweep_interval_seconds` and frees expired entries.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "tier/tier_manager.h"

namespace kvcache::node::ingest {

using IngestHandle = uint64_t;
inline constexpr IngestHandle kInvalidIngestHandle = 0;

struct IngestSlot {
    void*    addr     = nullptr;
    uint64_t bytes    = 0;
    uint32_t mr_key   = 0;
};

class MutableBufferPool {
   public:
    struct Options {
        std::chrono::seconds default_ttl       {300};   // 5 min, LLD §3.4
        std::chrono::seconds sweep_interval    {30};
        bool                 start_sweeper     = true;
    };

    MutableBufferPool(tier::TierManager* tm, const Options& opts);
    ~MutableBufferPool();
    MutableBufferPool(const MutableBufferPool&)            = delete;
    MutableBufferPool& operator=(const MutableBufferPool&) = delete;

    // Reserve a new ingest handle. Returns kInvalidIngestHandle if the
    // pinned-tier pool is exhausted.
    IngestHandle Reserve(std::chrono::seconds ttl = std::chrono::seconds::zero());

    // Look up the slot for an outstanding handle. Returns nullopt if the
    // handle is unknown (released, aborted, or never issued).
    std::optional<IngestSlot> GetSlot(IngestHandle h) const;

    // Caller-side abort or post-seal release. Returns true if the handle was
    // active and is now gone.
    bool Release(IngestHandle h);

    // Pause / resume the background sweeper (tests use this).
    void StopSweeper();
    void SweepOnce();  // visible for tests

    std::size_t ActiveCount() const noexcept;

   private:
    struct Entry {
        tier::SlotDesc slot;
        std::chrono::steady_clock::time_point deadline;
    };

    void SweeperLoop();

    tier::TierManager*   tm_;
    Options              opts_;
    mutable std::mutex   mu_;
    std::unordered_map<IngestHandle, Entry> active_;
    std::atomic<IngestHandle> next_handle_{1};

    std::atomic<bool>    stop_{false};
    std::thread          sweeper_;
};

}  // namespace kvcache::node::ingest
