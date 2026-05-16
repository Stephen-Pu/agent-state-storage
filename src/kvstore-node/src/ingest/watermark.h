// LLD §3.4 — Watermark progression for streaming writes.
//
// Per-handle monotonic byte offset. The engine advances the watermark via
// kv_publish; the priority scheduler reads it to decide when there's enough
// fresh data to issue a NIXL Pull (LLD §3.5).
//
// Concurrency:
//   * Producer: single engine thread per handle calls Publish.
//   * Consumer: the scheduler calls Read on any thread.
//   * The shared map is protected by a mutex; per-handle reads/writes go
//     through an atomic uint64 so neither side needs to hold the mutex while
//     advancing the counter on the hot path.
//
// Eviction: handles must be removed via Drop when the streaming session is
// sealed or aborted, mirroring MutableBufferPool::Release.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "ingest/mutable_buffer.h"

namespace kvcache::node::ingest {

class WatermarkTracker {
   public:
    WatermarkTracker()  = default;
    ~WatermarkTracker() = default;
    WatermarkTracker(const WatermarkTracker&)            = delete;
    WatermarkTracker& operator=(const WatermarkTracker&) = delete;

    // Begin tracking a handle. No-op if already tracked.
    void Track(IngestHandle h);

    // Advance the watermark. Refuses to regress. Returns the new value
    // (or 0 if the handle is unknown).
    uint64_t Publish(IngestHandle h, uint64_t new_watermark);

    // Current watermark, or 0 if untracked.
    uint64_t Read(IngestHandle h) const;

    // Stop tracking. Returns the final watermark, or 0 if untracked.
    uint64_t Drop(IngestHandle h);

    std::size_t Size() const noexcept;

   private:
    struct Slot {
        std::atomic<uint64_t> wm{0};
    };
    mutable std::mutex mu_;
    std::unordered_map<IngestHandle, std::unique_ptr<Slot>> map_;
};

}  // namespace kvcache::node::ingest
