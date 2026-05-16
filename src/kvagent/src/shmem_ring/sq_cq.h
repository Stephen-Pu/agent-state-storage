// LLD §6.1.3 — MPMC SQ/CQ rings in /dev/shm; header versioning.
#pragma once

#include <cstdint>

namespace kvcache::agent::shmem_ring {

class SqCq {
   public:
    SqCq() = default;
    ~SqCq() = default;
};

}  // namespace kvcache::agent::shmem_ring
