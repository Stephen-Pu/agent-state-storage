# Build & Test Guide

This document covers the local developer workflow and the CI pipeline. All
commands assume you are at the project root (the directory containing this
file).

## TL;DR

```bash
make all          # Build C/C++, run ctest, build Go, run Go unit tests, run Python E2E.
```

Or run any stage on its own — see `make help`.

## Prerequisites

| Stage              | Required                                              |
| ------------------ | ----------------------------------------------------- |
| C/C++              | `cmake ≥ 3.22`, `ninja-build`, `g++` (or `clang++`)    |
| Go components      | `go 1.22+`                                            |
| Python E2E adapter | `python 3.10+`, `pip install cffi pytest`             |
| Helm lint (CI)     | `helm ≥ 3.14`                                          |

### Ubuntu 22.04 one-liner

```bash
sudo apt-get install -y cmake ninja-build g++ pkg-config python3-pip golang-1.22
pip install cffi pytest
```

### macOS

```bash
brew install cmake ninja python go
pip3 install cffi pytest
```

No external dependency is required for the default build:
* **GTest** is fetched at configure time via CMake `FetchContent` if not on
  the system (~30s extra on first configure).
* **RocksDB / etcd / NIXL / SPDK** are gated behind CMake options
  (`KVCACHE_ENABLE_ROCKSDB`, `KVCACHE_ENABLE_ETCD`, ...) and default to OFF.
  The default build links against the in-tree facades, which return
  "compiled-out" errors for these backends — sufficient for unit tests and
  the in-process demo.

## C/C++ build

```bash
make build           # configure + compile (Debug, Ninja by default)
make test            # run ctest after build
```

Equivalent CMake commands:

```bash
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Build options

```bash
cmake -S src -B build \
  -DKVCACHE_ENABLE_ROCKSDB=ON  \  # link real librocksdb (apt: librocksdb-dev)
  -DKVCACHE_ENABLE_ETCD=ON     \  # compile GrpcEtcdClient (needs vendored proto)
  -DKVCACHE_ENABLE_SPDK=ON     \  # SPDK NVMe backend (TODO)
  -DKVCACHE_ENABLE_CUDA=ON     \  # GPUDirect paths (TODO)
  -DKVCACHE_BUILD_TESTS=OFF       # skip the 26 gtest binaries
```

### Outputs

| Target                      | Path                                  |
| --------------------------- | ------------------------------------- |
| `libkvcache.so` (Core ABI)  | `build/core-abi/libkvcache.{so,dylib}` |
| `kvstore-node` binary       | `build/kvstore-node/kvstore-node`     |
| `kvagent` binary            | `build/kvagent/kvagent`               |
| Test binaries (26)          | `build/tests/unit/test_*`             |

### Running a single test

```bash
ctest --test-dir build -R '^test_art_index$' --output-on-failure
# or
./build/tests/unit/test_art_index --gtest_filter='ArtIndexTest.*'
```

## Go components

```bash
make go             # build control-plane + operator + kvctl
make go-test        # vet + short unit tests (no integration)
make go-it          # integration: embedded-etcd registry tests (~10–30s)
```

The embedded-etcd integration tests are guarded by the `integration` build
tag, so they only run when explicitly invoked. They reserve two free TCP
ports per test invocation — no external etcd is needed.

## Python E2E adapter

```bash
make py-test        # builds libkvcache.so first, then pytest
```

The test (`src/adapters/vllm/tests/test_e2e_demo.py`) exercises the full
two-request prefix-reuse flow against the in-process backend:

```
Request A: 32 tokens → reserve → write → publish → seal
Request B: 32 + 3 tokens → lookup → expect 32 matched tokens → fetch → verify bytes
```

To run against a pre-built library:

```bash
KVCACHE_LIB=/abs/path/to/libkvcache.so pytest src/adapters/vllm/tests -v
```

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs 7 jobs on every push and PR:

| Job                  | Time   | What it does                                  |
| -------------------- | ------ | --------------------------------------------- |
| `cpp`                | ~3 min | CMake + Ninja + ctest (26 binaries)            |
| `go-cp`              | ~1 min | `go vet`, `go build`, `go test -short`         |
| `go-cp-integration`  | ~2 min | embedded-etcd registry tests                   |
| `go-operator`        | ~1 min | `go vet`, `go build`                           |
| `go-kvctl`           | ~30 s  | `go vet`, `go build`                           |
| `python-adapter`     | ~3 min | Build `libkvcache.so` + pytest E2E             |
| `helm-lint`          | ~30 s  | `helm lint` + `helm template`                  |

Total wall-clock ≈ 5 minutes (jobs run in parallel).

## Troubleshooting

### "GTest not found"

Expected on fresh systems. The configure step will automatically fall back to
`FetchContent` (downloads GoogleTest v1.14.0 from the release archive). The
first configure adds ~30 s; subsequent configures cache the source.

If FetchContent fails (e.g. no network), install the system package:
```bash
sudo apt-get install libgtest-dev libgmock-dev
```

### "RocksDB not compiled in (define KVCACHE_HAVE_ROCKSDB)"

Expected when running the seal-path test without RocksDB installed. The
test takes the facade branch and asserts the error contract. To run the
full integration:

```bash
sudo apt-get install librocksdb-dev
cmake -S src -B build -DKVCACHE_ENABLE_ROCKSDB=ON
cmake --build build -j
ctest --test-dir build -R rocksdb_store
```

### "libkvcache.so not found" (Python)

The adapter walks up to `build/core-abi/`. If you used a non-default build
directory, set `KVCACHE_LIB` explicitly:

```bash
export KVCACHE_LIB=$PWD/release-build/core-abi/libkvcache.so
```

### Embedded etcd "not ready in 20s"

The default 20-second deadline is generous for CI. On a heavily loaded
machine, increase the timeout by editing the test or run only one at a time:

```bash
go test -tags=integration -run TestRegistry_RegisterAndExpire -timeout 5m ./internal/membership
```

### gRPC / Protobuf build attempts

Build will skip `core/proto/` if `protoc` isn't installed. Install it only
when you need the generated stubs:

```bash
sudo apt-get install protobuf-compiler
```

## Repository layout

```
KV_Cache/
├── BUILD.md                # this file
├── Makefile                # top-level entry point
├── .github/workflows/ci.yml
├── scripts/
│   ├── build.sh
│   └── test.sh
├── KV_Cache_HLD_*.md        # design documents (not generated)
├── KV_Cache_LLD_*.md
└── src/                     # all source (English-only)
    ├── CMakeLists.txt
    ├── include/kvcache/     # public C ABI headers
    ├── core/                # shared C++ libs + protobuf schemas
    ├── core-abi/            # libkvcache.so
    ├── kvstore-node/        # data-plane C++ node binary
    ├── kvagent/             # sidecar C++ binary
    ├── control-plane/       # Go CP
    ├── operator/            # Go K8s operator
    ├── kvctl/               # Go CLI
    ├── adapters/            # vLLM / SGLang / AIBrix / TRT-LLM
    ├── deploy/              # Helm + CRDs
    └── tests/               # unit (gtest) + e2e (pytest)
```
