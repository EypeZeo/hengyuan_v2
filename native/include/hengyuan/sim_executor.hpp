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

        // Simulated fee = ref * qty * fee_ppm / 1_000_000 (overflow-guarded).
        // At BTC prices (ref ~6e12, qty ~1e5, ppm=750) the naive product
        // overflows int64. Three-tier fallback keeps precision:
        //   1. exact:  notional * fee_ppm / 1M  (if both muls fit)
        //   2. split:  (notional / 1000) * fee_ppm / 1000  (if ref*qty fits)
        //   3. reorder: (ref * fee_ppm / 1M) * qty  (if ref*qty doesn't fit)
        std::int64_t fee = 0;
        std::int64_t notional = 0;
        if (safe_mul_i64(ref, intent.qty_lots, notional)) {
            std::int64_t fee_num = 0;
            if (safe_mul_i64(notional, config_.fee_ppm, fee_num)) {
                fee = fee_num / 1'000'000;
            } else {
                fee = (notional / 1000) * config_.fee_ppm / 1000;
            }
        } else {
            std::int64_t ref_fee = 0;
            if (safe_mul_i64(ref, config_.fee_ppm, ref_fee)) {
                std::int64_t fee_per = ref_fee / 1'000'000;
                if (!safe_mul_i64(fee_per, intent.qty_lots, fee)) {
                    ++stats_.overflow_guard;
                }
            } else {
                ++stats_.overflow_guard;
            }
        }

        apply_fill(pos, intent.side, intent.qty_lots, ref);
        pos.realized_pnl -= fee;  // fee is a realized cost

        fill.status = FillStatus::Filled;
        fill.filled_qty_lots = intent.qty_lots;
        fill.fill_price_ticks = ref;
        fill.fee_ticks = fee;
        ++stats_.filled;

        // Drawdown check → arm kill switch (never auto-rearm).
        if (config_.max_drawdown > 0) {
            const std::int64_t mark = (best_bid_ticks + best_ask_ticks) / 2;
            const std::int64_t equity = pos.realized_pnl + unrealized_pnl(pos, mark);
            if (equity < -config_.max_drawdown) {
                kill_.arm();
            }
        }

        return fill;
    }

private:
    std::int64_t unrealized_pnl(const Position& pos, std::int64_t mark_ticks) const noexcept {
        if (pos.net_qty_lots == 0 || mark_ticks <= 0) return 0;
        const std::int64_t diff = mark_ticks - pos.avg_entry_price_ticks;
        std::int64_t pnl = 0;
        if (!safe_mul_i64(diff, pos.net_qty_lots, pnl)) {
            return 0;  // overflow-guard
        }
        return pnl;
    }

    void apply_fill(Position& pos, OrderSide side, std::int64_t qty, std::int64_t price) noexcept {
        const std::int64_t signed_qty = (side == OrderSide::Buy) ? qty : -qty;
        const std::int64_t old_net = pos.net_qty_lots;
        const std::int64_t new_net = old_net + signed_qty;

        const bool same_dir = (old_net == 0) || ((old_net > 0) == (signed_qty > 0));
        if (same_dir) {
            // Opening or increasing: weighted-average entry price.
            std::int64_t old_notional = 0;
            std::int64_t add_notional = 0;
            safe_mul_i64(pos.avg_entry_price_ticks, abs_i64(old_net), old_notional);
            safe_mul_i64(price, qty, add_notional);
            const std::int64_t abs_new = abs_i64(new_net);
            pos.avg_entry_price_ticks = (abs_new != 0) ? (old_notional + add_notional) / abs_new : 0;
        } else {
            // Reducing or flipping: realize PnL on the closed quantity.
            const std::int64_t closing = (qty < abs_i64(old_net)) ? qty : abs_i64(old_net);
            const std::int64_t pnl_per =
                (old_net > 0) ? (price - pos.avg_entry_price_ticks)
                              : (pos.avg_entry_price_ticks - price);
            std::int64_t realized = 0;
            if (safe_mul_i64(pnl_per, closing, realized)) {
                pos.realized_pnl += realized;
            } else {
                ++stats_.overflow_guard;
            }
            if (abs_i64(signed_qty) > abs_i64(old_net)) {
                // Flipped past zero: remaining quantity opens at fill price.
                pos.avg_entry_price_ticks = price;
            }
        }
        pos.net_qty_lots = new_net;
    }

    SimConfig config_;
    RiskGate gate_;
    KillSwitch kill_{};
    SimStats stats_{};
    std::array<Position, MaxSymbols> positions_{};
};

}  // namespace hy
