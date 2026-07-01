# A9 DR Warm-Standby Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the local, in-process core of A9 DR warm-standby — a standby `ReplicationConsumer` that mirrors a primary's warm KV chunks by subscribing to its event stream and pulling each chunk by locator — verifiable with two in-process `HeadlessNode`s (no second cluster, no gRPC).

**Architecture:** Pull-based replica subscriber (A9 spec Approach A). The primary gains a read-only `ReplicaFetch(locator) → {chunk_path, bytes}` that resolves via the existing G-1 content index (`evict_index_`). The standby gains `SealByChunkPath` (a `Seal` variant that takes a pre-computed `chunk_path` instead of deriving it from tokens — the standby never has the tokens). A standby-side `ReplicationConsumer` ties them together: subscribe → warm-filter → fetch → commit.

**Tech Stack:** C++20, CMake+Ninja, GoogleTest (FetchContent), the existing `HeadlessNode` / `EventStream` / ART / tier stack.

**Spec:** `docs/superpowers/specs/2026-07-01-a9-cross-cluster-dr-federation-design.md`. This plan **refines spec §3 step 4**: the standby commit is keyed by `chunk_path` (via `ReplicaFetch` + `SealByChunkPath`), not by tokens — because the replication event carries a locator, not tokens. The spec §3/§6 text is updated to match.

## Global Constraints

- C++20; CMake ≥ 3.28 in practice (repo FetchContent uses a URL_HASH form 3.22 rejects).
- No new third-party dependency; reuse the in-tree `HeadlessNode`, `EventStream`, ART, DRAM tier.
- Default build unaffected: all new code is additive; no ABI-version bump (additive methods only).
- Follow existing patterns: seed-then-watch consumer (mirror `NodeDirectory`/`IdentityWatcher`); tests as gtest under `src/tests/unit/`, wired in `src/tests/unit/CMakeLists.txt`.
- `ChunkHash = std::array<uint8_t,8>`; `chunk_path = std::vector<ChunkHash>`.
- TDD: failing test → run-fail → minimal impl → run-pass → commit, per step.

## File Structure

- `src/core-abi/src/headless_node.h` / `.cpp` — add `ReplicaFetch` + `SealByChunkPath` (both additive public methods).
- `src/kvstore-node/src/replication/warm_filter.h` — pure warm-set predicate (header-only).
- `src/kvstore-node/src/replication/replication_cursor.h` — pure epoch cursor (header-only).
- `src/kvstore-node/src/replication/replication_consumer.h` / `.cpp` — the orchestrator.
- `src/tests/unit/replication/warm_filter_test.cpp`, `replication_cursor_test.cpp`, `replication_consumer_test.cpp` — tests.
- `src/tests/unit/CMakeLists.txt` — wire the three test targets.
- gRPC `ReplicaFetch` RPC surface is **out of scope for this plan** (grpc-gated; a follow-on) — see the spec §9.

---

### Task 1: `ReplicaFetch` on HeadlessNode — resolve a locator to `{chunk_path, bytes}`

**Files:**
- Modify: `src/core-abi/src/headless_node.h` (add method decl + a small result struct)
- Modify: `src/core-abi/src/headless_node.cpp` (impl)
- Test: `src/tests/unit/replication/replica_fetch_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: existing private `evict_index_` (`LocatorContentKey(locator) → chunk_path`, populated at `Seal`, headless_node.cpp §Seal); the DRAM tier accessor used by `Fetch` to read a sealed chunk's bytes.
- Produces:
  ```cpp
  struct ReplicaChunk {
      std::vector<node::prefix::ChunkHash> chunk_path;  // K 8-byte hashes
      std::vector<uint8_t>                 bytes;        // the sealed KV bytes
  };
  // Returns KV_OK and fills *out on hit; KV_E_NOT_FOUND if the locator has no
  // sealed chunk (evicted / never sealed). Read-only; no refcount side effects.
  int ReplicaFetch(const kv_locator_t& locator, ReplicaChunk* out);
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// src/tests/unit/replication/replica_fetch_test.cpp
#include "headless_node.h"
#include <gtest/gtest.h>
using namespace kvcache::core;   // adjust to HeadlessNode's namespace

