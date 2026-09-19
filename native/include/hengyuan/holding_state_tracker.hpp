// SPDX-License-Identifier: proprietary
// holding_state_tracker.hpp — 批次 6 6a-2: translates StreamingEvaluator::step()'s absolute
// target-position output into a discrete "suggested action" that fires only on a state
// transition.
//
// External-review-driven safety requirement (批次 6 计划's first review round, verified
// independently before adoption): StreamingEvaluator::step() (strategy_spec_evaluator.hpp,
// Batch 5) returns an ABSOLUTE target position, not a discrete buy/sell event -- boolean mode
// yields exactly 0.0 or 1.0 every bar (apply_signal_mode()'s own contract). A naive "signal
// crossed a threshold -> propose an order" translation would fire on EVERY bar while the signal
// stays at 1.0 during a sustained trend (unbounded repeated re-buying), and would never emit a
// closing sell when the signal falls back to 0.0 (nothing "crosses" on the way down if the check
// only watches the way up). This tracker fixes that at the source by only emitting a suggested
// action on an actual Flat<->Long transition; Long->Long and Flat->Flat are silent by design --
// the caller still sees the raw per-bar signal (that is StreamingEvaluator's own return value,
// unaffected by this class), just no repeated/missing "action" on top of it.
//
// Boolean-mode signal handling only, this batch's own scope note (批次 6 6a-2 计划正文): scaled
// mode's sign handling (positive/negative target position, not just zero/nonzero) is deferred
// to 6b-1's implementation.
//
// No I/O, no network, no order-construction code of any kind -- a pure, standalone value-type
// state machine.
//
// *** NOT AN ORDER SOURCE (外部复核 P0-02, verified 2026-09-19) ***
// This tracker is for LOGGING signal transitions (6a-2's demo, 6b-1's preflight probe) and nothing
// else. It advances state_ in the same call that emits the action, before any order exists; it
// defaults to Flat and never reads the real account. So a rejected confirmation, a failed POST, an
// UNKNOWN outcome, a partial fill or a restart leaves it disagreeing with the actual position:
// after an emitted-but-failed Open every later Long signal is silent (no retry), after an
// emitted-but-unfilled Close a real holding is never closed, and after a restart with a holding it
// still says Flat. Anything that derives an ORDER must use target_position_planner.hpp, which is
// level-triggered against the actual position and keeps no per-signal state to reconcile.

#pragma once

#include <cstdint>

namespace hy {

enum class HoldingState : std::uint8_t {
    Flat = 0,
    Long = 1,
};

enum class SuggestedAction : std::uint8_t {
    None = 0,   // Long->Long or Flat->Flat -- no transition, deliberately silent
    Open = 1,   // Flat->Long -- suggest opening (BUY)
    Close = 2,  // Long->Flat -- suggest closing (SELL)
};

// Boolean-mode target position -> HoldingState boundary. apply_signal_mode()'s boolean-mode
// contract guarantees the value is exactly 0.0 or 1.0, never anything in between -- this
// comparison is exact-equality-safe for that reason, not a floating-point hazard here.
inline constexpr double kFlatTargetPosition = 0.0;

class HoldingStateTracker {
public:
    // Feeds one bar's target position (StreamingEvaluator::step()'s return value, boolean-mode
    // spec). Returns the suggested action for THIS bar only -- None on every bar except the one
    // that actually crosses Flat<->Long.
    SuggestedAction on_target_position(double target_position) noexcept {
        const HoldingState next =
            target_position != kFlatTargetPosition ? HoldingState::Long : HoldingState::Flat;
        SuggestedAction action = SuggestedAction::None;
        if (state_ == HoldingState::Flat && next == HoldingState::Long) {
            action = SuggestedAction::Open;
        } else if (state_ == HoldingState::Long && next == HoldingState::Flat) {
            action = SuggestedAction::Close;
        }
        state_ = next;
        return action;
    }

    HoldingState state() const noexcept { return state_; }

private:
    HoldingState state_{HoldingState::Flat};
};

}  // namespace hy
