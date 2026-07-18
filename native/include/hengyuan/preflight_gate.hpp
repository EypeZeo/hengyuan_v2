// SPDX-License-Identifier: proprietary
// preflight_gate.hpp — CODE-PREFLIGHT manual confirmation gate.
//
// Naming note: this is a *code-layer* 7-item checklist (kill switch / risk
// gate / depth / heartbeat / signer / confirm / regression). It is a
// DIFFERENT checklist from ADR-018's "D3-LIVE" section, which is an
// *operational* 7-item checklist (no-withdrawal key + IP allowlist,
// account-truth reconciliation, fail-closed on stale data, hard risk caps,
// manual out-of-band drills, geo/service re-verification, SSH+firewall
// hardening). Both gate "entering L5 live," but neither substitutes for
// the other — passing this one does NOT mean ADR-018's D3-LIVE items are
// satisfied, and vice versa. Historically both were called "D3-LIVE 7
// items," which was a naming collision; this file now calls its own
// checklist "CODE-PREFLIGHT" to keep the two distinct in logs and code.
//
// Before ANY live execution can activate, the operator must pass this
// preflight checklist that verifies system state and requires explicit
// typed confirmation. Fail-closed: without confirmation, the system
// stays in simulation-only mode.
//
// CODE-PREFLIGHT 7 prerequisites (all must be ✅):
//   1. Kill switch state machine
//   2. Pre-trade risk gate
//   3. Depth snapshot bootstrap
//   4. External heartbeat (watchdog)
//   5. HMAC signing layer
//   6. Manual confirm CLI (this component)
//   7. Regression test certification
//
// Governance: L5 gate infrastructure. Does NOT enable live by itself —
// it is one of 7 prerequisites that must ALL pass. It is also NOT a
// substitute for ADR-018's separate D3-LIVE operational checklist — both
// must independently pass before live submission (see ADR-019 D1's "双
// 清单制度": ADR-018 D3-LIVE 7/7 + this file's CODE-PREFLIGHT 7/7, each
// verified independently, neither self-reported).
// SIMULATION ONLY until both checklists are fully satisfied.

#pragma once

#include <hengyuan/depth_manager.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/shm_heartbeat.hpp>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace hy {

struct PreflightStatus {
    bool kill_switch_normal{false};
    bool risk_gate_configured{false};
    bool depth_synced{false};
    bool heartbeat_active{false};
    bool signer_ready{false};
    bool operator_confirmed{false};
    bool regression_passed{false};

    bool all_pass() const noexcept {
        return kill_switch_normal && risk_gate_configured && depth_synced &&
               heartbeat_active && signer_ready && operator_confirmed &&
               regression_passed;
    }

    int pass_count() const noexcept {
        return static_cast<int>(kill_switch_normal) +
               static_cast<int>(risk_gate_configured) +
               static_cast<int>(depth_synced) +
               static_cast<int>(heartbeat_active) +
               static_cast<int>(signer_ready) +
               static_cast<int>(operator_confirmed) +
               static_cast<int>(regression_passed);
    }
};

inline void print_preflight(const PreflightStatus& s) noexcept {
    auto mark = [](bool v) { return v ? "PASS" : "FAIL"; };
    std::printf("=== CODE-PREFLIGHT Checklist (distinct from ADR-018 D3-LIVE) ===\n");
    std::printf("  [%s] 1. Kill switch state machine\n", mark(s.kill_switch_normal));
    std::printf("  [%s] 2. Pre-trade risk gate configured\n", mark(s.risk_gate_configured));
    std::printf("  [%s] 3. Depth snapshot synced\n", mark(s.depth_synced));
    std::printf("  [%s] 4. External heartbeat (watchdog)\n", mark(s.heartbeat_active));
    std::printf("  [%s] 5. HMAC signing layer\n", mark(s.signer_ready));
    std::printf("  [%s] 6. Operator manual confirmation\n", mark(s.operator_confirmed));
    std::printf("  [%s] 7. Regression test certification\n", mark(s.regression_passed));
    std::printf("  Result: %d/7 %s\n", s.pass_count(), s.all_pass() ? "ALL PASS" : "BLOCKED");
}

// Check system readiness (automated checks, no user input).
inline PreflightStatus check_preflight(
    const KillSwitch& ks,
    bool risk_configured,
    const DepthManager& dm,
    const ShmHeartbeatWriter* hb,
    bool signer_initialized) noexcept {

    PreflightStatus s;
    s.kill_switch_normal = (ks.state() == KillState::Normal);
    s.risk_gate_configured = risk_configured;
    s.depth_synced = (dm.state() == DepthState::Tracking);
    s.heartbeat_active = (hb != nullptr && hb->block() != nullptr);
    s.signer_ready = signer_initialized;
    // operator_confirmed and regression_passed must be set externally
    return s;
}

}  // namespace hy