TEST(ReplicaFetch, ReturnsChunkPathAndBytesForSealedLocator) {
    HeadlessNode node;                 // default in-process (loopback) init
    // Seal a chunk the normal way so evict_index_ + tiers are populated.
    kv_locator_t loc = MakeLocator(/*tenant*/1, /*model*/2, /*tokens*/{0..31});
    std::vector<uint8_t> payload(2 * 16 * 64, 0xAB);   // 32 tokens * 64B
    kv_handle_t h; kv_buffer_desc_t slot;
    ASSERT_EQ(node.Reserve(&loc, payload.size(), &h, &slot), KV_OK);
    std::memcpy(slot.addr, payload.data(), payload.size());
    ASSERT_EQ(node.Publish(h, {}, payload.size()), KV_OK);
    uint32_t toks[32]; for (int i=0;i<32;++i) toks[i]=i;
    ASSERT_EQ(node.Seal(h, toks, 32), KV_OK);

    HeadlessNode::ReplicaChunk rc;
    ASSERT_EQ(node.ReplicaFetch(loc, &rc), KV_OK);
    EXPECT_FALSE(rc.chunk_path.empty());
    EXPECT_EQ(rc.bytes, payload);
}

TEST(ReplicaFetch, NotFoundForUnsealedLocator) {
    HeadlessNode node;
    kv_locator_t loc = MakeLocator(9, 9, {100,101,102});
    HeadlessNode::ReplicaChunk rc;
    EXPECT_EQ(node.ReplicaFetch(loc, &rc), KV_E_NOT_FOUND);
}
```
*(`MakeLocator` helper: copy the one from `node_data_service_test` / write a 3-line local helper that fills tenant_id/model_id_hash/prefix_hash like the Python connector — reuse the existing test locator helper in this repo.)*

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test_replica_fetch && ./build/tests/unit/test_replica_fetch`
Expected: FAIL to compile — `ReplicaFetch` / `ReplicaChunk` not declared.

- [ ] **Step 3: Add the declaration**

In `headless_node.h`, inside `class HeadlessNode`'s public section:
```cpp
struct ReplicaChunk {
    std::vector<node::prefix::ChunkHash> chunk_path;
    std::vector<uint8_t>                 bytes;
};
int ReplicaFetch(const kv_locator_t& locator, ReplicaChunk* out);
```

- [ ] **Step 4: Implement**

In `headless_node.cpp`:
```cpp
int HeadlessNode::ReplicaFetch(const kv_locator_t& locator, ReplicaChunk* out) {
    if (!out) return KV_E_INVAL;
    std::lock_guard<std::mutex> lk(mu_);              // same mutex Seal/Fetch use
    auto it = evict_index_.find(LocatorContentKey(locator));
    if (it == evict_index_.end()) return KV_E_NOT_FOUND;
    out->chunk_path = it->second;                     // vector<ChunkHash>
    // Bytes for a sealed chunk live in the DRAM tier under the content key —
    // the same read Fetch performs. Reuse the existing tier read path.
    if (!dram_->Get(LocatorContentKey(locator), &out->bytes)) {
        return KV_E_NOT_FOUND;                         // evicted from tier
    }
    return KV_OK;
}
```
*(If `dram_->Get(DramKey, std::vector<uint8_t>*)` is not the exact signature in `dram_tier.h`, use whatever read `Fetch` uses to obtain the chunk bytes for a resident leaf; the point is a read-only byte fetch by content key. Verify against `dram_tier.h` before writing.)*

- [ ] **Step 5: Run to verify pass**

Run: `./build/tests/unit/test_replica_fetch`
Expected: PASS (2 tests).

