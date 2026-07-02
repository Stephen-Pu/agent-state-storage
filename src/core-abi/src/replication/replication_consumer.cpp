// Task 5 / Task 6 — A9 DR warm-standby: ReplicationConsumer implementation.
//
// headless_node.h lives in core-abi/src; callers that compile this TU must
// add core-abi/src to their include path. The kvcache shared library and the
// test binary both satisfy this requirement.
#include "replication/replication_consumer.h"

#include <cstring>

#include "headless_node.h"          // kvcache::abi::HeadlessNode (core-abi/src)
#include "kvcache/kv_errors.h"
#include "kvcache/kv_types.h"

namespace kvcache::node::replication {

ReplicationConsumer::ReplicationConsumer(abi::HeadlessNode& primary,
                                         abi::HeadlessNode& standby,
                                         Options opts)
    : primary_(primary), standby_(standby), opts_(opts) {}

ReplicationConsumer::~ReplicationConsumer() { Stop(); }

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

// ---------------------------------------------------------------------------
// Task 6 — Live subscribe/poll loop.
//
// Start() subscribes to the primary's EventStream via SubscribeEvents and
// spawns a background thread that translates incoming kv_event_t callbacks
// into prefix::Events and drives ApplyEvent.
//
// HeadlessNode::SubscribeEvents spawns its OWN per-subscription poller thread
// internally. That thread delivers kv_event_t via the user callback cb. Our
// outer thread (thread_) is therefore a thin trampoline that just waits; we
// use an SPSC queue approach: the SubscribeEvents callback enqueues the event
// and our thread dequeues it.
//
// Simpler alternative: since SubscribeEvents already delivers events through
// a background thread, we can let the callback itself call ApplyEvent directly
// and keep thread_ just as a lifecycle handle (start/join). We use a single
// mutex+condvar so the callback can be called from the HeadlessNode poller
// thread while ApplyEvent (and cursor mutation) are serialised.
//
// Concurrency model:
//   - stop flag (running_) is atomic; set by Stop() before UnsubscribeEvents.
//   - UnsubscribeEvents() joins the internal HeadlessNode poller thread before
//     returning, guaranteeing no further cb() calls after it returns.
//   - After UnsubscribeEvents() returns, thread_ exits its wait loop.
//   - Stop() then joins thread_.
//   - cursor_ and the standby writes in ApplyEvent are only touched inside cb()
//     which is serialised by HeadlessNode's internal per-subscription poller
//     (SPSC: one producer thread, one consumer poller thread). No additional
//     lock is needed for cursor_ correctness.
// ---------------------------------------------------------------------------

// Callback invoked by HeadlessNode's internal poller thread for every event.
// `user` points to `this` ReplicationConsumer.
static void EventCbDispatch(const kv_event_t* raw_ev, void* user) {
    auto* self = static_cast<ReplicationConsumer*>(user);

    // Translate kv_event_t (C ABI) → prefix::Event (C++ internal).
    prefix::Event ev{};
    ev.type       = static_cast<prefix::EventType>(raw_ev->type);
    ev.tier       = static_cast<prefix::Tier>(raw_ev->tier);
    ev.locator    = raw_ev->locator;
    ev.epoch      = raw_ev->epoch;

    // Dispatch to the synchronous ApplyEvent (no additional lock needed:
    // the HeadlessNode poller is the sole caller of this callback for our
    // subscription).
    self->ApplyEvent(ev);
}

void ReplicationConsumer::Start() {
    // Idempotent: only one Start() takes effect (CAS false→true).
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return;  // already running
    }

    // Subscribe to the primary's event stream. HeadlessNode spawns its own
    // poller thread that calls EventCbDispatch for each event.
    // SubscriptionId is uint64_t; static_assert in headless_node.h confirms.
    sub_id_ = static_cast<uint64_t>(
        primary_.SubscribeEvents(EventCbDispatch, this));

    // Spawn a lightweight sentinel thread whose sole purpose is to be
    // joinable by Stop(). The actual work happens in EventCbDispatch via
    // HeadlessNode's internal poller. This thread waits until Stop() sets
    // running_=false and then exits, giving Stop() a clean join target.
    thread_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
}

void ReplicationConsumer::Stop() {
    // Idempotent: only one Stop() does the teardown.
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel)) {
        return;  // already stopped or never started
    }

    // Unsubscribe from the primary's event stream. This joins the internal
    // HeadlessNode poller thread, guaranteeing that EventCbDispatch is never
    // called after UnsubscribeEvents() returns. It is therefore safe to
    // destroy `this` (and cursor_) immediately after Stop() returns.
    if (sub_id_ != 0) {
        primary_.UnsubscribeEvents(
            static_cast<abi::HeadlessNode::SubscriptionId>(sub_id_));
        sub_id_ = 0;
    }

    // Join the sentinel thread (it will have observed running_=false and exited).
    if (thread_.joinable()) {
        thread_.join();
    }
}

}  // namespace kvcache::node::replication
