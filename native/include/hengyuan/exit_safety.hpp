// SPDX-License-Identifier: proprietary
// exit_safety.hpp — Binance exit-safety / kill-switch post-trigger contract.
//
// Governance: L1 (pure policy logic, no network, no secret).
// ADR-019 D9 + Architect M8:
//   ✅ Emergency cancel policy (what happens after kill-switch fires)
//   ✅ Post-kill allowed actions (read-only queries, exit-only orders)
//   ✅ Post-kill forbidden actions (new orders, rearm, auto-resume)
//   ✅ Residual position handling (operator runbook, not auto-close)
//   ✅ Human takeover boundary
//   ✅ Independent of Kraken / simulation skeleton (M8)

#pragma once

#include <hengyuan/kill_switch.hpp>
#include <cstdint>

namespace hy {

// --- Post-kill action classification ---

enum class PostKillAction : std::uint8_t {
    ReadOnlyQuery = 0,     // GET /account, GET /order — always allowed
    ExitOnlyCancel = 1,    // DELETE /order — allowed only for known open orders
    NewOrder = 2,          // POST /order — always forbidden after kill
    Rearm = 3,             // Re-enable trading — forbidden (requires human ceremony)
    AutoResume = 4,        // Automatic restart — always forbidden (ADR-015)
};

enum class ActionPermission : std::uint8_t {
    Allowed = 0,
    Forbidden = 1,
    RequiresOperatorApproval = 2,
};

// Determine if an action is permitted given current kill-switch state.
inline ActionPermission check_post_kill_permission(
    KillState ks_state,
    PostKillAction action) noexcept {

    // Normal state: all actions permitted (subject to other gates)
    if (ks_state == KillState::Normal) {
        return ActionPermission::Allowed;
    }

    // Kill triggered or any non-Normal state
    switch (action) {
        case PostKillAction::ReadOnlyQuery:
            return ActionPermission::Allowed;
        case PostKillAction::ExitOnlyCancel:
            return ActionPermission::RequiresOperatorApproval;
        case PostKillAction::NewOrder:
            return ActionPermission::Forbidden;
        case PostKillAction::Rearm:
            return ActionPermission::Forbidden;
        case PostKillAction::AutoResume:
            return ActionPermission::Forbidden;
    }

    return ActionPermission::Forbidden;
}

// --- Exit safety status ---

struct ExitSafetyStatus {
    bool kill_switch_triggered{false};
    bool has_open_orders{false};
    bool has_residual_position{false};
    bool operator_present{false};

    // Can we safely shut down the process?
    bool can_safe_shutdown() const noexcept {
        return !has_open_orders && !has_residual_position;
    }

    // Do we need operator intervention before shutdown?
    bool needs_operator() const noexcept {
        return kill_switch_triggered && (has_open_orders || has_residual_position);
    }
};

// --- Emergency cancel policy ---

enum class EmergencyCancelPolicy : std::uint8_t {
    // Attempt to cancel all known open orders, then report residual
    CancelAllKnownOpen = 0,
    // Do not attempt cancel — operator handles via Binance App (ADR-018 D3)
    OperatorManualOnly = 1,
};

struct EmergencyCancelResult {
    std::uint32_t orders_cancel_attempted{0};
    std::uint32_t orders_cancel_confirmed{0};
    std::uint32_t orders_cancel_failed{0};
    std::uint32_t orders_cancel_ambiguous{0};
    bool all_resolved{false};
};

// Determine what to do on kill-switch trigger.
// This function does NOT execute cancels — it only determines the policy.
// Actual cancel execution is in the runtime layer (L5).
inline EmergencyCancelPolicy determine_emergency_policy(
    bool operator_present,
    bool has_open_orders) noexcept {

    (void)operator_present;
    (void)has_open_orders;
    return EmergencyCancelPolicy::OperatorManualOnly;
}

// --- Residual position handling ---

enum class ResidualAction : std::uint8_t {
    // No position — nothing to do
    NoPosition = 0,
    // Position exists — operator must handle via Binance App
    OperatorManualExit = 1,
    // Position exists but operator not reachable — log and wait
    WaitForOperator = 2,
};

inline ResidualAction determine_residual_action(
    bool has_position,
    bool operator_present) noexcept {

    if (!has_position) return ResidualAction::NoPosition;
    if (operator_present) return ResidualAction::OperatorManualExit;
    return ResidualAction::WaitForOperator;
}

}  // namespace hy