- [ ] **Step 6: Commit**

```bash
git add src/core-abi/src/headless_node.h src/core-abi/src/headless_node.cpp \
        src/tests/unit/replication/replica_fetch_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(A9): HeadlessNode::ReplicaFetch(locator) -> {chunk_path, bytes}"
```

---

### Task 2: `SealByChunkPath` on HeadlessNode — commit a replicated chunk without tokens

**Files:**
- Modify: `src/core-abi/src/headless_node.h`, `.cpp`
- Test: `src/tests/unit/replication/seal_by_chunk_path_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: the existing `Seal` body (headless_node.cpp) — everything after `chunk_path` is computed.
- Produces:
  ```cpp
  // Same as Seal, but the caller supplies chunk_path directly (the standby has
  // it from ReplicaFetch and does NOT have the original tokens). Commits the
  // reserved handle's bytes into the ART at chunk_path + emits KV_EVENT_ADD.
  int SealByChunkPath(kv_handle_t handle,
                      const std::vector<node::prefix::ChunkHash>& chunk_path);
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// src/tests/unit/replication/seal_by_chunk_path_test.cpp
TEST(SealByChunkPath, CommittedChunkIsLookupableByOriginalTokens) {
    HeadlessNode primary, standby;
    kv_locator_t loc = MakeLocator(1, 2, /*tokens 0..31*/);
    std::vector<uint8_t> payload(2*16*64, 0xCD);
    // Seal on primary the normal way.
    /* Reserve/write/Publish/Seal(primary, tokens 0..31) as in Task 1 */
    HeadlessNode::ReplicaChunk rc;
    ASSERT_EQ(primary.ReplicaFetch(loc, &rc), KV_OK);

    // Commit on standby by chunk_path (no tokens).
    kv_handle_t h; kv_buffer_desc_t slot;
    ASSERT_EQ(standby.Reserve(&loc, rc.bytes.size(), &h, &slot), KV_OK);
    std::memcpy(slot.addr, rc.bytes.data(), rc.bytes.size());
    ASSERT_EQ(standby.Publish(h, {}, rc.bytes.size()), KV_OK);
    ASSERT_EQ(standby.SealByChunkPath(h, rc.chunk_path), KV_OK);

    // The standby now resolves the SAME tokens the primary sealed.
    kv_locator_t meta; kv_handle_t lh; uint32_t matched=0;
    uint32_t toks[32]; for (int i=0;i<32;++i) toks[i]=i;
    EXPECT_EQ(standby.Lookup("", TenantHash(1), ModelHash(2), toks, 32,
                             &meta, &lh, &matched), KV_OK);
    EXPECT_EQ(matched, 32u);
}
```

- [ ] **Step 2: Run to verify fail**

Run: `cmake --build build --target test_seal_by_chunk_path && ./build/tests/unit/test_seal_by_chunk_path`
Expected: FAIL — `SealByChunkPath` not declared.

- [ ] **Step 3: Refactor `Seal` to share the commit body**

In `headless_node.cpp`, extract the post-`chunk_path` portion of `Seal` into a private helper and call it from both:
```cpp
int HeadlessNode::SealCommit(kv_handle_t handle, HandleState& st,
                             const std::vector<node::prefix::ChunkHash>& chunk_path) {
    // ... the existing Seal body from `SealCommitter::Deps deps{...}` onward,
    //     using `chunk_path` (the arg) instead of the locally-derived one ...
}
int HeadlessNode::Seal(kv_handle_t handle, const uint32_t* tokens, std::size_t n) {
    // ... resolve st; compute ns + chunk_path = ChunkifyNS({tokens,n}, ns) ...
    return SealCommit(handle, st, chunk_path);
}
```

- [ ] **Step 4: Add `SealByChunkPath`**

```cpp
int HeadlessNode::SealByChunkPath(
    kv_handle_t handle, const std::vector<node::prefix::ChunkHash>& chunk_path) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* st = LookupHandleState(handle);            // same resolution Seal uses
    if (!st) return KV_E_NOT_FOUND;
    return SealCommit(handle, *st, chunk_path);
}
```

- [ ] **Step 5: Run to verify pass**

Run: `./build/tests/unit/test_seal_by_chunk_path`
Expected: PASS. Then run the full node suite to confirm the `Seal` refactor didn't regress: `cd build && ctest -R "node_data|seal|art" --output-on-failure`.

- [ ] **Step 6: Commit**

```bash
git add src/core-abi/src/headless_node.h src/core-abi/src/headless_node.cpp \
        src/tests/unit/replication/seal_by_chunk_path_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(A9): HeadlessNode::SealByChunkPath — token-free replicated commit"
