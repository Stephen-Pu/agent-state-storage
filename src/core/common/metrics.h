// Prometheus metrics facade. LLD §6.2.
//
// Cardinality discipline (LLD §6.2.1):
//   Tier 1 (always-on, < 1k labels):    request count, latency hist, hit rate,
//                                       capacity, NIXL bytes
//   Tier 2 (aggregated, < 100k):        per-tenant + per-model rollups
//   Tier 3 (on-demand, debugging only): per-prefix, per-request trace
//
// MVP implementation: in-process maps from (metric, label-values) to atomic
// counters/gauges/histograms; `Registry::Scrape` emits Prometheus text
// exposition format. Phase-2 may switch to prometheus-cpp.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kvcache::metrics {

class Counter;
class Gauge;
class Histogram;

struct Label {
    std::string_view key;
    std::string_view value;
};

// ---- Internal series storage. Public types just expose Inc/Set/Observe. ----

namespace internal {

// All metric series share a label key vector (registered at create time) plus
// per-label-values storage.
struct Series {
    std::vector<std::string> label_keys;
    // Map label-values-string → value. Counter & Gauge store doubles;
    // Histogram stores BucketCounts. We use a single atomic-double approach
    // for counters/gauges and a struct for histograms.
};

struct CounterSeries : Series {
    std::mutex                                        mu;
    std::unordered_map<std::string, std::atomic<uint64_t>> values;  // *1000 fixed-point
};
struct GaugeSeries : Series {
    std::mutex                                        mu;
    std::unordered_map<std::string, std::atomic<int64_t>>   values;
};
struct HistogramBucket {
    std::vector<uint64_t> counts;   // size = bucket_bounds.size()+1 (incl. +Inf)
    uint64_t              count = 0;
    double                sum = 0.0;
};
struct HistogramSeries : Series {
    std::vector<double>                              bucket_bounds;  // sorted asc
    std::mutex                                        mu;
    std::unordered_map<std::string, HistogramBucket>  values;
};

}  // namespace internal

class Counter {
   public:
    Counter() noexcept = default;
    explicit Counter(internal::CounterSeries* s) noexcept : series_(s) {}
    void Inc(double v, std::span<const Label> labels) noexcept;

   private:
    internal::CounterSeries* series_ = nullptr;
};

class Gauge {
   public:
    Gauge() noexcept = default;
    explicit Gauge(internal::GaugeSeries* s) noexcept : series_(s) {}
    void Set(double v, std::span<const Label> labels) noexcept;
    void Inc(double v, std::span<const Label> labels) noexcept;
    void Dec(double v, std::span<const Label> labels) noexcept;

   private:
    internal::GaugeSeries* series_ = nullptr;
};

class Histogram {
   public:
    Histogram() noexcept = default;
    explicit Histogram(internal::HistogramSeries* s) noexcept : series_(s) {}
    void Observe(double v, std::span<const Label> labels) noexcept;

   private:
    internal::HistogramSeries* series_ = nullptr;
};

class Registry {
   public:
    static Registry& Default();

    Counter&   GetOrCreateCounter  (std::string_view name,
                                    std::string_view help,
                                    std::span<const std::string_view> label_keys);
    Gauge&     GetOrCreateGauge    (std::string_view name,
                                    std::string_view help,
                                    std::span<const std::string_view> label_keys);
    Histogram& GetOrCreateHistogram(std::string_view name,
                                    std::string_view help,
                                    std::span<const std::string_view> label_keys,
                                    std::span<const double> bucket_bounds);

    void Scrape(std::string& out) const;

    // Phase B10.2 — scrape-time hook. A hook appends extra Prometheus
    // lines to the scrape body on every Scrape() call — used to surface
    // metrics that live OUTSIDE the registered Counter/Gauge/Histogram
    // set (e.g. RocksDB's internal Statistics tickers). Hooks run AFTER
    // the registered series are rendered and OUTSIDE the registry mutex,
    // so a hook may safely take its own locks (RocksDB does) and even
    // touch the Registry without deadlocking. Hooks are append-only
    // (no removal) — they live for the process; register at startup.
    using ScrapeHook = std::function<void(std::string& out)>;
    void RegisterScrapeHook(ScrapeHook hook);

   private:
    struct CounterEntry  { std::string help; std::unique_ptr<internal::CounterSeries> s;   Counter   handle; };
    struct GaugeEntry    { std::string help; std::unique_ptr<internal::GaugeSeries> s;     Gauge     handle; };
    struct HistEntry     { std::string help; std::unique_ptr<internal::HistogramSeries> s; Histogram handle; };

    mutable std::mutex                                       mu_;
    std::vector<ScrapeHook>                                  scrape_hooks_;
    std::unordered_map<std::string, CounterEntry>            counters_;
    std::unordered_map<std::string, GaugeEntry>              gauges_;
    std::unordered_map<std::string, HistEntry>               histograms_;
};

// Conventional canonical metric names (kept in sync with Helm dashboards and
// the 5 default alert rules in deploy/helm/).
namespace name {
inline constexpr std::string_view RequestsTotal    = "kv_requests_total";
inline constexpr std::string_view LatencySeconds   = "kv_latency_seconds";
inline constexpr std::string_view CacheHitsTotal   = "kv_cache_hits_total";
inline constexpr std::string_view CapacityBytes    = "kv_capacity_bytes";
inline constexpr std::string_view NixlBytesTotal   = "kv_nixl_bytes_total";

inline constexpr std::string_view TenantUsageBytes = "kv_tenant_usage_bytes";
inline constexpr std::string_view TenantP99Latency = "kv_tenant_p99_latency";
inline constexpr std::string_view ModelRequestCount= "kv_model_request_count";
inline constexpr std::string_view NodeEvictionRate = "kv_node_eviction_rate";
}  // namespace name

}  // namespace kvcache::metrics
