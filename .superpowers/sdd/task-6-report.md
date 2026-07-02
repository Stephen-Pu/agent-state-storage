# Task 6 Report — ReplicationConsumer live subscribe/poll loop + kvcache lib homing

Commit: `1f51cf0`
Branch: `main`
Full-suite result: **479/479 tests passed, 0 failures** (16 integration tests skipped — same as all prior tasks)

---

## Final file layout after Part A move

```
src/core-abi/src/replication/
  replication_consumer.h        ← moved from kvstore-node/src/replication/
  replication_consumer.cpp      ← moved from kvstore-node/src/replication/

src/kvstore-node/src/replication/   (unchanged — these stay here)
  warm_filter.h
  replication_cursor.h
```

`warm_filter.h` and `replication_cursor.h` remain in `kvstore-node/src/replication/` because `core-abi/CMakeLists.txt` already adds `${CMAKE_SOURCE_DIR}/kvstore-node/src` to its PRIVATE include path, so `#include "replication/warm_filter.h"` and `#include "replication/replication_cursor.h"` continue to resolve from both the kvcache lib and the test binary.

---

## How kvcache links the consumer

`src/core-abi/CMakeLists.txt` — `kvcache` SHARED library source list now includes:
```cmake
add_library(kvcache SHARED
    src/kv_abi.cpp
    src/kv_status.cpp
    src/headless_node.cpp
    src/ctx_options.cpp
    src/replication/replication_consumer.cpp  # A9 DR — warm-standby consumer
)
```

`src/tests/unit/CMakeLists.txt` — `test_replication_consumer` compiles `replication_consumer.cpp` **directly** (from its new `core-abi/src/replication/` path) alongside `headless_node.cpp`, `ctx_options.cpp`, and `kv_status.cpp`. This mirrors the existing pattern for `test_replica_fetch` and `test_seal_by_chunk_path`: compiling these TUs directly gives the test access to non-ABI C++ symbols (ReplicaFetch, SealByChunkPath, SubscribeEvents) that the hidden-visibility shared lib exports only as C ABI. The test links `kvstore_node_core` + `kvcache_common` for the full subsystem stack.

---

## SubscribeEvents / Unsubscribe mechanism + thread lifecycle

`HeadlessNode::SubscribeEvents(EventCallback cb, void* user)` registers a callback and returns a `SubscriptionId` (`uint64_t`). Internally it calls `events_->Subscribe()` to get a `SubscriberHandle` (SPSC ring), then spawns a **per-subscription poller thread** (`EventSub::poller`) that busy-polls `events_->Poll(ring_handle, &ev)` in a tight loop and delivers each event to `cb(ev, user)`. `UnsubscribeEvents(id)` sets the `EventSub::stop` flag, then **joins** the poller thread — guaranteeing no further `cb` calls after it returns.

`ReplicationConsumer::Start()`:
1. CAS `running_` false→true (idempotent guard).
2. Calls `primary_.SubscribeEvents(EventCbDispatch, this)` — stores the `SubscriptionId` in `sub_id_`.
3. Spawns a **sentinel thread** (`thread_`) that spins on `running_` with 5 ms sleeps. Its only purpose is to give `Stop()` a clean joinable handle; all real event work happens on HeadlessNode's internal poller thread via `EventCbDispatch`.

`ReplicationConsumer::Stop()`:
1. CAS `running_` true→false (idempotent guard).
2. Calls `primary_.UnsubscribeEvents(sub_id_)` — joins the HeadlessNode internal poller; no further `EventCbDispatch` calls after this returns.
3. Joins `thread_` (sentinel, now sees `running_=false` and exits its spin).

Destructor calls `Stop()`.

Lifecycle mirrors `ClusterViewWatcher` (`src/kvagent/src/router/cluster_view.cpp`): CAS-guarded Start/Stop, atomic `running_` flag, `thread_.joinable()` check before join.

---

## Concurrency guards

| Resource | Protection |
|---|---|
| `running_` | `std::atomic<bool>`, acq/rel CAS in Start/Stop |
| `sub_id_` | Written by Start() before thread spawn; read by Stop() after CAS serialises; no race |
| `cursor_` (in ApplyEvent) | Serialised by HeadlessNode's internal per-subscription SPSC poller — only one thread calls `EventCbDispatch` at a time; no additional lock needed |
| Standby writes in ApplyEvent | Same — called only from the HeadlessNode poller thread |
| Stop vs in-flight callback | `UnsubscribeEvents` joins the poller before returning → callback is fully quiesced before `this` can be destroyed |

No deadlock risk: `UnsubscribeEvents` does not hold any `ReplicationConsumer` lock, and `EventCbDispatch` does not call back into `Stop()`.

---

## Live-test wait mechanism

`WaitFor(pred, budget=2s)` — defined locally in `replication_consumer_test.cpp`:
```cpp
bool WaitFor(std::function<bool()> pred,
             std::chrono::milliseconds budget = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}
```

Matches the pattern in `node_registry_test.cpp` and `etcd_client_test.cpp`. The live test uses it to poll `LookupMatched` for each of the 3 warm chunks with a 2-second budget per chunk (well within the observed 36 ms runtime).

---

## Full ctest summary

```
100% tests passed, 0 tests failed out of 479

Total Test time (real) =  20.77 sec
```

16 tests skipped (require external infrastructure — etcd, NVMe uring, S3, OTLP): same set as Tasks 1–5.

---

## Concerns

**Minor — single-singleton cascade in test**: Because `primary_ == standby_` in the test, every `SealByChunkPath` call (inside `ApplyEvent`) emits a new ADD event that the live loop will also process. The cursor dedup prevents re-applying the same `(epoch, locator)` pair, but each `SealByChunkPath` produces a fresh epoch, so the loop can spin replaying the same locator with increasing epochs until `Stop()` is called. `SealByChunkPath` returns `KV_OK` for `kReplaced` (same chunk_path already in ART), so the loop is idempotent and the test passes cleanly. In production, primary and standby are separate nodes — this cascade cannot occur. The test comment documents this limitation.

**Note on SubscriptionId type**: `HeadlessNode::SubscriptionId` is `uint64_t`. The header forward-declares `HeadlessNode` (to avoid pulling core-abi headers into all kvstore-node TUs that include this header), so the nested typedef is not visible. The field is declared as `uint64_t sub_id_` with an explanatory comment; the `.cpp` casts to/from `SubscriptionId` explicitly.
