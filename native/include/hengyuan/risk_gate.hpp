// SPDX-License-Identifier: proprietary
// risk_gate.hpp — P2-EXEC-SIM-01: pre-trade hard risk checks (simulated).
// Mirrors the execution-plane design note: kill-switch state, max position,
// max exposure. int64 fixed-point, __int128 for notional overflow safety.
// Governance: L2, simulation. No network/token/order.

#pragma once

#include <hengyuan/execution_types.hpp>
#include <hengyuan/fixed_point.hpp>
#include <hengyuan/kill_switch.hpp>
#include <cstdint>

namespace hy {

struct RiskLimits {
    std::int64_t max_position_lots{0};       // per-symbol absolute net cap (0 = disabled)
    std::int64_t max_notional_ticks{0};      // per-intent notional cap (0 = disabled)
};

enum class RiskDecision : std::uint8_t {
    Allow = 0,
    BlockKillSwitch = 1,
    BlockMaxPosition = 2,
    BlockMaxNotional = 3,
    BlockInvalidQty = 4,
    // AUDIT RISK-REFPRICE-033: a non-positive reference price used to skip the
    // notional cap entirely (product_exceeds() answers "does not exceed" for a
    // non-positive operand), so an invalid price silently BOUGHT permission
    // instead of being refused. Now its own explicit rejection.
    BlockInvalidPrice = 5,
    // AUDIT RISK-INTOVF-032: the projected-position arithmetic overflowed
    // int64. Reaching this means the caller's position/qty are outside the
    // range this gate can reason about at all -- refuse rather than compare a
    // wrapped value against the cap.
    BlockPositionOverflow = 6,
};

class RiskGate {
public:
    explicit RiskGate(RiskLimits limits) noexcept : limits_(limits) {}

    // Pure check — does not mutate. `current_net` is the symbol's current
    // net position; `ref_price_ticks` is the marketable/limit price used to
    // size notional.
    RiskDecision check(const ExecutionIntent& intent,
                       std::int64_t current_net,
                       std::int64_t ref_price_ticks,
                       const KillSwitch& ks) const noexcept {
        if (intent.qty_lots <= 0) {
            return RiskDecision::BlockInvalidQty;
        }

        // Opening / increasing exposure requires NORMAL. Reducing is allowed
        // while ARMED (you may always flatten), but TRIGGERED/LATCHED blocks all.
        const bool is_reducing = is_reducing_position(intent, current_net);
        if (ks.state() == KillState::Triggered || ks.state() == KillState::Latched) {
            return RiskDecision::BlockKillSwitch;
        }
        if (!is_reducing && !ks.can_open()) {
            return RiskDecision::BlockKillSwitch;
        }

        // AUDIT RISK-REFPRICE-033: reject a non-positive reference price up
        // front instead of letting it fall through to a notional check that
        // treats it as "no violation". A price of 0 with any quantity used to
        // return Allow even against a 1-tick cap.
        if (ref_price_ticks <= 0) {
            return RiskDecision::BlockInvalidPrice;
        }

        // Max position (absolute net after this intent).
        //
        // AUDIT RISK-INTOVF-032: this used to be a raw `current_net + delta`
        // followed by `projected < 0 ? -projected : projected`. Both are
        // signed-overflow UB at the int64 boundary (confirmed under UBSan),
        // and -- worse than a crash -- the wrapped value compared as SMALLER
        // than the cap, so an out-of-range position was answered with Allow.
        // Overflow now fails closed, and the magnitude is taken in the
        // unsigned domain where |INT64_MIN| is representable.
        if (limits_.max_position_lots > 0) {
            // qty_lots > 0 is guaranteed by the BlockInvalidQty gate above, so
            // negating it is always representable -- only the ADDITION below
            // can overflow.
            const std::int64_t delta =
                (intent.side == OrderSide::Buy) ? intent.qty_lots : -intent.qty_lots;
            std::int64_t projected = 0;
            if (!safe_add_i64(current_net, delta, projected)) {
                return RiskDecision::BlockPositionOverflow;
            }
            if (abs_magnitude_u64(projected) >
                static_cast<std::uint64_t>(limits_.max_position_lots)) {
                return RiskDecision::BlockMaxPosition;
            }
        }

        // Max notional per intent. Portable overflow-safe check (no __int128):
        // ref * qty > max  <=>  ref > max / qty  (positive ints, P1-3).
        // ref_price_ticks > 0 is guaranteed by the BlockInvalidPrice gate above.
        if (limits_.max_notional_ticks > 0) {
            if (product_exceeds(ref_price_ticks, intent.qty_lots, limits_.max_notional_ticks)) {
                return RiskDecision::BlockMaxNotional;
            }
        }

        return RiskDecision::Allow;
    }

    const RiskLimits& limits() const noexcept { return limits_; }

private:
    static bool is_reducing_position(const ExecutionIntent& intent,
                                     std::int64_t current_net) noexcept {
        if (current_net > 0 && intent.side == OrderSide::Sell) return true;
        if (current_net < 0 && intent.side == OrderSide::Buy) return true;
        return false;
    }

    RiskLimits limits_;
};

}  // namespace hy
