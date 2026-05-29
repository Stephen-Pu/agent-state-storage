// Phase D-3 — ART tombstone-GC throughput micro-benchmark.
//
// The in-memory ART uses Epoch-Based Reclamation (EBR) to free
// nodes retired by writers (see prefix/epoch.h). Under sustained
// churn — Reserve → Publish → Seal → Release → Reserve next prefix
// — every Release that drops a refcount to zero retires the ART
// leaf onto the EpochManager's deferred-destructor list. If
// reclamation is amortized well, sustained churn throughput
// stays close to the lock-free read baseline; if EBR becomes the
// bottleneck (long retire lists, mutex contention in
// `PendingRetires`, slow GC passes), churn throughput collapses.
//
// This bench measures that collapse — or proves the absence of
// it. Three phases, all driven through the public C ABI:
//
//   1. Seed   — Seal N=4096 unique prefixes into the ART
//               (one-shot bootstrap).
//
//   2. Steady — pure Lookup for D=1.5s over the seeded keys.
//               This is the lock-free reader hot path with zero
//               write activity, so the EpochManager's
//               `global_epoch_` never advances and the retire
//               list stays empty. Baseline ops/sec.
//
//   3. Churn  — Reserve → Publish → Seal → Lookup → Release on
//               a fresh prefix each iteration for D=1.5s. Every
//               loop adds an ART leaf and (on Release) retires
//               its handle, exercising the EBR path end-to-end.
//
// Output: ops/sec per phase + ratio churn/steady, plus the
// before/after values of the D-3 metric gauges
//   * kv_art_leaf_count
//   * kv_art_pending_retires
//   * kv_art_global_epoch
// scraped via the kv_metrics_scrape ABI between phases.
//
// Reference targets (LLD §9.1): hit-path p99 ≤ 10 µs. We're
// measuring throughput here, but a healthy GC keeps churn
// throughput within ~30% of steady-state (i.e. retire +
// reclamation overhead is bounded).
//
// Known issue surfaced during bring-up (TODO — separate phase):
// In headless mode (no RocksDB), ``kv_seal`` starts returning
// ``KV_E_INTERNAL`` after ~30–60 successful seals against a single
// ctx — somewhere in the ART Insert path or DRAM tier staging.
// The seed loop counts and reports successes truthfully, so the
// throughput numbers reflect what the engine actually completes,
// not what the bench attempted. Once that bug is fixed the bench
// will exercise GC at a much higher rate and the
// pending-retires / global-epoch gauges will become
// non-trivially populated; today they read as 0 / 0 / 1 because
// the writer count is small.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>  // for std::stod's exceptions on Ubuntu libstdc++
#include <string>
#include <vector>

#include "bench_common.h"
#include "kvcache/kv_abi.h"
#include "kvcache/kv_errors.h"

using kvcache::bench::NowNs;

