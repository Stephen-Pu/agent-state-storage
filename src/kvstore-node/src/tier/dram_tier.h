// LLD §3.3 T2 — Pageable DRAM tier with 2Q replacement and Ghost Cache.
//
// The 2Q algorithm (Johnson & Shasha 1994) prevents the LRU pollution that
// happens when a one-time scan touches every entry. It maintains three queues:
//
//   A1in   : FIFO of newly-admitted entries (probationary).
//   A1out  : "Ghost" key-only FIFO of items evicted from A1in. Lets us detect
//            second-touch within a window without keeping data around.
//   Am     : LRU of "trusted" entries that hit while in A1out.
//
// Admission rules (LLD §3.3):
//   * On a hit in A1in : do not promote.
//   * On a hit in Am   : move to head of Am (LRU promotion).
//   * On a hit in A1out: promote to Am head; the data is re-fetched.
//   * On a miss        : admit to A1in head.
//
// Sizing defaults (per the 2Q paper):
//   A1in  = 25% of total capacity
//   A1out = 50% of total entry count (key-only)
//   Am    = 75% of total capacity
//
// Concurrency: the MVP uses a single mutex for all three queues. Phase-2 may
// shard by key-hash for write-heavy workloads.
//
// Note on data storage: this tier owns std::vector<uint8_t> blobs keyed by a
// 16-byte identity (tenant|model|prefix). Higher-level callers (TierManager)
// translate between Locator and Key.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "value_policy.h"  // SS-2 spine spike, Task 5 — evict seam (ValuePolicy)

namespace kvcache::node::tier {

// 16-byte content-address key. Higher-level callers compute this from the
// SealedChunkKey (LLD §2.3). We don't depend on the meta/ module here to keep
// the tier reusable.
struct DramKey {
    std::array<uint8_t, 16> bytes{};
    bool operator==(const DramKey& o) const noexcept { return bytes == o.bytes; }
};

struct DramKeyHash {
    std::size_t operator()(const DramKey& k) const noexcept {
        // FNV-1a; replace with BLAKE3-64 once vendored (Step 1 facade ready).
        uint64_t h = 0xcbf29ce484222325ULL;
        for (auto b : k.bytes) { h ^= b; h *= 0x100000001b3ULL; }
        return static_cast<std::size_t>(h);
    }
};

class DramTier {
   public:
    // Phase G-1 — called synchronously inside EvictToFit when bytes get
    // dropped from A1in or Am. The DramTier mutex is HELD during the
    // call, so the callback must NOT call back into the tier (Insert /
    // Lookup will deadlock) — its job is to update upstream state (ART
    // leaf removal, KV_EVENT_EVICT publication) only.
    using OnEvictFn = std::function<void(const DramKey&)>;

    struct Options {
        uint64_t  capacity_bytes      = 16ull << 30;  // 16 GiB default
        uint32_t  a1out_max_entries   = 200000;       // ghost queue, key-only
        OnEvictFn on_evict;                           // optional
        // SS-2 spine spike, Task 5 — evict seam. When set, EvictToFit
        // consults this policy before discarding a chosen victim. Entries
        // here carry only a content-address DramKey (no locator), so the
        // projected StateIdentity is SK_KV-only for this spike (the KV
        // policy is kind-agnostic in shouldEvict). Not owned; must outlive
        // the DramTier. Defaults to nullptr (no gating — today's
        // unconditional-evict behavior).
        // Pointee is non-const: ValuePolicy's virtual methods (shouldEvict
        // et al.) are not const-qualified (see value_policy.h) — every
        // existing call site (ValuePolicyRegistry::of() included) goes
        // through a non-const reference/pointer.
        kvcache::common::ValuePolicy* evict_policy = nullptr;
    };

    explicit DramTier(const Options& opts);
    ~DramTier();
    DramTier(const DramTier&)            = delete;
    DramTier& operator=(const DramTier&) = delete;

    enum class HitWhere { kMiss = 0, kA1in, kAm, kGhost };

    struct LookupResult {
        HitWhere where = HitWhere::kMiss;
        // When kA1in or kAm, `data` points at the entry's bytes. The pointer
        // is stable as long as the caller doesn't Insert/Touch concurrently
        // (DramTier holds the mutex internally for each call; callers needing
        // pointer stability across calls must copy out).
        const uint8_t* data       = nullptr;
        std::size_t    data_bytes = 0;
    };

    // Returns the data and updates 2Q bookkeeping per admission rules.
    LookupResult Lookup(const DramKey& key);

    // Read-only peek: returns data for a key resident in A1in or Am WITHOUT
    // updating eviction order (no Am splice, no ghost promotion). Used by
    // ReplicaFetch to satisfy the primary-oblivious / no-tier-mutation contract
    // required by A9 R5.
    LookupResult Peek(const DramKey& key) const;

    // Insert / replace a value. Triggers eviction(s) to keep within capacity.
    // The data is copied into the tier (callers can free their buffer).
    void Insert(const DramKey& key, const uint8_t* data, std::size_t n);

    // Drop a key from any of A1in / Am / A1out.
    bool Erase(const DramKey& key);

    // Capacity accounting.
    uint64_t UsedBytes()   const noexcept;
    uint64_t Capacity()    const noexcept { return capacity_bytes_; }
    uint64_t A1inBytes()   const noexcept;
    uint64_t AmBytes()     const noexcept;
    std::size_t GhostSize() const noexcept;

   private:
    enum class Queue : uint8_t { A1in, Am };

    struct Entry {
        DramKey               key;
        Queue                 q;
        std::vector<uint8_t>  data;
    };
    using EntryList = std::list<Entry>;
    using EntryIt   = EntryList::iterator;

    void EvictToFit(std::size_t incoming_bytes);
    void GhostInsert(const DramKey& key);
    // SS-2 spine spike, Task 5 — evict seam helper (see Options::evict_policy).
    bool IsNotEvictable() const;

    mutable std::mutex mu_;

    EntryList a1in_;        // front = newest
    EntryList am_;          // front = most-recently-used
    std::list<DramKey> a1out_;
    std::unordered_map<DramKey, EntryIt, DramKeyHash>   index_;
    std::unordered_map<DramKey, std::list<DramKey>::iterator, DramKeyHash> ghost_index_;

    uint64_t capacity_bytes_   = 0;
    uint64_t a1in_capacity_    = 0;  // 25% of total
    // Note: Am capacity is implied by (capacity_bytes_ - a1in_bytes_used_)
    // in EvictToFit; an explicit am_capacity_ field would be redundant.
    uint32_t a1out_max_        = 0;

    uint64_t a1in_bytes_used_  = 0;
    uint64_t am_bytes_used_    = 0;

    OnEvictFn on_evict_;

    // SS-2 spine spike, Task 5 — evict seam (see Options::evict_policy).
    kvcache::common::ValuePolicy* evict_policy_ = nullptr;
};

}  // namespace kvcache::node::tier
