# A9 gRPC ReplicaFetch Surface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the cross-*process* surface for A9 DR replication — a `ReplicaFetch` RPC on `NodeData` (gated by A11 `VerifyInternalPeer`) plus a client seam so a standby's `ReplicationConsumer` can pull warm chunks from a primary running in a separate process, not just in-process.

**Architecture:** The in-process A9 core already ships (`HeadlessNode::ReplicaFetch`, `ReplicationConsumer`). This adds: (1) a `ReplicaFetch` RPC on the `NodeData` gRPC service whose handler enforces `VerifyInternalPeer` (internal-workload SVID only) then delegates to the in-process `HeadlessNode::ReplicaFetch`; (2) a small `ReplicaSource` abstraction the consumer fetches through — `InProcessReplicaSource` (wraps a `HeadlessNode&`, preserving today's behavior) and `GrpcReplicaSource` (dials a `NodeData::Stub`, marshals the RPC). `ReplicationConsumer` is refactored to hold a `ReplicaSource&` instead of `HeadlessNode& primary_`, with an in-process convenience constructor so every existing A9-core test and the live loop keep working unchanged. All new gRPC code is behind `KVCACHE_HAVE_GRPC`.

**Tech Stack:** C++20, CMake+Ninja, gRPC + protobuf (already in the toolchain: `protoc`, `grpc_cpp_plugin`, `gRPC_DIR` set), GoogleTest. Verified locally via **in-process gRPC loopback** (a real `grpc::Server` + `NodeData::Stub` in one test process, the exact pattern of `src/tests/unit/grpc/node_data_service_test.cpp`). True multi-*cluster* network E2E stays deferred (needs a second cluster).

**Spec:** `docs/superpowers/specs/2026-07-01-a9-cross-cluster-dr-federation-design.md` §6 ("`ReplicaFetch` ... gated by `VerifyInternalPeer` (A11) on the gRPC surface") and §9 (gRPC surface is the follow-on). This plan builds that follow-on.

## Global Constraints

- C++20; CMake ≥ 3.28 in practice.
- All new gRPC code compiles ONLY under `KVCACHE_HAVE_GRPC` (mirror the existing `#if`/CMake gating in `node_data_service.{h,cpp}` and `src/kvstore-node/CMakeLists.txt`). Non-gRPC builds must be byte-for-byte unchanged.
- No new third-party dependency; reuse the in-tree gRPC/proto stack, `HeadlessNode::ReplicaFetch` (A9 core), `MtlsRegistry::VerifyInternalPeer` (A11).
- Additive: the new RPC + messages append to `node.proto`; the `ReplicaSource` refactor keeps `ReplicationConsumer`'s existing behavior and public convenience constructor. No existing RPC, message, or public signature changes meaning.
- `ChunkHash = std::array<uint8_t,8>`; `chunk_path = std::vector<ChunkHash>`. On the wire a chunk_path is a `repeated fixed64` (each hash reinterpreted as one little-endian uint64) OR `repeated bytes` — pick `repeated bytes` (each exactly 8 bytes) to avoid endianness coupling; the handler/client memcpy 8 bytes per element.
- **Fail-closed auth:** the handler denies (gRPC `PERMISSION_DENIED`) any peer that is not a verified internal workload; an unauthenticated/absent peer cert is a deny.
- TDD: failing test → run-fail → minimal impl → run-pass → commit, per step.

## File Structure

- `src/core/proto/node.proto` — add `rpc ReplicaFetch` + `ReplicaFetchRequest`/`ReplicaFetchResponse`. Task 1.
- `src/kvstore-node/src/grpc/node_data_service.{h,cpp}` — the `ReplicaFetch` handler (auth gate + delegate). Task 2.
- `src/core-abi/src/replication/replica_source.h` — `ReplicaSource` interface + `InProcessReplicaSource` (header-only). Task 3.
- `src/core-abi/src/replication/replication_consumer.{h,cpp}` — refactor to hold `ReplicaSource&`; keep in-process convenience ctor. Task 3.
- `src/kvstore-node/src/grpc/grpc_replica_source.{h,cpp}` — `GrpcReplicaSource` (dials `NodeData::Stub`). Task 4.
- Tests: `src/tests/unit/grpc/replica_fetch_rpc_test.cpp` (Task 2 handler + Task 4 loopback), `replica_source` coverage folded into existing replication tests (Task 3).
- `src/core/proto/CMakeLists.txt` (proto regen — usually automatic), `src/kvstore-node/CMakeLists.txt`, `src/tests/unit/CMakeLists.txt`, `README.md`.

---

### Task 1: `ReplicaFetch` RPC + messages on the `NodeData` proto

**Files:**
- Modify: `src/core/proto/node.proto`
- Test: none (proto-only; exercised by Tasks 2/4). Verified by a successful codegen build.

**Interfaces:**
- Produces (proto):
  ```protobuf
  // In service NodeData { ... }:
  // Primary-side read for A9 DR replication: resolve a locator to its sealed
  // chunk_path + bytes. Read-only; gated by VerifyInternalPeer (A11) in the
  // handler. Deferred cross-cluster surface of the in-process ReplicaFetch.
  rpc ReplicaFetch (ReplicaFetchRequest) returns (ReplicaFetchResponse);

  message ReplicaFetchRequest {
    Locator locator = 1;   // the sealed chunk's locator (prefix_hash etc.)
  }
  message ReplicaFetchResponse {
    int32          status     = 1;  // KV_OK (0) on hit; KV_E_NOT_FOUND otherwise
    repeated bytes chunk_path = 2;  // K entries, each exactly 8 bytes (a ChunkHash)
    bytes          data       = 3;  // the sealed KV bytes (empty on miss)
  }
  ```

- [ ] **Step 1: Add the RPC + messages**

In `src/core/proto/node.proto`, add the `rpc ReplicaFetch(...)` line inside `service NodeData { ... }` (after `Subscribe`), and the two messages near the other request/response messages. Reuse the existing `Locator` message (already defined in this file and used by `LookupResponse`/`SealResponse` — confirm its field layout and that `HeadlessNode`/the handler can build a `kv_locator_t` from it; the Seal/Lookup handlers already convert between `Locator` proto and `kv_locator_t`, so mirror that conversion).

- [ ] **Step 2: Regenerate + build to verify codegen**

Run: `cmake --build build --target kvstore_node_grpc` (or the proto/codegen target — check `src/core/proto/CMakeLists.txt` for the generated target name; `node.pb.h`/`node.grpc.pb.h` regenerate as part of the build).
Expected: builds clean; `ReplicaFetchRequest`, `ReplicaFetchResponse`, and `NodeData::Service::ReplicaFetch` / `NodeData::Stub::ReplicaFetch` symbols now exist. (A grep of the generated `build/**/node.grpc.pb.h` for `ReplicaFetch` confirms.)

- [ ] **Step 3: Commit**

```bash
git add src/core/proto/node.proto
git commit -m "feat(A9): ReplicaFetch RPC + messages on NodeData proto"
```

---

### Task 2: `ReplicaFetch` server handler — VerifyInternalPeer gate + delegate

**Files:**
- Modify: `src/kvstore-node/src/grpc/node_data_service.h` (declare the override)
- Modify: `src/kvstore-node/src/grpc/node_data_service.cpp` (implement)
- Test: `src/tests/unit/grpc/replica_fetch_rpc_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `HeadlessNode::ReplicaFetch(const kv_locator_t&, HeadlessNode::ReplicaChunk*)` (A9 core, in-process); `MtlsRegistry::VerifyInternalPeer(const CertInfo&, const std::string& required_trust_domain)`; the `NodeData::Service` base.
- Produces:
  ```cpp
  ::grpc::Status ReplicaFetch(::grpc::ServerContext* context,
                              const kvcache::proto::ReplicaFetchRequest* request,
                              kvcache::proto::ReplicaFetchResponse* response) override;
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// src/tests/unit/grpc/replica_fetch_rpc_test.cpp  (guarded by KVCACHE_HAVE_GRPC)
// In-process gRPC loopback, modeled on node_data_service_test.cpp:
//   - stand up a NodeDataServiceImpl over a real grpc::Server on 127.0.0.1:0
//   - seal a chunk on the backing HeadlessNode
//   - dial via NodeData::NewStub and call ReplicaFetch
// Two cases: (a) a verified internal peer gets KV_OK + chunk_path + bytes;
//            (b) a non-internal / unauthenticated peer gets PERMISSION_DENIED.
TEST_F(ReplicaFetchRpcTest, InternalPeerGetsChunkPathAndBytes) {
    SealChunkOnBackingNode(/*tokens*/{0..31}, /*payload*/0xAB);   // helper (mirror existing seal helpers)
    kvcache::proto::ReplicaFetchRequest req;
    *req.mutable_locator() = LocatorProtoFor({0..31});            // same conversion Lookup/Seal use
    kvcache::proto::ReplicaFetchResponse resp;
    grpc::ClientContext ctx;
    auto st = internal_peer_stub_->ReplicaFetch(&ctx, req, &resp);
    ASSERT_TRUE(st.ok()) << st.error_message();
    EXPECT_EQ(resp.status(), 0 /*KV_OK*/);
    EXPECT_FALSE(resp.chunk_path().empty());
    EXPECT_EQ(resp.data().size(), 32u*... /*payload size*/);
}

TEST_F(ReplicaFetchRpcTest, NonInternalPeerIsDenied) {
    kvcache::proto::ReplicaFetchRequest req;
    *req.mutable_locator() = LocatorProtoFor({0..31});
    kvcache::proto::ReplicaFetchResponse resp;
    grpc::ClientContext ctx;
    auto st = non_internal_stub_->ReplicaFetch(&ctx, req, &resp);   // stub w/o internal SVID
    EXPECT_EQ(st.error_code(), grpc::StatusCode::PERMISSION_DENIED);
}
```
*(Model the fixture on `node_data_service_test.cpp`: `ServerBuilder` + `RegisterService(&impl)` + `BuildAndStart()` + `NodeData::NewStub(channel)`. For the auth cases: the existing service tests already exercise mTLS/peer identity — reuse whatever they use to present an internal-workload SVID vs a non-internal peer. If the loopback test uses insecure creds (no cert), then "no verified internal peer" must be `PERMISSION_DENIED` — assert that as case (b), and for case (a) inject a verified identity through the same seam the existing tests use to simulate an authenticated internal peer. Read the fixture before writing; do NOT invent an auth path.)*

- [ ] **Step 2: Run to verify fail** — `cmake --build build --target test_replica_fetch_rpc && ./build/tests/unit/test_replica_fetch_rpc` — FAIL (handler not implemented → `UNIMPLEMENTED`, or compile error on the missing override).

- [ ] **Step 3: Declare + implement the handler**

Declare the override in `node_data_service.h` next to `Fetch`. Implement in `node_data_service.cpp`:
```cpp
::grpc::Status NodeDataServiceImpl::ReplicaFetch(
    ::grpc::ServerContext* context,
    const kvcache::proto::ReplicaFetchRequest* request,
    kvcache::proto::ReplicaFetchResponse* response) {
    // A11 internal-peer gate. Extract the peer cert/identity the same way the
    // other guarded handlers do (read how this class already obtains peer
    // identity — e.g. a helper over context->auth_context()), then:
    auto id = VerifyPeerIsInternal(context);   // returns false/nullopt → deny
    if (!id) {
        return ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED,
                              "ReplicaFetch requires a verified internal workload peer");
    }
    kv_locator_t loc = LocatorFromProto(request->locator());   // reuse existing conversion
    HeadlessNode::ReplicaChunk rc;
    int rv = node_->ReplicaFetch(loc, &rc);                    // node_ = the backing HeadlessNode
    response->set_status(rv);
    if (rv == KV_OK) {
        for (const auto& h : rc.chunk_path) {
            response->add_chunk_path(std::string(reinterpret_cast<const char*>(h.data()), h.size()));
        }
        response->set_data(std::string(rc.bytes.begin(), rc.bytes.end()));
    }
    return ::grpc::Status::OK;   // transport OK; app-level miss is status=KV_E_NOT_FOUND in the body
}
```
*(Ground every helper against the real class: how `node_` / the backing `HeadlessNode` is referenced, the existing `LocatorFromProto`/`ProtoFromLocator` conversion used by Lookup/Seal, and the existing peer-identity extraction. `VerifyPeerIsInternal` should wrap `MtlsRegistry::VerifyInternalPeer` over the peer cert obtained from `context` — if the class already has an internal-peer check for other RPCs, reuse it; otherwise add a small private helper. Gate the whole method body under `KVCACHE_HAVE_GRPC` consistent with the rest of the file.)*

- [ ] **Step 4: Run to verify pass** — `./build/tests/unit/test_replica_fetch_rpc` — PASS (both cases). Then regression: `cd build && ctest -R "node_data_service|grpc" --output-on-failure`.

- [ ] **Step 5: Commit**

```bash
git add src/kvstore-node/src/grpc/node_data_service.h src/kvstore-node/src/grpc/node_data_service.cpp \
        src/tests/unit/grpc/replica_fetch_rpc_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(A9): ReplicaFetch gRPC handler — VerifyInternalPeer gate + delegate to HeadlessNode"
