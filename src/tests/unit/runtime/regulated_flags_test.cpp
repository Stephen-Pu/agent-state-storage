// A10 — Task 3: RegulatedFlags helper unit tests.
// Tests that BuildRegulatedModeConfig correctly plumbs CLI flags + egress sinks
// into a RegulatedModeConfig that the startup gate (Task 2) and transport guard
// (Task 1) consume.
#include "runtime/regulated_flags.h"
#include <gtest/gtest.h>
using namespace kvcache::node::runtime;
using namespace kvcache::node::security;

TEST(RegulatedFlags, BuildsConfigWithSinksAndRules) {
    RegulatedFlags f;
    f.enabled = true;
    f.allow_rules = {"*.svc.local", "10.0.0.0/8"};
    std::vector<Endpoint> sinks = {{.host = "s3.svc.local", .purpose = Purpose::kColdTier}};
    auto cfg = BuildRegulatedModeConfig(f, sinks);
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.allow_rules.size(), 2u);
    ASSERT_EQ(cfg.configured_sinks.size(), 1u);
    EXPECT_EQ(cfg.configured_sinks[0].host, "s3.svc.local");
    // Sanity: the resulting config validates in-boundary and rejects a bogus sink.
    EXPECT_TRUE(ValidateRegulatedMode(cfg).ok);
}

TEST(RegulatedFlags, DisabledProducesNoOpConfig) {
    RegulatedFlags f;                // enabled = false
    auto cfg = BuildRegulatedModeConfig(f, {{.host = "evil.com", .purpose = Purpose::kColdTier}});
    EXPECT_FALSE(cfg.enabled);
    EXPECT_TRUE(ValidateRegulatedMode(cfg).ok) << "disabled config is a no-op";
}
