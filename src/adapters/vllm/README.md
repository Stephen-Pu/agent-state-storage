# vLLM adapter

Implements vLLM's `KVConnectorBase` (5 hooks) by calling the Core ABI through
`cffi`. LLD §6.1.4.

Hooks mapped:
- `start_load_kv`     → `kv_lookup` + `kv_fetch` + `kv_wait`
- `wait_for_layer_load` → `kv_wait`
- `save_kv_layer`     → `kv_reserve` + `kv_publish`
- `wait_for_save`     → `kv_wait` + `kv_seal`
- `get_finished`      → poll completions

Supported versions: latest stable + one prior. Update `pyproject.toml` on every
vLLM minor release.

## Running the end-to-end demo (in-process backend)

The Step-6 deliverable is a self-contained Python E2E test that exercises the
full ABI without requiring real vLLM. It writes a 2-chunk prefix, then verifies
LPM on a second request that extends that prefix by 3 tokens.

```bash
# Build libkvcache.so (no external deps required for the loopback / headless path):
cmake -S ../.. -B ../../build -DCMAKE_BUILD_TYPE=Debug
cmake --build ../../build -j

# Run the demo. The adapter discovers the .so under ../../build/core-abi/ by
# default; set KVCACHE_LIB to override.
pip install cffi pytest
pytest tests/test_e2e_demo.py -v
```

Expected: two passing tests — `test_prefix_reuse_across_two_requests` and
`test_lookup_miss_returns_none`.