```

---

### Task 3: `ReplicaSource` abstraction + refactor `ReplicationConsumer` to fetch through it

**Files:**
- Create: `src/core-abi/src/replication/replica_source.h` (header-only interface + in-process impl)
- Modify: `src/core-abi/src/replication/replication_consumer.h` / `.cpp`
- Test: extend `src/tests/unit/replication/replication_consumer_test.cpp` (all existing cases must still pass through the refactor)

**Interfaces:**
- Produces:
  ```cpp
  namespace kvcache::node::replication {
  // How the consumer obtains a chunk for a locator — in-process or remote.
  class ReplicaSource {
   public:
    virtual ~ReplicaSource() = default;
    virtual int Fetch(const kv_locator_t& locator,
                      abi::HeadlessNode::ReplicaChunk* out) = 0;   // KV_OK / KV_E_NOT_FOUND
  };
  // Default: read the primary in-process (today's behavior, zero overhead).
  class InProcessReplicaSource final : public ReplicaSource {
   public:
    explicit InProcessReplicaSource(abi::HeadlessNode& primary) : primary_(primary) {}
    int Fetch(const kv_locator_t& l, abi::HeadlessNode::ReplicaChunk* o) override {
        return primary_.ReplicaFetch(l, o);
    }
   private:
    abi::HeadlessNode& primary_;
  };
  }
  ```
- `ReplicationConsumer` changes: replace the `HeadlessNode& primary_` member with `ReplicaSource& source_`. Keep the existing constructor `ReplicationConsumer(HeadlessNode& primary, HeadlessNode& standby, Options)` as a **convenience overload** that internally owns an `InProcessReplicaSource` (store it as a member `std::optional`/`unique_ptr` so its lifetime matches the consumer) and points `source_` at it — so every existing caller/test is unchanged. Add a second constructor `ReplicationConsumer(ReplicaSource& source, HeadlessNode& standby, Options)` for the remote case. `ApplyEvent` calls `source_.Fetch(...)` instead of `primary_.ReplicaFetch(...)`. The live `Start()` loop still subscribes to a `HeadlessNode&` primary for events (event subscription stays in-process for this plan — only the chunk *fetch* is abstracted); keep the primary reference for `SubscribeEvents` in the in-process ctor, and document that the remote-events path is a further follow-on.

- [ ] **Step 1: Write the failing test**

Add to `replication_consumer_test.cpp` a case that drives the consumer through an explicit `InProcessReplicaSource`:
```cpp
TEST(ReplicationConsumer, FetchesThroughReplicaSource) {
    HeadlessNode primary, standby;                 // (or the shared-singleton pattern the file uses)
    InProcessReplicaSource src(primary);
    ReplicationConsumer rc(src, standby, {.warm = {.max_tier = /*Dram*/3}});
    prefix::Event add = SealAndCaptureEvent(primary, {0..31}, 0xAB);
    EXPECT_TRUE(rc.ApplyEvent(add));
    EXPECT_EQ(LookupMatched(standby, {0..31}), 32u);
}
```
The existing tests (which use the `(HeadlessNode&, HeadlessNode&, Options)` ctor) must continue to compile and pass unchanged — that is the refactor's safety net.

- [ ] **Step 2: Run to verify fail** — `cmake --build build --target test_replication_consumer && ./build/tests/unit/test_replication_consumer` — FAIL (no `ReplicaSource` ctor / header).

- [ ] **Step 3: Implement `replica_source.h` + refactor the consumer**

Create `replica_source.h` as above. In `replication_consumer.{h,cpp}`: add the `source_` reference member + the owned-`InProcessReplicaSource` (via `std::unique_ptr<InProcessReplicaSource> owned_source_` set by the convenience ctor); add the `(ReplicaSource&, HeadlessNode&, Options)` ctor; change `ApplyEvent`'s fetch call to `source_.Fetch(ev.locator, &rc)`. Keep `standby_` and the event-subscription `primary_`-for-`SubscribeEvents` as-is (the in-process ctor still takes `HeadlessNode& primary` for the event stream; the remote ctor takes a `ReplicaSource&` for fetch + still needs an event source — for this plan the remote ctor may require the caller to also pass the event-subscription primary, or document that Start() is in-process-only in the remote variant; keep it simple: the remote ctor is fetch-only + `ApplyEvent`, and `Start()` remains available only on the in-process ctor path).

- [ ] **Step 4: Run to verify pass** — `./build/tests/unit/test_replication_consumer` — PASS (new case + ALL existing cases green — the refactor preserved behavior). Then `cd build && ctest -R "replication|replica" --output-on-failure`.

- [ ] **Step 5: Commit**

```bash
git add src/core-abi/src/replication/replica_source.h \
        src/core-abi/src/replication/replication_consumer.h src/core-abi/src/replication/replication_consumer.cpp \
        src/tests/unit/replication/replication_consumer_test.cpp
