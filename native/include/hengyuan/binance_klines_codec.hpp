// SPDX-License-Identifier: proprietary
// binance_klines_codec.hpp — 批次 6 6b-0f-1: pure JSON parsing/validation for Binance's
// GET /api/v3/klines response (the native side of the "REST backfill" that 6a-1's design has
// pointed at since the start but that never existed -- 外部复核 P0-05, verified: the only klines
// client in the repo was py_core/market_data/binance_public_rest.py).
//
// Deliberately zero Boost/OpenSSL dependency, same split as binance_depth_snapshot_codec.hpp vs.
// binance_rest_snapshot.hpp: this is the fail-closed validation logic, unit-testable against
// synthetic JSON under the default HY_BUILD_TESTS build. binance_klines_rest.hpp is the transport.
//
// Response shape (one array per bar):
//   [ openTime(int), "open", "high", "low", "close", "volume", closeTime(int), "quoteVolume",
//     trades(int), "takerBaseVol", "takerQuoteVol", "ignore" ]
//
// FAIL-CLOSED, the same discipline as the depth codec: any single malformed or inconsistent bar
// aborts the whole parse. The caller uses this data to REBUILD indicator state after a gap, so a
// backfill that is quietly missing or corrupting one bar is worse than no backfill at all -- the
// exact silent partial-success the whole 6b-0 line exists to rule out. Validated per bar:
// positive finite prices, non-negative volume, high >= low, open/close within [low, high],
// close_time > open_time, and (when the interval has a fixed span) close - open + 1 == span.
// Validated across bars: strictly increasing open times and exact 1ms adjacency
// (open == previous close + 1 -- the same continuity rule KlineBarGapGuard enforces live).
//
// The FORMING candle: without an endTime, Binance returns the current, still-open candle as the
// last element. A bar is closed only if its close_time < now_ms; an unclosed LAST bar is dropped
// (never handed to an indicator -- StreamingEvaluator::step() must only ever see closed bars),
// and an unclosed bar anywhere but last is an error.

#pragma once

#include <hengyuan/kline_bar.hpp>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace hy {

// kMaxBackfillBars and KlineBackfill live in kline_bar.hpp (data types, no simdjson) so the feed
// driver can use them without this header's JSON dependency.

enum class KlinesParseError : std::uint8_t {
    None = 0,
    MalformedJson = 1,      // not JSON, or the top level is not an array
    MalformedRow = 2,       // a row is not an array of >= 7 elements, or an element has the wrong type
    BadNumber = 3,          // a price/volume string is not a finite decimal, or is out of range
    BadBarShape = 4,        // close <= open time, span mismatch, high < low, or open/close outside [low, high]
    NonMonotonic = 5,       // open times not strictly increasing
    GapInside = 6,          // increasing, but not exactly adjacent (open != previous close + 1)
    UnclosedNotLast = 7,    // an unclosed bar followed by another bar
    TooManyRows = 8,        // more rows than kMaxBackfillBars
    Empty = 9,              // no CLOSED bars at all (empty response, or only the forming candle)
};

inline constexpr const char* klines_parse_error_name(KlinesParseError e) noexcept {
    switch (e) {
        case KlinesParseError::None: return "None";
        case KlinesParseError::MalformedJson: return "MalformedJson";
        case KlinesParseError::MalformedRow: return "MalformedRow";
        case KlinesParseError::BadNumber: return "BadNumber";
        case KlinesParseError::BadBarShape: return "BadBarShape";
        case KlinesParseError::NonMonotonic: return "NonMonotonic";
        case KlinesParseError::GapInside: return "GapInside";
        case KlinesParseError::UnclosedNotLast: return "UnclosedNotLast";
        case KlinesParseError::TooManyRows: return "TooManyRows";
        case KlinesParseError::Empty: return "Empty";
    }
    return "?";
}

namespace detail {

// std::from_chars is allocation-free and locale-independent (same choice as KlineJsonParser).
// Prices must be > 0 and volume >= 0, both finite.
inline bool parse_kline_decimal(std::string_view s, bool allow_zero, double& out) noexcept {
    if (s.empty()) return false;
    double v = 0.0;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) return false;  // whole string consumed
    if (!std::isfinite(v)) return false;
    if (allow_zero ? v < 0.0 : v <= 0.0) return false;
    out = v;
    return true;
}

}  // namespace detail

