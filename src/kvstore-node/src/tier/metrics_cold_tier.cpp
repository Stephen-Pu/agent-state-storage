// LLD §9.1 — MetricsColdTier implementation (Phase O-4).
#include "tier/metrics_cold_tier.h"

#include "metrics.h"

namespace kvcache::node::tier {

std::unique_ptr<MetricsColdTier> MetricsColdTier::Create(
    std::unique_ptr<IColdTier> inner, metrics::Registry& reg,
    std::string* err) {
    if (!inner) {
        if (err) *err = "cold_tier/metrics: inner tier is null";
        return nullptr;
    }
    auto t = std::unique_ptr<MetricsColdTier>(new MetricsColdTier());
    t->inner_ = std::move(inner);

    t->put_total_     = &reg.GetOrCreateCounter("kv_cold_put_total",          "Cold-tier Put calls",                {});
    t->put_bytes_     = &reg.GetOrCreateCounter("kv_cold_put_bytes_total",    "Cold-tier bytes written via Put",    {});
    t->put_errors_    = &reg.GetOrCreateCounter("kv_cold_put_errors_total",   "Cold-tier Put failures",             {});
    t->get_total_     = &reg.GetOrCreateCounter("kv_cold_get_total",          "Cold-tier Get hits",                 {});
    t->get_bytes_     = &reg.GetOrCreateCounter("kv_cold_get_bytes_total",    "Cold-tier bytes read via Get",       {});
    t->get_miss_      = &reg.GetOrCreateCounter("kv_cold_get_miss_total",     "Cold-tier Get misses (not found)",   {});
    t->get_errors_    = &reg.GetOrCreateCounter("kv_cold_get_errors_total",   "Cold-tier Get failures",             {});
    t->delete_total_  = &reg.GetOrCreateCounter("kv_cold_delete_total",       "Cold-tier Delete calls",             {});
    t->delete_errors_ = &reg.GetOrCreateCounter("kv_cold_delete_errors_total","Cold-tier Delete failures",          {});
    t->exists_total_  = &reg.GetOrCreateCounter("kv_cold_exists_total",       "Cold-tier Exists checks",            {});

    // Seed every series at 0 so a scrape before the first op sees the line.
    for (auto* c : {t->put_total_, t->put_bytes_, t->put_errors_, t->get_total_,
                    t->get_bytes_, t->get_miss_, t->get_errors_, t->delete_total_,
                    t->delete_errors_, t->exists_total_}) {
        c->Inc(0.0, {});
    }
    return t;
}

std::string MetricsColdTier::Name() const { return inner_->Name() + "+metrics"; }

bool MetricsColdTier::Put(const DramKey& key, const uint8_t* data,
                          std::size_t n, std::string* err) {
    put_total_->Inc(1.0, {});
    const bool ok = inner_->Put(key, data, n, err);
    if (ok) put_bytes_->Inc(static_cast<double>(n), {});
    else    put_errors_->Inc(1.0, {});
    return ok;
}

bool MetricsColdTier::Get(const DramKey& key, std::vector<uint8_t>* out,
                          std::string* err) {
    std::string local_err;
    const bool ok = inner_->Get(key, out, &local_err);
    if (ok) {
        get_total_->Inc(1.0, {});
        if (out) get_bytes_->Inc(static_cast<double>(out->size()), {});
    } else if (local_err.empty()) {
        get_miss_->Inc(1.0, {});   // clean not-found, per IColdTier convention
    } else {
        get_errors_->Inc(1.0, {});
    }
    if (err) *err = local_err;
    return ok;
}

bool MetricsColdTier::Delete(const DramKey& key, std::string* err) {
    delete_total_->Inc(1.0, {});
    const bool ok = inner_->Delete(key, err);
    if (!ok) delete_errors_->Inc(1.0, {});
    return ok;
}

bool MetricsColdTier::Exists(const DramKey& key) const {
    exists_total_->Inc(1.0, {});
    return inner_->Exists(key);
}

}  // namespace kvcache::node::tier
