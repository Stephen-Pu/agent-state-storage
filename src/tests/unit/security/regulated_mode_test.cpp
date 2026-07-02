// src/tests/unit/security/regulated_mode_test.cpp
#include "security/regulated_mode.h"
#include <gtest/gtest.h>
using namespace kvcache::node::security;

TEST(RegulatedMode, DisabledIsNoOp) {
    RegulatedModeConfig cfg;  // enabled = false
    cfg.configured_sinks = {{.host = "anywhere.evil.com", .purpose = Purpose::kColdTier}};
    auto r = ValidateRegulatedMode(cfg);
    EXPECT_TRUE(r.ok) << "disabled Regulated Mode never blocks";
}

TEST(RegulatedMode, AllInBoundaryBoots) {
    RegulatedModeConfig cfg;
    cfg.enabled = true;
    cfg.allow_rules = {"*.svc.local", "10.0.0.0/8"};
    cfg.configured_sinks = {
        {.host = "s3.svc.local", .purpose = Purpose::kColdTier},
        {.host = "10.0.1.2",     .purpose = Purpose::kEtcd},
    };
    auto r = ValidateRegulatedMode(cfg);
    EXPECT_TRUE(r.ok) << r.error;
}

TEST(RegulatedMode, OutOfBoundarySinkRefusesAndNamesIt) {
    RegulatedModeConfig cfg;
    cfg.enabled = true;
    cfg.allow_rules = {"*.svc.local"};
    cfg.configured_sinks = {
        {.host = "s3.svc.local",  .purpose = Purpose::kColdTier},
        {.host = "otel.public.io", .purpose = Purpose::kTelemetry},  // out of boundary
    };
    auto r = ValidateRegulatedMode(cfg);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("otel.public.io"), std::string::npos)
        << "error must name the offending sink";
}

TEST(RegulatedMode, UnmetRequirementRefuses) {
    RegulatedModeConfig cfg;
    cfg.enabled = true;
    cfg.allow_rules = {"*"};
    cfg.configured_sinks = {};
    cfg.requirements = {
        []() -> std::optional<std::string> { return std::nullopt; },       // satisfied
        []() -> std::optional<std::string> { return "FIPS provider not active"; },  // missing
    };
    auto r = ValidateRegulatedMode(cfg);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("FIPS"), std::string::npos);
}

TEST(RegulatedMode, EmptyAllowlistIsAirGapRefusesAnySink) {
    RegulatedModeConfig cfg;
    cfg.enabled = true;                 // no allow_rules => air-gap
    cfg.configured_sinks = {{.host = "s3.svc.local", .purpose = Purpose::kColdTier}};
    auto r = ValidateRegulatedMode(cfg);
    EXPECT_FALSE(r.ok) << "air-gap must deny every egress sink";
}
