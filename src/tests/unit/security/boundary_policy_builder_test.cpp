// src/tests/unit/security/boundary_policy_builder_test.cpp
#include "security/boundary_policy_builder.h"
#include "security/boundary_guard.h"
#include <gtest/gtest.h>
using namespace kvcache::node::security;

TEST(BuildPolicy, ClassifiesCidrVsHostAndRoundTripsThroughGuard) {
    auto p = BuildPolicy({"*.svc.cluster.local", "10.0.0.0/8", "s3.gov.local"});
    ASSERT_EQ(p.allow.size(), 3u);
    EXPECT_TRUE(p.default_deny);
    BoundaryGuard g(p);
    EXPECT_TRUE(g.Check({.host = "n.svc.cluster.local"}).allow);
    EXPECT_TRUE(g.Check({.host = "10.9.9.9"}).allow);
    EXPECT_TRUE(g.Check({.host = "s3.gov.local"}).allow);
    EXPECT_FALSE(g.Check({.host = "8.8.8.8"}).allow);
}

TEST(BuildPolicy, EmptyIsAirGap) {
    auto p = BuildPolicy({});
    EXPECT_TRUE(p.allow.empty());
    EXPECT_TRUE(p.default_deny);
    BoundaryGuard g(p);
    EXPECT_FALSE(g.Check({.host = "anything"}).allow);
}
