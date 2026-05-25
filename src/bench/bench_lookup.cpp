// Phase S-1 — Lookup latency micro-benchmark.
//
// Seeds the in-process HeadlessNode (loopback NIXL) with N pre-sealed
// prefixes, then runs M Lookup probes mixed across "guaranteed hit"
// (a sealed prefix) and "guaranteed miss" (a never-seen token vector)
// keys. Prints p50/p95/p99/p99.9/max latency in microseconds for each
// of {hit, miss, mixed}.
//
// Reference target (LLD §9.1): hit-path p99 ≤ 10 µs.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "bench_common.h"
#include "kvcache/kv_abi.h"
#include "kvcache/kv_errors.h"

using kvcache::bench::LatencyStats;
using kvcache::bench::NowNs;
using kvcache::bench::PrintLatencyTable;
using kvcache::bench::Summarise;

namespace {

constexpr std::size_t kPrefixTokens = 32;    // 2 chunks per prefix
constexpr std::size_t kSealedCount  = 1024;  // N
constexpr std::size_t kProbeCount   = 50000; // M

std::vector<uint32_t> MakePrefix(uint32_t seed) {
    std::vector<uint32_t> out(kPrefixTokens);
    for (std::size_t i = 0; i < kPrefixTokens; ++i) {
        out[i] = seed * 7919u + static_cast<uint32_t>(i) * 31u;
    }
    return out;
}

void SealOne(kv_ctx_t* ctx, const std::vector<uint32_t>& tokens) {
    kv_locator_t loc{};
    std::memset(&loc, 0, sizeof(loc));
    // Locator's identity bytes don't matter for the *bench* path — the
    // C ABI seeds them from ctx + tokens internally during the seal +
    // ART insert. We only need a unique chunk_path per call, which
    // tokens supply.
    loc.range.token_count = static_cast<uint32_t>(tokens.size());
    loc.version           = 1;
    const std::size_t bytes_total = tokens.size() * 64;
    kv_handle_t      h    = 0;
    kv_buffer_desc_t slot{};
    if (kv_reserve(ctx, &loc, bytes_total, &h, &slot) != KV_OK) return;
    if (slot.addr) {
        std::memset(slot.addr, 0xCC, bytes_total);
    }
    kv_buffer_desc_t empty{};
    kv_publish(ctx, h, empty, bytes_total);
    kv_seal(ctx, h, tokens.data(), tokens.size());
}

}  // namespace

int main() {
    std::printf("kvcache bench: Lookup latency\n");
    std::printf("  sealed prefixes: %zu\n  probe count: %zu\n  tokens/prefix: %zu\n\n",
                  kSealedCount, kProbeCount, kPrefixTokens);

    // Open one ctx, seal N prefixes.
    kv_ctx_config_t cfg{};
    cfg.abi_version = KVCACHE_ABI_VERSION;
    cfg.tenant_id   = "bench";
    cfg.model_id    = "bench-model";
    kv_ctx_t* ctx = nullptr;
    if (kv_ctx_open(&cfg, &ctx) != KV_OK || !ctx) {
        std::fprintf(stderr, "kv_ctx_open failed\n");
        return 1;
    }
    std::vector<std::vector<uint32_t>> sealed;
    sealed.reserve(kSealedCount);
    const auto seal_t0 = NowNs();
    for (std::size_t i = 0; i < kSealedCount; ++i) {
        auto tk = MakePrefix(static_cast<uint32_t>(i + 1));
        SealOne(ctx, tk);
        sealed.push_back(std::move(tk));
    }
    const auto seal_elapsed_ns = NowNs() - seal_t0;
    std::printf("seed: sealed %zu prefixes in %.1f ms (%.1f µs/seal)\n\n",
                  kSealedCount, seal_elapsed_ns / 1.0e6,
                  (seal_elapsed_ns / 1000.0) / static_cast<double>(kSealedCount));

    std::mt19937_64 rng(0xC0FFEEull);
    std::uniform_int_distribution<std::size_t> pick(0, kSealedCount - 1);

    // ---- hit-only probes ------------------------------------------
    std::vector<uint64_t> hit_ns;
    hit_ns.reserve(kProbeCount);
    for (std::size_t i = 0; i < kProbeCount; ++i) {
        const auto& tokens = sealed[pick(rng)];
        kv_locator_t meta{};
        kv_handle_t  h = 0;
        uint32_t     m = 0;
        const auto t0 = NowNs();
        const int rc = kv_lookup(ctx, tokens.data(), tokens.size(),
                                    &meta, &h, &m);
        const auto t1 = NowNs();
        if (rc == KV_OK) {
            hit_ns.push_back(t1 - t0);
            kv_release(ctx, h);
        }
    }

    // ---- miss-only probes -----------------------------------------
    std::vector<uint64_t> miss_ns;
    miss_ns.reserve(kProbeCount);
    for (std::size_t i = 0; i < kProbeCount; ++i) {
        auto tokens = MakePrefix(
            static_cast<uint32_t>(kSealedCount + 1 + i));
        kv_locator_t meta{};
        kv_handle_t  h = 0;
        uint32_t     m = 0;
        const auto t0 = NowNs();
        kv_lookup(ctx, tokens.data(), tokens.size(),
                    &meta, &h, &m);
        const auto t1 = NowNs();
        miss_ns.push_back(t1 - t0);
    }

    // ---- mixed (~50/50) -------------------------------------------
    std::vector<uint64_t> mixed_ns;
    mixed_ns.reserve(kProbeCount);
    std::bernoulli_distribution flip(0.5);
    for (std::size_t i = 0; i < kProbeCount; ++i) {
        std::vector<uint32_t> tokens = flip(rng)
            ? sealed[pick(rng)]
            : MakePrefix(static_cast<uint32_t>(kSealedCount + 100000 + i));
        kv_locator_t meta{};
        kv_handle_t  h = 0;
        uint32_t     m = 0;
        const auto t0 = NowNs();
        const int rc = kv_lookup(ctx, tokens.data(), tokens.size(),
                                    &meta, &h, &m);
        const auto t1 = NowNs();
        mixed_ns.push_back(t1 - t0);
        if (rc == KV_OK) kv_release(ctx, h);
    }

    std::printf("Latencies (loopback, single-thread, warm cache)\n");
    std::printf("-----------------------------------------------------------------------------\n");
    PrintLatencyTable("kv_lookup [hit ]",   Summarise(hit_ns));
    PrintLatencyTable("kv_lookup [miss]",   Summarise(miss_ns));
    PrintLatencyTable("kv_lookup [mixed]",  Summarise(mixed_ns));
    std::printf("-----------------------------------------------------------------------------\n");

    kv_ctx_close(ctx);
    return 0;
}
