// LLD §9.1 — Cold-tier observability (Phase O-4).
//
// `MetricsColdTier` is a decorator over any `IColdTier` that records
// operation counts, bytes, and error/miss rates to the shared metrics
// `Registry`, surfacing the cold tier on the same `/metrics` Prometheus
// scrape as the pinned-tier counters (Phase G-3). It's wrapped OUTERMOST in
// the cold-tier stack (above compression / encryption) so the numbers it
// reports are the logical, operator-facing ones: how many Put/Get/Delete the
// node issued, how many bytes flowed through the API, and how often a Get
// missed vs errored.
//
// Like CompressingColdTier / EncryptingColdTier it's a transparent decorator
// — Name() composition aside, callers and the inner tier see no behavioural
// change. Miss vs error on Get follows the IColdTier convention: inner Get
// returns false with an EMPTY *err on a clean not-found, and a non-empty
// *err on a real failure.
//
// Emitted series (no labels, prefix kv_cold_):
//   kv_cold_put_total / kv_cold_put_bytes_total / kv_cold_put_errors_total
//   kv_cold_get_total / kv_cold_get_bytes_total / kv_cold_get_miss_total
//                      / kv_cold_get_errors_total
//   kv_cold_delete_total / kv_cold_delete_errors_total
//   kv_cold_exists_total
#pragma once

#include <memory>
#include <string>

#include "tier/cold_tier.h"

namespace kvcache::metrics { class Counter; class Registry; }

namespace kvcache::node::tier {

class MetricsColdTier final : public IColdTier {
   public:
    // Takes ownership of the inner tier; records to `registry`. Returns
    // nullptr + *err if inner is null. Counters are seeded at 0 so a scrape
    // at t=0 sees `metric 0` rather than an absent series.
    static std::unique_ptr<MetricsColdTier> Create(
        std::unique_ptr<IColdTier> inner, metrics::Registry& registry,
        std::string* err);

    ~MetricsColdTier() override = default;

    std::string Name() const override;

    bool Put   (const DramKey&, const uint8_t* data, std::size_t n, std::string* err) override;
    bool Get   (const DramKey&, std::vector<uint8_t>* out, std::string* err) override;
    bool Delete(const DramKey&, std::string* err) override;
    bool Exists(const DramKey&) const override;

   private:
    MetricsColdTier() = default;

    std::unique_ptr<IColdTier> inner_;
    // Borrowed Counter handles owned by the Registry (stable across its
    // lifetime). The Registry must outlive this tier.
    metrics::Counter* put_total_        = nullptr;
    metrics::Counter* put_bytes_        = nullptr;
    metrics::Counter* put_errors_       = nullptr;
    metrics::Counter* get_total_        = nullptr;
    metrics::Counter* get_bytes_        = nullptr;
    metrics::Counter* get_miss_         = nullptr;
    metrics::Counter* get_errors_       = nullptr;
    metrics::Counter* delete_total_     = nullptr;
    metrics::Counter* delete_errors_    = nullptr;
    metrics::Counter* exists_total_     = nullptr;
};

}  // namespace kvcache::node::tier
