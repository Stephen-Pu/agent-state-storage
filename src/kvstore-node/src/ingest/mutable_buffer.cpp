// LLD §3.4 — Mutable buffer pool implementation.
#include "ingest/mutable_buffer.h"

namespace kvcache::node::ingest {

MutableBufferPool::MutableBufferPool(tier::TierManager* tm, const Options& opts)
    : tm_(tm), opts_(opts) {
    if (opts_.start_sweeper) {
        sweeper_ = std::thread([this] { SweeperLoop(); });
    }
}

MutableBufferPool::~MutableBufferPool() { StopSweeper(); }

void MutableBufferPool::StopSweeper() {
    if (sweeper_.joinable()) {
        stop_.store(true, std::memory_order_release);
        sweeper_.join();
    }
}

IngestHandle MutableBufferPool::Reserve(std::chrono::seconds ttl) {
    auto slot = tm_->AcquirePinnedSlot();
    if (!slot) return kInvalidIngestHandle;
    const IngestHandle h = next_handle_.fetch_add(1, std::memory_order_relaxed);
    const auto deadline  = std::chrono::steady_clock::now() +
        (ttl.count() ? ttl : opts_.default_ttl);
    std::lock_guard lk(mu_);
    active_[h] = Entry{*slot, deadline};
    return h;
}

std::optional<IngestSlot> MutableBufferPool::GetSlot(IngestHandle h) const {
    std::lock_guard lk(mu_);
    auto it = active_.find(h);
    if (it == active_.end()) return std::nullopt;
    IngestSlot s{};
    s.addr   = it->second.slot.addr;
    s.bytes  = it->second.slot.bytes;
    s.mr_key = it->second.slot.mr_key;
    return s;
}

bool MutableBufferPool::Release(IngestHandle h) {
    tier::SlotId id;
    {
        std::lock_guard lk(mu_);
        auto it = active_.find(h);
        if (it == active_.end()) return false;
        id = it->second.slot.id;
        active_.erase(it);
    }
    tm_->ReleasePinnedSlot(id);
    return true;
}

std::size_t MutableBufferPool::ActiveCount() const noexcept {
    std::lock_guard lk(mu_);
    return active_.size();
}

void MutableBufferPool::SweepOnce() {
    std::vector<tier::SlotId> to_free;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lk(mu_);
        for (auto it = active_.begin(); it != active_.end(); ) {
            if (it->second.deadline <= now) {
                to_free.push_back(it->second.slot.id);
                it = active_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto id : to_free) tm_->ReleasePinnedSlot(id);
}

void MutableBufferPool::SweeperLoop() {
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(opts_.sweep_interval);
        if (stop_.load(std::memory_order_acquire)) break;
        SweepOnce();
    }
}

}  // namespace kvcache::node::ingest
