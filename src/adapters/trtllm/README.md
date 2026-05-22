# TRT-LLM adapter

C++ backend that plugs into TRT-LLM's `KVCacheManager`. LLD §6.1.4.

This is the only C++ engine adapter — TRT-LLM's plugin surface is native
C++, so unlike vLLM / SGLang / AIBrix (Python over `cffi`) the
integration point is a real C++ library linked at build time.

## What's here

* `kvcache::trtllm::TrtLlmKVCacheBackend` — RAII class with
  `Lookup / Store / Retrieve / Drop` (same shape as the SGLang and
  AIBrix Python adapters, so anyone reading one recognises the others).
* `libkvcache_trtllm.a` — static archive that production TRT-LLM
  plugins link into their own `.so`. The archive depends only on
  `libkvcache.{so,dylib}` (the public C ABI), not on any
  `kvstore-node` internals.

## Usage

```cpp
#include <kvcache_trtllm/backend.h>

kvcache::trtllm::TrtLlmKVCacheBackend kv("tenant-a",
                                          "llama-3-70b",
                                          /*bytes_per_token=*/64);

if (auto matched = kv.Lookup(token_ids); matched < token_ids.size()) {
    // recompute the tail
}
if (auto cached = kv.Retrieve(token_ids); cached) {
    use(*cached);
}
kv.Store(token_ids, finished_kv_bytes);   // commit when generation ends
```

## Build & test

The library + tests come along with the rest of the C/C++ tree:

```bash
make build                    # builds libkvcache.{so,dylib} + libkvcache_trtllm.a
cd build && ctest -R TrtLlm   # runs the 8 backend tests
```

Eight tests cover store → retrieve round-trip, chunk-aligned LPM on
extended tokens, miss-returns-nullopt, prefix truncation when the
caller asks for more chunks than are cached, constructor validation,
the `Drop` no-op, accessors, and move semantics.

## Versions

Supported TRT-LLM versions: latest stable + one prior. The C ABI is
stable across TRT-LLM releases; only the cpp wrapper here tracks the
engine's plugin interface.
