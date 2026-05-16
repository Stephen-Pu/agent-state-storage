// LLD §3.4 — WatermarkTracker.
#include "ingest/watermark.h"

namespace kvcache::node::ingest {

void WatermarkTracker::Track(IngestHandle h) {
    std::lock_guard lk(mu_);
    map_.try_emplace(h, std::make_unique<Slot>());
}

uint64_t WatermarkTracker::Publish(IngestHandle h, uint64_t new_wm) {
    Slot* s = nullptr;
    {
        std::lock_guard lk(mu_);
        auto it = map_.find(h);
        if (it == map_.end()) return 0;
        s = it->second.get();
    }
    // CAS-loop so concurrent producers (rare; engine is single-writer-per-
    // handle in practice) can never regress the watermark.
    uint64_t cur = s->wm.load(std::memory_order_acquire);
    while (new_wm > cur) {
        if (s->wm.compare_exchange_weak(cur, new_wm,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            return new_wm;
        }
    }
    return cur;
}

uint64_t WatermarkTracker::Read(IngestHandle h) const {
    std::lock_guard lk(mu_);
    auto it = map_.find(h);
    if (it == map_.end()) return 0;
    return it->second->wm.load(std::memory_order_acquire);
}

uint64_t WatermarkTracker::Drop(IngestHandle h) {
    std::lock_guard lk(mu_);
    auto it = map_.find(h);
    if (it == map_.end()) return 0;
    const uint64_t final_wm = it->second->wm.load(std::memory_order_acquire);
    map_.erase(it);
    return final_wm;
}

std::size_t WatermarkTracker::Size() const noexcept {
    std::lock_guard lk(mu_);
    return map_.size();
}

}  // namespace kvcache::node::ingest
