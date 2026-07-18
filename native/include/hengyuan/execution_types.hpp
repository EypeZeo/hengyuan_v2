// SPDX-License-Identifier: proprietary
// execution_types.hpp — P2-EXEC-SIM-01: dry-run simulation types.
// SIMULATION ONLY. No Binance Private API, no HMAC, no token, no real order,
// no network, no money. int64 fixed-point throughout. Governance: L2.

#pragma once

#include <cstdint>

namespace hy {

enum class OrderSide : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

enum class OrderType : std::uint8_t {
    Market = 0,  // fills at best opposing price
    Limit = 1,   // immediate-or-cancel: fills only if marketable
};

// An execution intent produced by a (future) strategy and consumed by the
// SIMULATED executor. In the live system this would cross to the I/O thread
// for signing + submission (L5); here it only drives simulation.
struct ExecutionIntent {
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Market};
    std::int64_t qty_lots{0};
    std::int64_t limit_price_ticks{0};  // ignored for Market
};

enum class FillStatus : std::uint8_t {
    Filled = 0,
    Rejected = 1,    // risk gate / kill switch blocked it
    NoLiquidity = 2, // empty book or limit not marketable
    NoFill = 3,      // zero/invalid qty
};

struct SimFill {
    FillStatus status{FillStatus::NoFill};
    std::int64_t filled_qty_lots{0};
    std::int64_t fill_price_ticks{0};
    std::int64_t fee_ticks{0};  // simulated fee in price-ticks * qty terms
};

// Per-symbol simulated position. Long = positive net_qty, short = negative.
struct Position {
    std::int64_t net_qty_lots{0};
    std::int64_t avg_entry_price_ticks{0};
    std::int64_t realized_pnl{0};  // in ticks*lots units (simulated)
};

}  // namespace hy