```

---

### Task 3: `WarmSetFilter` — decide which ADD events to replicate

**Files:**
- Create: `src/kvstore-node/src/replication/warm_filter.h` (header-only)
- Test: `src/tests/unit/replication/warm_filter_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `node::prefix::Event` (`{type, tier, locator, epoch}`) — the `tier` field already exists (`kv_event_t.tier`, `Event.tier`).
- Produces:
  ```cpp
  namespace kvcache::node::replication {
  struct WarmPolicy { int max_tier = 1; };   // replicate tier <= max_tier (T0/T1 hot)
  // True iff this ADD event's chunk is in a hot tier worth replicating.
  inline bool IsWarm(const prefix::Event& ev, const WarmPolicy& p);
  }
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// src/tests/unit/replication/warm_filter_test.cpp
#include "replication/warm_filter.h"
#include <gtest/gtest.h>
using kvcache::node::replication::IsWarm;
using kvcache::node::replication::WarmPolicy;
using kvcache::node::prefix::Event;

TEST(WarmFilter, AdmitsHotTierAddRejectsColdAndNonAdd) {
    WarmPolicy p{.max_tier = 1};
    Event add_hot{}; add_hot.type = /*ADD*/1; add_hot.tier = 1;
    Event add_cold{}; add_cold.type = 1; add_cold.tier = 3;
    Event evict{}; evict.type = /*EVICT*/2; evict.tier = 1;
    EXPECT_TRUE(IsWarm(add_hot, p));
    EXPECT_FALSE(IsWarm(add_cold, p)) << "cold-tier chunk not replicated";
    EXPECT_FALSE(IsWarm(evict, p)) << "only ADD is replicated by the filter";
}
```

- [ ] **Step 2: Run to verify fail**

Run: `cmake --build build --target test_warm_filter && ./build/tests/unit/test_warm_filter`
Expected: FAIL — header missing.

- [ ] **Step 3: Implement the header**

```cpp
// src/kvstore-node/src/replication/warm_filter.h
#pragma once
#include "prefix/kv_event_stream.h"
namespace kvcache::node::replication {
struct WarmPolicy { int max_tier = 1; };
inline bool IsWarm(const prefix::Event& ev, const WarmPolicy& p) {
    return ev.type == /*KV_EVENT_ADD*/1 && ev.tier >= 0 && ev.tier <= p.max_tier;
}
}  // namespace
```
*(Use the real `KV_EVENT_ADD` enum value from `kv_types.h` rather than the literal `1`.)*

- [ ] **Step 4: Run to verify pass** — Run: `./build/tests/unit/test_warm_filter` — Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/kvstore-node/src/replication/warm_filter.h \
        src/tests/unit/replication/warm_filter_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(A9): warm-set filter (hot-tier ADD events)"
