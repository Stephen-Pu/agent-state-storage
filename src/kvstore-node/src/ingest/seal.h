// LLD §3.4 — Atomic sealing.
//
// SealCommitter is the orchestrator that turns a completed streaming-write
// session into a visible, sealed leaf. The seal is the single atomicity
// boundary in the entire system (LLD §4.4 invariant: "write does not block
// read; seal is an atomic flip").
//
// Commit order (deterministic, crash-safe):
//
//   1. RocksDB: PutSealedChunkAtomic(key, value, new_epoch)
//      — durable; persists on disk.
//      — if the process crashes between step 1 and step 2, the next boot
//        replays the sealed_chunks CF into the ART before serving traffic.
//   2. ArtIndex::Insert(chunk_path, leaf)
//      — makes the chunk visible to in-memory Lookup.
//   3. EventStream::Publish(Add event)
//      — notifies subscribers (bloom-sketch updater, gRPC Subscribe
//        listeners, metrics).
//
// Caller wraps a single seal request in a SealCommitter::Commit call; the
// committer does not own any state beyond pointers to the four collaborators
// it coordinates.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "kvcache/kv_types.h"
#include "meta/rocksdb_store.h"
#include "prefix/art_index.h"
#include "prefix/kv_event_stream.h"

namespace kvcache::node::ingest {

class WatermarkTracker;
class MutableBufferPool;

class SealCommitter {
   public:
    struct Deps {
        meta::RocksdbStore*          rocks    = nullptr;
        prefix::ArtIndex*            art      = nullptr;
        prefix::EventStream*         events   = nullptr;
        MutableBufferPool*           buffers  = nullptr;
        WatermarkTracker*            wm       = nullptr;
    };

    explicit SealCommitter(const Deps& deps);

    struct Request {
        uint64_t                                 handle;         // IngestHandle
        kv_locator_t                             locator;        // identity (range filled in)
        std::vector<prefix::ChunkHash>           chunk_path;     // K chunk hashes
        uint32_t                                 tier_residency_bitmap; // initial residency
    };

    struct Result {
        bool        ok           = false;
        std::string error;
        uint64_t    new_epoch    = 0;
        uint64_t    sealed_bytes = 0;
    };

    Result Commit(const Request& req);

   private:
    Deps deps_;
};

}  // namespace kvcache::node::ingest
