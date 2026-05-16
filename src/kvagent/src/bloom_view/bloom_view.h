// LLD §4.2 — Cluster-wide bloom-sketch view (30s refresh).
#pragma once

#include <cstdint>

namespace kvcache::agent::bloom_view {

class BloomView {
   public:
    BloomView() = default;
    ~BloomView() = default;
};

}  // namespace kvcache::agent::bloom_view