```

---

### Task 4: `ReplicationCursor` — track last-applied epoch for resume

**Files:**
- Create: `src/kvstore-node/src/replication/replication_cursor.h` (header-only)
- Test: `src/tests/unit/replication/replication_cursor_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  namespace kvcache::node::replication {
  class ReplicationCursor {
   public:
    bool ShouldApply(uint64_t epoch) const;  // true iff epoch > last_
    void Advance(uint64_t epoch);             // last_ = max(last_, epoch)
    uint64_t Last() const;
   private:
    uint64_t last_ = 0;
  };
  }
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// src/tests/unit/replication/replication_cursor_test.cpp
#include "replication/replication_cursor.h"
#include <gtest/gtest.h>
using kvcache::node::replication::ReplicationCursor;
TEST(ReplicationCursor, AdvancesMonotonicallyAndDedups) {
    ReplicationCursor c;
    EXPECT_TRUE(c.ShouldApply(5));  c.Advance(5);
    EXPECT_FALSE(c.ShouldApply(5)) << "already applied";
    EXPECT_FALSE(c.ShouldApply(3)) << "older event on reconnect";
    EXPECT_TRUE(c.ShouldApply(6));  c.Advance(6);
    EXPECT_EQ(c.Last(), 6u);
}
```

- [ ] **Step 2: Run to verify fail** — `cmake --build build --target test_replication_cursor && ./build/tests/unit/test_replication_cursor` — FAIL (missing header).

- [ ] **Step 3: Implement**

```cpp
// src/kvstore-node/src/replication/replication_cursor.h
#pragma once
#include <cstdint>
#include <algorithm>
namespace kvcache::node::replication {
class ReplicationCursor {
 public:
  bool ShouldApply(uint64_t epoch) const { return epoch > last_; }
  void Advance(uint64_t epoch) { last_ = std::max(last_, epoch); }
  uint64_t Last() const { return last_; }
 private:
  uint64_t last_ = 0;
};
}  // namespace
```

- [ ] **Step 4: Run to verify pass** — `./build/tests/unit/test_replication_cursor` — PASS.

- [ ] **Step 5: Commit**

```bash
git add src/kvstore-node/src/replication/replication_cursor.h \
        src/tests/unit/replication/replication_cursor_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(A9): replication cursor (monotonic epoch, resume dedup)"
```

---

### Task 5: `ReplicationConsumer` — subscribe → filter → fetch → commit

**Files:**
- Create: `src/kvstore-node/src/replication/replication_consumer.h`, `.cpp`
- Modify: `src/kvstore-node/CMakeLists.txt` (add the .cpp to `NODE_CORE_SRCS`)
- Test: `src/tests/unit/replication/replication_consumer_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 `ReplicaFetch`, Task 2 `SealByChunkPath`, Task 3 `IsWarm`, Task 4 `ReplicationCursor`, and the primary node's event subscription (`HeadlessNode::SubscribeEvents(EventCallback, void*)`).
- Produces:
  ```cpp
  namespace kvcache::node::replication {
  class ReplicationConsumer {
   public:
    struct Options { WarmPolicy warm; };
    // primary + standby are borrowed; must outlive the consumer.
    ReplicationConsumer(HeadlessNode& primary, HeadlessNode& standby, Options);
    // Apply one event synchronously (test/seam entry): warm-filter, cursor,
    // ReplicaFetch from primary, commit to standby. Returns true if a chunk
    // was replicated, false if filtered/duplicate/not-found (all benign).
    bool ApplyEvent(const prefix::Event& ev);
    uint64_t CursorEpoch() const;
   private:
    HeadlessNode& primary_;  HeadlessNode& standby_;
    Options opts_;  ReplicationCursor cursor_;
  };
  }
  ```
  *(The live subscribe/poll loop + fetch-worker thread wrap `ApplyEvent`; keeping `ApplyEvent` a pure synchronous step is what makes the integration test deterministic. The threaded loop is a thin wrapper — subscribe, `Poll` in a loop, call `ApplyEvent`.)*

- [ ] **Step 1: Write the failing integration test**