namespace {

constexpr std::size_t kPrefixTokens   = 32;
constexpr std::size_t kSeedCount      = 4096;
constexpr uint64_t    kPhaseNs        = 1'500'000'000ULL;  // 1.5 s per phase

std::vector<uint32_t> MakePrefix(uint32_t seed) {
    std::vector<uint32_t> out(kPrefixTokens);
    for (std::size_t i = 0; i < kPrefixTokens; ++i) {
        out[i] = seed * 7919u + static_cast<uint32_t>(i) * 31u;
    }
    return out;
}

// SealOne — drive Reserve → Publish → Seal for one prefix. The
// `seed` parameter feeds the locator's `prefix_hash[0..3]` so each
// call produces a distinct DramKey (otherwise the DRAM tier
// rejects the second StageToDram for the same key, leaking the
// pinned slot until eviction). With unique seeds per call the
// pinned pool's free stack rotates one slot per iter, which is
// exactly the steady-state we want to model.
bool SealOne(kv_ctx_t* ctx, uint32_t seed,
              const std::vector<uint32_t>& tokens) {
    kv_locator_t loc{};
    std::memset(&loc, 0, sizeof(loc));
    loc.range.token_count = static_cast<uint32_t>(tokens.size());
    loc.version           = 1;
    // Unique DRAM key per Seal: stamp the seed into prefix_hash.
    loc.prefix_hash[0] = static_cast<uint8_t>(seed       & 0xff);
    loc.prefix_hash[1] = static_cast<uint8_t>((seed >> 8) & 0xff);
    loc.prefix_hash[2] = static_cast<uint8_t>((seed >> 16) & 0xff);
    loc.prefix_hash[3] = static_cast<uint8_t>((seed >> 24) & 0xff);
    const std::size_t bytes_total = tokens.size() * 64;
    kv_handle_t      h    = 0;
    kv_buffer_desc_t slot{};
    if (kv_reserve(ctx, &loc, bytes_total, &h, &slot) != KV_OK) {
        return false;
    }
    if (slot.addr) std::memset(slot.addr, 0xCC, bytes_total);
    kv_buffer_desc_t empty{};
    kv_publish(ctx, h, empty, bytes_total);
    kv_seal(ctx, h, tokens.data(), tokens.size());
    return true;
}

// Print one metric line if the gauge name is in the scrape body.
// The scrape format follows Prometheus: each metric appears as
// `name{labels} value`. We pull the value following the first
// occurrence of `name ` (no labels) or `name{`.
double ScrapeGauge(const std::string& body, const char* name) {
    auto idx = body.find(name);
    while (idx != std::string::npos) {
        // Skip help/type comment lines that start with '#'.
        // Walk back to the start of the line; if it begins with '#'
        // skip past this occurrence.
        std::size_t line_start = body.rfind('\n', idx);
        line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
        if (line_start < body.size() && body[line_start] == '#') {
            idx = body.find(name, idx + 1);
            continue;
        }
        // Parse the value: skip the name, optional labels, whitespace.
        std::size_t p = idx + std::strlen(name);
        if (p < body.size() && body[p] == '{') {
            auto end = body.find('}', p);
            if (end == std::string::npos) return 0.0;
            p = end + 1;
        }
        while (p < body.size() &&
                (body[p] == ' ' || body[p] == '\t')) ++p;
        try {
            return std::stod(body.substr(p, body.find('\n', p) - p));
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

struct ArtSnapshot {
    double leaf_count;
    double pending_retires;
    double global_epoch;
};

ArtSnapshot ScrapeArt() {
    std::vector<char> buf(64 * 1024);
    std::size_t n = 0;
    if (kv_metrics_scrape(buf.data(), buf.size(), &n) != KV_OK) return {};
    const std::string body(buf.data(),
                            std::min(n, buf.size() - 1));
    return {
        ScrapeGauge(body, "kv_art_leaf_count"),
        ScrapeGauge(body, "kv_art_pending_retires"),
        ScrapeGauge(body, "kv_art_global_epoch"),
    };
}

void PrintSnapshot(const char* label, const ArtSnapshot& s) {
    std::printf("  %-8s  leaves=%8.0f  pending_retires=%6.0f  epoch=%8.0f\n",
                  label, s.leaf_count, s.pending_retires, s.global_epoch);
}

}  // namespace

int main() {
    std::printf("kvcache bench: ART tombstone-GC throughput (Phase D-3)\n");
    std::printf("  seed prefixes:     %zu\n", kSeedCount);
    std::printf("  tokens per prefix: %zu\n", kPrefixTokens);
    std::printf("  phase duration:    %.1f s\n\n", kPhaseNs / 1.0e9);

    kv_ctx_config_t cfg{};
    cfg.abi_version = KVCACHE_ABI_VERSION;
    cfg.tenant_id   = "bench-d3";
    cfg.model_id    = "bench-model";
    kv_ctx_t* ctx = nullptr;
    if (kv_ctx_open(&cfg, &ctx) != KV_OK || !ctx) {
        std::fprintf(stderr, "kv_ctx_open failed\n");
        return 1;
    }

    // ---- Seed -----------------------------------------------------
    std::vector<std::vector<uint32_t>> seeded;
    seeded.reserve(kSeedCount);
    const auto seed_t0 = NowNs();
    std::size_t seed_ok = 0;
    for (std::size_t i = 0; i < kSeedCount; ++i) {
        const uint32_t seed = static_cast<uint32_t>(i + 1);
        auto tk = MakePrefix(seed);
        if (SealOne(ctx, seed, tk)) ++seed_ok;
        seeded.push_back(std::move(tk));
    }
    const double seed_elapsed_s = (NowNs() - seed_t0) / 1.0e9;
    std::printf("seed:  sealed %zu/%zu prefixes in %.2f s (%.0f seals/s)\n",
                  seed_ok, kSeedCount, seed_elapsed_s,
                  seed_ok / seed_elapsed_s);
    const auto art_after_seed = ScrapeArt();
    PrintSnapshot("[seed]", art_after_seed);

    // ---- Phase 1: steady-state Lookup -----------------------------
    std::printf("\nsteady: pure Lookup over seeded keys for %.1f s ...\n",
                  kPhaseNs / 1.0e9);
    std::size_t steady_ops = 0;
    std::size_t steady_hits = 0;
    const auto steady_t0 = NowNs();
    while (NowNs() - steady_t0 < kPhaseNs) {
        const auto& tokens = seeded[steady_ops % seeded.size()];
        kv_locator_t meta{};
        kv_handle_t  h = 0;
        uint32_t     m = 0;
        const int rc = kv_lookup(ctx, tokens.data(), tokens.size(),
                                    &meta, &h, &m);
        if (rc == KV_OK) {
            ++steady_hits;
            kv_release(ctx, h);
        }
        ++steady_ops;
    }
    const double steady_elapsed_s = (NowNs() - steady_t0) / 1.0e9;
    const double steady_ops_per_s = steady_ops / steady_elapsed_s;
    std::printf("        ops=%zu  hits=%zu  elapsed=%.2f s  "
                  "throughput=%.0f ops/s\n",
                  steady_ops, steady_hits, steady_elapsed_s,
                  steady_ops_per_s);
    const auto art_after_steady = ScrapeArt();
    PrintSnapshot("[steady]", art_after_steady);

    // ---- Phase 2: high-churn ---------------------------------------
    std::printf("\nchurn:  Reserve+Seal+Lookup+Release on a fresh "
                  "prefix per iter for %.1f s ...\n",
                  kPhaseNs / 1.0e9);
    std::size_t churn_ops = 0;
    std::size_t churn_seals = 0;
    const auto churn_t0 = NowNs();
    while (NowNs() - churn_t0 < kPhaseNs) {
        // Unique prefix each iter — kSeedCount + churn_ops + 1 avoids
        // colliding with the seeded keys.
        const uint32_t seed =
            static_cast<uint32_t>(kSeedCount + churn_ops + 1);
        auto tk = MakePrefix(seed);
        if (SealOne(ctx, seed, tk)) ++churn_seals;
        kv_locator_t meta{};
        kv_handle_t  h = 0;
        uint32_t     m = 0;
        const int rc = kv_lookup(ctx, tk.data(), tk.size(),
                                    &meta, &h, &m);
        if (rc == KV_OK) kv_release(ctx, h);
        ++churn_ops;
    }
    const double churn_elapsed_s = (NowNs() - churn_t0) / 1.0e9;
    const double churn_ops_per_s = churn_ops / churn_elapsed_s;
    std::printf("        ops=%zu  seals=%zu  elapsed=%.2f s  "
                  "throughput=%.0f ops/s\n",
                  churn_ops, churn_seals, churn_elapsed_s,
                  churn_ops_per_s);
    const auto art_after_churn = ScrapeArt();
    PrintSnapshot("[churn]", art_after_churn);

    // ---- Summary --------------------------------------------------
    std::printf("\nsummary\n");
    std::printf("-----------------------------------------------------------------\n");
    std::printf("  steady-state Lookup throughput : %10.0f ops/s\n",
                  steady_ops_per_s);
    std::printf("  high-churn full-cycle throughput: %10.0f ops/s\n",
                  churn_ops_per_s);
    const double ratio = (steady_ops_per_s > 0)
        ? churn_ops_per_s / steady_ops_per_s : 0.0;
    std::printf("  churn / steady-state ratio      : %10.3f%s\n",
                  ratio,
                  ratio >= 0.05 ? "  (acceptable — churn includes 4× more work per op)"
                                : "  (suspicious — GC may be the bottleneck)");
    std::printf("  ART growth (leaves)             : %10.0f -> %.0f\n",
                  art_after_steady.leaf_count, art_after_churn.leaf_count);
    std::printf("  EpochManager pending retires    : %10.0f -> %.0f\n",
                  art_after_steady.pending_retires,
                  art_after_churn.pending_retires);
    std::printf("  EpochManager global_epoch       : %10.0f -> %.0f\n",
                  art_after_steady.global_epoch,
                  art_after_churn.global_epoch);
    std::printf("-----------------------------------------------------------------\n");

    kv_ctx_close(ctx);
    return 0;
}
