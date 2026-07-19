// SS-2 spine spike, Task 2 — ValuePolicy interface + state_kind registry.
#include "value_policy.h"

#include <gtest/gtest.h>

#include <memory>

using namespace kvcache::common;

namespace {
struct FakePolicy : ValuePolicy {
    bool store;
    EvictDecision ev;
    OnMissAction miss;
    FakePolicy(bool s, EvictDecision e, OnMissAction m) : store(s), ev(e), miss(m) {}
    bool shouldStore(const StateIdentity&, const CostModel&) override { return store; }
    EvictDecision shouldEvict(const StateIdentity&, int) override { return ev; }
    OnMissAction onMiss(const StateIdentity&) override { return miss; }
};
}  // namespace

TEST(ValuePolicyRegistry, RegisterAndRetrieveByStateKind) {
    ValuePolicyRegistry reg;
    EXPECT_FALSE(reg.has(SK_KV));
    reg.registerPolicy(SK_KV, std::make_unique<FakePolicy>(true, EvictDecision::kEvictable,
                                                            OnMissAction::kRecompute));
    ASSERT_TRUE(reg.has(SK_KV));

    StateIdentity id{};
    id.state_kind = SK_KV;

    EXPECT_TRUE(reg.of(SK_KV).shouldStore(id, CostModel{}));
    EXPECT_EQ(reg.of(SK_KV).shouldEvict(id, /*tier=*/2), EvictDecision::kEvictable);
    EXPECT_EQ(reg.of(SK_KV).onMiss(id), OnMissAction::kRecompute);
}
