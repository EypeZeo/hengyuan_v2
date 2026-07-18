// P2-EXEC-LIVE-01 D12-6: exit-safety / kill-switch post-trigger contract tests.
#include <gtest/gtest.h>
#include <hengyuan/exit_safety.hpp>

using hy::ActionPermission;
using hy::EmergencyCancelPolicy;
using hy::ExitSafetyStatus;
using hy::KillState;
using hy::PostKillAction;
using hy::ResidualAction;
using hy::check_post_kill_permission;
using hy::determine_emergency_policy;
using hy::determine_residual_action;

// --- Post-kill permissions in Normal state ---

TEST(ExitSafety, NormalStateAllowsAll) {
    EXPECT_EQ(check_post_kill_permission(KillState::Normal, PostKillAction::ReadOnlyQuery),
              ActionPermission::Allowed);
    EXPECT_EQ(check_post_kill_permission(KillState::Normal, PostKillAction::ExitOnlyCancel),
              ActionPermission::Allowed);
    EXPECT_EQ(check_post_kill_permission(KillState::Normal, PostKillAction::NewOrder),
              ActionPermission::Allowed);
    EXPECT_EQ(check_post_kill_permission(KillState::Normal, PostKillAction::Rearm),
              ActionPermission::Allowed);
}

// --- Post-kill permissions after trigger ---

TEST(ExitSafety, TriggeredAllowsReadOnly) {
    EXPECT_EQ(check_post_kill_permission(KillState::Triggered, PostKillAction::ReadOnlyQuery),
              ActionPermission::Allowed);
}

TEST(ExitSafety, TriggeredRequiresApprovalForCancel) {
    EXPECT_EQ(check_post_kill_permission(KillState::Triggered, PostKillAction::ExitOnlyCancel),
              ActionPermission::RequiresOperatorApproval);
}

TEST(ExitSafety, TriggeredForbidsNewOrder) {
    EXPECT_EQ(check_post_kill_permission(KillState::Triggered, PostKillAction::NewOrder),
              ActionPermission::Forbidden);
}

TEST(ExitSafety, TriggeredForbidsRearm) {
    EXPECT_EQ(check_post_kill_permission(KillState::Triggered, PostKillAction::Rearm),
              ActionPermission::Forbidden);
}

TEST(ExitSafety, TriggeredForbidsAutoResume) {
    EXPECT_EQ(check_post_kill_permission(KillState::Triggered, PostKillAction::AutoResume),
              ActionPermission::Forbidden);
}

// --- Exit safety status ---

TEST(ExitSafetyStatus, CanShutdownWhenClean) {
    ExitSafetyStatus s{};
    s.kill_switch_triggered = true;
    s.has_open_orders = false;
    s.has_residual_position = false;
    EXPECT_TRUE(s.can_safe_shutdown());
}

TEST(ExitSafetyStatus, CannotShutdownWithOpenOrders) {
    ExitSafetyStatus s{};
    s.has_open_orders = true;
    EXPECT_FALSE(s.can_safe_shutdown());
}

TEST(ExitSafetyStatus, CannotShutdownWithResidualPosition) {
    ExitSafetyStatus s{};
    s.has_residual_position = true;
    EXPECT_FALSE(s.can_safe_shutdown());
}

TEST(ExitSafetyStatus, NeedsOperatorWhenKilledWithOpenOrders) {
    ExitSafetyStatus s{};
    s.kill_switch_triggered = true;
    s.has_open_orders = true;
    EXPECT_TRUE(s.needs_operator());
}

TEST(ExitSafetyStatus, NoOperatorNeededWhenClean) {
    ExitSafetyStatus s{};
    s.kill_switch_triggered = true;
    s.has_open_orders = false;
    s.has_residual_position = false;
    EXPECT_FALSE(s.needs_operator());
}

// --- Emergency cancel policy ---

TEST(EmergencyCancel, NoOpenOrdersIsManual) {
    EXPECT_EQ(determine_emergency_policy(true, false),
              EmergencyCancelPolicy::OperatorManualOnly);
}

TEST(EmergencyCancel, FirstPathAlwaysManual) {
    // First path: never auto-cancel, always operator via Binance App
    EXPECT_EQ(determine_emergency_policy(true, true),
              EmergencyCancelPolicy::OperatorManualOnly);
    EXPECT_EQ(determine_emergency_policy(false, true),
              EmergencyCancelPolicy::OperatorManualOnly);
}

// --- Residual position handling ---

TEST(ResidualAction, NoPositionNoAction) {
    EXPECT_EQ(determine_residual_action(false, true), ResidualAction::NoPosition);
    EXPECT_EQ(determine_residual_action(false, false), ResidualAction::NoPosition);
}

TEST(ResidualAction, PositionWithOperatorIsManualExit) {
    EXPECT_EQ(determine_residual_action(true, true), ResidualAction::OperatorManualExit);
}

TEST(ResidualAction, PositionWithoutOperatorWaits) {
    EXPECT_EQ(determine_residual_action(true, false), ResidualAction::WaitForOperator);
}
