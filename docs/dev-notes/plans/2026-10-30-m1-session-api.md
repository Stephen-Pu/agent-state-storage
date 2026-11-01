# M1 Session API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A durable, tenant-scoped `agentstate.v1.AgentState` gRPC service in `kvstore-node`, a Python SDK for it, and OpenAI Agents SDK + LangGraph integrations on top.

**Architecture:** Three storage primitives (items, records, blobs) in `kvstore-node/src/session/`, backed by an ordered in-memory map plus an append-only log, served by a new gRPC service on the existing server. The Python package wraps the generated stubs; integrations map each framework's persistence interface onto items, records and blobs.

**Tech Stack:** C++17, gRPC/protobuf, GoogleTest; Python ≥ 3.10, grpcio, pytest, openai-agents 0.24, langgraph 1.2 / langgraph-checkpoint 4.3, langgraph-checkpoint-conformance.

**Spec:** `docs/dev-notes/specs/2026-10-30-m1-session-api-design.md`

## Global Constraints

- Build the service only under `KVCACHE_HAVE_GRPC`; the default build must still pass without RocksDB.
- Item and record values ≤ 4 MiB; blobs chunked at 1 MiB, ≤ 4 GiB.
- TLS on ⇒ tenant from the client cert; mismatching request tenant ⇒ `PERMISSION_DENIED`. TLS off ⇒ request `tenant` required.
- Python ≥ 3.10; stubs committed under `agent_state_storage/_proto/`.
- Commits: author stephenpu@163.com, no AI attribution.

## Review Focus

1. **Crash mid-append** (torn log tail): reopen keeps every complete batch, drops the partial one. → Task 1 `TornTailIsTruncated`.
2. **Tenant crossing** with equal session ids or record keys: tenant B never sees A's data. → Task 3 `TenantsAreIsolated`.
3. **Concurrent appends** to one session: no lost or duplicated sequence numbers. → Task 3 `ConcurrentAppendsKeepEveryItem`.
4. **Keys with separator-like bytes** (`\0`, `/`, unicode) in thread ids, namespaces, session ids: no collisions between neighbours. → Task 7 `test_thread_ids_with_separators_do_not_collide`, Task 8 `test_namespace_prefix_does_not_match_longer_label`.
5. **Node restart** between an agent's turns: history, checkpoints and blobs are back. → Task 5 `test_state_survives_node_restart`.

---

### Task 1: OrderedLogStore

**Files:**
- Create: `src/kvstore-node/src/session/ordered_log_store.{h,cpp}`
- Modify: `src/kvstore-node/CMakeLists.txt` (add to `NODE_CORE_SRCS`), `src/tests/unit/CMakeLists.txt`
- Test: `src/tests/unit/session/ordered_log_store_test.cpp`

**Interfaces — Produces** (namespace `kvcache::node::session`):
```cpp
struct Mutation { enum Op : uint8_t { kPut = 1, kDel = 2 } op; std::string key, value; };
struct ScanOptions { std::string prefix, start_after, end_before; std::size_t limit = 0; bool reverse = false; };
struct KV { std::string key, value; };
class OrderedLogStore {
  static std::unique_ptr<OrderedLogStore> Open(const std::string& path, std::string* err); // "" = in memory
  bool Apply(const std::vector<Mutation>& batch, std::string* err);   // atomic, one fsynced record
  std::optional<std::string> Get(const std::string& key) const;
  std::vector<KV> Scan(const ScanOptions& o, bool* has_more) const;    // limit 0 = no limit
  std::size_t DeletePrefix(const std::string& prefix, std::string* err);
  std::size_t LiveBytes() const; std::size_t LogBytes() const;
};
```
Log record: `u32 len | u32 n | n × (u8 op | u32 klen | key | u32 vlen | value) | u32 crc32`. Compact (temp file + rename) when `LogBytes() > max(64 MiB, 2 × LiveBytes())`; tests set the threshold via `Open(path, err, compact_min_bytes)` overload.