git commit -m "feat(A9): ReplicaSource seam — consumer fetches via in-process or remote source"
```

---

### Task 4: `GrpcReplicaSource` client + in-process loopback test; README + full regression

**Files:**
- Create: `src/kvstore-node/src/grpc/grpc_replica_source.{h,cpp}` (guarded by `KVCACHE_HAVE_GRPC`)
- Test: extend `src/tests/unit/grpc/replica_fetch_rpc_test.cpp` with an end-to-end `GrpcReplicaSource → RPC → HeadlessNode` case
- Modify: `src/kvstore-node/CMakeLists.txt`, `src/tests/unit/CMakeLists.txt`, `README.md`

**Interfaces:**
- Consumes: `NodeData::Stub` (generated), Task 3 `ReplicaSource`, Task 1 messages.
- Produces:
  ```cpp
  namespace kvcache::node::grpc_client {
  // A ReplicaSource that pulls chunks from a remote primary over the
  // ReplicaFetch RPC. Marshals kv_locator_t → ReplicaFetchRequest and
  // ReplicaFetchResponse → ReplicaChunk. Constructed with a NodeData::Stub
  // (built by the caller with the right mTLS creds so the peer authenticates
  // as an internal workload).
  class GrpcReplicaSource final : public replication::ReplicaSource {
   public:
    explicit GrpcReplicaSource(std::shared_ptr<kvcache::proto::NodeData::Stub> stub);
    int Fetch(const kv_locator_t& locator,
              abi::HeadlessNode::ReplicaChunk* out) override;
   private:
    std::shared_ptr<kvcache::proto::NodeData::Stub> stub_;
  };
  }
  ```

- [ ] **Step 1: Write the failing test** — add to `replica_fetch_rpc_test.cpp`:
```cpp
TEST_F(ReplicaFetchRpcTest, GrpcReplicaSourceRoundTrips) {
    SealChunkOnBackingNode({0..31}, 0xCD);
    GrpcReplicaSource src(internal_peer_stub_shared_);   // stub authenticating as internal
    kv_locator_t loc = LocatorFor({0..31});
    HeadlessNode::ReplicaChunk rc;
    ASSERT_EQ(src.Fetch(loc, &rc), KV_OK);
    EXPECT_FALSE(rc.chunk_path.empty());
    EXPECT_EQ(rc.bytes.size(), /*payload size*/);
}
```

- [ ] **Step 2: Run to verify fail** — build the test target — FAIL (`GrpcReplicaSource` missing).

- [ ] **Step 3: Implement `grpc_replica_source.{h,cpp}`**
```cpp
int GrpcReplicaSource::Fetch(const kv_locator_t& locator,
                             abi::HeadlessNode::ReplicaChunk* out) {
    kvcache::proto::ReplicaFetchRequest req;
    *req.mutable_locator() = ProtoFromLocator(locator);   // reuse the conversion
    kvcache::proto::ReplicaFetchResponse resp;
    ::grpc::ClientContext ctx;
    ::grpc::Status st = stub_->ReplicaFetch(&ctx, req, &resp);
    if (!st.ok()) return KV_E_NOT_FOUND;                  // transport/auth failure → treat as miss (benign)
    if (resp.status() != KV_OK) return resp.status();
    out->chunk_path.clear();
    for (const auto& h : resp.chunk_path()) {
        node::prefix::ChunkHash ch{};
        if (h.size() == ch.size()) std::memcpy(ch.data(), h.data(), ch.size());
        out->chunk_path.push_back(ch);
    }
    out->bytes.assign(resp.data().begin(), resp.data().end());
    return KV_OK;
}
```

- [ ] **Step 4: Run to verify pass** — the loopback test round-trips a real chunk through the RPC into a `ReplicaChunk`. Then wire `grpc_replica_source.cpp` into the gRPC-gated node target in `src/kvstore-node/CMakeLists.txt` (mirror where `node_data_service.cpp` is listed under the `KVCACHE_HAVE_GRPC` block).

- [ ] **Step 5: README + FULL regression** — add the A9-gRPC-surface line to `README.md` (match prior A-item format; no VERSION/badge). Run `cd build && cmake --build . -j4 && ctest --output-on-failure` — prior green count + the new gRPC tests, 0 failures. Capture the summary.

- [ ] **Step 6: Commit**
```bash
git add src/kvstore-node/src/grpc/grpc_replica_source.h src/kvstore-node/src/grpc/grpc_replica_source.cpp \
        src/tests/unit/grpc/replica_fetch_rpc_test.cpp src/kvstore-node/CMakeLists.txt src/tests/unit/CMakeLists.txt README.md
