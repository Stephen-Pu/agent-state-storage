// LLD §6.1.3 — eventfd-based doorbell to avoid busy-wait.
#pragma once

#include <cstdint>

namespace kvcache::agent::shmem_ring {

class Doorbell {
   public:
    Doorbell() = default;
    ~Doorbell() = default;
};

}  // namespace kvcache::agent::shmem_ring
