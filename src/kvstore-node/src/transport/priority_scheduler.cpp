// LLD §5.1 — Priority Scheduler.
#include "transport/priority_scheduler.h"

namespace kvcache::node::transport {

PriorityScheduler::PriorityScheduler(const Options& opts) {
    max_starvation_skips_ = opts.max_starvation_skips;
    // Normalize percentages defensively — if the caller passes anything that
    // doesn't sum to 100, we scale to match the configured total.
    const uint64_t total = opts.total_window_bytes;
    const uint64_t sum   = opts.p0_pct + opts.p1_pct + opts.p2_pct;
    const uint64_t denom = sum == 0 ? 100 : sum;
    reserved_[idx(Priority::P0)] = total * opts.p0_pct / denom;
    reserved_[idx(Priority::P1)] = total * opts.p1_pct / denom;
    reserved_[idx(Priority::P2)] = total * opts.p2_pct / denom;
    for (std::size_t i = 0; i < kNumPriorities; ++i) {
        classes_[i].reservation = reserved_[i];
    }
}

WorkId PriorityScheduler::Submit(Priority p, uint64_t bytes, void* user) {
    const WorkId id = next_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(mu_);
    classes_[idx(p)].queue.push_back(WorkItem{id, p, bytes, user});
    id_to_class_[id] = p;
    id_to_bytes_[id] = bytes;
    return id;
}

bool PriorityScheduler::AdmissibleLocked(const Class& c, const WorkItem& w) const {
    if (c.in_flight + w.bytes <= c.reservation) return true;
    // Starvation override: this class has been skipped for too long.
    if (c.consecutive_skips >= max_starvation_skips_) return true;
    // Idle-credit loan: if higher-priority classes are not using their
    // reservation, lend their unused share down. This is the "P0 unused → P1
    // → P2" loan path. We only loan downward (a higher class cannot borrow
    // from a lower one) so high-priority workloads always have first claim.
    uint64_t loan = 0;
    for (std::size_t i = 0; i < kNumPriorities; ++i) {
        if (i < idx(w.priority)) {
            const auto& h = classes_[i];
            if (h.reservation > h.in_flight) {
                loan += h.reservation - h.in_flight;
            }
        }
    }
    return c.in_flight + w.bytes <= c.reservation + loan;
}

std::optional<WorkItem> PriorityScheduler::TryNext() {
    std::lock_guard lk(mu_);

    // Walk classes in priority order; first admissible non-empty class wins.
    for (std::size_t i = 0; i < kNumPriorities; ++i) {
        auto& c = classes_[i];
        if (c.queue.empty()) {
            c.consecutive_skips = 0;
            continue;
        }
        const auto& head = c.queue.front();
        if (!AdmissibleLocked(c, head)) {
            ++c.consecutive_skips;
            continue;
        }
        WorkItem w = head;
        c.queue.pop_front();
        c.in_flight += w.bytes;
        c.consecutive_skips = 0;
        return w;
    }
    return std::nullopt;
}

bool PriorityScheduler::OnComplete(WorkId id) {
    std::lock_guard lk(mu_);
    auto it_p = id_to_class_.find(id);
    auto it_b = id_to_bytes_.find(id);
    if (it_p == id_to_class_.end() || it_b == id_to_bytes_.end()) return false;
    classes_[idx(it_p->second)].in_flight -= it_b->second;
    id_to_class_.erase(it_p);
    id_to_bytes_.erase(it_b);
    return true;
}

std::size_t PriorityScheduler::QueueDepth(Priority p) const {
    std::lock_guard lk(mu_);
    return classes_[idx(p)].queue.size();
}

uint64_t PriorityScheduler::InFlightBytes(Priority p) const {
    std::lock_guard lk(mu_);
    return classes_[idx(p)].in_flight;
}

uint64_t PriorityScheduler::Skips(Priority p) const {
    std::lock_guard lk(mu_);
    return classes_[idx(p)].consecutive_skips;
}

}  // namespace kvcache::node::transport
