// SPDX-License-Identifier: proprietary
// binance_json_parser.hpp — P2-CORE-IO-01: simdjson Binance flat-JSON parser.
// Parses raw WS bytes into BinanceMarketEvent with int64 fixed-point.
// Governance: L1/L2, no network/token/order.

#pragma once

#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/ws_frame_limits.hpp>

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
    // Allocates ALL of the parser's memory, once, and parse() never allocates again (Appendix D L-38): simdjson's working arrays and
    // string buffer sized for a frame of kWsReadMessageMaxBytes -- the largest a session can deliver -- and the padded copy buffer to
    // match. About 28 MB of address space, 4 MiB of it touched (ws_frame_limits.hpp has the arithmetic). Throws std::bad_alloc if that
    // fails: a parser that could not allocate would be a feed that silently drops every frame, which is worse than one that does not start.
    BinanceJsonParser();

    // Returns false (and registers nothing) if the table is full, the symbol name is
    // empty/too long for SymbolEntry::name, or symbol_id is out of range.
    //
    // AUDIT API-SYM-020: symbol_id used to be an unchecked std::uint32_t, but every
    // downstream consumer indexes a fixed 64-entry table with it -- InputValidator's
    // seq_state_ and HotThread's books_. Both bounds-check, so there was no
    // out-of-range access, but the failure mode was worse than a crash: HotThread's
    // book() accessor silently falls back to books_[0], so an event with
    // symbol_id >= 64 fired the on_tob_ callback carrying ITS symbol id and symbol
    // ZERO's prices. Rejecting the registration is the only place that can make that
    // unrepresentable, since the id is caller-supplied here and never validated
    // again.
    [[nodiscard]] bool register_symbol(std::string_view binance_symbol,
                                        std::uint32_t symbol_id,
                                        std::int64_t price_mult = 100'000'000,
                                        std::int64_t qty_mult = 100'000'000) noexcept;

    // Upper bound on a registrable symbol_id, pinned to the smallest per-symbol table
    // any consumer keeps. Kept in sync by static_asserts in hot_thread.hpp and
    // input_validator.hpp rather than by convention.
    static constexpr std::uint32_t kMaxSymbolId = 63;

    // Parse a single JSON message into one or more events.
    // trade/aggTrade produce 1 event; depthUpdate produces 1 per price level.
    // Returns status. On Ok, out_count holds the number of events written.
    // Never allocates. A body longer than kWsReadMessageMaxBytes -- more than a session can deliver -- is refused before it is copied:
    // MalformedJson, counted in ParseCounters::malformed.
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

    // AUDIT PERF-ALLOC-012, completed by Appendix D L-38: parse() used to build a fresh
    // simdjson::padded_string(json_bytes) per message -- a heap allocation plus a
    // full copy of every WS frame, on the hot path, directly against CLAUDE.md's
    // zero-heap-allocation mandate.
    //
    // simdjson needs SIMDJSON_PADDING readable bytes past the document, which an
    // arbitrary caller-supplied std::string_view cannot promise, so the copy itself
    // has to stay. What does not have to stay is the ALLOCATION. The first fix kept one
    // buffer that "only ever grows" and claimed zero mallocs "after the first few
    // messages" -- true only once the LARGEST frame had been seen: simdjson's on-demand
    // parser sizes its own arrays lazily inside iterate(), at that frame's length, and
    // again for every longer frame (measured with a counting operator new: 4 to 5
    // allocations on each new high-water mark, ~28 MB for a 4 MiB frame; see
    // tests/test_binance_json_parser_allocation.cpp). So everything is sized once, in
    // the constructor, for the largest frame a session can deliver: parser_.allocate()
    // for simdjson's arrays and string buffer (on-demand needs no priming parse, unlike
    // dom::parser), and this buffer for the copy. There is no growth path; parse()
    // refuses a longer body before it is copied.
    //
    // Never call parser_.set_max_capacity(): in simdjson 3.12.3 the on-demand version has
    // its comparison inverted and clamps the maximum to 32 bytes, after which iterate()
    // fails with CAPACITY on any larger document.
    std::string padded_buf_;

    const SymbolConfig* find_symbol(std::string_view name) const noexcept;
};

}  // namespace hy
