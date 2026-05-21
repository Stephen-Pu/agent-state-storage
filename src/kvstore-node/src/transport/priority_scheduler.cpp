// LLD §5.1 — Priority Scheduler.
#include "transport/priority_scheduler.h"

#include <array>
#include <string_view>

#include "metrics.h"

namespace kvcache::node::transport {

namespace {

// Lazily register the per-process metrics on first PriorityScheduler ctor.
// All instances share these series; the priority label distinguishes them.
struct SchedulerMetrics {
    kvcache::metrics::Counter* admissions_normal = nullptr;
    kvcache::metrics::Counter* admissions_forced = nullptr;
    kvcache::metrics::Gauge*   queue_depth       = nullptr;
    kvcache::metrics::Gauge*   in_flight_bytes   = nullptr;
};

SchedulerMetrics& Sm() {
    static SchedulerMetrics m = [] {
        SchedulerMetrics x;
        auto& r = kvcache::metrics::Registry::Default();
        static constexpr std::string_view kPrio[] = {"priority"};
        x.admissions_normal = &r.GetOrCreateCounter(
            "kv_sched_admissions_total",
            "Work items admitted to NIXL via the priority scheduler.",
            kPrio);
        x.admissions_forced = &r.GetOrCreateCounter(
            "kv_sched_forced_admissions_total",
            "Work items admitted via the starvation-override path.",
            kPrio);
        x.queue_depth = &r.GetOrCreateGauge(
            "kv_sched_queue_depth",
            "Number of work items currently queued per priority class.",
            kPrio);
        x.in_flight_bytes = &r.GetOrCreateGauge(
            "kv_sched_in_flight_bytes",
            "Bytes of currently in-flight Pulls per priority class.",
            kPrio);
        return x;
    }();
    return m;
}

const char* PriorityLabel(Priority p) {
    switch (p) {
        case Priority::P0: return "P0";
        case Priority::P1: return "P1";
        case Priority::P2: return "P2";
    }
    return "?";
}

}  // namespace

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
    // Touch the metrics block so the series exist by the time anyone scrapes.
    (void)Sm();
}

WorkId PriorityScheduler::Submit(Priority p, uint64_t bytes, void* user) {
    return Submit(p, kSystemTenantHash, bytes, user);
}

