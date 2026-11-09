// SPDX-License-Identifier: proprietary
// sim_executor.hpp — P2-EXEC-SIM-01: dry-run simulated executor.
//
// SIMULATION ONLY. Takes an ExecutionIntent + current top-of-book, runs the
// risk gate + kill switch, simulates a fill, and tracks paper position / PnL.
// NO Binance Private API, NO HMAC, NO token, NO real order, NO network, NO money.
// This is NOT live execution and NOT authorization for any. Governance: L2.

#pragma once

#include <hengyuan/execution_types.hpp>
#include <hengyuan/fixed_point.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/risk_gate.hpp>

#include <array>
#include <cstdint>
#include <limits>

namespace hy {

struct SimConfig {
    RiskLimits limits{};
    std::int64_t fee_ppm{750};           // simulated taker fee, parts per million (750 = 0.075%)
    std::int64_t max_drawdown{0};        // realized+unrealized loss that arms kill switch (0 = off)
};

struct SimStats {
    std::uint64_t intents{0};
    std::uint64_t filled{0};
    std::uint64_t rejected{0};
    std::uint64_t no_liquidity{0};
    std::uint64_t overflow_guard{0};
};

template <std::size_t MaxSymbols = 64>
class SimExecutor {
public:
    explicit SimExecutor(SimConfig config) noexcept
        : config_(config), gate_(config.limits) {}

    KillSwitch& kill_switch() noexcept { return kill_; }
    const KillSwitch& kill_switch() const noexcept { return kill_; }
    const SimStats& stats() const noexcept { return stats_; }

    const Position& position(std::uint32_t symbol_id) const noexcept {
        return positions_[symbol_id < MaxSymbols ? symbol_id : 0];
    }

    std::int64_t total_realized_pnl() const noexcept {
        std::int64_t sum = 0;
        for (const auto& p : positions_) {
            sum += p.realized_pnl;
        }
        return sum;
    }

    // Simulate executing `intent` against the given top-of-book.
    // best_bid_ticks / best_ask_ticks == 0 means that side is empty.
    SimFill execute(const ExecutionIntent& intent,
                    std::int64_t best_bid_ticks,
                    std::int64_t best_ask_ticks) noexcept {
        ++stats_.intents;
        SimFill fill{};

        if (intent.symbol_id >= MaxSymbols || intent.qty_lots <= 0) {
            fill.status = FillStatus::NoFill;
            return fill;
        }

        // Reference / fill price from the book.
        const std::int64_t ref = (intent.side == OrderSide::Buy) ? best_ask_ticks : best_bid_ticks;
        if (ref <= 0) {
            fill.status = FillStatus::NoLiquidity;
            ++stats_.no_liquidity;
            return fill;
        }

        // Limit orders are immediate-or-cancel: must be marketable.
        if (intent.type == OrderType::Limit) {
            const bool marketable =
                (intent.side == OrderSide::Buy) ? (intent.limit_price_ticks >= ref)
                                                : (intent.limit_price_ticks <= ref);
            if (!marketable) {
                fill.status = FillStatus::NoLiquidity;
                ++stats_.no_liquidity;
                return fill;
            }
        }

        Position& pos = positions_[intent.symbol_id];

        // Pre-trade risk gate.
        const RiskDecision rd = gate_.check(intent, pos.net_qty_lots, ref, kill_);
        if (rd != RiskDecision::Allow) {
            fill.status = FillStatus::Rejected;
            ++stats_.rejected;
            return fill;
        }

        // Simulated fee = ref * qty * fee_ppm / 1_000_000. At BTC prices (ref ~6e12, qty ~1e5, ppm=750)
        // the naive product overflows int64, so there are two tiers (compute_fee()). The notional itself
        // (ref * qty) must be representable: the average entry price below is built from it.
        //
        // Audit F-A-006. Every step of the fill -- the fee, the position, the cost basis, the realized PnL --
        // is computed on a COPY of the position and committed only if all of it was representable. The
        // checked multiplications used to be ignored: an overflow left a zero where a price belonged, the
        // fill was still reported as Filled, and the position carried an average entry price of 0. A fill
        // whose arithmetic does not fit is now refused whole, with the position untouched.
        std::int64_t notional = 0;
        std::int64_t fee = 0;
        Position next = pos;
        if (!safe_mul_i64(ref, intent.qty_lots, notional) || !compute_fee(notional, fee) ||
            !apply_fill(next, intent.side, intent.qty_lots, ref) ||
            !safe_sub_i64(next.realized_pnl, fee, next.realized_pnl)) {  // the fee is a realized cost
            return refuse_unrepresentable(fill);
        }
        pos = next;

        fill.status = FillStatus::Filled;
        fill.filled_qty_lots = intent.qty_lots;
        fill.fill_price_ticks = ref;
        fill.fee_ticks = fee;
        ++stats_.filled;

        // Drawdown check → arm kill switch (never auto-rearm). An equity that cannot be computed is itself
        // a reason to arm: the one thing it must not do is read as "no loss".
        if (config_.max_drawdown > 0) {
            std::int64_t mark_sum = 0;
            std::int64_t unrealized = 0;
            std::int64_t equity = 0;
            if (!safe_add_i64(best_bid_ticks, best_ask_ticks, mark_sum) ||
                !unrealized_pnl(pos, mark_sum / 2, unrealized) ||
                !safe_add_i64(pos.realized_pnl, unrealized, equity)) {
                ++stats_.overflow_guard;
                kill_.arm();
            } else if (equity < -config_.max_drawdown) {
                kill_.arm();
            }
        }

        return fill;
    }

private:
    // fee = notional * fee_ppm / 1_000_000. Exact while the product fits; otherwise the notional is divided
    // first ((notional / 1000) * fee_ppm / 1000), which keeps the leading digits at the cost of the lowest
    // three of the notional. false when even that overflows (a fee_ppm that large is a configuration error).
    bool compute_fee(std::int64_t notional, std::int64_t& fee) const noexcept {
        std::int64_t fee_num = 0;
        if (safe_mul_i64(notional, config_.fee_ppm, fee_num)) {
            fee = fee_num / 1'000'000;
            return true;
        }
        if (!safe_mul_i64(notional / 1000, config_.fee_ppm, fee_num)) return false;
        fee = fee_num / 1000;
        return true;
    }