// `now_ms`: the exchange-time-corrected "now" (BinancePrivateRestClient's clock offset applies) --
// used only to decide which bar is still forming. `expected_span_ms`: pass
// kline_interval_span_ms(interval); 0 skips the per-bar span check (variable "1M"). `symbol_id`
// is stamped onto every bar, exactly like the live session does.
// On any error `out` may be partially filled and MUST NOT be used.
inline KlinesParseError parse_klines_response(std::string_view body, std::int64_t now_ms,
                                               std::uint32_t symbol_id, std::int64_t expected_span_ms,
                                               KlineBackfill& out) {
    out.count = 0;
    out.dropped_unclosed_tail = false;

    const auto padded = simdjson::padded_string(body);
    simdjson::ondemand::parser json_parser;
    simdjson::ondemand::document doc;
    if (json_parser.iterate(padded).get(doc)) return KlinesParseError::MalformedJson;

    simdjson::ondemand::array rows;
    if (doc.get_array().get(rows) != simdjson::SUCCESS) return KlinesParseError::MalformedJson;

    bool unclosed_seen = false;
    std::size_t row_count = 0;
    for (auto row_result : rows) {
        ++row_count;
        if (row_count > kMaxBackfillBars) return KlinesParseError::TooManyRows;
        if (unclosed_seen) return KlinesParseError::UnclosedNotLast;

        simdjson::ondemand::array row;
        if (row_result.get_array().get(row) != simdjson::SUCCESS) return KlinesParseError::MalformedRow;

        auto it = row.begin();
        auto next_i64 = [&](std::int64_t& value_out) -> bool {
            if (it == row.end()) return false;
            const bool ok = (*it).get_int64().get(value_out) == simdjson::SUCCESS;
            ++it;
            return ok;
        };
        auto next_str = [&](std::string_view& value_out) -> bool {
            if (it == row.end()) return false;
            const bool ok = (*it).get_string().get(value_out) == simdjson::SUCCESS;
            ++it;
            return ok;
        };

        std::int64_t open_time = 0;
        if (!next_i64(open_time)) return KlinesParseError::MalformedRow;

        std::string_view s_open, s_high, s_low, s_close, s_volume;
        if (!next_str(s_open) || !next_str(s_high) || !next_str(s_low) || !next_str(s_close) ||
            !next_str(s_volume)) {
            return KlinesParseError::MalformedRow;
        }

        std::int64_t close_time = 0;
        if (!next_i64(close_time)) return KlinesParseError::MalformedRow;

        double open = 0.0, high = 0.0, low = 0.0, close = 0.0, volume = 0.0;
        if (!detail::parse_kline_decimal(s_open, false, open) ||
            !detail::parse_kline_decimal(s_high, false, high) ||
            !detail::parse_kline_decimal(s_low, false, low) ||
            !detail::parse_kline_decimal(s_close, false, close) ||
            !detail::parse_kline_decimal(s_volume, true, volume)) {
            return KlinesParseError::BadNumber;
        }

        if (close_time <= open_time) return KlinesParseError::BadBarShape;
        if (expected_span_ms > 0 && close_time - open_time + 1 != expected_span_ms) {
            return KlinesParseError::BadBarShape;
        }
        if (high < low || open < low || open > high || close < low || close > high) {
            return KlinesParseError::BadBarShape;
        }

        if (close_time >= now_ms) {
            // Still forming. Only legal as the very last element; it is never returned.
            unclosed_seen = true;
            out.dropped_unclosed_tail = true;
            continue;
        }

        if (out.count > 0) {
            const KlineWsEvent& prev = out.bars[out.count - 1];
            if (open_time <= prev.open_time_ms) return KlinesParseError::NonMonotonic;
            if (open_time != prev.close_time_ms + 1) return KlinesParseError::GapInside;
        }

        KlineWsEvent& bar = out.bars[out.count];
        bar = KlineWsEvent{};
        bar.open_time_ms = open_time;
        bar.close_time_ms = close_time;
        bar.open = open;
        bar.high = high;
        bar.low = low;
        bar.close = close;
        bar.volume = volume;
        bar.symbol_id = symbol_id;
        bar.is_closed = true;
        ++out.count;
    }

    if (out.count == 0) return KlinesParseError::Empty;
    return KlinesParseError::None;
}

}  // namespace hy