- [ ] **Step 1: Failing tests**: `PutGetDelete`, `ScanPrefixForwardAndReverseWithLimit` (has_more true when truncated), `ScanBoundsAreExclusive`, `BatchIsAtomicAcrossReopen`, `ReopenReplaysLog`, `TornTailIsTruncated` (append garbage half-record; reopen keeps earlier batches and the next Apply succeeds), `CompactionKeepsLiveData` (threshold 1 KiB, many overwrites, LogBytes shrinks, reopen equal), `InMemoryModeNeedsNoFile`, `DeletePrefixRemovesOnlyThatPrefix`.
- [ ] **Step 2:** Build and run; expect compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `ctest --test-dir build-m1 -R ordered_log_store` passes.
- [ ] **Step 5:** Commit `feat(session): ordered append-only log store`.

### Task 2: BlobStore

**Files:** Create `src/kvstore-node/src/session/blob_store.{h,cpp}`; test `src/tests/unit/session/blob_store_test.cpp`.

**Interfaces — Produces:**
```cpp
class BlobStore {
  static std::unique_ptr<BlobStore> Open(const std::string& dir, std::string* err); // "" = in memory
  class Writer { public: virtual bool Write(const char*, std::size_t, std::string* err) = 0;
                 virtual bool Commit(std::string* err) = 0; virtual ~Writer() = default; }; // dtor without Commit discards
  std::unique_ptr<Writer> Create(const std::string& tenant, const std::string& name, std::string* err);
  bool Read(const std::string& tenant, const std::string& name,
            const std::function<bool(const char*, std::size_t)>& sink, std::string* err) const; // false+empty err = not found
  std::optional<uint64_t> Size(const std::string& tenant, const std::string& name) const;
  bool Delete(const std::string& tenant, const std::string& name);
};
```
- [ ] Tests: `RoundTripLargeBlob` (5 MiB), `UncommittedWriterLeavesNoBlob`, `OverwriteReplacesAtomically`, `TenantsDoNotShareNames`, `SurvivesReopen`, `DeleteThenMissing`. Implement (sha256 via OpenSSL when available, else BLAKE3 already in tree), run, commit `feat(session): blob store`.

### Task 3: agentstate.proto + AgentStateService

**Files:**
- Create: `src/core/proto/agentstate.proto`; `src/kvstore-node/src/session/agent_state_service.{h,cpp}`
- Modify: `src/core/proto/CMakeLists.txt` (add to PROTO_FILES and both custom-command OUTPUT lists), `src/kvstore-node/CMakeLists.txt` (add service to `kvstore_node_grpc`)
- Test: `src/tests/unit/session/agent_state_service_test.cpp` (in-process server on port 0, insecure; plus one TLS test reusing the cert generation in `grpc/grpc_server_tls_test.cpp`)

