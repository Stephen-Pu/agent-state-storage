// ValuePolicyToolResult implementation (A-plane generalization, Task 2).
#include "value_policy_tool_result.h"

namespace kvcache::common {

// LLD §2.1c gate FIRST: a non-idempotent tool result must never be stored,
// regardless of economics — this check dominates the economic gate below.
// Once idempotency is confirmed, apply the same economic decision as
// ValuePolicyKv (D-PERF-1): unknown recompute cost -> store (safe default);
// otherwise store only if fetching the stored copy is cheaper than
// recomputing it (re-calling the tool).
bool ValuePolicyToolResult::shouldStore(const StateIdentity& id, const CostModel& cost) {
    if (!(id.flags & SIF_IDEMPOTENT)) return false;           // §2.1c: never materialize non-idempotent results
    if (cost.recompute_cost_ms <= 0.0) return true;           // unknown recompute cost → store (safe default)
    return cost.fetch_cost_ms < cost.recompute_cost_ms;       // store only if fetch beats recompute
}

EvictDecision ValuePolicyToolResult::shouldEvict(const StateIdentity& /*id*/, int /*tier*/) {
    return EvictDecision::kEvictable;                          // A-class: cost-driven eviction may proceed
}

OnMissAction ValuePolicyToolResult::onMiss(const StateIdentity& /*id*/) {
    return OnMissAction::kRecompute;                           // re-call the idempotent tool
}

}  // namespace kvcache::common
