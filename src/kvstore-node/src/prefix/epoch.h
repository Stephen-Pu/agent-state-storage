// LLD §3.2 — Epoch-Based Reclamation (EBR) for lock-free reads.
//
// Background:
//   Lock-free data structures need a way to delay destruction of nodes that
//   were "unlinked" from the structure but might still be held by concurrent
//   readers. EBR is the canonical answer (Fraser 2004 / Linux RCU family).
//
// Mental model:
//   * A monotonic `global_epoch_` counter advances on every writer retire.
//   * Each reader thread, on EnterRead(), publishes the current global_epoch_
//     into a per-thread slot — this is its "witness".
//   * Writers publish new nodes via atomic store and push old pointers onto
//     a `retired_` list, tagged with the global_epoch_ AFTER the bump.
//   * A pointer retired at epoch X is safe to free once min(reader witnesses)
//     > X — at that point, no live reader could possibly hold it.
//
// Hot-path cost:
//   EnterRead   : 2 atomic operations + 1 seq_cst fence (~50 ns on ARM Mac)
//   Lookup load : .load(memory_order_acquire) — zero blocking
//   ExitRead    : 1 atomic store + 1 fence
//
// Per LLD §9.1 the lookup hot path budget is p99 ≤ 10µs; EBR puts us
// comfortably below that, with the lookup itself being O(K) atomic loads
// for a K-chunk key.
//
// Limits:
//   * `kMaxReaderThreads = 256` — slot pool. If your application spawns
//     more reader threads than this, the manager aborts on first EnterRead.
//     Raise if needed; cost is one cacheline per slot.
//   * Retired items are kept in a `std::vector<RetiredItem>` guarded by a
//     mutex. Reclaim() is O(retired.size()); call periodically from a
//     background thread or on every Nth retire.
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <vector>

namespace kvcache::node::prefix {

class EpochGuard;

class EpochManager {
   public:
    EpochManager();
    ~EpochManager();
    EpochManager(const EpochManager&)            = delete;
    EpochManager& operator=(const EpochManager&) = delete;

    // Maximum simultaneous reader threads this manager can track.
    static constexpr std::size_t kMaxReaderThreads = 256;

    // Sentinel published to a thread's slot when it has no active read.
    static constexpr uint64_t kQuiescent = std::numeric_limits<uint64_t>::max();

    // ---- Reader-side API (called by EpochGuard ctor / dtor) ----

    // Publish "I am reading at global_epoch_" into my slot.
    void EnterRead() noexcept;
    // Publish "I am idle" — frees future Reclaim() calls to advance past me.
    void ExitRead() noexcept;

    // ---- Writer-side API ----

    // Push a deferred destructor. The retire-epoch is recorded; the
    // destructor runs in a future Reclaim() once no reader's witness is
    // older than that epoch. Writers MUST hold their own serialization
    // mutex around Retire — this manager does not order writers.
    void Retire(std::function<void()> deleter);

    // Walk the retire list, invoke any destructor whose retire-epoch is
    // strictly less than the current min(reader witnesses). Returns the
    // number of deleters invoked.
    std::size_t Reclaim();

    // Forcefully reclaim everything regardless of reader witnesses. Only
    // safe at structure destruction time (no readers remain).
    std::size_t ForceReclaimAll();

    // ---- Observability ----

    uint64_t GlobalEpoch() const noexcept {
        return global_epoch_.load(std::memory_order_acquire);
    }
    std::size_t PendingRetires() const noexcept;
    std::size_t ActiveReaders() const noexcept;

   private:
    struct alignas(64) ThreadEntry {
        std::atomic<uint64_t> epoch{kQuiescent};
    };
    struct RetiredItem {
        uint64_t                  epoch;
        std::function<void()>     deleter;
    };

    // Get-or-allocate this thread's slot in `entries_`. First call from a
    // given (thread, manager) pair claims a slot via fetch_add on
    // `next_slot_`; subsequent calls are cached in a thread-local map.
    ThreadEntry& MyEntry();

    // Compute the smallest witness across all claimed slots. Slots in
    // kQuiescent state are skipped.
    uint64_t MinActiveEpochLocked() const noexcept;

    std::atomic<uint64_t>                  global_epoch_{1};
    std::array<ThreadEntry, kMaxReaderThreads> entries_{};
    std::atomic<std::size_t>               next_slot_{0};

    mutable std::mutex                     retired_mu_;
    std::vector<RetiredItem>               retired_;
};

// RAII reader-side guard. Construct to enter epoch protection, destruct
// to exit. Non-copyable; moveable so callers can return by value. A
// default-constructed guard is inert (mgr_ == nullptr) and is mainly
// useful as an assignment target to release the previous guard early.
class EpochGuard {
   public:
    EpochGuard() noexcept : mgr_(nullptr) {}
    explicit EpochGuard(EpochManager& mgr) noexcept;
    ~EpochGuard();

    EpochGuard(const EpochGuard&)            = delete;
    EpochGuard& operator=(const EpochGuard&) = delete;

    EpochGuard(EpochGuard&& o) noexcept;
    EpochGuard& operator=(EpochGuard&& o) noexcept;

    bool active() const noexcept { return mgr_ != nullptr; }

   private:
    EpochManager* mgr_;
};

}  // namespace kvcache::node::prefix
