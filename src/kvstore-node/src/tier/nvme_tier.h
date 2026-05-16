// LLD §3.3 T3 — NVMe tier.
//
// Persistent on-NVMe slab allocator. One backing file; fixed-size slots; an
// in-memory index from content key to slot id. Free slots tracked as a stack.
//
// MVP backend selection:
//   * Default: blocking pread/pwrite. Adequate for unit tests and bring-up.
//   * KVCACHE_ENABLE_URING: linux-aio path via liburing — TODO(stephen). The
//     surface area below stays the same; the differences live inside Put/Get.
//   * KVCACHE_ENABLE_SPDK : SPDK direct-NVMe path — TODO. LLD §3.3 calls out
//     io_uring + SPDK as a dual-backend.
//
// Persistence semantics:
//   * The backing file is durable on close (we fdatasync after each write).
//   * The in-memory index is NOT persisted in MVP — it is rebuilt on the next
//     boot from the RocksDB sealed_chunks.tier_residency_bitmap (Step 6 will
//     wire the rebuild path; for MVP we treat the NVMe tier as best-effort
//     cache that may need re-population across restarts).
//   * O_DIRECT is intentionally not used yet — it requires aligned buffers
//     and sizes, and gains real value only with io_uring. TODO(stephen).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tier/dram_tier.h"  // DramKey + DramKeyHash reused as content key

namespace kvcache::node::tier {

class NvmeTier {
   public:
    struct Options {
        std::string path;                         // backing file path
        uint64_t    pool_bytes        = 64ull << 30;  // 64 GiB default
        uint64_t    slot_bytes        = 64ull << 20;  // 64 MiB default
        bool        create_if_missing = true;
        bool        fdatasync_on_put  = true;
    };

    static std::unique_ptr<NvmeTier> Create(const Options& opts, std::string* err);

    ~NvmeTier();
    NvmeTier(const NvmeTier&)            = delete;
    NvmeTier& operator=(const NvmeTier&) = delete;

    // Write `n` bytes against `key`. Replaces any existing entry for the key.
    // Returns false if the pool is full or the write fails; sets `err`.
    bool Put(const DramKey& key, const uint8_t* data, std::size_t n, std::string* err);

    // Copy bytes into the caller-provided buffer. `*out_bytes` receives the
    // actual entry size on success. Returns false if `key` is missing or the
    // caller buffer is too small.
    bool Get(const DramKey& key, uint8_t* dst, std::size_t dst_capacity,
             std::size_t* out_bytes, std::string* err) const;

    // Heap-allocating convenience variant for code that doesn't know the size
    // up front (e.g. cold-tier staging).
    bool Get(const DramKey& key, std::vector<uint8_t>* out, std::string* err) const;

    bool Erase(const DramKey& key);
    bool Contains(const DramKey& key) const;

    uint64_t    UsedBytes() const noexcept;
    uint64_t    Capacity() const noexcept { return pool_bytes_; }
    uint32_t    SlotsInUse() const noexcept {
        return in_use_.load(std::memory_order_relaxed);
    }
    uint32_t    SlotCount() const noexcept { return slot_count_; }

   private:
    NvmeTier() = default;

    struct Entry {
        uint32_t slot_id;
        uint64_t bytes;
    };

    mutable std::mutex mu_;
    int                fd_         = -1;
    std::string        path_;
    uint64_t           pool_bytes_ = 0;
    uint64_t           slot_bytes_ = 0;
    uint32_t           slot_count_ = 0;
    bool               fdatasync_  = true;

    std::vector<uint32_t> free_stack_;
    std::unordered_map<DramKey, Entry, DramKeyHash> index_;
    std::atomic<uint32_t> in_use_{0};
};

}  // namespace kvcache::node::tier
