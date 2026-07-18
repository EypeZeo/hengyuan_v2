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

        // Max position (absolute net after this intent).
        if (limits_.max_position_lots > 0) {
            const std::int64_t delta =
                (intent.side == OrderSide::Buy) ? intent.qty_lots : -intent.qty_lots;
            const std::int64_t projected = current_net + delta;
            const std::int64_t abs_proj = projected < 0 ? -projected : projected;
            if (abs_proj > limits_.max_position_lots) {
                return RiskDecision::BlockMaxPosition;
            }
        }

        // Max notional per intent. Portable overflow-safe check (no __int128):
        // ref * qty > max  <=>  ref > max / qty  (positive ints, P1-3).
        if (limits_.max_notional_ticks > 0 && ref_price_ticks > 0) {
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