    SimFill& refuse_unrepresentable(SimFill& fill) noexcept {
        fill.status = FillStatus::Rejected;
        ++stats_.rejected;
        ++stats_.overflow_guard;
        return fill;
    }

    // false (out untouched) when the unrealized PnL does not fit int64.
    static bool unrealized_pnl(const Position& pos, std::int64_t mark_ticks, std::int64_t& out) noexcept {
        if (pos.net_qty_lots == 0 || mark_ticks <= 0) {
            out = 0;
            return true;
        }
        std::int64_t diff = 0;
        return safe_sub_i64(mark_ticks, pos.avg_entry_price_ticks, diff) && safe_mul_i64(diff, pos.net_qty_lots, out);
    }

    // Applies one fill to `pos` (a copy the caller commits only on success). false when any step does not fit
    // int64 -- `pos` is then partly written and must be discarded.
    static bool apply_fill(Position& pos, OrderSide side, std::int64_t qty, std::int64_t price) noexcept {
        constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
        const std::int64_t signed_qty = (side == OrderSide::Buy) ? qty : -qty;  // qty > 0 here: execute() refused anything else
        const std::int64_t old_net = pos.net_qty_lots;
        std::int64_t new_net = 0;
        // INT64_MIN has no magnitude in int64, and the cost basis below is built from magnitudes.
        if (old_net == kMin || !safe_add_i64(old_net, signed_qty, new_net) || new_net == kMin) return false;

        const bool same_dir = (old_net == 0) || ((old_net > 0) == (signed_qty > 0));
        if (same_dir) {
            // Opening or increasing: weighted-average entry price.
            std::int64_t old_notional = 0;
            std::int64_t add_notional = 0;
            std::int64_t total_notional = 0;
            if (!safe_mul_i64(pos.avg_entry_price_ticks, abs_i64(old_net), old_notional) ||
                !safe_mul_i64(price, qty, add_notional) || !safe_add_i64(old_notional, add_notional, total_notional)) {
                return false;
            }
            const std::int64_t abs_new = abs_i64(new_net);
            pos.avg_entry_price_ticks = (abs_new != 0) ? total_notional / abs_new : 0;
        } else {
            // Reducing or flipping: realize PnL on the closed quantity.
            const std::int64_t closing = (qty < abs_i64(old_net)) ? qty : abs_i64(old_net);
            std::int64_t pnl_per = 0;
            std::int64_t realized = 0;
            if (!(old_net > 0 ? safe_sub_i64(price, pos.avg_entry_price_ticks, pnl_per)
                              : safe_sub_i64(pos.avg_entry_price_ticks, price, pnl_per)) ||
                !safe_mul_i64(pnl_per, closing, realized) || !safe_add_i64(pos.realized_pnl, realized, pos.realized_pnl)) {
                return false;
            }
            if (abs_i64(signed_qty) > abs_i64(old_net)) {
                // Flipped past zero: remaining quantity opens at fill price.
                pos.avg_entry_price_ticks = price;
            }
        }
        pos.net_qty_lots = new_net;
        return true;
    }

    SimConfig config_;
    RiskGate gate_;
    KillSwitch kill_{};
    SimStats stats_{};
    std::array<Position, MaxSymbols> positions_{};
};

}  // namespace hy
