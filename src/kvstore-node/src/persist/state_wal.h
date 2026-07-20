// StateWal — append-only persistence engine for B-class state (⑬ v0).
//
// B-class state (Agent memory, execution state) is irreplaceable: unlike A/KV
// it cannot be recomputed, so it needs a durable ledger. This is the first,
// minimal cut of that ledger — a single append-only WAL file, fsynced on every
// append, replayed into an in-memory key→bytes map on Open. Single node, no
// replication / strong consistency / lineage (all deferred B-plane slices).
//
// On-disk record (little-endian, packed; one per Append):
//   record_len u32    total bytes INCLUDING this field
//   op         u8     1 = PUT (2 = DEL reserved, not emitted this slice)
//   key        u8[16] DramKey
//   identity   u8[128] StateIdentity (opaque; stored for future lineage/debug)
//   blob_len   u32
//   blob       u8[blob_len]
//   crc32      u32    IEEE poly over [op .. last blob byte]
//
// A torn tail record (short read or CRC/len mismatch) is truncated on Open and
// every fully-committed record before it is kept — same discipline as ArtWal.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "state_identity.h"
#include "tier/dram_tier.h"

namespace kvcache::node::persist {

class StateWal {
   public:
    // Open (create if absent) at `path`, replaying existing records into the
    // in-memory map. Missing file is fine. A torn tail is truncated. Only a
    // hard error (cannot open/create the file) returns nullptr with *err set.
    static std::unique_ptr<StateWal> Open(const std::string& path, std::string* err);

    ~StateWal();
    StateWal(const StateWal&)            = delete;
    StateWal& operator=(const StateWal&) = delete;

    // Append a PUT record, fsync, then update the in-memory map. Returns false
    // (and sets *err) if the write or fsync fails; on failure the map is NOT
    // updated (caller must treat the entry as not persisted).
    bool Append(const tier::DramKey& key, const common::StateIdentity& id,
                const uint8_t* data, std::size_t n, std::string* err);

    // Latest blob for `key`. Returns false if absent.
    bool Get(const tier::DramKey& key, std::vector<uint8_t>* out) const;

    std::size_t Size() const;

   private:
    StateWal() = default;

    int                                                          fd_ = -1;
    mutable std::mutex                                           mu_;
    std::unordered_map<tier::DramKey, std::vector<uint8_t>,
                       tier::DramKeyHash>                        map_;
};

}  // namespace kvcache::node::persist
