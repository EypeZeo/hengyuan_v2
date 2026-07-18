// SPDX-License-Identifier: proprietary
//
// binance_market_event.hpp — HengYuan native core, Data Plane.
//
// Cache-line-aligned (64B), int64 fixed-point market event for the Binance
// flat-JSON feed. This is the unit passed from the I/O thread (Boost.Asio +
// simdjson, future P2-CORE-IO-01) to the pinned hot thread via SpscRing.
//
// Governance (P2-CORE-01-IMPL, ADR-016 L1):
//   - Header-only data structure. NO network, NO token, NO HMAC/signing,
//     NO Binance Private API, NO order placement, NO DB.
//   - double is BANNED for price/qty. All money values are int64 fixed-point.
//   - This is the repo's first native code; it carries no side effects.
//
// Fixed-point convention:
//   real_price = price_ticks * tick_size(symbol_id)
//   real_qty   = qty_lots   * lot_size(symbol_id)
//   tick_size / lot_size live in a static cold table keyed by symbol_id and
//   are intentionally NOT part of this hot-path struct.

#pragma once

#include <cstddef>  // offsetof
#include <cstdint>
#include <type_traits>

namespace hy {

// Taker aggressor side. Binance trade stream carries "m" (buyer is maker):
//   m == true  -> buyer is maker  -> the aggressor (taker) is the SELLER -> Sell
//   m == false -> seller is maker -> the aggressor (taker) is the BUYER  -> Buy
enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

enum class EventType : std::uint8_t {
    Trade = 0,       // <symbol>@trade
    AggTrade = 1,    // <symbol>@aggTrade
    DepthDelta = 2,  // <symbol>@depth (order book delta — NOT conflatable)
    Kline = 3,       // <symbol>@kline_<interval>
};

// Bit flags for the `flags` field (combine with bitwise OR).
namespace event_flag {
inline constexpr std::uint16_t kNone = 0;
inline constexpr std::uint16_t kSnapshot = 1u << 0;        // part of an initial snapshot
inline constexpr std::uint16_t kStale = 1u << 1;           // freshness check failed
inline constexpr std::uint16_t kResyncRequired = 1u << 2;  // book gap -> must REST resync
inline constexpr std::uint16_t kClockAnomaly = 1u << 3;    // exchange ts non-monotonic
}  // namespace event_flag

// 64-byte, cache-line-aligned market event. Exactly one cache line so that
// a single event never straddles two lines (avoids split-line loads on the
// hot path and keeps SpscRing slots line-aligned).
struct alignas(64) BinanceMarketEvent {
    std::uint64_t event_id;       // [ 0] trade id / aggTrade id / depth final updateId
    std::int64_t price_ticks;     // [ 8] fixed-point price (NEVER double)
    std::int64_t qty_lots;        // [16] fixed-point quantity (NEVER double)
    std::uint64_t ts_event_ms;    // [24] Binance event time "E" (ms); validate monotonicity
    std::uint64_t ts_recv_ns;     // [32] local monotonic receive time (CLOCK_MONOTONIC)
    std::uint32_t symbol_id;      // [40] local symbol-table index (never a string)
    EventType type;               // [44]
    Side side;                    // [45] taker aggressor side
    std::uint16_t flags;          // [46] event_flag::* bitmask
    std::uint64_t aux_id;         // [48] depth first updateId (U); 0 for non-depth
    std::uint8_t _pad[8];         // [56..63] explicit pad to a full cache line
};

// ABI / layout invariants. These are the "tests" for this header: they fire at
// compile time on any C++20 compiler (e.g. the Tokyo VPS). See P2-CORE-04 for
// the cross-compiler ABI contract that will extend these.
static_assert(sizeof(BinanceMarketEvent) == 64, "BinanceMarketEvent must be exactly one 64B cache line");
static_assert(alignof(BinanceMarketEvent) == 64, "BinanceMarketEvent must be 64B-aligned");
static_assert(std::is_trivially_copyable_v<BinanceMarketEvent>,
              "BinanceMarketEvent must be trivially copyable for zero-alloc SPSC memcpy semantics");
static_assert(std::is_standard_layout_v<BinanceMarketEvent>,
              "BinanceMarketEvent must be standard-layout for stable cross-language/cross-compiler ABI");

// Explicit field offsets — guard against silent layout drift across compilers.
static_assert(offsetof(BinanceMarketEvent, event_id) == 0);
static_assert(offsetof(BinanceMarketEvent, price_ticks) == 8);
static_assert(offsetof(BinanceMarketEvent, qty_lots) == 16);
static_assert(offsetof(BinanceMarketEvent, ts_event_ms) == 24);
static_assert(offsetof(BinanceMarketEvent, ts_recv_ns) == 32);
static_assert(offsetof(BinanceMarketEvent, symbol_id) == 40);
static_assert(offsetof(BinanceMarketEvent, flags) == 46);
static_assert(offsetof(BinanceMarketEvent, aux_id) == 48);

}  // namespace hy
