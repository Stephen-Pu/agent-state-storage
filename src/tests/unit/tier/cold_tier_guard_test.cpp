// A10 — BoundaryGuard wired into the cold-tier factory.
//
// Verifies that a native-rest cold tier built with a deny-all guard refuses
// egress without dialing — the deny observer fires, proving GuardedHttpTransport
// wrapped the inner transport before any network call was made.
#include "tier/cold_tier.h"
#include "security/boundary_guard.h"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace kvcache::node::tier;
using namespace kvcache::node::security;

// With a deny-all guard, a native-rest cold tier must refuse egress WITHOUT
// dialing — Get returns not-found/error and no network occurs (the guard
// short-circuits before libcurl). This proves the factory wrapped the transport.
TEST(ColdTierGuard, NativeRestWithDenyAllGuardBlocksEgress) {
    ColdTierOptions opts;
    opts.type = "native-rest";
    opts.rest.base_url = "https://out-of-boundary.example.com/bucket";
    // Empty allowlist + default_deny = air-gap: denies everything.
    opts.guard = std::make_shared<BoundaryGuard>(BoundaryPolicy{{}, /*default_deny=*/true});
    bool denied = false;
    opts.deny_observer = [&](const Endpoint& ep, std::string_view) {
        denied = true;
        EXPECT_EQ(ep.purpose, Purpose::kColdTier);
    };

    std::string err;
    auto tier = CreateColdTier(opts, &err);
    ASSERT_NE(tier, nullptr) << err;   // factory still constructs the tier

    // A read must be denied by the guard (no network). RestColdTier maps a
    // transport error to a miss/error; the key assertion is the observer fired
    // and the call returned quickly without a real DNS/connect.
    std::vector<uint8_t> out;
    DramKey k{};                       // any key; content irrelevant
    (void)tier->Get(k, &out, &err);    // IColdTier::Get(const DramKey&, std::vector<uint8_t>*, std::string*)
    EXPECT_TRUE(denied) << "guard must have denied the out-of-boundary egress";
}

// Verify that a guarded native-rest cold tier built WITH TLS/timeout options
// constructs successfully (non-null tier, no error). The CurlHttpTransport TLS
// fields (timeout_ms_, ca_, cert_, key_) are TU-private and not externally
// inspectable, so we assert successful construction here. The knob-application
// itself is guaranteed by RestColdTier::Create delegating to
// MakeCurlHttpTransport(opts), which is the same overload exercised by the
// existing RestColdTier TLS unit tests.
TEST(ColdTierGuard, NativeRestGuardedWithTlsOptionsConstructsSuccessfully) {
    ColdTierOptions opts;
    opts.type = "native-rest";
    opts.rest.base_url               = "https://s3.local/bucket";
    opts.rest.ca_pem_path            = "/tmp/ca.pem";
    opts.rest.client_cert_pem_path   = "/tmp/client.crt";
    opts.rest.client_key_pem_path    = "/tmp/client.key";
    opts.rest.timeout_ms             = 1234;
    // Allow-all guard: single rule with host_glob="*", default_deny=false.
    opts.guard = std::make_shared<BoundaryGuard>(
        BoundaryPolicy{{{.host_glob = "*"}}, /*default_deny=*/false});

    std::string err;
    auto tier = CreateColdTier(opts, &err);
    ASSERT_NE(tier, nullptr) << "guarded native-rest tier with TLS opts must construct: " << err;
    EXPECT_EQ(tier->Name(), "native-rest");
}
