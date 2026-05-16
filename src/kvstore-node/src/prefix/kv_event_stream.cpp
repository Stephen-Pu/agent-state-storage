// LLD §2.2 — KV event stream.
#include "prefix/kv_event_stream.h"

#include <algorithm>
#include <chrono>

namespace kvcache::node::prefix {

namespace {

// Round up to next power of two, with a sane lower bound.
uint32_t Roundup(uint32_t v) {
    if (v < 8) return 8;
    --v;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// SubscriberRing
// ---------------------------------------------------------------------------

SubscriberRing::SubscriberRing(uint32_t capacity_pow2)
    : capacity_(Roundup(capacity_pow2)),
      mask_(capacity_ - 1),
      buf_(std::make_unique<Event[]>(capacity_)) {}

bool SubscriberRing::TryPush(const Event& ev) noexcept {
    const uint64_t h = head_.load(std::memory_order_relaxed);
    const uint64_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= capacity_) {
        overflow_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    buf_[h & mask_] = ev;
    head_.store(h + 1, std::memory_order_release);
    return true;
}

bool SubscriberRing::TryPop(Event* out) noexcept {
    const uint64_t t = tail_.load(std::memory_order_relaxed);
    const uint64_t h = head_.load(std::memory_order_acquire);
    if (t == h) return false;
    *out = buf_[t & mask_];
    tail_.store(t + 1, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// EventStream
// ---------------------------------------------------------------------------

void EventStream::Publish(const Event& ev) noexcept {
    // Stamp epoch + clock unconditionally (epoch monotonicity is the contract
    // even if the event has fields the caller pre-filled).
    Event stamped = ev;
    stamped.epoch = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (stamped.unix_nanos == 0) {
        using namespace std::chrono;
        stamped.unix_nanos = static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count());
    }

    // We hold the mutex only to safely iterate the subscriber vector. The push
    // into each per-subscriber ring is wait-free, so total time under the lock
    // is O(N_subscribers) memory stores — fine for our scale (≤ 64 subs).
    std::lock_guard lk(mu_);
    for (auto& s : subs_) {
        s.ring->TryPush(stamped);  // overflow is counted inside the ring
    }
}

SubscriberHandle EventStream::Subscribe(uint32_t ring_capacity_pow2) {
    std::lock_guard lk(mu_);
    SubscriberHandle h = next_handle_.fetch_add(1, std::memory_order_relaxed);
    subs_.push_back({h, std::make_unique<SubscriberRing>(ring_capacity_pow2)});
    return h;
}

void EventStream::Unsubscribe(SubscriberHandle h) {
    std::lock_guard lk(mu_);
    auto it = std::find_if(subs_.begin(), subs_.end(),
                           [h](const Sub& s) { return s.handle == h; });
    if (it != subs_.end()) subs_.erase(it);
}

bool EventStream::Poll(SubscriberHandle h, Event* out) {
    // We must keep the ring alive while we Pop. Hold the mutex briefly to
    // resolve the handle to a raw pointer, then release before the (lock-free)
    // pop. Unsubscribe is the only path that destroys the ring, and it takes
    // the same mutex — so the pointer stays valid as long as we don't release
    // and re-acquire between resolve and pop.
    std::lock_guard lk(mu_);
    for (auto& s : subs_) {
        if (s.handle == h) return s.ring->TryPop(out);
    }
    return false;
}

}  // namespace kvcache::node::prefix
