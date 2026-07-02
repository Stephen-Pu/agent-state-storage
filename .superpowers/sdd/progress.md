# A9 DR Warm-Standby — SDD progress

Plan: docs/superpowers/plans/2026-07-01-a9-dr-warm-standby.md
Base: c1a7d31

Task 1: complete (commits c1a7d31..4621d05, review clean after 1 fix loop)
  - Added HeadlessNode::ReplicaFetch + ReplicaChunk; DramTier::Peek + TierManager::PeekDram (const, non-mutating read).
  - MINOR (final-review triage): docstring says "*out fully cleared on any non-KV_OK return" but the evict_index_-miss early return doesn't clear *out (no stale leak — fields never written on that path). 2-line fix or reword.
  - MINOR (final-review triage): no isolated unit test asserting DramTier::Peek leaves Am LRU order unchanged (const is compiler-enforced; behavioral test would guard future regressions).
Task 2: complete (commits 4621d05..eee90bb, review clean after 1 fix loop)
  - SealCommit refactor (Seal body shared) + SealByChunkPath (token-free commit). Refactor verified byte-for-byte (locks/ART insert/event emission/cleanup preserved).
  - Fixed I1 (path_span from param not req copy), M2 (test rename), M3 (added ReadHandleReturnsInval KV_E_INVAL test).
  - MINOR (final-review triage): Task 2 test uses a single shared node singleton (distinct token ranges avoid conflict) — true 2-process primary→standby replication is deferred to a DR system test / Task 6 live loop.
Task 3: complete (commit 6a61862, review clean, 0 Critical/Important)
  - warm_filter.h (header-only) IsWarm + WarmPolicy{max_tier=1}. Real Tier enum: Unspecified=0,Hbm=1,Pinned=2,Dram=3,Nvme=4,Cold=5. IsWarm gates >=Hbm && <=max_tier; enum ordering verified correct.
  - MINOR (final-review triage): no test for Tier::Unspecified rejection (guard correct, untested).
  - MINOR (final-review triage): test name DefaultPolicyAcceptsHbmAndPinned is inverse of its assertion (actually asserts Pinned REJECTED at max_tier=1) — rename to ...AcceptsHbmOnlyRejectsPinned.
Task 4: complete (commit bc4ddcd, verified directly, correct)
  - replication_cursor.h (header-only) ShouldApply/Advance/Last, monotonic. Test 1/1.
  - MINOR (final-review triage): test_replication_cursor CMake links NODE_PREFIX_SRCS it doesn't need (cursor test only includes replication_cursor.h → <algorithm>/<cstdint>). Harmless compile bloat; drop the sources arg.
Task 5: complete (commit 8459762, review clean, 0 Critical/Important)
  - ReplicationConsumer.{h,cpp} ApplyEvent (warm-filter→cursor→ReplicaFetch→Reserve/Publish/SealByChunkPath→advance). ApplyEvent sequence + cursor-advance + no-handle-leak verified. New 3/3 + regression 13/13.
  - Circular-dep avoidance justified: kvcache→kvstore_node_core already, and consumer needs HeadlessNode (core-abi) → can't go in NODE_CORE_SRCS. .cpp currently compiled into test binary only (in NO shipping lib).
  - ARCHITECTURAL (folded into Task 6): move replication_consumer.{h,cpp} to core-abi/src/replication/, add .cpp to kvcache lib target, link test via kvcache. warm_filter.h/replication_cursor.h stay in kvstore-node/src (core-abi already has that include path).
  - MINOR (final-review triage): permanent Publish/SealByChunkPath failure would retry same epoch forever (transient-only assumption; unlikely). Test doesn't assert &primary==&standby singleton assumption. Test MakeLocator uses bespoke hash, not production ChunkifyNS path (documented) — LookupMatched verifies insert, not real-Lookup discoverability.
Task 6: complete (commit 1f51cf0, 479/479 ctest green, 0 failures)
  - Part A: replication_consumer.{h,cpp} moved to core-abi/src/replication/; .cpp added to kvcache SHARED lib target. warm_filter.h/replication_cursor.h stay in kvstore-node/src. test_replication_consumer now compiles consumer .cpp from core-abi/src. Task 5 tests still pass.
  - Part B: Start()/Stop() live subscribe/poll loop. HeadlessNode::SubscribeEvents delivers kv_event_t via its own internal poller thread; EventCbDispatch translates to prefix::Event and calls ApplyEvent directly (SPSC serialisation, no additional mutex needed on cursor_). Sentinel thread is the join target for Stop(). Stop() calls UnsubscribeEvents (joins internal poller, guarantees no further callbacks), then joins sentinel thread. Both Start()/Stop() are idempotent; destructor calls Stop(). New test LiveLoopReplicatesWarmNotCold: Start() before sealing 3 warm chunks; WaitFor bounded-poll (10 ms slices, 2 s budget) until standby Lookup finds all 3; Stop() joins cleanly; second Stop() no-op. 4/4 tests pass.
  - Part C: 479/479 ctest (0 failures, 16 integration skipped — same as before). A9 warm-standby series complete.
  - MINOR concern: single-singleton test (primary==standby) means SealByChunkPath emits cascading ADD events that the live loop processes; cursor dedup + kReplaced (KV_OK) make it idempotent but the loop may spin on re-seals until Stop() is called. Production always uses separate nodes — no issue there.
