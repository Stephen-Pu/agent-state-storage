#include "cluster/membership_fsm.h"

#include <gtest/gtest.h>

using namespace kvcache::node::cluster;

TEST(MembershipFsmTest, HappyPath) {
    MembershipFsm fsm;
    EXPECT_EQ(fsm.State(), NodeState::Joining);
    EXPECT_EQ(fsm.Activate(),   TransitionResult::kOk);
    EXPECT_EQ(fsm.State(), NodeState::Active);
    EXPECT_EQ(fsm.Drain(),      TransitionResult::kOk);
    EXPECT_EQ(fsm.Terminate(),  TransitionResult::kOk);
    EXPECT_EQ(fsm.State(), NodeState::Terminated);
}

TEST(MembershipFsmTest, IllegalSkips) {
    MembershipFsm fsm;
    // Joining → Draining is illegal.
    EXPECT_EQ(fsm.Drain(),     TransitionResult::kIllegalTransition);
    // Joining → Terminated is illegal.
    EXPECT_EQ(fsm.Terminate(), TransitionResult::kIllegalTransition);
}

TEST(MembershipFsmTest, LeaseLossPathsCorrectly) {
    MembershipFsm fsm;
    fsm.Activate();
    EXPECT_EQ(fsm.LeaseLost(),      TransitionResult::kOk);
    EXPECT_EQ(fsm.State(), NodeState::Unreachable);
    EXPECT_EQ(fsm.LeaseRecovered(), TransitionResult::kOk);
    EXPECT_EQ(fsm.State(), NodeState::Joining);
}

TEST(MembershipFsmTest, TerminatedIsTerminal) {
    MembershipFsm fsm;
    fsm.Activate(); fsm.Drain(); fsm.Terminate();
    EXPECT_EQ(fsm.Activate(),       TransitionResult::kIllegalTransition);
    EXPECT_EQ(fsm.LeaseRecovered(), TransitionResult::kIllegalTransition);
}

TEST(MembershipFsmTest, TrafficWeightAcrossStates) {
    MembershipFsm fsm;
    EXPECT_DOUBLE_EQ(fsm.TrafficWeight(), 0.1);
    fsm.Activate();
    EXPECT_DOUBLE_EQ(fsm.TrafficWeight(), 1.0);
    fsm.Drain();
    EXPECT_DOUBLE_EQ(fsm.TrafficWeight(), 0.0);
}

TEST(MembershipFsmTest, CallbackFires) {
    MembershipFsm fsm;
    int n = 0;
    NodeState last_from = NodeState::Terminated, last_to = NodeState::Terminated;
    fsm.SetStateChangeCallback([&](NodeState from, NodeState to) {
        ++n; last_from = from; last_to = to;
    });
    fsm.Activate();
    EXPECT_EQ(n, 1);
    EXPECT_EQ(last_from, NodeState::Joining);
    EXPECT_EQ(last_to,   NodeState::Active);
}