**Proto** (`package agentstate.v1`; every request carries `string tenant = 1`):
```
service AgentState {
  rpc AppendItems(AppendItemsRequest{tenant, session, repeated bytes items}) returns (AppendItemsResponse{uint64 next_seq});
  rpc GetItems(GetItemsRequest{tenant, session, uint32 limit /*0=all*/}) returns (GetItemsResponse{repeated bytes items}); // chronological
  rpc PopItem(PopItemRequest{tenant, session}) returns (PopItemResponse{bool found, bytes item});
  rpc ClearSession(ClearSessionRequest{tenant, session}) returns (ClearSessionResponse{uint64 removed});
  rpc ForkSession(ForkSessionRequest{tenant, source, target}) returns (ForkSessionResponse{uint64 copied}); // target must be empty: FAILED_PRECONDITION
  rpc WriteRecords(WriteRecordsRequest{tenant, repeated Record puts, repeated bytes deletes}) returns (WriteRecordsResponse{});
  rpc GetRecords(GetRecordsRequest{tenant, repeated bytes keys}) returns (GetRecordsResponse{repeated Record found});
  rpc ScanRecords(ScanRecordsRequest{tenant, bytes prefix, bytes start_after, bytes end_before, uint32 limit, bool reverse}) returns (ScanRecordsResponse{repeated Record records, bool has_more});
  rpc DeleteRecordPrefix(DeleteRecordPrefixRequest{tenant, bytes prefix}) returns (DeleteRecordPrefixResponse{uint64 removed});
  rpc PutBlob(stream PutBlobChunk{tenant, name /*first chunk only*/, bytes data}) returns (PutBlobResponse{uint64 size});
  rpc GetBlob(GetBlobRequest{tenant, name}) returns (stream BlobChunk{bytes data});   // NOT_FOUND if absent
  rpc StatBlob(StatBlobRequest{tenant, name}) returns (StatBlobResponse{bool exists, uint64 size});
  rpc DeleteBlob(DeleteBlobRequest{tenant, name}) returns (DeleteBlobResponse{bool existed});
}
message Record { bytes key = 1; bytes value = 2; }
```
**Interfaces — Produces:** `AgentStateServiceImpl(OrderedLogStore*, BlobStore*)`, `void EnableCertTenant(const security::MtlsRegistry* registry)` (TLS mode), `static std::string EncodeKey(const std::string& tenant, char kind, std::string_view rest)` (`u32be len | tenant | kind | rest`). Items key rest = `u32be len | session | u64be seq`. Appends to one session serialize on a striped mutex (64 stripes, hash of tenant+session).

- [ ] Tests: `AppendGetPopClear`, `GetItemsLimitReturnsLatestInOrder`, `ForkCopiesAndRefusesNonEmptyTarget`, `RecordsBatchScanAndDeletePrefix`, `BlobStreamRoundTrip` (3 MiB), `MissingTenantIsInvalidArgument`, `TenantsAreIsolated` (same session id and record key under two tenants), `ConcurrentAppendsKeepEveryItem` (8 threads × 200 appends, 1600 distinct items in order per thread), `OversizeValueIsRejected` (> 4 MiB ⇒ `INVALID_ARGUMENT` or `RESOURCE_EXHAUSTED`), `TlsTenantComesFromCertificate` (cert CN tenant; request tenant mismatch ⇒ `PERMISSION_DENIED`; empty request tenant ⇒ uses cert tenant).
- [ ] Implement, run `ctest -R agent_state_service`, run full ctest (no regressions), commit `feat(session): agentstate.v1 gRPC service`.

### Task 4: Wire into kvstore-node

**Files:** Modify `src/kvstore-node/src/grpc/grpc_server.{h,cpp}` (add `GrpcServer(const Options&, std::vector<::grpc::Service*>)`; old ctor delegates), `src/kvstore-node/src/main.cpp` (`--state-dir DIR`; open stores; register `AgentStateServiceImpl`; when all three TLS paths are set call `EnableCertTenant(nullptr)`; warn `kvstore-node: --state-dir not set; agent state is in memory only` otherwise). Test: extend `grpc_server_tls_test.cpp` with `TwoServicesShareOnePort`.

- [ ] Test, implement, full ctest, manual smoke: `build-m1/kvstore-node/kvstore-node --grpc-port 0 --metrics-port 0 --state-dir /tmp/x` prints `listening (grpc=`. Commit `feat(node): serve agentstate.v1 with --state-dir`.

### Task 5: Python SDK

**Files:**
- Create: `src/sdk/python/pyproject.toml` (name `agent-state-storage`, version `0.1.0`, deps `grpcio>=1.60`, `protobuf>=4.25`; extras `openai-agents = ["openai-agents>=0.24"]`, `langgraph = ["langgraph>=1.2", "langgraph-checkpoint>=4.3"]`, `test = [pytest, pytest-asyncio, langgraph-checkpoint-conformance]`), `src/sdk/python/agent_state_storage/{__init__,client,_aio,_errors}.py`, `_proto/` (generated), `scripts/gen_python_proto.sh`, `src/sdk/python/tests/conftest.py`, `tests/test_client.py`, `src/sdk/python/README.md`.

