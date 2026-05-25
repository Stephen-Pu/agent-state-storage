// Phase S-1 — Fetch throughput micro-benchmark.
//
// Seeds a single sealed prefix, registers a long-lived dst buffer
// (Phase M-5), then issues back-to-back kv_lookup + kv_fetch + kv_wait
// for N iterations. Reports:
//
//   * Bytes/sec — wall-clock across the whole loop.
//   * Per-call latency p50/p99 — sanity check on tail behaviour
//     under sustained load.
//
// All loopback (in-process memcpy through the NIXL backend) — this is
// the floor; the cross-process TCP path will be 1-2 orders of
// magnitude slower depending on link speed.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench_common.h"
#include "kvcache/kv_abi.h"
#include "kvcache/kv_errors.h"

using kvcache::bench::NowNs;
using kvcache::bench::PrintLatencyTable;
using kvcache::bench::Summarise;

namespace {

constexpr std::size_t kPrefixTokens = 32;
constexpr std::size_t kBytesPerToken = 64 * 1024;  // 64 KiB
constexpr std::size_t kIterations    = 500;

}  // namespace

int main() {
    const std::size_t bytes_per_fetch = kPrefixTokens * kBytesPerToken;
    std::printf("kvcache bench: Fetch throughput\n");
    std::printf("  bytes per fetch: %zu (%zu KiB)\n", bytes_per_fetch,
                  bytes_per_fetch / 1024);
    std::printf("  iterations: %zu\n\n", kIterations);

    kv_ctx_config_t cfg{};
    cfg.abi_version = KVCACHE_ABI_VERSION;
    cfg.tenant_id   = "bench";
    cfg.model_id    = "bench-fetch";
    kv_ctx_t* ctx = nullptr;
    if (kv_ctx_open(&cfg, &ctx) != KV_OK || !ctx) {
        std::fprintf(stderr, "kv_ctx_open failed\n");
        return 1;
    }

    // Seed one sealed prefix sized to bytes_per_fetch.
    std::vector<uint32_t> tokens(kPrefixTokens);
    for (std::size_t i = 0; i < kPrefixTokens; ++i) {
        tokens[i] = 0xBEEF0000u + static_cast<uint32_t>(i);
    }
    kv_locator_t loc{};
    std::memset(&loc, 0, sizeof(loc));
    loc.range.token_count = static_cast<uint32_t>(kPrefixTokens);
    loc.version           = 1;
    kv_handle_t      hwrite = 0;
    kv_buffer_desc_t slot{};
    if (kv_reserve(ctx, &loc, bytes_per_fetch, &hwrite, &slot) != KV_OK) {
        std::fprintf(stderr, "kv_reserve failed\n");
        return 2;
    }
    if (slot.addr) std::memset(slot.addr, 0x77, bytes_per_fetch);
    kv_buffer_desc_t empty{};
    kv_publish(ctx, hwrite, empty, bytes_per_fetch);
    kv_seal(ctx, hwrite, tokens.data(), tokens.size());

    // Pre-register the dst buffer (Phase M-5 hot-path knob — no
    // per-fetch MR churn).
    std::vector<uint8_t> dst(bytes_per_fetch);
    uint32_t dst_mr = 0;
    if (kv_register_local_mr(ctx, dst.data(), dst.size(), &dst_mr) != KV_OK) {
        std::fprintf(stderr, "kv_register_local_mr failed\n");
        return 3;
    }

    std::vector<uint64_t> per_call_ns;
    per_call_ns.reserve(kIterations);

    const auto wall_t0 = NowNs();
    for (std::size_t i = 0; i < kIterations; ++i) {
        kv_locator_t meta{};
        kv_handle_t  hread = 0;
        uint32_t     matched = 0;
        const auto t0 = NowNs();
        if (kv_lookup(ctx, tokens.data(), tokens.size(),
                        &meta, &hread, &matched) != KV_OK) {
            std::fprintf(stderr, "lookup miss on iteration %zu\n", i);
            return 4;
        }
        kv_buffer_desc_t dst_desc{};
        dst_desc.addr   = dst.data();
        dst_desc.len    = dst.size();
        dst_desc.mr_key = dst_mr;
        kv_completion_t cid = 0;
        if (kv_fetch(ctx, hread, /*ranges=*/nullptr, /*n=*/0,
                       dst_desc, &cid) != KV_OK) {
            std::fprintf(stderr, "fetch failed on iteration %zu\n", i);
            return 5;
        }
        if (kv_wait(ctx, cid, /*timeout_ms=*/5000) != KV_OK) {
            std::fprintf(stderr, "wait timed out on iteration %zu\n", i);
            return 6;
        }
        kv_release(ctx, hread);
        per_call_ns.push_back(NowNs() - t0);
    }
    const auto wall_elapsed_ns = NowNs() - wall_t0;

    const double total_mib =
        static_cast<double>(kIterations) *
        static_cast<double>(bytes_per_fetch) / (1024.0 * 1024.0);
    const double seconds = wall_elapsed_ns / 1.0e9;
    std::printf("Throughput (loopback, single-thread)\n");
    std::printf("------------------------------------------------------\n");
    std::printf("  bytes total: %.1f MiB\n", total_mib);
    std::printf("  wall time:   %.3f s\n", seconds);
    std::printf("  throughput:  %.1f MiB/s\n", total_mib / seconds);
    std::printf("  qps:         %.0f\n",
                  static_cast<double>(kIterations) / seconds);
    PrintLatencyTable("kv_lookup+fetch+wait (loopback)",
                      Summarise(per_call_ns));
    std::printf("------------------------------------------------------\n");

    kv_unregister_local_mr(ctx, dst_mr);
    kv_ctx_close(ctx);
    return 0;
}
