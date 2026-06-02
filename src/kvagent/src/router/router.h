// LLD §4.2 / §6.1.5 — kvagent request router.
//
// Phase A1.6 — the real SQ handler that replaces main.cpp's echo
// placeholder. An engine posts a fixed-layout Request into the SQ
// ring; the router decides where the prefix lives and posts a Response
// into the CQ ring. The decision walk for a Lookup:
//
//   1. BloomView.MaybeContains(tenant, prefix) == false
//        → proved-absent anywhere in the cluster → Response{kMiss}
//          WITHOUT touching the routing cache or the slow resolver.
//          This is the whole point of the agent-local bloom view:
//          kill negative lookups at the edge.
//   2. RoutingCache hit
//        → Response{kOk, node, source=kCache}. Sub-microsecond.
//   3. RoutingCache miss
//        → NodeResolver.ResolvePrimary (the slow path: NodeDirectory +
//          HRW, or — once A1.7 lands — a gRPC round-trip to a peer).
//          On success cache it + Response{kOk, node, source=kResolver};
//          on miss Response{kMiss}.
//
// Fetch / Publish opcodes are recognised but return kUnsupported in
// A1.6 — they need the NIXL data path wired (A1.7). The wire format
// reserves their opcodes now so the engine ABI is stable.
//
// Wire format (little-endian, fixed-size, fits any slot >= 64 bytes):
//
//   Request  (44 bytes):  op u32 | request_id u64 | tenant_id[16] | prefix_hash[16]
//   Response (64 bytes):  request_id u64 | status u32 | source u32 |
//                         matched_tokens u32 | node_id_len u32 | node_id[40]
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bloom_view/bloom_view.h"
#include "routing_cache/routing_cache.h"

namespace kvcache::agent::router {

enum class OpCode : uint32_t {
    kLookup  = 1,
    kFetch   = 2,
    kPublish = 3,
};

enum class Status : uint32_t {
    kOk          = 0,
    kMiss        = 1,
    kError       = 2,
    kUnsupported = 3,  // Fetch/Publish in A1.6
};

// Where a kOk answer came from — lets the engine + ops tell a hot
// cache hit from a slow-path resolve, and the test assert the walk.
enum class Source : uint32_t {
    kNone     = 0,
    kCache    = 1,
    kResolver = 2,
    kBloomNegative = 3,  // proved-absent; paired with kMiss
};

struct Request {
    OpCode                  op = OpCode::kLookup;
    uint64_t                request_id = 0;
    std::array<uint8_t, 16> tenant_id{};
    std::array<uint8_t, 16> prefix_hash{};

    // Serialize into exactly kWireSize bytes. Returns the buffer.
    std::vector<uint8_t> Serialize() const;
    static constexpr std::size_t kWireSize = 4 + 8 + 16 + 16;  // 44
    static std::optional<Request> Parse(std::span<const uint8_t> bytes);
};

struct Response {
    uint64_t    request_id = 0;
    Status      status     = Status::kError;
    Source      source     = Source::kNone;
    uint32_t    matched_tokens = 0;
    std::string node_id;  // <= 40 bytes; empty on miss/error

    std::vector<uint8_t> Serialize() const;
    static constexpr std::size_t kWireSize = 8 + 4 + 4 + 4 + 4 + 40;  // 64
    static constexpr std::size_t kMaxNodeId = 40;
    static std::optional<Response> Parse(std::span<const uint8_t> bytes);
};

// Slow-path primary resolver. A1.6 ships a fake for tests; A1.7 wires
// the real NodeDirectory + HRW (and, cross-node, a gRPC hop). Returns
// the primary node_id for (tenant, prefix), or nullopt on a true miss.
// matched_tokens is filled on hit (chunk-aligned LPM count).
struct ResolveResult {
    std::string node_id;
    uint32_t    matched_tokens = 0;
};
using NodeResolver = std::function<std::optional<ResolveResult>(
    const std::array<uint8_t, 16>& tenant_id,
    const std::array<uint8_t, 16>& prefix_hash)>;

class RequestRouter {
   public:
    RequestRouter(routing_cache::RoutingCache& cache,
                  bloom_view::BloomView& bloom,
                  NodeResolver resolver)
        : cache_(cache), bloom_(bloom), resolver_(std::move(resolver)) {}

    // Handle one parsed Request → Response. Pure (no ring I/O) so it's
    // unit-testable without /dev/shm.
    Response Handle(const Request& req);

    // Convenience: parse SQ bytes → Handle → serialize CQ bytes. Used
    // by the main.cpp event loop. On unparseable input returns a
    // kError response with request_id=0.
    std::vector<uint8_t> HandleRaw(std::span<const uint8_t> sq_bytes);

   private:
    // tenant_id + prefix_hash → RoutingCache::Key (raw bytes as the
    // opaque key fields).
    static routing_cache::Key MakeKey(const std::array<uint8_t, 16>& tenant_id,
                                      const std::array<uint8_t, 16>& prefix_hash);

    routing_cache::RoutingCache& cache_;
    bloom_view::BloomView&       bloom_;
    NodeResolver                 resolver_;
};

}  // namespace kvcache::agent::router
