// Phase S-1 — shared helpers for the kvcache micro-benchmarks.
//
// All benches link against libkvcache.dylib (the public C ABI) and
// measure end-to-end through the same surface engines use. The
// helpers here are deliberately tiny — no external benchmark
// framework, just <chrono>, sort, and a printf table.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace kvcache::bench {

// Sort+pick percentile over a vector of latencies in nanoseconds.
inline uint64_t Percentile(std::vector<uint64_t>& ns, double p) {
    if (ns.empty()) return 0;
    std::sort(ns.begin(), ns.end());
    const std::size_t idx = std::min(
        ns.size() - 1,
        static_cast<std::size_t>(p * static_cast<double>(ns.size())));
    return ns[idx];
}

struct LatencyStats {
    std::size_t n         = 0;
    uint64_t    min_ns    = 0;
    uint64_t    p50_ns    = 0;
    uint64_t    p95_ns    = 0;
    uint64_t    p99_ns    = 0;
    uint64_t    p999_ns   = 0;
    uint64_t    max_ns    = 0;
    double      mean_ns   = 0;
};

inline LatencyStats Summarise(std::vector<uint64_t> ns) {
    LatencyStats s;
    s.n = ns.size();
    if (ns.empty()) return s;
    long double sum = 0;
    for (auto v : ns) sum += v;
    s.mean_ns = static_cast<double>(sum / s.n);
    s.min_ns  = *std::min_element(ns.begin(), ns.end());
    s.max_ns  = *std::max_element(ns.begin(), ns.end());
    // The percentile pass sorts in place.
    s.p50_ns  = Percentile(ns, 0.50);
    s.p95_ns  = Percentile(ns, 0.95);
    s.p99_ns  = Percentile(ns, 0.99);
    s.p999_ns = Percentile(ns, 0.999);
    return s;
}

inline void PrintLatencyTable(const std::string& label,
                                const LatencyStats& s) {
    std::printf("%-28s  n=%zu  min=%7.1f µs  p50=%7.1f µs  "
                  "p95=%7.1f µs  p99=%7.1f µs  p99.9=%7.1f µs  "
                  "max=%7.1f µs  mean=%7.1f µs\n",
                  label.c_str(), s.n,
                  s.min_ns / 1000.0,  s.p50_ns / 1000.0,
                  s.p95_ns / 1000.0,  s.p99_ns / 1000.0,
                  s.p999_ns / 1000.0, s.max_ns / 1000.0,
                  s.mean_ns / 1000.0);
}

inline uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace kvcache::bench
