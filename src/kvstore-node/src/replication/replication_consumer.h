// Task 5 — A9 DR warm-standby: ReplicationConsumer.
//
// Ties together Tasks 1–4: for each incoming prefix::Event this class
//   1. warm-filters the event (IsWarm / WarmPolicy),
//   2. deduplicates via a monotonic epoch cursor (ReplicationCursor),
//   3. fetches the chunk from the primary (HeadlessNode::ReplicaFetch),
//   4. commits the chunk to the standby (Reserve + Publish + SealByChunkPath).
//
// ApplyEvent is a pure synchronous step — no threads, no side effects beyond
// the standby's ART. Task 6 wraps it with a threaded live-subscribe loop.
//
// Lifetime: primary and standby must outlive the ReplicationConsumer.
//
// Layer note: HeadlessNode lives in core-abi/ (one layer above kvstore-node/).
// This header forward-declares it and the .cpp includes headless_node.h;
// consumers that compile this .cpp must add core-abi/src to their include path.
#pragma once

#include <cstdint>

#include "prefix/kv_event_stream.h"
#include "replication/replication_cursor.h"
#include "replication/warm_filter.h"

// Forward-declare HeadlessNode to avoid pulling core-abi headers into every
// kvstore-node translation unit that includes this header.
namespace kvcache::abi {
class HeadlessNode;
}

namespace kvcache::node::replication {

class ReplicationConsumer {
   public:
    struct Options {
        WarmPolicy warm;
    };

    // primary: source of truth — ReplicaFetch is called here.
    // standby: warm mirror — Reserve/Publish/SealByChunkPath are called here.
    // Both references are borrowed; they must outlive the consumer.
    ReplicationConsumer(abi::HeadlessNode& primary,
                        abi::HeadlessNode& standby,
                        Options opts);

    // Apply one event synchronously. Returns true iff a chunk was replicated
    // to the standby. Returns false (benign) for:
    //   - filtered events (non-ADD or cold tier),
    //   - duplicate epochs (already applied),
    //   - locator not found on primary (evicted since the event was emitted).
    // On a primary miss the cursor is still advanced to avoid infinite retry.
    bool ApplyEvent(const prefix::Event& ev);

    // The last epoch seen by the cursor (0 = nothing applied yet).
    uint64_t CursorEpoch() const;

   private:
    abi::HeadlessNode& primary_;
    abi::HeadlessNode& standby_;
    Options            opts_;
    ReplicationCursor  cursor_;
};

}  // namespace kvcache::node::replication
