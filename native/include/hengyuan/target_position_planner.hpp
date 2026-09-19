// SPDX-License-Identifier: proprietary
// target_position_planner.hpp — 批次 6 6b-0c: derives an order intent from the strategy's TARGET
// position and the account's ACTUAL position, replacing HoldingStateTracker as the basis for any
// order (外部复核 P0-02, verified against holding_state_tracker.hpp:63).
//
// Why the tracker cannot be the order source: it is edge-triggered on the SIGNAL and advances its
// own state in the same call that emits the action, before any order exists. A rejected
// confirmation, a failed POST, an UNKNOWN outcome, a partial fill or a process restart then leaves
// it disagreeing with the real account -- after Flat->Long is emitted and the order fails, every
// later Long signal is silent (no retry); after Long->Flat is emitted and the SELL never fills, a
// real holding is never closed; after a restart it starts Flat regardless of what the account holds.
//
// This planner is LEVEL-triggered instead: each call recomputes "what would bring the actual
// position to the desired one" from scratch, so any failure heals on the next bar without any
// planner state to reconcile. The only state it keeps is a rejection cooldown.
//
// Pure logic: no I/O, no clocks, no exchange types. The caller supplies the account's base-asset
// balance already converted to the symbol's qty scale (see balance_to_qty_ticks()) and the
// booleans that come from other components (feed validity, evaluator warm-up, in-flight orders,
// position consistency). Every default is FAIL-CLOSED: a default-constructed PlannerInputs can
// never produce an intent.
//
// Not this file's job: price/notional filters (validate_pre_trade() applies MIN_NOTIONAL, price
// tick and the caps -- a proposal it rejects simply cools down and is re-planned), confirmation,
// and submission. Spot is long-only, so a non-positive signal means "desired position zero".

#pragma once

#include <hengyuan/account_truth.hpp>  // pow10_i64(), kBalanceScale

#include <cmath>
#include <cstdint>
#include <limits>

namespace hy {

enum class PlannedSide : std::uint8_t {
    None = 0,
    Buy = 1,
    Sell = 2,
};

enum class PlanReason : std::uint8_t {
    Intent = 0,            // an order intent was produced (side != None)
    AtTarget = 1,          // actual position already equals the desired one
    Dust = 2,              // difference exists but is smaller than one tradable lot
    FeedInvalid = 3,
    WarmupIncomplete = 4,
    OrderInFlight = 5,     // any in-flight order OR any order in UNKNOWN/ambiguous state
    CooldownActive = 6,    // a recent proposal was not executed -- do not re-prompt yet
    InvalidConfig = 7,
    PositionDiverged = 8,  // account-derived and fill-derived positions disagree
};

inline constexpr const char* plan_reason_name(PlanReason r) noexcept {
    switch (r) {
        case PlanReason::Intent: return "Intent";
        case PlanReason::AtTarget: return "AtTarget";
        case PlanReason::Dust: return "Dust";
        case PlanReason::FeedInvalid: return "FeedInvalid";
        case PlanReason::WarmupIncomplete: return "WarmupIncomplete";
        case PlanReason::OrderInFlight: return "OrderInFlight";
        case PlanReason::CooldownActive: return "CooldownActive";
        case PlanReason::InvalidConfig: return "InvalidConfig";
        case PlanReason::PositionDiverged: return "PositionDiverged";
    }
    return "?";
}

struct PlannerConfig {
    std::int64_t target_qty_ticks{0};    // position to hold when the signal is fully on ("小额", configured)
    std::int64_t min_qty_ticks{0};       // SymbolRules::min_qty_ticks
    std::int64_t step_size_ticks{0};     // SymbolRules::step_size_ticks
    std::int64_t max_qty_ticks{0};       // SymbolRules::max_qty_ticks; 0 = no per-order cap
    std::int64_t base_reserve_ticks{0};  // base-asset holdings the OPERATOR declares are not this strategy's
    std::uint32_t cooldown_bars{1};      // bars to wait after a proposal was not executed
};

struct PlannerInputs {
    double target_signal{0.0};             // StreamingEvaluator::step()'s return value
    std::int64_t base_total_ticks{0};      // account free+locked base asset, at the symbol's qty scale
    std::int64_t base_free_ticks{0};       // account free base asset, same scale
    std::uint64_t bar_index{0};            // closed bars processed so far (monotonic)
    // Fail-closed defaults: nothing is trusted until the caller says so.
    bool feed_valid{false};                // feed_validity_gate.hpp
    bool warmup_complete{false};           // StreamingEvaluator::warmup_complete()
    bool order_in_flight_or_unknown{true};  // InFlightRegistry non-empty, or any order Ambiguous
    bool position_consistent{false};       // position_consistent() below
};

struct PlannedIntent {
    PlannedSide side{PlannedSide::None};
    std::int64_t qty_ticks{0};
    PlanReason reason{PlanReason::FeedInvalid};