git commit -m "feat(A9): GrpcReplicaSource client + in-process loopback E2E; full-suite green"
```

---

### Deferred / follow-on

- **True multi-cluster network E2E** — two separate node processes/clusters over a real network with production mTLS certs. Needs a 2nd cluster; out of local scope.
- **Remote event subscription** — this plan abstracts the chunk *fetch* (`ReplicaSource`); the event *stream* the consumer's `Start()` loop subscribes to is still an in-process `HeadlessNode`. A remote consumer would also dial the primary's `Subscribe` RPC (M-2) — a further follow-on wiring the two remote seams together.
- **Consumer-side connection management** — retry/reconnect/backoff on the `GrpcReplicaSource` stub, channel pooling, and mapping the event's `node_id` → the owning primary's address (the consumer currently assumes one primary).

---

## Self-Review

**Spec coverage:** §6 "`ReplicaFetch` ... gated by `VerifyInternalPeer` (A11) on the gRPC surface" → Task 1 (RPC) + Task 2 (handler + auth gate). §6 "consumer dials it instead of calling the in-process `HeadlessNode`" → Task 3 (`ReplicaSource` seam) + Task 4 (`GrpcReplicaSource`). §9 "gRPC surface is follow-on, needs a second cluster to exercise E2E" → this plan builds the surface + verifies via in-process loopback; true multi-cluster E2E is explicitly deferred. The A11 `replica`/`node` workload-kind acceptance in `VerifyInternalPeer` (spec §6) is satisfied by reusing the existing `VerifyInternalPeer` (it already accepts `kNode`); no new workload kind needed.

**Placeholder scan:** the test bodies use `{0..31}` shorthand and "payload size" as stand-ins for the concrete token/size helpers the existing test file already provides — the implementer wires them to the real helpers (grounding notes say so), not TBDs. Every impl step has real code. The proto-only Task 1 has no test by design (exercised by 2/4) — noted, not a gap.

**Type consistency:** `ReplicaFetchRequest{locator}` / `ReplicaFetchResponse{status, chunk_path (repeated bytes, 8B each), data}` defined in Task 1, marshalled by the Task 2 handler and unmarshalled by the Task 4 client identically (8-byte `ChunkHash` memcpy both directions). `ReplicaSource::Fetch(const kv_locator_t&, HeadlessNode::ReplicaChunk*) -> int` defined in Task 3, implemented by `InProcessReplicaSource` (Task 3) and `GrpcReplicaSource` (Task 4). `ReplicationConsumer`'s new `(ReplicaSource&, HeadlessNode&, Options)` ctor (Task 3) is what the remote path uses; the existing `(HeadlessNode&, HeadlessNode&, Options)` ctor is preserved so all prior tests compile unchanged.

**Risk register:** the one behavior-changing task is the Task 3 consumer refactor — mitigated by keeping the existing constructor + requiring every existing `replication_consumer_test` case to stay green (the refactor's safety net). The auth path in Task 2 is grounded against the existing service test's peer-identity mechanism rather than invented. All gRPC code is `KVCACHE_HAVE_GRPC`-gated so non-gRPC builds are untouched.
