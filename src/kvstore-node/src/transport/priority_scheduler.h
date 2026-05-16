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

struct WorkItem {
    WorkId   id;
    Priority priority;
    uint64_t bytes;     // size of the upcoming Pull
    void*    user;      // opaque caller payload (FetchRequest pointer, etc.)
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
    WorkId Submit(Priority p, uint64_t bytes, void* user);

    // Return the next admissible work item, or std::nullopt if no class is
    // admissible. Caller is expected to hand the result to the NIXL backend
    // and call OnComplete once the transfer finishes.
    std::optional<WorkItem> TryNext();

    // Refund bandwidth window credit. Must be called exactly once per
    // TryNext() return. Returns false if the WorkId is unknown.
    bool OnComplete(WorkId id);

    // ---- stats ----
    std::size_t QueueDepth (Priority p) const;
    uint64_t    InFlightBytes(Priority p) const;
    uint64_t    Reserved(Priority p) const noexcept { return reserved_[idx(p)]; }
    uint64_t    Skips(Priority p) const;

   private:
    struct Class {
        std::deque<WorkItem> queue;
        uint64_t             in_flight = 0;
        uint64_t             reservation = 0;
        uint32_t             consecutive_skips = 0;
    };

    static constexpr std::size_t idx(Priority p) {
        return static_cast<std::size_t>(p);
    }

    // Internal admission decision for `c` given its reservation and the
    // starvation watchdog. Caller holds mu_.
    bool AdmissibleLocked(const Class& c, const WorkItem& w) const;

    mutable std::mutex     mu_;
    Class                  classes_[kNumPriorities];
    uint64_t               reserved_[kNumPriorities]{};
    uint32_t               max_starvation_skips_ = 50;
    std::atomic<WorkId>    next_id_{1};

    std::unordered_map<WorkId, Priority> id_to_class_;
    std::unordered_map<WorkId, uint64_t> id_to_bytes_;
};

}  // namespace kvcache::node::transport