    bool has_intent() const noexcept { return side != PlannedSide::None; }
};

// Converts an account balance (always at kBalanceScale, account_truth.hpp §4.2) to the symbol's
// qty scale by FLOOR division -- the conservative direction for a quantity that will be sold or
// compared against a holding. Returns false for a negative balance or an out-of-domain scale.
inline bool balance_to_qty_ticks(std::int64_t balance_ticks, std::uint8_t qty_scale,
                                  std::int64_t& out) noexcept {
    if (balance_ticks < 0 || qty_scale > kBalanceScale) return false;
    std::int64_t divisor = 0;
    if (!pow10_i64(static_cast<std::uint8_t>(kBalanceScale - qty_scale), divisor)) return false;
    out = balance_ticks / divisor;
    return true;
}

// True when the account-derived and fill-derived changes in the strategy's position agree within
// `tolerance_ticks` (which must absorb fees taken in the base asset). Overflow fails closed.
inline bool position_consistent(std::int64_t account_change_ticks, std::int64_t fill_derived_change_ticks,
                                std::int64_t tolerance_ticks) noexcept {
    if (tolerance_ticks < 0) return false;
    const std::int64_t a = account_change_ticks;
    const std::int64_t b = fill_derived_change_ticks;
    if ((b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) ||
        (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b)) {
        return false;
    }
    const std::int64_t d = a - b;
    if (d == std::numeric_limits<std::int64_t>::min()) return false;
    return (d < 0 ? -d : d) <= tolerance_ticks;
}

class TargetPositionPlanner {
public:
    explicit TargetPositionPlanner(const PlannerConfig& cfg) noexcept : cfg_(cfg) {}

    // The caller reports that a proposal derived at `bar_index` was NOT executed (operator
    // rejected it, it expired, or the order attempt failed). Re-planning is suppressed until
    // `cooldown_bars` further bars have closed, so a rejected prompt is not re-issued on every tick.
    void note_not_executed(std::uint64_t bar_index) noexcept {
        cooldown_until_bar_ = bar_index + cfg_.cooldown_bars;
    }

    PlannedIntent plan(const PlannerInputs& in) const noexcept {
        if (!config_valid()) return blocked(PlanReason::InvalidConfig);
        if (!in.feed_valid) return blocked(PlanReason::FeedInvalid);
        if (!in.warmup_complete) return blocked(PlanReason::WarmupIncomplete);
        if (!in.position_consistent) return blocked(PlanReason::PositionDiverged);
        if (in.order_in_flight_or_unknown) return blocked(PlanReason::OrderInFlight);
        if (in.bar_index < cooldown_until_bar_) return blocked(PlanReason::CooldownActive);

        if (in.base_total_ticks < 0 || in.base_free_ticks < 0) return blocked(PlanReason::InvalidConfig);

        // Long-only: a negative or undefined signal means "hold nothing".
        double s = std::isnan(in.target_signal) ? 0.0 : in.target_signal;
        if (s < 0.0) s = 0.0;
        if (s > 1.0) s = 1.0;
        std::int64_t desired = 0;
        if (s >= 1.0) {
            desired = cfg_.target_qty_ticks;
        } else if (s > 0.0) {
            desired = static_cast<std::int64_t>(std::floor(static_cast<double>(cfg_.target_qty_ticks) * s));
        }

        // The strategy's own holding: the account total less what the operator declared is not
        // ours. Never negative (spot cannot be short).
        std::int64_t actual = in.base_total_ticks - cfg_.base_reserve_ticks;
        if (actual < 0) actual = 0;

        if (desired == actual) return PlannedIntent{PlannedSide::None, 0, PlanReason::AtTarget};

        PlannedSide side = PlannedSide::Buy;
        std::int64_t raw = desired - actual;
        if (raw < 0) {
            side = PlannedSide::Sell;
            raw = actual - desired;
            // Never sell more than is actually free (part may be locked in open orders).
            if (raw > in.base_free_ticks) raw = in.base_free_ticks;
        }
        if (cfg_.max_qty_ticks > 0 && raw > cfg_.max_qty_ticks) raw = cfg_.max_qty_ticks;

        const std::int64_t qty = align_to_lot_lattice(raw);
        if (qty == 0) return PlannedIntent{PlannedSide::None, 0, PlanReason::Dust};
        return PlannedIntent{side, qty, PlanReason::Intent};
    }

private:
    bool config_valid() const noexcept {
        return cfg_.target_qty_ticks >= 0 && cfg_.min_qty_ticks > 0 && cfg_.step_size_ticks > 0 &&
               cfg_.base_reserve_ticks >= 0 &&
               (cfg_.max_qty_ticks == 0 || cfg_.max_qty_ticks >= cfg_.min_qty_ticks);
    }

    static PlannedIntent blocked(PlanReason r) noexcept { return PlannedIntent{PlannedSide::None, 0, r}; }

    // validate_pre_trade() (account_truth.hpp) requires qty >= min_qty and
    // (qty - min_qty) % step == 0; round DOWN onto that lattice, 0 if below one lot.
    std::int64_t align_to_lot_lattice(std::int64_t raw) const noexcept {
        if (raw < cfg_.min_qty_ticks) return 0;
        return cfg_.min_qty_ticks + ((raw - cfg_.min_qty_ticks) / cfg_.step_size_ticks) * cfg_.step_size_ticks;
    }

    PlannerConfig cfg_;
    std::uint64_t cooldown_until_bar_{0};
};

}  // namespace hy
