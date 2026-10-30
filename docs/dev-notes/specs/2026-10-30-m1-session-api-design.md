# M1: session API, Python SDK, OpenAI Agents SDK and LangGraph integrations

Date: 2026-10-30. Single node. Follows the v2.4 positioning decisions (agent session layer; OpenAI Agents SDK + LangGraph first; gRPC SDK).

## Intent

Give an agent developer a `pip install` path: start one `kvstore-node`, point an OpenAI Agents SDK agent or a LangGraph graph at it, and its conversation, checkpoints, long-term store and sandbox workspace snapshots survive a process crash. This is the developer entry point that M2 (session tree, erase · hold · audit) builds on.

## Goals

1. A gRPC service `agentstate.v1.AgentState` inside `kvstore-node`, on the same port, TLS settings and tenant identity as `NodeData`.
2. Three storage primitives behind it, all tenant-scoped and durable on one node:
   - **Items**: an ordered log per session (append, read latest N, pop last, clear, fork by copy).
   - **Records**: an ordered key-value space with atomic batches and prefix scans, forward and reverse.
   - **Blobs**: named byte streams (sandbox workspace tarballs), streamed in chunks.
3. A Python package `agent-state-storage` (import `agent_state_storage`): sync `Client` and async `AsyncClient` over the generated stubs.
4. Integrations in the same package, as optional extras:
   - `agent_state_storage.openai_agents`: `AgentStateSession` (the SDK's `Session` protocol) and `AgentStateSnapshotClient` (the `upload / download / exists` client a sandbox `RemoteSnapshotSpec` resolves).
   - `agent_state_storage.langgraph`: `AgentStateSaver` (a `BaseCheckpointSaver`, passing the `langgraph-checkpoint-conformance` base capabilities plus `copy_thread`) and `AgentStateStore` (a `BaseStore` without vector search).
5. CI runs the Python SDK and integration tests against a real `kvstore-node`.

## Non-goals (M1)

- Server-side session tree, O(1) copy-on-write fork, snapshot ids, erase / hold / audit, subject labels: M2. M1 fork is a copy.
- Vector search in the LangGraph store, store TTLs, `delete_for_runs` and `prune` in the checkpointer.
- Multi-node, replication, the KV prefix in a session.
- TypeScript SDK, MCP server.

## Anti-goals

- Claiming durability or isolation the code does not give. The README capability matrix moves an item to ✅ only when a test proves it.
- A tenant reading another tenant's state through the new service.
- Breaking the existing `NodeData` service or its clients.

## Constraints

- C++17, existing CMake layout; the service is built only when gRPC is found (`KVCACHE_HAVE_GRPC`), like `NodeData`.
- Storage must work in the default CI build (no RocksDB): an in-process ordered map plus an append-only, CRC-checked, fsynced log, same discipline as `StateWal`.
- Python ≥ 3.10. Runtime deps: `grpcio`, `protobuf`. Extras: `openai-agents>=0.24`, `langgraph>=1.2`, `langgraph-checkpoint>=4.3`.
- Generated Python stubs are committed under the package so `pip install` needs no protoc; a script regenerates them.
- Commits authored stephenpu@163.com, no AI attribution.

## Decisions

| Topic | Decision |
|---|---|
| Where the service lives | In `kvstore-node`, registered on the existing `GrpcServer` next to `NodeData` |
| Tenant | TLS on: resolved from the client certificate with `MtlsRegistry::ResolveTenant`; a request `tenant` that differs is `PERMISSION_DENIED`. TLS off (dev only): the request `tenant` field, required and non-empty |
| Durability | `--state-dir DIR` makes it durable (`DIR/records.log`, `DIR/blobs/`). Without it the service runs in memory and the node logs a warning |
| Key layout | One ordered key space. Every key starts with a length-prefixed tenant, then a kind byte (`I` items, `R` records); clients never see the tenant prefix |
| Item sequence | u64 big-endian per session, so reverse scans give the latest items |
| Blob names | Stored as `blobs/<sha256(tenant \0 name)>`; written to a temp file, fsynced, renamed |
| Limits | Item and record values ≤ 4 MiB (gRPC default message size); blobs streamed in 1 MiB chunks, ≤ 4 GiB |
| Compaction | Rewrite the log from the live map when it exceeds max(64 MiB, 2 × live bytes) |
| LangGraph checkpoints | Whole checkpoint serialized with the saver's `serde` in one record; checkpoint ids (uuid6) sort by time, so `list` is a reverse scan; metadata `filter` applied client-side |
| OpenAI items | JSON per item |

## Left to the builder

File-internal structure, error messages, Python class internals, test fixture mechanics, and the exact CI job layout, within the plan's interfaces.
