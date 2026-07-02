// Task 5 — A9 DR warm-standby: ReplicationConsumer implementation.
//
// headless_node.h lives in core-abi/src; callers that compile this TU must
// add core-abi/src to their include path (the kvstore_node_core static lib
// does not, because it lives at a lower layer — see CLAUDE.md layer note in
// replication_consumer.h). The test binary adds the path explicitly.
#include "replication/replication_consumer.h"

#include <cstring>

#include "headless_node.h"          // kvcache::abi::HeadlessNode (core-abi/src)
#include "kvcache/kv_errors.h"

namespace kvcache::node::replication {

ReplicationConsumer::ReplicationConsumer(abi::HeadlessNode& primary,
                                         abi::HeadlessNode& standby,
                                         Options opts)
    : primary_(primary), standby_(standby), opts_(opts) {}

bool ReplicationConsumer::ApplyEvent(const prefix::Event& ev) {
    // Step 1: warm-filter — only ADD events on hot tiers are replicated.
    if (!IsWarm(ev, opts_.warm)) return false;

    // Step 2: dedup — skip epochs we have already applied.
    if (!cursor_.ShouldApply(ev.epoch)) return false;

    // Step 3: fetch from primary.
    abi::HeadlessNode::ReplicaChunk chunk;
    if (primary_.ReplicaFetch(ev.locator, &chunk) != KV_OK) {
        // Chunk was evicted from primary since the event was emitted.
        // Advance the cursor so we never retry this epoch and return false
        // (benign miss — the primary simply moved on).
        cursor_.Advance(ev.epoch);
        return false;
    }

    // Step 4: commit to standby — Reserve → Publish → SealByChunkPath.
    //
    // tenant_hash and model_hash are used by Reserve to record the
    // namespace on the HandleState; SealByChunkPath then bypasses token
    // derivation and inserts directly via the pre-computed chunk_path.
    // We pass the locator's model_id_hash (already the FNV-1a of the model
    // string) and 0 for tenant_hash (system bucket — consistent with the
    // single-tenant headless path in other replication tests).
    const uint64_t tenant_hash = 0;
    const uint64_t model_hash  = ev.locator.model_id_hash;

    kv_handle_t      h{};
    kv_buffer_desc_t slot{};
    if (standby_.Reserve(&ev.locator, chunk.bytes.size(),
                         tenant_hash, model_hash,
                         &h, &slot) != KV_OK) {
        return false;
    }

    if (slot.addr && !chunk.bytes.empty()) {
        std::memcpy(slot.addr, chunk.bytes.data(), chunk.bytes.size());
    }

    // Publish with empty src (bytes are already in the mutable-buffer slot
    // that Reserve allocated) and watermark = byte count.
    kv_buffer_desc_t empty{};
    if (standby_.Publish(h, empty, chunk.bytes.size()) != KV_OK) {
        return false;
    }

    if (standby_.SealByChunkPath(h, chunk.chunk_path) != KV_OK) {
        return false;
    }

    // Step 5: advance the cursor past this epoch.
    cursor_.Advance(ev.epoch);
    return true;
}

uint64_t ReplicationConsumer::CursorEpoch() const {
    return cursor_.Last();
}

}  // namespace kvcache::node::replication
