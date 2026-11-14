// SPDX-License-Identifier: proprietary
// kill_switch.hpp — P2-EXEC-SIM-01: kill-switch state machine.
// Implements the design note's NORMAL -> ARMED -> TRIGGERED -> LATCHED ladder.
// LATCHED is terminal: NO auto-rearm / auto-resume (ADR-015 hard rule).
// Only an explicit operator reset() (process restart equivalent) clears it.
// Governance: L2, simulation control. No network/token/order.

#pragma once

#include <cstdint>
#include <limits>

namespace hy {

enum class KillState : std::uint8_t {
    Normal = 0,     // trading allowed
    Armed = 1,      // warning: no new opens, existing managed
    Triggered = 2,  // flatten in progress
    Latched = 3,    // terminal lockout — operator-only reset
};

class KillSwitch {
public:
    KillState state() const noexcept { return state_; }
    bool can_open() const noexcept { return state_ == KillState::Normal; }
    std::uint64_t revision() const noexcept { return revision_; }

    // Escalate to at least ARMED (e.g. drawdown / stale feed / API errors).
    // Never de-escalates automatically.
    void arm() noexcept {
        advance_revision();
        if (state_ == KillState::Normal) {
            state_ = KillState::Armed;
            ++arm_count_;
        }
    }

    // Escalate to TRIGGERED (flatten everything). Monotonic.
    void trigger() noexcept {
        advance_revision();
        if (state_ != KillState::Latched) {
            state_ = KillState::Triggered;
            ++trigger_count_;
        }
    }

    // After flatten completes, latch terminally. No path back except reset().
    void latch() noexcept {
        advance_revision();
        state_ = KillState::Latched;
    }

    // Operator-only reset (models a deliberate process restart). The ADR-015
    // invariant is that NO automatic code path calls this; only an explicit
    // human/operator action does.
    void operator_reset() noexcept {
        advance_revision();
        if (revision_ == std::numeric_limits<std::uint64_t>::max()) return;
        state_ = KillState::Normal;
    }

    std::uint64_t arm_count() const noexcept { return arm_count_; }
    std::uint64_t trigger_count() const noexcept { return trigger_count_; }

private:
    void advance_revision() noexcept {
        if (revision_ != std::numeric_limits<std::uint64_t>::max()) ++revision_;
        if (revision_ == std::numeric_limits<std::uint64_t>::max()) state_ = KillState::Latched;
    }
    std::uint64_t revision_{0};
    KillState state_{KillState::Normal};
    std::uint64_t arm_count_{0};
    std::uint64_t trigger_count_{0};
};

}  // namespace hy
