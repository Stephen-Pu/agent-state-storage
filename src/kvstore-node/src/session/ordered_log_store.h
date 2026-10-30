// OrderedLogStore — the ordered key space behind the agentstate.v1 service (M1).
//
// An in-memory std::map holds every live key; an append-only log file makes
// it durable. Each Apply() writes ONE record holding the whole batch, fsyncs
// it, and only then updates the map, so a batch is all-or-nothing across a
// crash. Open() replays the log; a torn or corrupt tail record is truncated
// and every complete record before it is kept (same discipline as StateWal).
//
// On-disk record (little-endian, packed):
//   record_len u32   total bytes INCLUDING this field and the crc
//   n          u32   mutation count
//   n x { op u8 (1 = PUT, 2 = DEL) | klen u32 | key | vlen u32 | value }
//   crc32      u32   IEEE poly over [n .. last value byte]
//
// The log is rewritten from the live map (temp file + fsync + rename) once it
// exceeds max(compact_min_bytes, 2 x live bytes). An empty path keeps
// everything in memory only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace kvcache::node::session {

struct Mutation {
    enum Op : uint8_t { kPut = 1, kDel = 2 };
    Op          op = kPut;
    std::string key;
    std::string value;
};

struct ScanOptions {
    std::string prefix;
    std::string start_after;  // exclusive; empty = from the start of the prefix
    std::string end_before;   // exclusive; empty = to the end of the prefix
    std::size_t limit   = 0;  // 0 = no limit
    bool        reverse = false;
};

struct KV {
    std::string key;
    std::string value;
};

class OrderedLogStore {
   public:
    static constexpr std::size_t kDefaultCompactMinBytes = 64u << 20;

    // Open (create if absent) the log at `path` and replay it. An empty path
    // gives a non-durable in-memory store. Returns nullptr with *err set only
    // when the file cannot be opened or created.
    static std::unique_ptr<OrderedLogStore> Open(const std::string& path, std::string* err,
                                                 std::size_t compact_min_bytes =
                                                     kDefaultCompactMinBytes);

    ~OrderedLogStore();
    OrderedLogStore(const OrderedLogStore&)            = delete;
    OrderedLogStore& operator=(const OrderedLogStore&) = delete;

    // Apply the batch atomically. On a write or fsync failure returns false
    // with *err set and leaves the map unchanged.
    bool Apply(const std::vector<Mutation>& batch, std::string* err);

    std::optional<std::string> Get(const std::string& key) const;

    // Keys under o.prefix strictly between o.start_after and o.end_before, in
    // key order (descending when o.reverse). *has_more is true when o.limit
    // cut the result short.
    std::vector<KV> Scan(const ScanOptions& o, bool* has_more) const;

    // Delete every key under `prefix` in one batch. Returns the count deleted;
    // 0 with *err set on failure.
    std::size_t DeletePrefix(const std::string& prefix, std::string* err);

    std::size_t LiveBytes() const;  // sum of key + value sizes in the map
    std::size_t LogBytes() const;   // current log file size; 0 in memory

   private:
    OrderedLogStore() = default;

    bool AppendLocked(const std::vector<Mutation>& batch, std::string* err);
    void ApplyToMapLocked(const std::vector<Mutation>& batch);
    bool MaybeCompactLocked(std::string* err);

    mutable std::mutex                 mu_;
    std::map<std::string, std::string> map_;
    std::string                        path_;
    int                                fd_                = -1;
    std::size_t                        log_bytes_         = 0;
    std::size_t                        live_bytes_        = 0;
    std::size_t                        compact_min_bytes_ = kDefaultCompactMinBytes;
};

}  // namespace kvcache::node::session
