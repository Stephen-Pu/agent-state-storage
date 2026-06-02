// Phase A1.6 — RequestRouter tests.
//
// Drives the router directly with a fake resolver + a real
// RoutingCache + BloomView (no /dev/shm, no node). Covers:
//   1. Request/Response wire round-trip (serialize → parse).
//   2. Lookup bloom-negative short-circuit (never touches cache/resolver).
//   3. Lookup cache hit (source=kCache).
//   4. Lookup resolver hit → caches it → second call is a cache hit.
//   5. Lookup true miss (bloom permissive, resolver misses).
//   6. Fetch/Publish → kUnsupported.
//   7. HandleRaw on garbage → kError.
#include "router/router.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>

using namespace kvcache::agent;
using router::OpCode;
using router::Request;
using router::Response;
using router::ResolveResult;
using router::Source;
using router::Status;

namespace {

std::array<uint8_t, 16> Tenant(uint8_t b) {
    std::array<uint8_t, 16> a{};
    a[0] = b;
    return a;
}
std::array<uint8_t, 16> Prefix(uint8_t b) {
    std::array<uint8_t, 16> a{};
    a[15] = b;
    return a;
}

// A BloomView that always answers "maybe" (permissive) — the default
// for tests that want the cache/resolver path. For the negative test
// we use a loader that returns an empty sketch for the tenant, which
// makes MaybeContains return... true (no sketch = permissive). So to
// force a NEGATIVE we need a real sketch that excludes the key. Build
// one via LocalBloom over a disjoint key set.
bloom_view::BloomView MakePermissiveBloom() {
    return bloom_view::BloomView({
        .loader = [](const std::vector<std::string>&) {
            return std::vector<bloom_view::TenantSketch>{};
        },
    });
}

}  // namespace

TEST(RouterWireTest, RequestRoundTrip) {
    Request r;
    r.op = OpCode::kLookup;
    r.request_id = 0xDEADBEEFCAFEull;
    r.tenant_id = Tenant(7);
    r.prefix_hash = Prefix(42);
    auto bytes = r.Serialize();
    ASSERT_EQ(bytes.size(), Request::kWireSize);
    auto back = Request::Parse(bytes);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->op, OpCode::kLookup);
    EXPECT_EQ(back->request_id, 0xDEADBEEFCAFEull);
    EXPECT_EQ(back->tenant_id, Tenant(7));
    EXPECT_EQ(back->prefix_hash, Prefix(42));
}

TEST(RouterWireTest, ResponseRoundTripWithNodeId) {
    Response r;
    r.request_id = 99;
    r.status = Status::kOk;
    r.source = Source::kResolver;
    r.matched_tokens = 32;
    r.node_id = "node-abc-123";
    auto bytes = r.Serialize();
    ASSERT_EQ(bytes.size(), Response::kWireSize);
    auto back = Response::Parse(bytes);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->request_id, 99u);
    EXPECT_EQ(back->status, Status::kOk);
    EXPECT_EQ(back->source, Source::kResolver);
    EXPECT_EQ(back->matched_tokens, 32u);
    EXPECT_EQ(back->node_id, "node-abc-123");
}

TEST(RouterTest, BloomNegativeShortCircuits) {
    routing_cache::RoutingCache cache;
    auto bloom = bloom_view::BloomView({
        .loader = [](const std::vector<std::string>& tenants) {
            // Real sketch for t1 containing ONLY prefix(1). A query for
            // prefix(2) should be proved-absent.
            std::vector<bloom_view::TenantSketch> out;
            for (const auto& t : tenants) {
                if (t.size() >= 1 && static_cast<uint8_t>(t[0]) == 1) {
                    auto p = kvcache::node::routing::BloomParams::ForCapacity(1024, 0.001);
                    kvcache::node::routing::LocalBloom b(p);
                    auto pre = Prefix(1);
                    b.Add({pre.data(), pre.size()});
                    out.push_back({t, p, b.Snapshot()});
                }
            }
            return out;
        },
    });
    std::string tenant(reinterpret_cast<const char*>(Tenant(1).data()), 16);
    bloom.RegisterTenant(tenant);
    ASSERT_EQ(bloom.RefreshOnce(), 1);

    std::atomic<int> resolver_calls{0};
    router::RequestRouter rt(cache, bloom,
        [&](const std::array<uint8_t, 16>&, const std::array<uint8_t, 16>&)
            -> std::optional<ResolveResult> {
            ++resolver_calls;
            return std::nullopt;
        });

    Request req;
    req.op = OpCode::kLookup;
    req.tenant_id = Tenant(1);
    req.prefix_hash = Prefix(2);  // NOT in the sketch
    auto resp = rt.Handle(req);
    EXPECT_EQ(resp.status, Status::kMiss);
    EXPECT_EQ(resp.source, Source::kBloomNegative);
    EXPECT_EQ(resolver_calls.load(), 0) << "bloom-negative must not call resolver";
    EXPECT_EQ(cache.SnapshotStats().misses, 0u) << "bloom-negative must not touch cache";
}

TEST(RouterTest, CacheHit) {
    routing_cache::RoutingCache cache;
    auto bloom = MakePermissiveBloom();
    // Pre-seed the cache.
    routing_cache::Key k;
    k.tenant_id.assign(reinterpret_cast<const char*>(Tenant(3).data()), 16);
    k.prefix.assign(reinterpret_cast<const char*>(Prefix(9).data()), 16);
    cache.Put(k, "node-cached");

    std::atomic<int> resolver_calls{0};
    router::RequestRouter rt(cache, bloom,
        [&](const std::array<uint8_t, 16>&, const std::array<uint8_t, 16>&)
            -> std::optional<ResolveResult> { ++resolver_calls; return std::nullopt; });

    Request req;
    req.tenant_id = Tenant(3);
    req.prefix_hash = Prefix(9);
    auto resp = rt.Handle(req);
    EXPECT_EQ(resp.status, Status::kOk);
    EXPECT_EQ(resp.source, Source::kCache);
    EXPECT_EQ(resp.node_id, "node-cached");
    EXPECT_EQ(resolver_calls.load(), 0) << "cache hit must not call resolver";
}