WorkId PriorityScheduler::Submit(Priority p, uint64_t tenant_hash,
                                  uint64_t bytes, void* user) {
    const WorkId id = next_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(mu_);
    auto& c = classes_[idx(p)];
    auto& q = c.tenant_queues[tenant_hash];
    if (q.empty()) {
        // First item for this tenant in this class → join the RR rotation
        // at the back so existing tenants finish their current slot first.
        c.rr_order.push_back(tenant_hash);
    }
    q.push_back(WorkItem{id, p, bytes, tenant_hash, user});
    ++c.total_depth;
    id_to_class_[id] = p;
    id_to_bytes_[id] = bytes;

    const kvcache::metrics::Label lbl[] = {
        {"priority", PriorityLabel(p)}};
    Sm().queue_depth->Set(static_cast<double>(c.total_depth), lbl);
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

std::optional<WorkItem> PriorityScheduler::TryDequeueClassLocked(Class& c) {
    if (c.rr_order.empty()) {
        c.consecutive_skips = 0;
        return std::nullopt;
    }

    // Walk the RR rotation in order. The first tenant whose head-of-queue
    // item is admissible wins. If none are admissible, the whole class is
    // skipped (and we bump consecutive_skips so the starvation override
    // can eventually trigger).
    //
    // Bounded by tenant_count so we never loop forever.
    const std::size_t tenants = c.rr_order.size();
    for (std::size_t i = 0; i < tenants; ++i) {
        const uint64_t t = c.rr_order.front();
        auto it = c.tenant_queues.find(t);
        // The rr_order entry should always have a non-empty queue, but
        // guard defensively — a missing entry just gets dropped.
        if (it == c.tenant_queues.end() || it->second.empty()) {
            c.rr_order.pop_front();
            continue;
        }
        const WorkItem& head = it->second.front();
        if (!AdmissibleLocked(c, head)) {
            // Move this tenant to the back so the next tenant gets a chance
            // (work-conserving: a blocked huge item from tenant A doesn't
            // starve a small admissible item from tenant B).
            c.rr_order.pop_front();
            c.rr_order.push_back(t);
            continue;
        }
        WorkItem w = head;
        const bool forced =
            c.in_flight + w.bytes > c.reservation &&
            c.consecutive_skips >= max_starvation_skips_;

        it->second.pop_front();
        --c.total_depth;
        if (it->second.empty()) {
            // Tenant ran dry → drop it from the rotation entirely.
            c.tenant_queues.erase(it);
            c.rr_order.pop_front();
        } else {
            // Rotate the dequeued tenant to the back.
            c.rr_order.pop_front();
            c.rr_order.push_back(t);
        }
        c.in_flight += w.bytes;
        c.consecutive_skips = 0;

        const kvcache::metrics::Label lbl[] = {
            {"priority", PriorityLabel(w.priority)}};
        if (forced) {
            forced_admissions_.fetch_add(1, std::memory_order_relaxed);
            Sm().admissions_forced->Inc(1.0, lbl);
        } else {
            normal_admissions_.fetch_add(1, std::memory_order_relaxed);
            Sm().admissions_normal->Inc(1.0, lbl);
        }
        return w;
    }

    // Nothing in this class admitted on this attempt.
    ++c.consecutive_skips;
    return std::nullopt;
}

std::optional<WorkItem> PriorityScheduler::TryNext() {
    std::lock_guard lk(mu_);

    // Walk classes in priority order; first class that admits wins.
    for (std::size_t i = 0; i < kNumPriorities; ++i) {
        auto& c = classes_[i];
        if (auto w = TryDequeueClassLocked(c)) {
            const kvcache::metrics::Label lbl[] = {
                {"priority", PriorityLabel(w->priority)}};
            Sm().queue_depth    ->Set(static_cast<double>(c.total_depth), lbl);
            Sm().in_flight_bytes->Set(static_cast<double>(c.in_flight),   lbl);
            return w;
        }
    }
    return std::nullopt;
}

bool PriorityScheduler::OnComplete(WorkId id) {
    std::lock_guard lk(mu_);
    auto it_p = id_to_class_.find(id);
    auto it_b = id_to_bytes_.find(id);
    if (it_p == id_to_class_.end() || it_b == id_to_bytes_.end()) return false;
    auto& c = classes_[idx(it_p->second)];
    c.in_flight -= it_b->second;
    const Priority p = it_p->second;
    id_to_class_.erase(it_p);
    id_to_bytes_.erase(it_b);

    const kvcache::metrics::Label lbl[] = {
        {"priority", PriorityLabel(p)}};
    Sm().in_flight_bytes->Set(static_cast<double>(c.in_flight), lbl);
    return true;
}

bool PriorityScheduler::HasWork() const {
    std::lock_guard lk(mu_);
    for (std::size_t i = 0; i < kNumPriorities; ++i) {
        if (classes_[i].total_depth > 0) return true;
    }
    return false;
}

std::size_t PriorityScheduler::QueueDepth(Priority p) const {
    std::lock_guard lk(mu_);
    return classes_[idx(p)].total_depth;
}

uint64_t PriorityScheduler::InFlightBytes(Priority p) const {
    std::lock_guard lk(mu_);
    return classes_[idx(p)].in_flight;
}

uint64_t PriorityScheduler::Skips(Priority p) const {
    std::lock_guard lk(mu_);
    return classes_[idx(p)].consecutive_skips;
}

std::size_t PriorityScheduler::TenantCount(Priority p) const {
    std::lock_guard lk(mu_);
    return classes_[idx(p)].tenant_queues.size();
}

}  // namespace kvcache::node::transport
