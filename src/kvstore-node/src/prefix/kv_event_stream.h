// LLD §2.2 — In-node KV event stream.
//
// One producer (the seal/evict/promote/demote path) and N consumers (gRPC
// Subscribe stream handlers, the local bloom-sketch updater, internal metrics
// listeners). Each subscriber gets its own bounded ring; if a slow consumer
// fills its ring it sees an OVERFLOW signal but the producer is never blocked.
//
// Lock-free property: the producer call (Publish) is wait-free across all
// subscribers. Subscriber registration / removal uses a mutex; this happens at
// connection-establishment frequency, not on the hot path.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "kvcache/kv_types.h"

namespace kvcache::node::prefix {

// Subscriber-visible event. Equivalent to the proto `Event` and the C ABI
// `kv_event_t`; we redefine here to keep this module free of proto deps.
enum class EventType : uint8_t {
    Add     = 1,
    Evict   = 2,
    Promote = 3,
    Demote  = 4,
};

enum class Tier : uint8_t {
    Unspecified = 0,
    Hbm         = 1,
    Pinned      = 2,
    Dram        = 3,
    Nvme        = 4,
    Cold        = 5,
};

struct Event {
    EventType    type;
    Tier         tier;
    kv_locator_t locator;
    uint64_t     epoch;        // monotonic, per-node
    uint64_t     unix_nanos;
};

// ---------------------------------------------------------------------------
// SubscriberRing — bounded SPSC ring (single producer = EventStream itself,
// single consumer = one subscriber thread). Capacity must be a power of two.
// ---------------------------------------------------------------------------
class SubscriberRing {
   public:
    explicit SubscriberRing(uint32_t capacity_pow2);

    // Producer side. Returns false if ring is full; caller decides whether to
    // drop, count an overflow, or apply backpressure (EventStream drops).
    bool TryPush(const Event& ev) noexcept;

    // Consumer side. Returns false if empty.
    bool TryPop(Event* out) noexcept;

    // Number of events the producer attempted to push while full, since
    // creation. Used by the metric kv_event_stream_overflow_total.
    uint64_t Overflow() const noexcept {
        return overflow_.load(std::memory_order_relaxed);
    }

    uint32_t Capacity() const noexcept { return capacity_; }

   private:
    const uint32_t capacity_;
    const uint32_t mask_;
    std::unique_ptr<Event[]> buf_;
    alignas(64) std::atomic<uint64_t> head_{0};  // producer writes
    alignas(64) std::atomic<uint64_t> tail_{0};  // consumer reads
    alignas(64) std::atomic<uint64_t> overflow_{0};
};

using SubscriberHandle = uint64_t;

// ---------------------------------------------------------------------------
// EventStream — node-wide fan-out.
// ---------------------------------------------------------------------------
class EventStream {
   public:
    // Publish from any producer thread. Wait-free w.r.t. subscriber count.
    void Publish(const Event& ev) noexcept;

    // Register a subscriber. The returned handle owns the ring; the subscriber
    // thread polls via Poll(handle, out).
    SubscriberHandle Subscribe(uint32_t ring_capacity_pow2 = 4096);
    void Unsubscribe(SubscriberHandle h);
    bool Poll(SubscriberHandle h, Event* out);

    // Current monotonic node epoch (next event's epoch == NodeEpoch()+1).
    uint64_t NodeEpoch() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }

   private:
    struct Sub {
        SubscriberHandle handle;
        std::unique_ptr<SubscriberRing> ring;
    };

    mutable std::mutex mu_;
    std::vector<Sub>   subs_;
    std::atomic<SubscriberHandle> next_handle_{1};
    std::atomic<uint64_t>         epoch_{0};
};

}  // namespace kvcache::node::prefix
