// ValuePolicyKv implementation (SS-2 spine spike, Task 3).
#include "value_policy_kv.h"

namespace kvcache::common {

// D-PERF-1 economic decision. Behavior-preserving when the recompute cost is
// unknown (<= 0, e.g. the zeroed CostModel{} the hot path supplies today):
// storing is the default (returns true), so existing unconditional-store
// behavior is unchanged. When a real CostModel is supplied, decline to store
// iff fetching the stored copy is not cheaper than recomputing it.
bool ValuePolicyKv::shouldStore(const StateIdentity& /*id*/, const CostModel& cost) {
    if (cost.recompute_cost_ms <= 0.0) return true;          // unknown recompute cost → store (safe default)
    return cost.fetch_cost_ms < cost.recompute_cost_ms;      // store only if fetch beats recompute
}

EvictDecision ValuePolicyKv::shouldEvict(const StateIdentity& /*id*/, int /*tier*/) {
    return EvictDecision::kEvictable;                        // A-class: cost-driven eviction may proceed
}

OnMissAction ValuePolicyKv::onMiss(const StateIdentity& /*id*/) {
    return OnMissAction::kRecompute;                         // engine recomputes on miss
}

}  // namespace kvcache::common
