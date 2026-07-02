// src/tests/unit/tier/guarded_transport_test.cpp
#include "tier/guarded_transport.h"
#include "tier/rest_cold_tier.h"
#include "security/boundary_guard.h"
#include <gtest/gtest.h>
#include <memory>
using namespace kvcache::node::tier;
using namespace kvcache::node::security;

namespace {
// Records whether the inner transport was ever dialed.
struct SpyTransport : IHttpTransport {
    int calls = 0;
    HttpResult Request(const std::string&, const std::string&,
                       const std::vector<std::string>&, const uint8_t*, std::size_t) override {
        ++calls; return HttpResult{200, "ok", ""};
    }
};
}  // namespace

TEST(GuardedHttpTransport, AllowsInBoundaryHostAndDelegates) {
    auto spy = std::make_shared<SpyTransport>();
    auto guard = std::make_shared<BoundaryGuard>(BoundaryPolicy{{{.host_glob = "*.svc.local"}}, true});
    GuardedHttpTransport g(spy, guard);
    auto r = g.Request("GET", "https://store.svc.local/bucket/obj.kv", {}, nullptr, 0);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(spy->calls, 1) << "in-boundary request must reach inner transport";
}

TEST(GuardedHttpTransport, BlocksOutOfBoundaryHostAndNeverDials) {
    auto spy = std::make_shared<SpyTransport>();
    auto guard = std::make_shared<BoundaryGuard>(BoundaryPolicy{{{.host_glob = "*.svc.local"}}, true});
    bool denied = false; std::string reason;
    GuardedHttpTransport g(spy, guard, [&](const Endpoint& ep, std::string_view why) {
        denied = true; reason = std::string(why); EXPECT_EQ(ep.purpose, Purpose::kColdTier);
    });
    auto r = g.Request("GET", "https://exfil.evil.com/x", {}, nullptr, 0);
    EXPECT_EQ(spy->calls, 0) << "out-of-boundary request must NOT dial";
    EXPECT_NE(r.transport_err, "") << "deny surfaces as transport_err (fail-closed)";
    EXPECT_TRUE(denied);
    EXPECT_FALSE(reason.empty());
}

TEST(GuardedHttpTransport, MalformedUrlIsDenied) {
    auto spy = std::make_shared<SpyTransport>();
    auto guard = std::make_shared<BoundaryGuard>(BoundaryPolicy{{{.host_glob = "*"}}, true});
    GuardedHttpTransport g(spy, guard);
    auto r = g.Request("GET", "not-a-url", {}, nullptr, 0);
    EXPECT_EQ(spy->calls, 0);
    EXPECT_NE(r.transport_err, "");
}

TEST(HostFromUrl, ParsesHostAndPort) {
    uint16_t port = 0;
    EXPECT_EQ(HostFromUrl("https://a.b.com:8443/x/y", &port), "a.b.com");
    EXPECT_EQ(port, 8443);
    port = 0;
    EXPECT_EQ(HostFromUrl("http://host.local/x", &port), "host.local");
    EXPECT_EQ(port, 80);
    EXPECT_EQ(HostFromUrl("garbage", &port), "");
}

TEST(HostFromUrl, UserinfoPrefixIsStripped) {
    uint16_t port = 0;
    // Authority "store.svc.local@evil.svc.local" — real host libcurl dials is evil.svc.local.
    EXPECT_EQ(HostFromUrl("https://store.svc.local@evil.svc.local/", &port), "evil.svc.local");
    // Basic user:pw@ form.
    EXPECT_EQ(HostFromUrl("https://user:pw@host.local/", &port), "host.local");
    // Empty host after userinfo must fail-closed.
    EXPECT_EQ(HostFromUrl("https://user@/path", &port), "") << "empty host after userinfo -> fail-closed";
}

TEST(GuardedHttpTransport, UserinfoConfusionIsBlocked) {
    auto spy = std::make_shared<SpyTransport>();
    // Policy allows *.svc.local only.
    auto guard = std::make_shared<BoundaryGuard>(BoundaryPolicy{{{.host_glob = "*.svc.local"}}, true});
    // URL: authority = "store.svc.local@evil.external.com"
    // Old (buggy) code: host = "store.svc.local@evil.external.com", which ends in
    //   ".external.com" — DENY anyway. But the perimeter bypass is the reverse case:
    //   "allow.svc.local@evil.external.com" where old code returns the full string,
    //   which does NOT match *.svc.local, but we want to confirm the fixed code
    //   correctly extracts the real dialed host (evil.external.com) and blocks it.
    //
    // To demonstrate the bypass that was possible before the fix: if the authority
    // itself ended in .svc.local (e.g. "evil.external.com@proxy.svc.local"), the
    // OLD code would return "evil.external.com@proxy.svc.local" which matches
    // *.svc.local (suffix match) — an ALLOW — while libcurl would connect to
    // proxy.svc.local. The NEW code strips userinfo and yields "proxy.svc.local",
    // which is in the allowlist and correctly allowed. The bypass scenario requires
    // an out-of-allowlist host in the userinfo position and an in-allowlist host
    // after '@'; the real guard check must be on the post-strip host only.
    //
    // Concrete block test: userinfo="store.svc.local", real host="evil.external.com"
    // → after strip: "evil.external.com" → not in *.svc.local → DENY.
    auto r = GuardedHttpTransport(spy, guard)
                 .Request("GET", "https://store.svc.local@evil.external.com/x", {}, nullptr, 0);
    EXPECT_EQ(spy->calls, 0) << "userinfo host-confusion must not dial";
    EXPECT_NE(r.transport_err, "");
}
