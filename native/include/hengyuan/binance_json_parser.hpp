// SPDX-License-Identifier: proprietary
// binance_json_parser.hpp — P2-CORE-IO-01: simdjson Binance flat-JSON parser.
// Parses raw WS bytes into BinanceMarketEvent with int64 fixed-point.
// Governance: L1/L2, no network/token/order.

#pragma once

#include <hengyuan/binance_market_event.hpp>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace hy {

struct SymbolConfig {
    std::uint32_t symbol_id;
    std::int64_t price_multiplier;  // e.g. 1e8 for 8 decimal places
    std::int64_t qty_multiplier;    // e.g. 1e8
};

enum class ParseResult : std::uint8_t {
    Ok = 0,
    EventIgnored = 1,    // heartbeat, subscribe ack, etc.
    MalformedJson = 2,
    UnknownSymbol = 3,
    UnknownEventType = 4,
    PriceOverflow = 5,
    QtyOverflow = 6,
};

struct ParseCounters {
    std::uint64_t parsed_ok{0};
    std::uint64_t ignored{0};
    std::uint64_t malformed{0};
    std::uint64_t unknown_symbol{0};
    std::uint64_t unknown_event{0};
    std::uint64_t price_overflow{0};
    std::uint64_t qty_overflow{0};
};

class BinanceJsonParser {
public:
    void register_symbol(std::string_view binance_symbol,
                         std::uint32_t symbol_id,
                         std::int64_t price_mult = 100'000'000,
                         std::int64_t qty_mult = 100'000'000) noexcept;

    // Parse a single JSON message into one or more events.
    // trade/aggTrade produce 1 event; depthUpdate produces 1 per price level.
    // Returns status. On Ok, out_count holds the number of events written.
    ParseResult parse(std::string_view json_bytes,
                      std::uint64_t recv_ns,
                      BinanceMarketEvent* out_events,
                      std::size_t max_events,
                      std::size_t& out_count) noexcept;

    const ParseCounters& counters() const noexcept { return counters_; }
    void reset_counters() noexcept { counters_ = {}; }

    static std::optional<std::int64_t> parse_decimal_to_fixed(
        std::string_view s, std::int64_t multiplier) noexcept;

private:
    static constexpr std::size_t kMaxSymbols = 64;
    struct SymbolEntry {
        char name[24]{};
        std::size_t name_len{0};
        SymbolConfig config{};
    };
    std::array<SymbolEntry, kMaxSymbols> symbols_{};
    std::size_t symbol_count_{0};

    simdjson::ondemand::parser parser_;
    ParseCounters counters_{};

    const SymbolConfig* find_symbol(std::string_view name) const noexcept;
};

}  // namespace hy
