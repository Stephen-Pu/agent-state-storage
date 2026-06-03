// Prometheus metrics implementation. LLD §6.2.
//
// In-process storage + Prometheus text-exposition format export. The chosen
// scope is "good enough for a real /metrics endpoint" and stays bounded to
// the cardinality budget by relying on caller discipline (LLD §6.2.1).
#include "metrics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>

namespace kvcache::metrics {

namespace {

// Encode label values into a stable key for the map. Format: v1\0v2\0v3.
std::string LabelKeyFor(std::span<const Label> labels,
                        const std::vector<std::string>& label_keys) {
    std::string out;
    out.reserve(64);
    // For each registered label key in order, find the caller's value (or "")
    // so the map key is stable regardless of caller ordering.
    for (const auto& k : label_keys) {
        bool found = false;
        for (const auto& l : labels) {
            if (l.key == k) {
                out.append(l.value.data(), l.value.size());
                found = true;
                break;
            }
        }
        out.push_back('\0');
        (void)found;  // missing value renders as empty string
    }
    return out;
}

std::string EscapeLabelValue(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\\' || c == '"' || c == '\n') {
            out.push_back('\\');
            out.push_back(c == '\n' ? 'n' : c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Render a label set inline: {k1="v1",k2="v2"}. Empty if no labels.
std::string RenderLabels(const std::vector<std::string>& keys,
                          const std::string& packed) {
    if (keys.empty()) return {};
    std::string out;
    out.push_back('{');
    std::size_t off = 0;
    bool first = true;
    for (const auto& k : keys) {
        const std::size_t end = packed.find('\0', off);
        const std::string v = packed.substr(off, end - off);
        if (!first) out.push_back(',');
        first = false;
        out.append(k);
        out.append("=\"");
        out.append(EscapeLabelValue(v));
        out.push_back('"');
        off = end + 1;
    }
    out.push_back('}');
    return out;
}

constexpr uint64_t kCounterScale = 1000;  // 3 decimal digits of precision

}  // namespace

// ---------------------------------------------------------------------------
// Counter / Gauge / Histogram hot paths
// ---------------------------------------------------------------------------

void Counter::Inc(double v, std::span<const Label> labels) noexcept {
    if (!series_ || v < 0) return;
    const std::string k = LabelKeyFor(labels, series_->label_keys);
    std::lock_guard lk(series_->mu);
    auto [it, _] = series_->values.try_emplace(k, 0);
    it->second.fetch_add(static_cast<uint64_t>(v * kCounterScale),
                          std::memory_order_relaxed);
}

void Gauge::Set(double v, std::span<const Label> labels) noexcept {
    if (!series_) return;
    const std::string k = LabelKeyFor(labels, series_->label_keys);
    std::lock_guard lk(series_->mu);
    auto [it, _] = series_->values.try_emplace(k, 0);
    it->second.store(static_cast<int64_t>(v * kCounterScale),
                      std::memory_order_relaxed);
}
void Gauge::Inc(double v, std::span<const Label> labels) noexcept {
    if (!series_) return;
    const std::string k = LabelKeyFor(labels, series_->label_keys);
    std::lock_guard lk(series_->mu);
    auto [it, _] = series_->values.try_emplace(k, 0);
    it->second.fetch_add(static_cast<int64_t>(v * kCounterScale),
                          std::memory_order_relaxed);
}
void Gauge::Dec(double v, std::span<const Label> labels) noexcept {
    Inc(-v, labels);
}

void Histogram::Observe(double v, std::span<const Label> labels) noexcept {
    if (!series_) return;
    const std::string k = LabelKeyFor(labels, series_->label_keys);
    std::lock_guard lk(series_->mu);
    auto [it, inserted] = series_->values.try_emplace(k);
    if (inserted) {
        it->second.counts.assign(series_->bucket_bounds.size() + 1, 0);
    }
    auto& b = it->second;
    b.count++;
    b.sum += v;
    // Find the lowest bucket that v belongs to.
    auto upper = std::upper_bound(series_->bucket_bounds.begin(),
                                    series_->bucket_bounds.end(), v);
    // Cumulative counters: bucket i counts observations with value <= bounds[i].
    const std::size_t idx = static_cast<std::size_t>(
        upper - series_->bucket_bounds.begin());
    for (std::size_t i = idx; i < b.counts.size(); ++i) b.counts[i]++;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Registry& Registry::Default() {
    static Registry inst;
    return inst;
}

Counter& Registry::GetOrCreateCounter(std::string_view name, std::string_view help,
                                       std::span<const std::string_view> label_keys) {
    std::lock_guard lk(mu_);
    auto it = counters_.find(std::string(name));
    if (it == counters_.end()) {
        CounterEntry e;
        e.help = std::string(help);
        e.s    = std::make_unique<internal::CounterSeries>();
        for (auto k : label_keys) e.s->label_keys.emplace_back(k);
        e.handle = Counter(e.s.get());
        it = counters_.emplace(std::string(name), std::move(e)).first;
    }
    return it->second.handle;
}

Gauge& Registry::GetOrCreateGauge(std::string_view name, std::string_view help,
                                   std::span<const std::string_view> label_keys) {
    std::lock_guard lk(mu_);
    auto it = gauges_.find(std::string(name));
    if (it == gauges_.end()) {
        GaugeEntry e;
        e.help = std::string(help);
        e.s    = std::make_unique<internal::GaugeSeries>();
        for (auto k : label_keys) e.s->label_keys.emplace_back(k);
        e.handle = Gauge(e.s.get());
        it = gauges_.emplace(std::string(name), std::move(e)).first;
    }
    return it->second.handle;
}

Histogram& Registry::GetOrCreateHistogram(std::string_view name, std::string_view help,
                                          std::span<const std::string_view> label_keys,
                                          std::span<const double> bucket_bounds) {
    std::lock_guard lk(mu_);
    auto it = histograms_.find(std::string(name));
    if (it == histograms_.end()) {
        HistEntry e;
        e.help = std::string(help);
        e.s    = std::make_unique<internal::HistogramSeries>();
        for (auto k : label_keys)    e.s->label_keys.emplace_back(k);
        for (auto b : bucket_bounds) e.s->bucket_bounds.push_back(b);
        std::sort(e.s->bucket_bounds.begin(), e.s->bucket_bounds.end());
        e.handle = Histogram(e.s.get());
        it = histograms_.emplace(std::string(name), std::move(e)).first;
    }
    return it->second.handle;
}

void Registry::Scrape(std::string& out) const {
    std::ostringstream os;
    std::vector<ScrapeHook> hooks;
    {
    std::lock_guard lk(mu_);

    // Counters
    for (const auto& [name, e] : counters_) {
        os << "# HELP " << name << " " << e.help << "\n";
        os << "# TYPE " << name << " counter\n";
        std::lock_guard lks(e.s->mu);
        for (const auto& [packed, val] : e.s->values) {
            const double v = static_cast<double>(val.load(std::memory_order_relaxed))
                              / static_cast<double>(kCounterScale);
            os << name << RenderLabels(e.s->label_keys, packed)
               << " " << v << "\n";
        }
    }
    // Gauges
    for (const auto& [name, e] : gauges_) {
        os << "# HELP " << name << " " << e.help << "\n";
        os << "# TYPE " << name << " gauge\n";
        std::lock_guard lks(e.s->mu);
        for (const auto& [packed, val] : e.s->values) {
            const double v = static_cast<double>(val.load(std::memory_order_relaxed))
                              / static_cast<double>(kCounterScale);
            os << name << RenderLabels(e.s->label_keys, packed)
               << " " << v << "\n";
        }
    }
    // Histograms
    for (const auto& [name, e] : histograms_) {
        os << "# HELP " << name << " " << e.help << "\n";
        os << "# TYPE " << name << " histogram\n";
        std::lock_guard lks(e.s->mu);
        for (const auto& [packed, b] : e.s->values) {
            const std::string lbls = RenderLabels(e.s->label_keys, packed);
            for (std::size_t i = 0; i < e.s->bucket_bounds.size(); ++i) {
                // Insert "le" label into the existing brace set. If lbls is
                // empty we start a new one.
                std::string lb;
                if (lbls.empty()) {
                    lb = "{le=\"" + std::to_string(e.s->bucket_bounds[i]) + "\"}";
                } else {
                    lb = lbls.substr(0, lbls.size() - 1) +
                         ",le=\"" + std::to_string(e.s->bucket_bounds[i]) + "\"}";
                }
                os << name << "_bucket" << lb << " " << b.counts[i] << "\n";
            }
            const std::string lb_inf = lbls.empty()
                ? std::string("{le=\"+Inf\"}")
                : lbls.substr(0, lbls.size() - 1) + ",le=\"+Inf\"}";
            os << name << "_bucket" << lb_inf << " " << b.count << "\n";
            os << name << "_sum"   << lbls   << " " << b.sum   << "\n";
            os << name << "_count" << lbls   << " " << b.count << "\n";
        }
    }
    hooks = scrape_hooks_;  // copy under mu_ so we can run them unlocked
    }  // release mu_

    out = os.str();
    // Phase B10.2 — run scrape hooks OUTSIDE the registry mutex so a hook
    // may take its own locks (RocksDB Statistics) or re-enter the Registry
    // without deadlocking. Each hook appends its own Prometheus lines.
    for (const auto& h : hooks) {
        if (h) h(out);
    }
}

void Registry::RegisterScrapeHook(ScrapeHook hook) {
    std::lock_guard lk(mu_);
    scrape_hooks_.push_back(std::move(hook));
}

}  // namespace kvcache::metrics
