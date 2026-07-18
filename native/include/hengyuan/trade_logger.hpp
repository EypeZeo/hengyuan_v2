// SPDX-License-Identifier: proprietary
// trade_logger.hpp — CSV trade log for dry-run simulation results.
//
// Appends one row per simulated fill with position, PnL, fee, kill state.
// Used for post-run equity curve analysis and trade review.
// Governance: L2, no network/token/order. File I/O only.

#pragma once

#include <hengyuan/execution_types.hpp>
#include <hengyuan/kill_switch.hpp>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace hy {

class TradeLogger {
public:
    TradeLogger() = default;
    ~TradeLogger() { close(); }

    TradeLogger(const TradeLogger&) = delete;
    TradeLogger& operator=(const TradeLogger&) = delete;

    bool open(const std::string& path) noexcept {
        close();
        fp_ = std::fopen(path.c_str(), "w");
        if (!fp_) return false;
        std::fprintf(fp_,
            "trade_num,timestamp_ms,symbol_id,side,qty_lots,fill_price_ticks,"
            "fee_ticks,net_position_lots,avg_entry_ticks,realized_pnl,"
            "unrealized_pnl,kill_state\n");
        return true;
    }

    void log(int trade_num,
             std::uint64_t timestamp_ms,
             std::uint32_t symbol_id,
             OrderSide side,
             const SimFill& fill,
             const Position& pos,
             std::int64_t unrealized_pnl,
             KillState ks) noexcept {
        if (!fp_) return;
        std::fprintf(fp_,
            "%d,%" PRIu64 ",%u,%s,%" PRId64 ",%" PRId64 ","
            "%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ","
            "%" PRId64 ",%s\n",
            trade_num,
            timestamp_ms,
            symbol_id,
            side == OrderSide::Buy ? "BUY" : "SELL",
            fill.filled_qty_lots,
            fill.fill_price_ticks,
            fill.fee_ticks,
            pos.net_qty_lots,
            pos.avg_entry_price_ticks,
            pos.realized_pnl,
            unrealized_pnl,
            kill_state_str(ks));
        ++rows_written_;
    }

    void close() noexcept {
        if (fp_) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    bool is_open() const noexcept { return fp_ != nullptr; }
    std::uint64_t rows_written() const noexcept { return rows_written_; }

private:
    static const char* kill_state_str(KillState s) noexcept {
        switch (s) {
            case KillState::Normal:    return "NORMAL";
            case KillState::Armed:     return "ARMED";
            case KillState::Triggered: return "TRIGGERED";
            case KillState::Latched:   return "LATCHED";
        }
        return "?";
    }

    std::FILE* fp_{nullptr};
    std::uint64_t rows_written_{0};
};

}  // namespace hy
