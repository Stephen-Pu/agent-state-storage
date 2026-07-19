// ValuePolicyToolResult — tool-result-class value policy (A-plane
// generalization, Task 2, LLD §2.1c).
//
// Defining rule: a non-idempotent tool result must NEVER be materialized.
// Caching and replaying a side-effecting call's stale result (e.g. an email
// send, a payment, a write) would silently re-execute or misrepresent that
// side effect. shouldStore therefore checks SIF_IDEMPOTENT FIRST, before any
// economic reasoning — the idempotency gate dominates cost. Only once an
// identity is known-idempotent does the same economic gate as ValuePolicyKv
// (D-PERF-1) apply.
#pragma once

#include "value_policy.h"

namespace kvcache::common {

class ValuePolicyToolResult final : public ValuePolicy {
 public:
    bool shouldStore(const StateIdentity& id, const CostModel& cost) override;
    EvictDecision shouldEvict(const StateIdentity& id, int tier) override;
    OnMissAction onMiss(const StateIdentity& id) override;
};

}  // namespace kvcache::common
