# aibrix adapter

Adapter for the [AIBrix](https://github.com/vllm-project/aibrix) engine.
LLD §6.1.4.

## What's here

* `kvcache_aibrix.AIBrixKVConnector` — sync AIBrix KVCache Connector v1
  surface (`get` / `put` / `delete` / `exists`). Thin wrapper over the
  shared :mod:`kvcache_core` connector — calls
  `libkvcache.so` via `cffi`.
* `kvcache_aibrix.AsyncAIBrixKVConnector` — same sync verbs plus an
  async path (`prefetch` / `finished_ids` / `pop` / `cancel`) so the
  AIBrix runtime can overlap the cache Fetch with model setup
  instead of blocking inline on `get`. Built on the reusable
  `AsyncLoadDriver` (Phase Q-AB-1) — same shape as the SGLang
  adapter's Phase Q-SG-1 driver, reshaped to AIBrix's
  `prefetch(key)` vocabulary.
* `kvcache_aibrix.AsyncLoadDriver` — executor + lifecycle primitive
  that backs `AsyncAIBrixKVConnector`. Connector-protocol-driven so
  it has clean unit coverage with an in-Python fake (no live
  `libkvcache.so` needed for the threading tests).
* `kvcache_aibrix.KVCacheConnector` — re-export of the engine-agnostic
  connector from `kvcache_core`, for callers that want the lower-level
  reserve / publish / seal verbs.

## Usage

```python
from kvcache_aibrix import AIBrixKVConnector

with AIBrixKVConnector(tenant_id="t1",
                        model_id="llama-3-70b",
                        bytes_per_token=64) as kv:
    if kv.exists(token_ids):
        kv_bytes = kv.get(token_ids)        # None on miss
    ...
    kv.put(token_ids, finished_kv_bytes)    # commit when generation ends
```

### Async prefetch

```python
from kvcache_aibrix import AsyncAIBrixKVConnector

with AsyncAIBrixKVConnector(tenant_id="t1", model_id="llama-3-70b",
                              bytes_per_token=64, workers=4) as kv:
    matched = kv.prefetch("req-7", token_ids)   # sync lookup + async fetch
    if matched == 0:
        # cache miss — recompute or fall through to `get`
        ...
    else:
        # ... model setup overlaps with the fetch
        ...
        kv_bytes = kv.pop("req-7")              # blocks if not done
    kv.put(token_ids, finished_kv_bytes)
```

If the request is cancelled before `pop`, call `kv.cancel("req-7")` —
the driver blocks on the in-flight fetch first (so the worker isn't
still touching the inner handle), then releases.

## Tests

```bash
make build                                    # builds libkvcache.{so,dylib}
KVCACHE_LIB=$PWD/build/core-abi/libkvcache.dylib \
    pytest src/adapters/aibrix/tests -v
```

Six tests cover put → get round-trip, exists transitions,
miss-returns-None, prefix-only retrieval for extended keys, constructor
validation, and the `delete` no-op. Ten additional tests
(`test_aibrix_async.py`) cover the `AsyncLoadDriver` lifecycle:
miss/hit prefetch, the prefetch-is-actually-async timing invariant,
finished_ids polling + idempotency, cancel-releases-handle, same-rid
back-to-back prefetch releasing the prior handle, worker exception
surfacing through finished_ids, close-blocks-and-releases, constructor
validation, plus one e2e round-trip against the real C ABI.

## Versions

Supported AIBrix versions: latest stable + one prior. The Core ABI is
stable across AIBrix releases; only this adapter's wiring tracks the
engine's connector interface.
