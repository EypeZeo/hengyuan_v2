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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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
    // A depthUpdate carried more price levels than out_events could hold. Depth
    // deltas are explicitly NOT conflatable (binance_market_event.hpp), so the
    // partial levels are discarded and out_events[0] is a single synthetic event
    // carrying event_flag::kResyncRequired instead. The caller MUST push that event
    // exactly as it pushes an Ok result -- it is what drives DepthManager back to
    // Buffering. Distinct from Ok so the outcome is countable rather than silent
    // (audit MD-TRUNC-015: this used to `break` and return Ok, which diverged the
    // book from the exchange with no signal anywhere).
    TruncatedResync = 7,
};

struct ParseCounters {
    std::uint64_t parsed_ok{0};
    std::uint64_t ignored{0};
    std::uint64_t malformed{0};
    std::uint64_t unknown_symbol{0};
    std::uint64_t unknown_event{0};
    std::uint64_t price_overflow{0};
    std::uint64_t qty_overflow{0};
    std::uint64_t truncated_resync{0};  // audit MD-TRUNC-015
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

    // AUDIT PERF-ALLOC-012: parse() used to build a fresh
    // simdjson::padded_string(json_bytes) per message -- a heap allocation plus a
    // full copy of every WS frame, on the hot path, directly against CLAUDE.md's
    // zero-heap-allocation mandate.
    //
    // simdjson needs SIMDJSON_PADDING readable bytes past the document, which an
    // arbitrary caller-supplied std::string_view cannot promise, so the copy itself
    // has to stay. What does not have to stay is the ALLOCATION: this buffer is
    // reused across calls and only ever grows, so after the first few messages the
    // steady state is a memcpy into already-owned storage and zero mallocs. Sized at
    // construction to cover realistic Binance frames (depth@100ms with 20+20 levels
    // is a few KB) so warm-up is immediate rather than gradual.
    static constexpr std::size_t kInitialPaddedCapacity = 64 * 1024;
    std::string padded_buf_ = std::string(kInitialPaddedCapacity, '\0');

    const SymbolConfig* find_symbol(std::string_view name) const noexcept;
};

}  // namespace hy
