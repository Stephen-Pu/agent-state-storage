// LLD §5.1 — Priority context propagation header.
//
// Every internal message that crosses a subsystem boundary (gRPC RPC, NIXL
// Pull request, Priority Scheduler submission, RocksDB write) carries one of
// these. P0 = latency-critical, P1 = default, P2 = best-effort.
//
// The default class is P1. Engines that need P0 attach it at the caller end
// (kv_ctx_config_t extension — TODO when needed).
#pragma once

#include <cstdint>

namespace kvcache::node::qos {

enum class Priority : uint8_t {
    P0 = 0,
    P1 = 1,
    P2 = 2,
};

inline constexpr Priority kDefaultPriority = Priority::P1;

// PriorityContext travels in struct form alongside other request-scoped
// metadata. Keep it small — it's copied on every hop.
struct PriorityContext {
    Priority class_id   = kDefaultPriority;
    uint64_t request_id = 0;
    uint64_t tenant_id_hash = 0;
};

const char* PriorityName(Priority p);

}  // namespace kvcache::node::qos
