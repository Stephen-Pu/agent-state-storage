// ValuePolicy — per-state-kind economic policy interface (SS-2 spine spike,
// Task 2, LLD §3.3.8).
//
// The tiering engine / evictor / fetch-decider all consult a ValuePolicy for
// a given StateIdentity to decide whether to store it, whether it may be
// evicted from a tier, and what to do on a cache miss. Policies are looked
// up by state_kind via ValuePolicyRegistry so each state kind (KV, sandbox,
// embedding, memory, ...) can plug in its own economics without touching the
// tiering engine itself.
#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>

#include "state_identity.h"

namespace kvcache::common {

// Whether a tier may evict this state right now. kNotEvictable is
// demote-only: the tiering engine may push the state to a colder tier but
// must not discard it outright.
enum class EvictDecision { kEvictable, kNotEvictable };

// What the fetch-decider should do when a lookup for this state misses.
enum class OnMissAction { kRecompute, kReplayFromPersist, kNone };

// Inputs to the store-vs-recompute economic decision.
struct CostModel {
    double fetch_cost_ms = 0.0;      // est. cost to fetch/store+retrieve this state
    double recompute_cost_ms = 0.0;  // est. cost to regenerate it
    uint32_t reuse_hint = 0;         // observed/expected reuse count (0 = unknown)
};

// Pure-virtual policy interface. One implementation per state_kind.
class ValuePolicy {
 public:
    virtual ~ValuePolicy() = default;

    virtual bool shouldStore(const StateIdentity& id, const CostModel& cost) = 0;
    virtual EvictDecision shouldEvict(const StateIdentity& id, int tier) = 0;
    virtual OnMissAction onMiss(const StateIdentity& id) = 0;
};

// state_kind-keyed registry; the tiering engine / evictor / fetch-decider
// all call of(kind) to get the policy for a given state kind.
class ValuePolicyRegistry {
 public:
    void registerPolicy(uint16_t state_kind, std::unique_ptr<ValuePolicy> p) {
        assert(state_kind < SK_MAX);
        policies_[state_kind] = std::move(p);
    }

    // Precondition: a policy was registered for state_kind.
    ValuePolicy& of(uint16_t state_kind) {
        assert(state_kind < SK_MAX && policies_[state_kind] &&
               "no ValuePolicy registered for this state_kind");
        return *policies_[state_kind];
    }

    bool has(uint16_t state_kind) const {
        return state_kind < SK_MAX && policies_[state_kind] != nullptr;
    }

 private:
    std::array<std::unique_ptr<ValuePolicy>, SK_MAX> policies_{};
};

}  // namespace kvcache::common
