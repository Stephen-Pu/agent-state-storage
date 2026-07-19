// SS-2 spine spike, Task 3 — ValuePolicyKv: first-implement the D-PERF-1
// economic shouldStore decision (behavior-preserving on the hot path).
#include "value_policy_kv.h"

#include <gtest/gtest.h>

using namespace kvcache::common;

TEST(ValuePolicyKv, ShouldStoreIsBehaviorPreservingOnUnknownCost) {
    ValuePolicyKv p;
    StateIdentity id{}; id.state_kind = SK_KV;
    // Hot-path default (no telemetry): both costs 0 / unknown → store (preserves today's behavior).
    EXPECT_TRUE(p.shouldStore(id, CostModel{}));
}

TEST(ValuePolicyKv, ShouldStoreDeclinesWhenRecomputeIsCheaper) {
    ValuePolicyKv p;
    StateIdentity id{}; id.state_kind = SK_KV;
    // D-PERF-1: if fetching the stored copy costs as much as (or more than) recomputing,
    // storing is not worth it → decline. (fetch >= recompute → false)
    EXPECT_FALSE(p.shouldStore(id, CostModel{.fetch_cost_ms = 10.0, .recompute_cost_ms = 5.0}));
    // fetch clearly cheaper than recompute → store.
    EXPECT_TRUE(p.shouldStore(id, CostModel{.fetch_cost_ms = 1.0, .recompute_cost_ms = 20.0}));
}

TEST(ValuePolicyKv, EvictableAndRecomputeOnMiss) {
    ValuePolicyKv p;
    StateIdentity id{}; id.state_kind = SK_KV;
    EXPECT_EQ(p.shouldEvict(id, /*tier*/2), EvictDecision::kEvictable);  // A-class: cost-evictable
    EXPECT_EQ(p.onMiss(id), OnMissAction::kRecompute);
}