```cpp
// src/tests/unit/replication/replication_consumer_test.cpp
TEST(ReplicationConsumer, MirrorsWarmChunksNotColdOrDuplicate) {
    HeadlessNode primary, standby;
    ReplicationConsumer rc(primary, standby, {.warm = {.max_tier = 1}});

    // Seal a warm chunk on primary; capture the ADD event it emits.
    prefix::Event add = SealAndCaptureEvent(primary, /*tokens*/{0..31}, /*payload*/0xAB);
    ASSERT_EQ(add.type, /*ADD*/1);

    EXPECT_TRUE(rc.ApplyEvent(add)) << "warm chunk replicated";
    // Standby now resolves the same tokens.
    EXPECT_EQ(LookupMatched(standby, {0..31}), 32u);

    // Duplicate (same epoch) is a no-op.
    EXPECT_FALSE(rc.ApplyEvent(add));

    // A cold-tier ADD is filtered.
    prefix::Event cold = add; cold.tier = 3; cold.epoch = add.epoch + 1;
    EXPECT_FALSE(rc.ApplyEvent(cold));

    // An ADD for a locator the primary already evicted → skipped cleanly.
    prefix::Event gone{}; gone.type=1; gone.tier=1; gone.epoch=add.epoch+2;
    gone.locator = MakeLocator(7,7,{500,501});   // never sealed on primary
    EXPECT_FALSE(rc.ApplyEvent(gone));
}
```

- [ ] **Step 2: Run to verify fail** — `cmake --build build --target test_replication_consumer && ./build/tests/unit/test_replication_consumer` — FAIL (class missing).

- [ ] **Step 3: Implement `ApplyEvent`**

```cpp
// replication_consumer.cpp
bool ReplicationConsumer::ApplyEvent(const prefix::Event& ev) {
    if (!IsWarm(ev, opts_.warm)) return false;
    if (!cursor_.ShouldApply(ev.epoch)) return false;      // dedup / resume
    HeadlessNode::ReplicaChunk rc;
    if (primary_.ReplicaFetch(ev.locator, &rc) != KV_OK) {  // evicted → skip
        cursor_.Advance(ev.epoch);                          // don't retry forever
        return false;
    }
    kv_handle_t h; kv_buffer_desc_t slot;
    if (standby_.Reserve(&ev.locator, rc.bytes.size(), &h, &slot) != KV_OK) return false;
    std::memcpy(slot.addr, rc.bytes.data(), rc.bytes.size());
    standby_.Publish(h, {}, rc.bytes.size());
    if (standby_.SealByChunkPath(h, rc.chunk_path) != KV_OK) return false;
    cursor_.Advance(ev.epoch);
    return true;
}
uint64_t ReplicationConsumer::CursorEpoch() const { return cursor_.Last(); }
```
*(`prefix::Event.locator` is a `kv_locator_t`; confirm the field name in `kv_event_stream.h` `Event` and adapt. `Reserve` takes `const kv_locator_t*`.)*

- [ ] **Step 4: Run to verify pass** — `./build/tests/unit/test_replication_consumer` — PASS (all assertions).

- [ ] **Step 5: Commit**

```bash
git add src/kvstore-node/src/replication/replication_consumer.h \
        src/kvstore-node/src/replication/replication_consumer.cpp \
        src/kvstore-node/CMakeLists.txt \
        src/tests/unit/replication/replication_consumer_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(A9): ReplicationConsumer — warm-filter + ReplicaFetch + SealByChunkPath commit"
```

---

### Task 6: Live subscribe/poll loop + full-suite regression

