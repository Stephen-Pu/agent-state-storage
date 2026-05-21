// LLD §3.2 — Epoch-Based Reclamation implementation.
#include "prefix/epoch.h"

#include <cstdlib>          // std::abort
#include <unordered_map>

namespace kvcache::node::prefix {

namespace {

// Per-thread map from EpochManager* to slot index. Lets one thread interact
// with multiple EpochManagers (e.g. multiple ArtIndex instances) without
// collisions. Entries are not removed when threads exit — slots leak per
// thread, which is fine because total threads are bounded.
thread_local std::unordered_map<const EpochManager*, std::size_t>* g_tls_slots = nullptr;

}  // namespace

// ---------------------------------------------------------------------------
// EpochManager
// ---------------------------------------------------------------------------

EpochManager::EpochManager()  = default;
EpochManager::~EpochManager() {
    // Drain anything pending. If there are still active readers at this
    // point, that's an application bug; ForceReclaimAll() runs deleters
    // regardless, which is what we want during destruction.
    ForceReclaimAll();
}

EpochManager::ThreadEntry& EpochManager::MyEntry() {
    // Lazily initialise the per-thread cache map.
    if (g_tls_slots == nullptr) {
        // Static lifetime so we don't leak on thread exit; this is the
        // canonical "leak the thread-local on purpose" pattern.
        g_tls_slots = new std::unordered_map<const EpochManager*, std::size_t>();
    }
    auto it = g_tls_slots->find(this);
    if (it != g_tls_slots->end()) {
        return entries_[it->second];
    }
    // Claim a new slot.
    const std::size_t slot =
        next_slot_.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kMaxReaderThreads) {
        // Out of slots. This is an application-level mistake; aborting is
        // cleaner than silently corrupting reclamation invariants.
        std::abort();
    }
    g_tls_slots->emplace(this, slot);
    return entries_[slot];
}

void EpochManager::EnterRead() noexcept {
    auto& e = MyEntry();
    // 1. Snapshot the current global epoch.
    const uint64_t cur = global_epoch_.load(std::memory_order_seq_cst);
    // 2. Publish it. seq_cst ordering ensures every later load by this
    //    thread (i.e. the tree-walk's children.load(acquire)) is sequenced
    //    after this publish — so Reclaim() running concurrently will see
    //    this thread's witness before freeing anything we might be about
    //    to read.
    e.epoch.store(cur, std::memory_order_seq_cst);
}

void EpochManager::ExitRead() noexcept {
    auto& e = MyEntry();
    // Reset to quiescent. The fence ensures the reader's last load is
    // sequenced before this store, so Reclaim() seeing us at kQuiescent
    // genuinely means we're no longer touching the structure.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    e.epoch.store(kQuiescent, std::memory_order_seq_cst);
}

void EpochManager::Retire(std::function<void()> deleter) {
    // Bump first, then record. The bump's effect is that any reader
    // entering AFTER this point will witness the new (post-bump) epoch
    // and will not be a concern for this retire — they'll only see the
    // post-mutation tree state, no dangling ref to the retired pointer.
    const uint64_t e =
        global_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::lock_guard lk(retired_mu_);
    retired_.push_back({e, std::move(deleter)});
}

uint64_t EpochManager::MinActiveEpochLocked() const noexcept {
    uint64_t min_e = kQuiescent;
    const std::size_t claimed =
        next_slot_.load(std::memory_order_acquire);
    const std::size_t scan_n =
        claimed > kMaxReaderThreads ? kMaxReaderThreads : claimed;
    for (std::size_t i = 0; i < scan_n; ++i) {
        const uint64_t v = entries_[i].epoch.load(std::memory_order_acquire);
        if (v < min_e) min_e = v;
    }
    return min_e;
}

std::size_t EpochManager::Reclaim() {
    // Snapshot min epoch without holding retired_mu_.
    const uint64_t safe = MinActiveEpochLocked();

    std::lock_guard lk(retired_mu_);
    std::size_t freed = 0;
    // Safety condition: retire_at <= min_active. Reasoning — a reader that
    // observed `global_epoch == X` has, by the acquire/release pairing on
    // global_epoch and the children atoms, also observed every writer store
    // that preceded the bump-to-X. So a reader with witness X cannot hold
    // a stale pointer to a node retired AT epoch X (the writer stored the
    // replacement before bumping). Strict less-than would be too
    // conservative and leave items dangling forever when retire_at equals
    // the only reader's witness.
    auto last = retired_.size();
    for (std::size_t i = 0; i < last; ) {
        if (retired_[i].epoch <= safe) {
            retired_[i].deleter();
            retired_[i] = std::move(retired_[last - 1]);
            --last;
            ++freed;
        } else {
            ++i;
        }
    }
    retired_.resize(last);
    return freed;
}

std::size_t EpochManager::ForceReclaimAll() {
    std::lock_guard lk(retired_mu_);
    const std::size_t n = retired_.size();
    for (auto& r : retired_) r.deleter();
    retired_.clear();
    return n;
}

std::size_t EpochManager::PendingRetires() const noexcept {
    std::lock_guard lk(retired_mu_);
    return retired_.size();
}

std::size_t EpochManager::ActiveReaders() const noexcept {
    std::size_t n = 0;
    const std::size_t claimed = next_slot_.load(std::memory_order_acquire);
    const std::size_t scan_n =
        claimed > kMaxReaderThreads ? kMaxReaderThreads : claimed;
    for (std::size_t i = 0; i < scan_n; ++i) {
        if (entries_[i].epoch.load(std::memory_order_acquire) != kQuiescent) {
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// EpochGuard
// ---------------------------------------------------------------------------

EpochGuard::EpochGuard(EpochManager& mgr) noexcept : mgr_(&mgr) {
    mgr.EnterRead();
}

EpochGuard::~EpochGuard() {
    if (mgr_) mgr_->ExitRead();
}

EpochGuard::EpochGuard(EpochGuard&& o) noexcept : mgr_(o.mgr_) {
    o.mgr_ = nullptr;
}

EpochGuard& EpochGuard::operator=(EpochGuard&& o) noexcept {
    if (this != &o) {
        if (mgr_) mgr_->ExitRead();
        mgr_ = o.mgr_;
        o.mgr_ = nullptr;
    }
    return *this;
}

}  // namespace kvcache::node::prefix
