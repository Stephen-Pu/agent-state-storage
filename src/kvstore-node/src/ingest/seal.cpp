// LLD §3.4 — Atomic seal orchestrator.
#include "ingest/seal.h"

#include <chrono>

#include "ingest/mutable_buffer.h"
#include "ingest/watermark.h"

namespace kvcache::node::ingest {

SealCommitter::SealCommitter(const Deps& deps) : deps_(deps) {}

SealCommitter::Result SealCommitter::Commit(const Request& req) {
    Result r{};
    if (!deps_.rocks || !deps_.art || !deps_.events ||
        !deps_.buffers || !deps_.wm) {
        r.error = "seal: incomplete dependencies";
        return r;
    }
    if (req.chunk_path.empty()) {
        r.error = "seal: chunk_path must be non-empty";
        return r;
    }

    // 1) Retrieve final byte count from the watermark tracker. Sealed bytes is
    // the watermark at the moment of seal; subsequent writes to the same slot
    // are forbidden (the engine adapter enforces this on its side).
    const uint64_t sealed_bytes = deps_.wm->Read(req.handle);
    if (sealed_bytes == 0) {
        r.error = "seal: handle unknown or watermark == 0";
        return r;
    }

    // 2) RocksDB durable write (atomic with cluster_epoch advance).
    meta::SealedChunkValue v{};
    v.version               = meta::kSchemaVersion;
    v.sealed_at_nanos       = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    v.bytes_total           = sealed_bytes;
    v.tier_residency_bitmap = req.tier_residency_bitmap;
    v.range_covered         = req.locator.range;

    const uint64_t new_epoch = deps_.rocks->CurrentEpoch() + 1;
    auto key = meta::SealedChunkKey::From(req.locator);
    std::string err;
    if (!deps_.rocks->PutSealedChunkAtomic(key, v, new_epoch, &err)) {
        r.error = "seal/rocksdb: " + err;
        return r;
    }

    // 3) ART insert. Build the LeafData on the heap because the index takes
    // ownership.
    auto leaf = std::make_unique<prefix::LeafData>();
    leaf->locator               = req.locator;
    leaf->tier_residency_bitmap = req.tier_residency_bitmap;
    leaf->sealed_at_nanos       = v.sealed_at_nanos;
    leaf->bytes_total           = sealed_bytes;
    // refcount starts at 1: the caller (engine) owns one reference until
    // kv_release is invoked.
    leaf->refcount.Acquire();

    const auto ins = deps_.art->Insert(
        {req.chunk_path.data(), req.chunk_path.size()},
        std::move(leaf));
    if (ins == prefix::ArtIndex::InsertResult::kPathConflict) {
        // We've already durably persisted the row; ART rejection here is a
        // structural bug (e.g. inserting on top of an inner node). We do NOT
        // attempt to roll back RocksDB — instead, surface the error so the
        // caller can resolve and the next boot's replay reconciles state.
        r.error = "seal/art: path conflict (persisted; replay required)";
        return r;
    }

    // 4) Publish ADD event.
    prefix::Event ev{};
    ev.type    = prefix::EventType::Add;
    ev.tier    = prefix::Tier::Pinned;  // sealed-from-Pinned in this MVP
    ev.locator = req.locator;
    deps_.events->Publish(ev);

    // 5) Final bookkeeping: release the watermark tracker entry and the
    // mutable-buffer handle. The Pinned slot returns to the pool — its data
    // has been promoted by the caller via TierManager::StageToDram (or other
    // tier-specific paths).
    deps_.wm->Drop(req.handle);
    deps_.buffers->Release(req.handle);

    r.ok           = true;
    r.new_epoch    = new_epoch;
    r.sealed_bytes = sealed_bytes;
    return r;
}

}  // namespace kvcache::node::ingest