**Interfaces — Produces:**
```python
class TlsConfig: ca: bytes; cert: bytes; key: bytes; server_name: str | None = None
class Record(NamedTuple): key: bytes; value: bytes
class Client:                       # sync; AsyncClient has the same methods as coroutines
    def __init__(self, target: str, tenant: str = "", *, tls: TlsConfig | None = None, timeout: float = 30.0)
    def append_items(self, session: str, items: Sequence[bytes]) -> int
    def get_items(self, session: str, limit: int | None = None) -> list[bytes]
    def pop_item(self, session: str) -> bytes | None
    def clear_session(self, session: str) -> int
    def fork_session(self, source: str, target: str) -> int
    def write_records(self, puts: Sequence[Record] = (), deletes: Sequence[bytes] = ()) -> None
    def get_records(self, keys: Sequence[bytes]) -> dict[bytes, bytes]
    def scan_records(self, prefix: bytes = b"", *, start_after: bytes = b"", end_before: bytes = b"", limit: int | None = None, reverse: bool = False) -> Iterator[Record]  # pages internally
    def delete_record_prefix(self, prefix: bytes) -> int
    def put_blob(self, name: str, data: bytes | BinaryIO) -> int
    def get_blob(self, name: str) -> bytes | None
    def open_blob(self, name: str) -> Iterator[bytes] | None
    def blob_exists(self, name: str) -> bool
    def delete_blob(self, name: str) -> bool
    def close(self) -> None   # also context manager
class AgentStateError(Exception); class NotFound(AgentStateError); class PermissionDenied(AgentStateError)
```
Test fixture `node` (session scope): starts `$KVSTORE_NODE_BIN` (default `build/kvstore-node/kvstore-node`, else skip) with `--grpc-port 0 --metrics-port 0 --state-dir <tmp>`, parses `listening (grpc=N`, yields `127.0.0.1:N`; offers `restart()`.

- [ ] Tests: `test_items_round_trip`, `test_scan_pages_past_server_limit` (2,500 records, page size 1,000), `test_blob_round_trip_streams` (5 MiB), `test_async_client_matches_sync`, `test_state_survives_node_restart`, `test_missing_tenant_raises`.
- [ ] Generate stubs, implement, run `pytest src/sdk/python/tests -q`, commit `feat(sdk): Python client for agentstate.v1`.

### Task 6: OpenAI Agents SDK integration

**Files:** Create `agent_state_storage/openai_agents.py`; test `tests/test_openai_agents.py` (`pytest.importorskip("agents")`).

**Interfaces — Produces:**
```python
class AgentStateSession:            # satisfies agents.memory.Session
    def __init__(self, session_id: str, client: AsyncClient, *, session_settings=None)
    async def get_items(self, limit: int | None = None) -> list[dict]; async def add_items(self, items) -> None
    async def pop_item(self) -> dict | None; async def clear_session(self) -> None
    async def fork(self, new_session_id: str) -> "AgentStateSession"
class AgentStateSnapshotClient:     # register under a key; RemoteSnapshotSpec(client_dependency_key=key)
    def __init__(self, client: AsyncClient, *, prefix: str = "openai-agents/sandbox/")
    async def upload(self, snapshot_id: str, data: io.IOBase) -> None
    async def download(self, snapshot_id: str) -> io.IOBase   # SpooledTemporaryFile, rewound
    async def exists(self, snapshot_id: str) -> bool
```
- [ ] Tests: `test_session_protocol_conformance` (`isinstance(s, agents.memory.Session)`), `test_runner_persists_history` (two `Runner.run` turns with `agents.testing` fake model; second turn's input includes first turn's items; new `AgentStateSession` with same id sees them), `test_fork_continues_independently`, `test_snapshot_client_round_trip` (tar bytes up, same bytes down, `exists`).
- [ ] Implement, run, commit `feat(sdk): OpenAI Agents SDK session and sandbox snapshots`.

### Task 7: LangGraph checkpointer

