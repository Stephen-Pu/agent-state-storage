# sglang adapter

Adapter for the [SGLang](https://github.com/sgl-project/sglang) engine.
LLD §6.1.4.

## What's here

* `kvcache_sglang.SGLangKVBackend` — L2 KV backend with the
  `lookup` / `store` / `retrieve` / `drop` method names SGLang's
  RadixAttention external-cache interface expects. Wraps the Core ABI
  (calls `libkvcache.so` via `cffi`) and folds the
  reserve → write → publish → seal sequence into a single `store`.
* `kvcache_sglang.KVCacheConnector` — the engine-agnostic Python
  surface, the same shape as the vLLM adapter's connector. Useful for
  ad-hoc demos or for engines whose call patterns don't fit the
  SGLang shape.

## Usage

```python
from kvcache_sglang import SGLangKVBackend

with SGLangKVBackend(tenant_id="t1",
                     model_id="llama-3-70b",
                     bytes_per_token=64) as kv:
    matched = kv.lookup(token_ids)         # 0 on miss; otherwise a
                                            # chunk-aligned token count
    if matched < len(token_ids):
        ...                                 # recompute the tail
    kv_bytes = kv.retrieve(token_ids)      # None on miss
    ...
    kv.store(token_ids, finished_kv_bytes) # commit when generation ends
```

## Tests

```bash
make build                                    # builds libkvcache.{so,dylib}
KVCACHE_LIB=$PWD/build/core-abi/libkvcache.dylib \
    pytest src/adapters/sglang/tests -v
```

Six tests cover store → retrieve round-trip, chunk-aligned LPM,
miss-returns-None, prefix truncation, constructor validation, and the
`drop` no-op.

## Versions

Supported SGLang versions: latest stable + one prior. The Core ABI is
stable across SGLang releases; only this adapter's wiring tracks the
engine's connector interface.
