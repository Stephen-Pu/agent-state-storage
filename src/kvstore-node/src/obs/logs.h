// LLD §6.2 — Structured JSON logging facade.
#pragma once

#include <cstdint>

namespace kvcache::node::obs {

// TODO(stephen): define interface and concrete implementation for logs.
class Logs {
   public:
    Logs() = default;
    ~Logs() = default;
};

}  // namespace kvcache::node::obs