**Files:** Create `agent_state_storage/langgraph/{__init__,_keys,checkpoint}.py`; tests `tests/test_langgraph_checkpoint.py`.

**Interfaces — Produces:** `AgentStateSaver(BaseCheckpointSaver)`: `__init__(client: Client, *, aclient: AsyncClient | None = None, serde=None)`; implements `get_tuple/list/put/put_writes/delete_thread/copy_thread` and async twins (async twins use `aclient` when given, else `asyncio.to_thread`). `_keys.pack(*parts: str) -> bytes` = concatenation of `u32be len | utf8`.

Record layout: checkpoint `pack("lg","cp",thread,ns,checkpoint_id)` → `serde`-typed blob of `{checkpoint, metadata, parent_checkpoint_id}`; writes `pack("lg","w",thread,ns,checkpoint_id,task_id) + u32be(idx)` → typed `(channel, value, task_path)`. `put_writes` keeps the first write for a given `(task_id, idx)` for non-special channels and overwrites for special ones, as `InMemorySaver` does (`WRITES_IDX_MAP`).

- [ ] Tests: `test_conformance_base_and_copy_thread` (run `langgraph.checkpoint.conformance.validate` with capabilities `put, put_writes, get_tuple, list, delete_thread, copy_thread`; assert report passes), `test_graph_resumes_after_node_restart` (StateGraph counter, 2 invokes, restart node, third invoke continues), `test_time_travel_fork` (update_state from an earlier checkpoint id, branch diverges), `test_thread_ids_with_separators_do_not_collide` (`"a\x00b"` vs `"a"`+ns `"b"`, `"a/b"`).
- [ ] Implement, run, commit `feat(sdk): LangGraph checkpointer`.

### Task 8: LangGraph store

**Files:** Create `agent_state_storage/langgraph/store.py`; tests `tests/test_langgraph_store.py`.

**Interfaces — Produces:** `AgentStateStore(BaseStore)`: `__init__(client: Client, *, aclient: AsyncClient | None = None)`; `batch` / `abatch` handle `GetOp`, `PutOp` (value None = delete), `SearchOp` (namespace_prefix, `filter` equality and `$eq/$ne/$gt/$gte/$lt/$lte`, offset, limit; a `query` raises `NotImplementedError("vector search is not supported yet")`), `ListNamespacesOp` (prefix/suffix match conditions, max_depth, limit, offset). Key: `pack("lg","st") + pack(*namespace) + b"\0\0\0\0" + pack(key)`; value JSON `{"value", "created_at", "updated_at"}` (ISO 8601 UTC).

- [ ] Tests: `test_put_get_delete`, `test_search_filters_and_paginates`, `test_list_namespaces_with_prefix_and_depth`, `test_namespace_prefix_does_not_match_longer_label` (prefix `("a",)` does not return `("ab",)`), `test_query_raises_not_implemented`, `test_async_batch`.
- [ ] Implement, run, commit `feat(sdk): LangGraph store`.

### Task 9: Example, docs, CI, SECURITY fix

**Files:** Create `examples/m1_quickstart.py` (no API key: LangGraph graph with checkpointer + store, fork from a checkpoint; OpenAI session with fake model; prints what survives a restart). Modify `README.md` (capability matrix rows for SDK + both integrations → ✅ single node; quickstart section with real M1 code; four-verbs snippet stays "planned M2"), `BUILD.md`, `.github/workflows/ci.yml` (job `python-sdk`: apt grpc toolchain as the C++ job, build target `kvstore-node`, Python 3.11, `pip install -e "src/sdk/python[openai-agents,langgraph,test]"`, `KVSTORE_NODE_BIN=build/kvstore-node/kvstore-node pytest src/sdk/python/tests -v`), `SECURITY.md` (state that the new service binds tenant to cert under TLS; `NodeData` binding exists but is off in the binary).

- [ ] Run the example end to end; run the full ctest + SDK suite on macOS and in the Ubuntu 22.04 Docker image; commit `docs: M1 quickstart, CI job for the Python SDK`.
