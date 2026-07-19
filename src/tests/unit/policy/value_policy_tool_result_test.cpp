// A-plane generalization, Task 2 — ValuePolicyToolResult: idempotent-only
// store gate (LLD §2.1c) + economic gate + evict/miss.
#include "value_policy_tool_result.h"

#include "state_identity.h"
#include <gtest/gtest.h>

using namespace kvcache::common;

TEST(ValuePolicyToolResult, NonIdempotentIsNeverStored) {
    ValuePolicyToolResult p;
    StateIdentity id = StateIdentityForToolResult(1, "send_email", "to=x", /*idempotent=*/false);
    // LLD §2.1c: a non-idempotent tool result must never be materialized,
    // regardless of economics.
    EXPECT_FALSE(p.shouldStore(id, CostModel{.fetch_cost_ms = 1, .recompute_cost_ms = 9999}));
}

TEST(ValuePolicyToolResult, IdempotentUsesEconomicGate) {
    ValuePolicyToolResult p;
    StateIdentity id = StateIdentityForToolResult(1, "search", "q=cats", /*idempotent=*/true);
    EXPECT_TRUE(p.shouldStore(id, CostModel{}));                                   // unknown cost → store
    EXPECT_TRUE(p.shouldStore(id, CostModel{.fetch_cost_ms = 1, .recompute_cost_ms = 20})); // cheap fetch → store
    EXPECT_FALSE(p.shouldStore(id, CostModel{.fetch_cost_ms = 20, .recompute_cost_ms = 1})); // recompute cheaper → decline
}

TEST(ValuePolicyToolResult, EvictableAndRecomputeOnMiss) {
    ValuePolicyToolResult p;
    StateIdentity id = StateIdentityForToolResult(1, "search", "q=cats", true);
    EXPECT_EQ(p.shouldEvict(id, /*tier*/2), EvictDecision::kEvictable);   // A-class
    EXPECT_EQ(p.onMiss(id), OnMissAction::kRecompute);                    // re-call the idempotent tool
}
