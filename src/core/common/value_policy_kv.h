// ValuePolicyKv — KV-class value policy (SS-2 spine spike, Task 3).
//
// First-implements the D-PERF-1 economic shouldStore decision behind the
// ValuePolicy interface. shouldStore is behavior-preserving on the hot path:
// with the default/zeroed CostModel the hot path supplies today (unknown
// cost), it returns true (store), matching today's unconditional-store
// behavior. When a real CostModel is supplied (fetch/recompute costs known),
// the economic decision applies: store only if fetching the stored copy is
// cheaper than recomputing it.
#pragma once

#include "value_policy.h"

namespace kvcache::common {

class ValuePolicyKv final : public ValuePolicy {
 public:
    bool shouldStore(const StateIdentity& id, const CostModel& cost) override;
    EvictDecision shouldEvict(const StateIdentity& id, int tier) override;
    OnMissAction onMiss(const StateIdentity& id) override;
};

}  // namespace kvcache::common
