# KV Cache

Enterprise-grade, vendor-neutral KV Cache data layer for LLM inference.

- **5-tier storage** — HBM / Pinned / DRAM / NVMe / Cold (via Alluxio)
- **Prefix reuse** — Adaptive Radix Tree + LPM
- **Multi-tenant** — 3D quota + 3-class priority + RBAC + audit
- **Vendor-neutral** — vLLM / SGLang / TRT-LLM / AIBrix adapters

See [BUILD.md](./BUILD.md) for build and CI instructions.

## Quickstart

```bash
make all        # build + run all tests
License
Apache-2.0.
