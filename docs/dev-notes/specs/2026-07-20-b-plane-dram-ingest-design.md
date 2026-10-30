# B-plane DRAM Ingest + Minimal ⑬ Persistence — Design

> **Status:** approved (brainstorm 2026-07-20). Next step: `writing-plans`.
> **Scope owner:** Stephen Pu. **Repo:** `agent-state-storage` (local `KV_Cache`).

## Goal

A **B-class** state (irreplaceable memory, `state_kind = SK_MEMORY`) can be
stored, persisted durably, staged into DRAM, served, reclaimed from DRAM under
capacity pressure, and replayed back from persistence — end-to-end, single
node, reusing the A-plane tiering spine. This is the first concrete step of the
v2.1 B-plane, and it de-risks the **SS-1** claim ("one spine, swap the
`ValuePolicy` plugin, cover A and B") for B the way the tool-result slice did
for A.

It builds directly on the just-shipped DramTier per-kind eviction restructure
(`dd2e55f..e808367`): that closed the *eviction* half of the SS-2 B-plane trap
(a B entry can reside in DRAM without being wrongly discarded); this closes the
*ingest* (store) half and gives the eviction work a real producer.

## Honest boundary (what this is NOT)

Single-node, in-process WAL only. **"Durable" = survives process restart, NOT
node loss.** Explicitly out of scope, deferred to later B-plane slices:

- Replication, strong consistency (the rest of ⑬ per v2.1 §6 reversal table).
- Lineage engine ⑭ (multi-step DAG).
- B1 Agent Memory semantics (memory types, hybrid retrieval, pgvector/Milvus).
- B2 Durable Execution (Temporal/DBOS integration).
- Threading `StateIdentity` across the C ABI / gRPC wire (verdict-doc deferral
  stands — this slice is C++-core-only).
- WAL compaction/GC beyond restart-replay; bounded-memory WAL (this v0 keeps
  the replayed blob set in an in-memory map; a real ⑬ would memory-map).

## Global constraints (carried from the repo + prior slices)

- `kv_locator_t` frozen at 64 bytes; `StateIdentity` is a 128B C++-core-only
  superset — additive only, never on the wire in this slice.
- C++20, CMake + Ninja, GoogleTest. Core headers include as `"foo.h"` (the
  `src/core/common` and node `src/` roots are on the test include path).
- Full suite must stay green (currently 542/542, 15 pre-existing skips). The KV
  path must be **behavior-identical** — B is purely additive.
- No `Co-Authored-By` trailer; per-push confirmation on `main` (non-forked).

## Architecture

```
StatePut(id, bytes)
   1. StateWal.Append(key, id, bytes)  ──►  append record + fsync   (DURABLE FIRST)
   2. TierManager.StageToDram(key, bytes, SK_MEMORY)                (then cache in DRAM)

StateGet(id)
   LookupDram(key) ── hit ──► return bytes
        │ miss
        └─ policy(SK_MEMORY).onMiss() ⇒ kReplayFromPersist
              └─ StateWal.Get(key) ──► re-Insert(key, bytes, SK_MEMORY) ──► return

DRAM capacity pressure
   evictor drops the B DRAM copy  (SAFE: the WAL holds a durable copy)
   ⇒ this is DEMOTION, not discard; the next StateGet replays it back
```

**Why persist-first is the correctness spine:** because `Append`+fsync
completes before the entry is ever staged into DRAM, every B entry that exists
in DRAM is guaranteed to have a durable copy. That single ordering rule makes
"drop the DRAM copy" inherently safe (it is demotion, never data loss) and
collapses what would otherwise be a per-entry "is-it-persisted-yet?" state
machine.

## Components (all new, C++-core)

### 1. `HeadlessNode::StatePut` / `StateGet`

New core-internal B entry points, separate from the KV `Seal`/`Fetch` path (B
has no tokens, prefix, or ART leaf).

```cpp
// Store a B-class state. WAL-first, then stage to DRAM. Returns KV_OK on
// success; on WAL append failure returns an error and stages nothing (no
// DRAM-only entry can exist).
int StatePut(const kvcache::common::StateIdentity& id,
             const uint8_t* data, std::size_t n);

// Retrieve a B-class state: DRAM first, else replay from the WAL and
// re-stage. Fills *out; returns KV_OK on hit, KV_E_NOT_FOUND if neither DRAM
// nor the WAL has it.
int StateGet(const kvcache::common::StateIdentity& id,
             std::vector<uint8_t>* out);
```

- `StatePut`: `wal_->Append(key, id, {data,n})` → on failure return error;
  on success `tm_->StageToDram(key, data, n, SK_MEMORY)`.
- `StateGet`: `tm_->LookupDram(key)` → hit returns; miss consults
  `policy_reg_.of(SK_MEMORY).onMiss(id)`; on `kReplayFromPersist`,
  `wal_->Get(key, out)` → if found, re-`StageToDram(key, *out, SK_MEMORY)` and
  return; else `KV_E_NOT_FOUND`.
- Both compute `key = StateKeyFromIdentity(id)`.

### 2. `StateWal` — minimal ⑬ persistence engine (v0)

New append-only WAL for B state, reusing ArtWal's on-disk discipline.

On-disk record (little-endian, packed), one per mutation:

```
record_len  u32    total bytes INCLUDING this header field
op          u8     1 = PUT, 2 = DEL
key         u8[16] DramKey
identity    u8[128] StateIdentity (stored for future lineage/debug; not indexed)
blob_len    u32    (PUT only; 0 for DEL)
blob        u8[blob_len]  (PUT only)
crc32       u32    IEEE over [op .. last byte before crc]
```

```cpp
class StateWal {
 public:
    // Open (create if absent) at `path`, replaying existing records into an
    // in-memory key→bytes map. A torn tail record (len/CRC mismatch) is
    // truncated; every fully-committed record before it is kept. Missing
    // file is fine. Only an unparseable header (not a torn tail) sets *err.
    static std::unique_ptr<StateWal> Open(const std::string& path, std::string* err);

    // Append a PUT record, fsync, then update the in-memory map. Returns
    // false (and sets *err) if the write or fsync fails — caller must treat
    // the entry as NOT persisted.
    bool Append(const DramKey& key, const kvcache::common::StateIdentity& id,
                const uint8_t* data, std::size_t n, std::string* err);

    // Read the latest blob for key. Returns false if absent.
    bool Get(const DramKey& key, std::vector<uint8_t>* out) const;

    std::size_t Size() const;  // entries currently mapped
};
```

- **Ordering:** write bytes → `fsync` → only then insert into the in-memory
  map, so an in-map entry is always durable.
- The in-memory `key→bytes` map is both the fast read source and the
  post-replay reconstruction target. (Deferred: memory-mapped / bounded
  variant; DEL is defined in the format for forward-compat but this slice only
  emits PUT.)
- **Not thread-safe by itself** beyond an internal mutex guarding append +
  map; matches the single-writer StatePut path. (Concurrency hardening
  deferred — same posture as the DramTier MVP mutex note.)

### 3. `ValuePolicyPersistentWal` (SK_MEMORY, production B policy)

The real B policy the node registers for `SK_MEMORY`, replacing the
Q3-validation `ValuePolicyPersistentStub` in the node's registry.

| Method | Value | Why |
|---|---|---|
| `shouldStore` | `true` | Irreplaceable — always stored (ignores CostModel). |
| `shouldEvict` | `kEvictable` | Persist-first guarantees a durable copy, so dropping the DRAM copy is demotion, not discard. |
| `onMiss` | `kReplayFromPersist` | A DRAM miss is served by replaying from the WAL. |

`ValuePolicyPersistentStub` (`kNotEvictable`) is **retained** as a test fixture
for the DramTier eviction-skip tests (which deliberately model a pinned entry)
and as the interface exemplar for future pinned/pre-persist/strong-consistency
modes. The infra (per-kind evict dispatch + walk-and-skip) is unchanged and
still exercised by those tests.

### 4. Key projection

```cpp
// B DramKey from a StateIdentity: first 16 bytes of content_hash. Mirrors the
// KV LocatorContentKey projection. Deterministic; content-addressed.
DramKey StateKeyFromIdentity(const kvcache::common::StateIdentity& id);
```

## Data flow (detailed)

- **Put (happy path):** `StatePut` → `wal_->Append` (fsync) → `StageToDram(...,
  SK_MEMORY)`. Entry now in both WAL (durable) and DRAM (fast).
- **Get (DRAM hit):** `StateGet` → `LookupDram` hit → return. No WAL touch.
- **Get (miss → replay):** `LookupDram` miss → `onMiss` = `kReplayFromPersist`
  → `wal_->Get` hit → re-`StageToDram(..., SK_MEMORY)` → return. Entry
  re-resident in DRAM.
- **Demotion:** unrelated DRAM pressure triggers `EvictToFit`; the B entry's
  policy says `kEvictable`, so the LRU evictor drops it exactly like a KV
  entry. Because B keys are never in the ART, the KV `on_evict` callback (ART
  leaf removal / `KV_EVENT_EVICT`) finds nothing and no-ops. The WAL copy
  remains; the next `StateGet` replays it (the "Get miss → replay" path).

## Error handling

- **WAL-first ordering is invariant #1.** fsync completes before DRAM staging;
  a crash between them can only lose the DRAM copy (recoverable from WAL),
  never a durable ack'd write.
- **`Append` failure** (write/fsync error): `StatePut` returns an error and
  stages nothing — no DRAM-only B entry is ever created.
- **Torn tail on replay:** detected via `record_len` + `crc32`; truncated
  silently; all fully-committed prior records kept (ArtWal semantics).
- **`StateGet` with no record anywhere:** `KV_E_NOT_FOUND` — never fabricate
  bytes.
- **B key collision with a KV key:** content-addressed 16B keys; a B and KV
  entry colliding is astronomically unlikely and, if it occurred, the
  `state_kind` carried on the DramTier entry keeps eviction dispatch correct.
  Not specially handled (documented assumption).

## Testing

All C++ GoogleTest, driven at the core level (no ABI/wire).

| # | Test | Asserts |
|---|------|---------|
| 1 | WAL round-trip | `Append` then `Get` returns identical bytes. |
| 2 | WAL restart replay | New `StateWal::Open` on the same path replays; `Get` returns the pre-restart bytes. |
| 3 | WAL torn tail | A truncated final record is dropped on replay; earlier records intact. |
| 4 | `StatePut`→`StateGet` DRAM hit | Round-trips through the node, served from DRAM. |
| 5 | Demotion + replay | Fill DRAM so the B entry is evicted; `StateGet` replays from WAL byte-identical and the entry is re-resident. |
| 6 | `Append` failure | Simulated WAL write failure → `StatePut` errors, `LookupDram` shows nothing staged. |
| 7 | Policy contrast | `ValuePolicyPersistentWal` = `kEvictable` + `kReplayFromPersist`, distinct from KV (`kRecompute`) and the stub (`kNotEvictable`), from one registry. |
| 8 | KV regression | Full suite green; KV store/fetch/evict unchanged. |

## File structure

- Create: `src/core/common/value_policy_persistent_wal.{h,cpp}`
- Create: `src/kvstore-node/src/persist/state_wal.{h,cpp}` (new `persist/` unit)
- Create: `src/core/common/state_key.{h,cpp}` (or inline `StateKeyFromIdentity`
  next to `StateIdentity` if it stays a pure function — decide in the plan)
- Modify: `src/core-abi/src/headless_node.{h,cpp}` — add `StatePut`/`StateGet`,
  own a `StateWal`, register `ValuePolicyPersistentWal` for `SK_MEMORY`
- Modify: `src/kvstore-node/src/tier/tier_manager.{h,cpp}` — `StageToDram`
  gains a `state_kind` parameter (default `SK_KV`, so KV callers unchanged)
- Create: `src/tests/unit/persist/state_wal_test.cpp`,
  `src/tests/unit/.../state_ingest_test.cpp`, policy contrast test
- CMake wiring for the new `persist/` target + tests
- Docs: cross off the B-ingest deferral in `docs/design/ss2-spike-q3-verdict.md`
  ("Still deferred: DRAM store path") once landed; note replication/lineage
  remain deferred.

## Open decisions deferred to the plan (not blocking)

- Whether `StateKeyFromIdentity` is its own TU or a header-inline function.
- Whether `StageToDram`'s new param is threaded through `TierManager` only, or
  also the two promotion `Insert` sites (those stay `SK_KV` — KV promotion).
