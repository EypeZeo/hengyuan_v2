// P2-EXEC-SIM-01: KillSwitch state machine.
#include <gtest/gtest.h>
#include <hengyuan/kill_switch.hpp>

using hy::KillState;
using hy::KillSwitch;

TEST(KillSwitch, StartsNormal) {
    KillSwitch ks;
    EXPECT_EQ(ks.state(), KillState::Normal);
    EXPECT_TRUE(ks.can_open());
}

TEST(KillSwitch, ArmBlocksOpen) {
    KillSwitch ks;
    ks.arm();
    EXPECT_EQ(ks.state(), KillState::Armed);
    EXPECT_FALSE(ks.can_open());
    EXPECT_EQ(ks.arm_count(), 1u);
}

TEST(KillSwitch, ArmIsIdempotentFromNormal) {
    KillSwitch ks;
    ks.arm();
    ks.arm();
    EXPECT_EQ(ks.arm_count(), 1u);  // only counts the Normal->Armed transition
}

TEST(KillSwitch, TriggerThenLatch) {
    KillSwitch ks;
    ks.trigger();
    EXPECT_EQ(ks.state(), KillState::Triggered);
    EXPECT_EQ(ks.trigger_count(), 1u);
    ks.latch();
    EXPECT_EQ(ks.state(), KillState::Latched);
    EXPECT_FALSE(ks.can_open());
}

TEST(KillSwitch, LatchedIsTerminal_NoAutoRearm) {
    KillSwitch ks;
    ks.latch();
    EXPECT_EQ(ks.state(), KillState::Latched);
    // arm/trigger must NOT move it out of Latched
    ks.arm();
    EXPECT_EQ(ks.state(), KillState::Latched);
    ks.trigger();
    EXPECT_EQ(ks.state(), KillState::Latched);
}

TEST(KillSwitch, OnlyOperatorResetClears) {
    KillSwitch ks;
    ks.latch();
    EXPECT_EQ(ks.state(), KillState::Latched);
    ks.operator_reset();
    EXPECT_EQ(ks.state(), KillState::Normal);
    EXPECT_TRUE(ks.can_open());
}

TEST(KillSwitch, ArmDoesNotDeescalateTriggered) {
    KillSwitch ks;
    ks.trigger();
    ks.arm();  // arm only acts from Normal
    EXPECT_EQ(ks.state(), KillState::Triggered);
}