TEST(RouterTest, ResolverHitThenCached) {
    routing_cache::RoutingCache cache;
    auto bloom = MakePermissiveBloom();
    std::atomic<int> resolver_calls{0};
    router::RequestRouter rt(cache, bloom,
        [&](const std::array<uint8_t, 16>&, const std::array<uint8_t, 16>&)
            -> std::optional<ResolveResult> {
            ++resolver_calls;
            return ResolveResult{"node-resolved", 48};
        });

    Request req;
    req.tenant_id = Tenant(5);
    req.prefix_hash = Prefix(11);

    auto resp1 = rt.Handle(req);
    EXPECT_EQ(resp1.status, Status::kOk);
    EXPECT_EQ(resp1.source, Source::kResolver);
    EXPECT_EQ(resp1.node_id, "node-resolved");
    EXPECT_EQ(resp1.matched_tokens, 48u);
    EXPECT_EQ(resolver_calls.load(), 1);

    // Second call: must be served from the cache the first call populated.
    auto resp2 = rt.Handle(req);
    EXPECT_EQ(resp2.status, Status::kOk);
    EXPECT_EQ(resp2.source, Source::kCache);
    EXPECT_EQ(resp2.node_id, "node-resolved");
    EXPECT_EQ(resolver_calls.load(), 1) << "second lookup must hit cache, not resolver";
}

TEST(RouterTest, TrueMiss) {
    routing_cache::RoutingCache cache;
    auto bloom = MakePermissiveBloom();
    router::RequestRouter rt(cache, bloom,
        [](const std::array<uint8_t, 16>&, const std::array<uint8_t, 16>&)
            -> std::optional<ResolveResult> { return std::nullopt; });
    Request req;
    req.tenant_id = Tenant(8);
    req.prefix_hash = Prefix(1);
    auto resp = rt.Handle(req);
    EXPECT_EQ(resp.status, Status::kMiss);
    EXPECT_EQ(resp.source, Source::kNone);
    EXPECT_TRUE(resp.node_id.empty());
}

TEST(RouterTest, FetchResolvesLikeLookup) {
    // Phase A1.7 — Fetch is a read: it resolves the target node so the
    // engine can server-pull (NIXL) from it. Same walk as Lookup.
    routing_cache::RoutingCache cache;
    auto bloom = MakePermissiveBloom();
    router::RequestRouter rt(cache, bloom,
        [](const std::array<uint8_t, 16>&, const std::array<uint8_t, 16>&)
            -> std::optional<ResolveResult> {
            return ResolveResult{"node-fetch-target", 0};
        });
    Request req;
    req.op = OpCode::kFetch;
    req.request_id = 7;
    req.tenant_id = Tenant(2);
    req.prefix_hash = Prefix(3);
    auto resp = rt.Handle(req);
    EXPECT_EQ(resp.status, Status::kOk);
    EXPECT_EQ(resp.node_id, "node-fetch-target");
    EXPECT_EQ(resp.request_id, 7u);
}

TEST(RouterTest, PublishResolvesWithoutBloomGate) {
    // Phase A1.7 — Publish is a write: the chunk may not exist yet, so
    // the bloom gate is skipped. Even with a bloom that would prove the
    // prefix absent, Publish must still resolve a target node to seal to.
    routing_cache::RoutingCache cache;
    auto bloom = bloom_view::BloomView({
        .loader = [](const std::vector<std::string>& tenants) {
            std::vector<bloom_view::TenantSketch> out;
            for (const auto& t : tenants) {
                auto p = kvcache::node::routing::BloomParams::ForCapacity(1024, 0.001);
                kvcache::node::routing::LocalBloom b(p);  // empty — proves-absent
                out.push_back({t, p, b.Snapshot()});
            }
            return out;
        },
    });
    std::string tenant(reinterpret_cast<const char*>(Tenant(4).data()), 16);
    bloom.RegisterTenant(tenant);
    ASSERT_EQ(bloom.RefreshOnce(), 1);

    router::RequestRouter rt(cache, bloom,
        [](const std::array<uint8_t, 16>&, const std::array<uint8_t, 16>&)
            -> std::optional<ResolveResult> {
            return ResolveResult{"node-seal-target", 0};
        });
    Request req;
    req.op = OpCode::kPublish;
    req.tenant_id = Tenant(4);
    req.prefix_hash = Prefix(99);  // would be bloom-negative for a read
    auto resp = rt.Handle(req);
    EXPECT_EQ(resp.status, Status::kOk) << "Publish must skip the bloom gate";
    EXPECT_EQ(resp.node_id, "node-seal-target");
}

TEST(RouterTest, HandleRawGarbageReturnsError) {
    routing_cache::RoutingCache cache;
    auto bloom = MakePermissiveBloom();
    router::RequestRouter rt(cache, bloom,
        [](const std::array<uint8_t, 16>&, const std::array<uint8_t, 16>&)
            -> std::optional<ResolveResult> { return std::nullopt; });
    std::vector<uint8_t> garbage(10, 0xFF);  // shorter than kWireSize
    auto resp_bytes = rt.HandleRaw({garbage.data(), garbage.size()});
    auto resp = Response::Parse(resp_bytes);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, Status::kError);
}
