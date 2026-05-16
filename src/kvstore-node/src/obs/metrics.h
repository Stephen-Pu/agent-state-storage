// LLD §6.2 — Prometheus metrics, 3-tier cardinality control.
#pragma once

#include <cstdint>

namespace kvcache::node::obs {

// TODO(stephen): define interface and concrete implementation for metrics.
class Metrics {
   public:
    Metrics() = default;
    ~Metrics() = default;
};

}  // namespace kvcache::node::obs
