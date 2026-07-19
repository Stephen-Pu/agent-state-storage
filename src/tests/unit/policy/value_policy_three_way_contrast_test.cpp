// A-plane generalization, Task 3 — the generalization proof: THREE
// state_kinds (KV=A, tool-result=A idempotent-gated, memory=B) coexist in
// ONE ValuePolicyRegistry with one registerPolicy line each and distinct
// semantics. Adding the 3rd kind (this test) touched no interface, no hot
// path — see headless_node.h ctor for the actual registration.
#include "value_policy.h"
#include "value_policy_kv.h"
#include "value_policy_tool_result.h"
#include "value_policy_persistent_stub.h"
#include "state_identity.h"
#include <gtest/gtest.h>
using namespace kvcache::common;

// Generalization proof: THREE state_kinds — KV (A), tool-result (A, idempotent-gated),
// memory (B) — coexist in ONE registry, one registerPolicy line each, ZERO spine change,
// each with its own semantics. Adding the 3rd kind touched no interface, no hot path.
TEST(ThreeWayContrast, OneRegistryThreeKindsDistinctSemantics) {
    ValuePolicyRegistry reg;
    reg.registerPolicy(SK_KV,          std::make_unique<ValuePolicyKv>());
    reg.registerPolicy(SK_TOOL_RESULT, std::make_unique<ValuePolicyToolResult>());
    reg.registerPolicy(SK_MEMORY,      std::make_unique<ValuePolicyPersistentStub>());

    StateIdentity kv{}; kv.state_kind = SK_KV;
    StateIdentity tool_ok  = StateIdentityForToolResult(1, "search", "q=cats", /*idempotent=*/true);
    StateIdentity tool_bad = StateIdentityForToolResult(1, "send_email", "to=x", /*idempotent=*/false);
    StateIdentity mem{}; mem.state_kind = SK_MEMORY; mem.flags = SIF_PERSISTENT_B;

    CostModel cheap_recompute{.fetch_cost_ms = 10, .recompute_cost_ms = 1};
    // The distinguishing behavior of the NEW kind: non-idempotent tool result is refused
    // where KV (kind-agnostic on idempotency) would apply pure economics, and B stores unconditionally.
    EXPECT_FALSE(reg.of(SK_TOOL_RESULT).shouldStore(tool_bad, cheap_recompute)) << "non-idempotent → never store";
    EXPECT_FALSE(reg.of(SK_TOOL_RESULT).shouldStore(tool_ok,  cheap_recompute)) << "idempotent but recompute cheaper → decline";
    EXPECT_TRUE (reg.of(SK_TOOL_RESULT).shouldStore(tool_ok,  CostModel{}))     << "idempotent, unknown cost → store";
    EXPECT_FALSE(reg.of(SK_KV).shouldStore(kv, cheap_recompute))               << "KV: pure economics";
    EXPECT_TRUE (reg.of(SK_MEMORY).shouldStore(mem, cheap_recompute))          << "B: unconditional";

    // Evict/miss: both A-kinds evictable+recompute; B not-evictable+replay.
    EXPECT_EQ(reg.of(SK_TOOL_RESULT).shouldEvict(tool_ok, 2), EvictDecision::kEvictable);
    EXPECT_EQ(reg.of(SK_TOOL_RESULT).onMiss(tool_ok),         OnMissAction::kRecompute);
    EXPECT_EQ(reg.of(SK_MEMORY).shouldEvict(mem, 2),          EvictDecision::kNotEvictable);
}