**Files:**
- Modify: `src/kvstore-node/src/replication/replication_consumer.{h,cpp}` (add `Start()/Stop()` thread that subscribes to the primary's `EventStream` and drives `ApplyEvent`)
- Test: extend `replication_consumer_test.cpp` with a live-loop test

**Interfaces:**
- Consumes: `HeadlessNode::SubscribeEvents(EventCallback, void*)` (or `EventStream::Subscribe`/`Poll` if driving the ring directly).
- Produces: `void Start(); void Stop();` on `ReplicationConsumer` (a background thread; `Stop` joins). Fetch-worker pool is a bounded queue feeding `ApplyEvent`; for the MVP a single worker thread is sufficient and keeps ordering simple.

- [ ] **Step 1: Write the failing live test** — seal 3 warm + 1 cold chunk on the primary *after* `rc.Start()`; poll (with a bounded wait) until `standby.Lookup` finds all 3 warm and never the cold; then `rc.Stop()`. Use the repo's existing `WaitFor` poll helper (see `etcd_view_loader_test`/`node_directory` tests) rather than a fixed sleep.
- [ ] **Step 2: Run — FAIL** (`Start/Stop` missing).
- [ ] **Step 3: Implement `Start/Stop`** — background thread: `auto h = primary_.SubscribeEvents(cb,this)` (or `EventStream::Subscribe` + `Poll` loop); on each event call `ApplyEvent`; `Stop()` sets a stop flag, unsubscribes, joins. (Mirror the `ClusterViewWatcher` refresh-thread lifecycle in `src/kvagent/src/router/cluster_view.cpp`.)
- [ ] **Step 4: Run — PASS.** Then full regression: `cd build && cmake --build . -j4 && ctest` — expect the prior green count + the new replication tests, 0 failures.
- [ ] **Step 5: Commit**

```bash
git add src/kvstore-node/src/replication/ src/tests/unit/replication/replication_consumer_test.cpp
git commit -m "feat(A9): ReplicationConsumer live subscribe/poll loop + full-suite green"
```

---

### Task 7 (follow-on, out of this plan's build): gRPC `ReplicaFetch` surface

The cross-*process*/cross-cluster surface — a `ReplicaFetch` RPC on `NodeDataService` + a client the consumer uses when primary and standby are separate processes — is **grpc-gated** (`KVCACHE_HAVE_GRPC`) and needs a second running cluster to exercise end-to-end. It is documented here as the next increment but is **not** part of this locally-verifiable plan. When built, it wraps Task 1's `ReplicaFetch` behind `VerifyInternalPeer` (A11) and the consumer dials it instead of calling the in-process `HeadlessNode`. See spec §6/§9.

---

## Self-Review

**Spec coverage:** §2 R1–R5 → the consumer is standby-driven/primary-oblivious (R5), warm-set via Task 3 (R2), async/synchronous-`ApplyEvent`-wrapped-by-thread (R3), shared-domain auth deferred to the gRPC follow-on (R4, Task 7); topology R1 is a deployment fact. §3 data flow → Tasks 1/2/5. §4 components → Tasks 3/4/5/6. §6 interfaces → Task 1 (`ReplicaFetch`; refined to return `chunk_path`), Task 2 (commit), `warm` reuse of existing `tier` field (no ABI change — better than the spec's proposed new field; spec §6 updated). §7 error handling → Task 5 (evict→skip, dedup) + Task 6 (loop). §8 testing → Tasks 1–6 all local. **Gap:** the A11 `replica` workload-kind + gRPC auth live in Task 7 (follow-on) — matches the spec's "gRPC surface is follow-on."

**Placeholder scan:** the parenthetical "verify against `dram_tier.h`/`kv_event_stream.h` field names" notes are grounding instructions, not placeholders — every step has real code. No TBD/TODO in deliverables.

**Type consistency:** `ReplicaChunk{chunk_path: vector<ChunkHash>, bytes: vector<uint8_t>}` produced by Task 1, consumed verbatim by Task 5; `SealByChunkPath(handle, vector<ChunkHash>)` in Tasks 2 + 5 match; `WarmPolicy{max_tier}` / `IsWarm` in Tasks 3 + 5 match; `ReplicationCursor::{ShouldApply,Advance,Last}` in Tasks 4 + 5 match.

**Spec deltas to fold back:** A9 spec §3 step 4 (token-based commit → chunk_path-based) and §6 (`warm` new field → reuse existing `tier`). Update the spec for consistency.
