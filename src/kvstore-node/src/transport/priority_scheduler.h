// LLD §5.1 — Priority Scheduler that sits between the node's request planner
// and the NIXL backend.
//
// Three priority classes (LLD §5.1):
//   P0 (latency-critical) — reserved 20 % of the per-node bandwidth window
//   P1 (default)          — reserved 75 %
//   P2 (best-effort)      — reserved  5 %
//
// Admission rule (window-based):
//   A class is admitted iff
//     bytes_in_flight[c] + work.bytes <= reservation[c]
//   When a class has unused budget, idle credit is loaned to lower classes
//   (P0 → P1 → P2) so the link never sits idle. The loan is automatically
//   revoked when the higher class submits.
//
// Anti-starvation (LLD §5.1):
//   If a class has work but has been skipped over `max_starvation_skips`
//   dispatch attempts, it is force-admitted on the next attempt — even when
//   it would otherwise exceed its reservation. This bounds tail latency for
//   P2 when P0/P1 are saturating the link.
//
// Per-tenant fairness within a class (Phase E-1):
//   Each class holds one FIFO per `tenant_hash` and visits tenants in a
//   round-robin order. `TryNext` is work-conserving: if the current tenant's
//   head-of-queue item is too big to admit, the scheduler tries the next
//   tenant's head before giving up on the class. This stops one tenant's
//   huge transfer from starving smaller transfers in the same class.
//
// The scheduler does NOT own threads. Callers submit work and then drive
// TryNext() in their own dispatch loop. This keeps the scheduler easy to
// reason about and easy to test.
//
// Future work: token-bucket QPS limit per tenant (LLD §5.1) is layered on top
// of this scheduler by the qos/ module — TryNext only enforces bandwidth.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace kvcache::node::transport {

using WorkId = uint64_t;

enum class Priority : uint8_t {
    P0 = 0,
    P1 = 1,
    P2 = 2,
};

inline constexpr std::size_t kNumPriorities = 3;

// Conventional "no tenant / system traffic" hash. Submit overloads without an
// explicit tenant fall back to this; tests written before Phase E-1 keep
// working unchanged because every Submit collapses to the same bucket.
inline constexpr uint64_t kSystemTenantHash = 0;

struct WorkItem {
    WorkId   id;
    Priority priority;
    uint64_t bytes;        // size of the upcoming Pull
    uint64_t tenant_hash;  // 16-byte UUID hashed to u64; 0 = system
    void*    user;         // opaque caller payload (FetchRequest pointer, etc.)
};

class PriorityScheduler {
   public:
    struct Options {
        // The product of "bandwidth window seconds" and node NIC bandwidth.
        // The exact value matters less than the ratios; LLD §5.1 picks 1s.
        uint64_t total_window_bytes      = 1ull << 30;  // 1 GiB / window
        uint32_t p0_pct                  = 20;
        uint32_t p1_pct                  = 75;
        uint32_t p2_pct                  = 5;
        uint32_t max_starvation_skips    = 50;
    };

    explicit PriorityScheduler(const Options& opts);

    // Enqueue work and return its id. Always succeeds (queues are unbounded
    // here — the qos/ layer enforces per-tenant queue depth caps).
    //
    // The 3-arg overload preserves the pre-Phase-E-1 API; it bundles all
    // traffic under `kSystemTenantHash`. New callers should pass the tenant
    // hash explicitly so per-tenant fairness kicks in.
    WorkId Submit(Priority p, uint64_t bytes, void* user);
    WorkId Submit(Priority p, uint64_t tenant_hash, uint64_t bytes, void* user);

    // Return the next admissible work item, or std::nullopt if no class is
    // admissible. Caller is expected to hand the result to the NIXL backend
    // and call OnComplete once the transfer finishes.
    std::optional<WorkItem> TryNext();

    // Refund bandwidth window credit. Must be called exactly once per
    // TryNext() return. Returns false if the WorkId is unknown.
    bool OnComplete(WorkId id);

    // True iff some class has at least one queued work item. Cheap O(K)
    // (K=3) scan under the scheduler mutex. Used by drivers that want to
    // wait for arriving work before polling TryNext.
    bool HasWork() const;

    // ---- stats (point-in-time snapshots) ----
    std::size_t QueueDepth   (Priority p) const;
    uint64_t    InFlightBytes(Priority p) const;
    uint64_t    Reserved     (Priority p) const noexcept {
        return reserved_[idx(p)];
    }
    uint64_t    Skips        (Priority p) const;
    // Number of tenants currently holding queued work in `p`.
    std::size_t TenantCount  (Priority p) const;
    // Total forced admissions across all classes since construction.
    uint64_t    ForcedAdmissions() const noexcept {
        return forced_admissions_.load(std::memory_order_relaxed);
    }
    // Total admissions across all classes (excluding starvation overrides).
    uint64_t    NormalAdmissions() const noexcept {
        return normal_admissions_.load(std::memory_order_relaxed);
    }

   private:
    struct Class {
        // One FIFO per tenant_hash + a round-robin order of tenants that
        // currently hold work. `rr_order.front()` is the tenant whose head
        // item TryNext considers first.
        std::unordered_map<uint64_t, std::deque<WorkItem>> tenant_queues;
        std::deque<uint64_t> rr_order;
        std::size_t          total_depth       = 0;
        uint64_t             in_flight         = 0;
        uint64_t             reservation       = 0;
        uint32_t             consecutive_skips = 0;
    };

    static constexpr std::size_t idx(Priority p) {
        return static_cast<std::size_t>(p);
    }

    // Internal admission decision for `c` given its reservation and the
    // starvation watchdog. Caller holds mu_.
    bool AdmissibleLocked(const Class& c, const WorkItem& w) const;

    // Try to dequeue from class `c` under per-tenant round-robin, work-
    // conserving on per-item admission failures. Returns the dequeued item
    // (and increments c.in_flight); or nullopt if the class is skipped
    // (and bumps c.consecutive_skips). Caller holds mu_.
    std::optional<WorkItem> TryDequeueClassLocked(Class& c);

    mutable std::mutex     mu_;
    Class                  classes_[kNumPriorities];
    uint64_t               reserved_[kNumPriorities]{};
    uint32_t               max_starvation_skips_ = 50;
    std::atomic<WorkId>    next_id_{1};

    std::unordered_map<WorkId, Priority> id_to_class_;
    std::unordered_map<WorkId, uint64_t> id_to_bytes_;

    // Metrics counters (lock-free atomics so stats reads stay cheap).
    std::atomic<uint64_t> normal_admissions_{0};
    std::atomic<uint64_t> forced_admissions_{0};
};

}  // namespace kvcache::node::transport
