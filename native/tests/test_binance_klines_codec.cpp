// 批次 6 6b-0f-1: binance_klines_codec.hpp tests -- pure JSON, no network, no Boost.
//
// The codec's job is to be strict: a backfill is used to REBUILD indicator state, so a bar that is
// quietly wrong or missing is worse than no backfill. Most tests below are therefore negative
// controls, each pinning one reason the whole response must be refused.

#include <gtest/gtest.h>
#include <hengyuan/binance_klines_codec.hpp>

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>

using hy::kline_interval_span_ms;
using hy::KlineBackfill;
using hy::KlinesParseError;
using hy::parse_klines_response;

namespace {

constexpr std::int64_t kFarFuture = 10'000'000'000'000LL;  // every test bar is "closed" by default
constexpr std::int64_t kOneMinute = 60'000;

// One Binance kline row. Prices default to a sane bar (low 9, high 12, open 10, close 11).
std::string row(std::int64_t open_time, std::int64_t close_time, const char* o = "10", const char* h = "12",
                const char* l = "9", const char* c = "11", const char* v = "5") {
    return "[" + std::to_string(open_time) + ",\"" + o + "\",\"" + h + "\",\"" + l + "\",\"" + c + "\",\"" +
           v + "\"," + std::to_string(close_time) + ",\"1.0\",3,\"1.0\",\"1.0\",\"0\"]";
}

// n contiguous 1m bars starting at `first_open`.
std::string contiguous_rows(std::size_t n, std::int64_t first_open = 0, std::int64_t span = kOneMinute) {
    std::string s = "[";
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) s += ",";
        const std::int64_t open = first_open + static_cast<std::int64_t>(i) * span;
        s += row(open, open + span - 1);
    }
    return s + "]";
}

KlineBackfill& shared_out() {
    static auto out = std::make_unique<KlineBackfill>();  // 64 KB -- not a stack local
    return *out;
}

KlinesParseError parse(const std::string& body, std::int64_t now = kFarFuture,
                       std::int64_t span = kOneMinute) {
    return parse_klines_response(body, now, /*symbol_id=*/7, span, shared_out());
}

}  // namespace

// --- happy paths -----------------------------------------------------------------------------

// Binance's own documented sample row (7-day bar: 1499040000000 .. 1499644799999).
TEST(KlinesCodec, ParsesBinancesDocumentedSampleRow) {
    const std::string body =
        R"([[1499040000000,"0.01634790","0.80000000","0.01575800","0.01577100","148976.11427815",)"
        R"(1499644799999,"2434.19055334",308,"1756.87402397","28.46694368","0"]])";
    ASSERT_EQ(parse_klines_response(body, kFarFuture, 3, kline_interval_span_ms("1w"), shared_out()),
              KlinesParseError::None);
    ASSERT_EQ(shared_out().count, 1u);
    const auto& b = shared_out().bars[0];
    EXPECT_EQ(b.open_time_ms, 1499040000000);
    EXPECT_EQ(b.close_time_ms, 1499644799999);
    EXPECT_DOUBLE_EQ(b.open, 0.01634790);
    EXPECT_DOUBLE_EQ(b.high, 0.8);
    EXPECT_DOUBLE_EQ(b.low, 0.015758);
    EXPECT_DOUBLE_EQ(b.close, 0.015771);
    EXPECT_DOUBLE_EQ(b.volume, 148976.11427815);
    EXPECT_EQ(b.symbol_id, 3u);  // stamped by the caller, not parsed
    EXPECT_TRUE(b.is_closed);
    EXPECT_FALSE(shared_out().dropped_unclosed_tail);
}

TEST(KlinesCodec, ParsesManyContiguousBarsInOrder) {
    ASSERT_EQ(parse(contiguous_rows(50)), KlinesParseError::None);
    ASSERT_EQ(shared_out().count, 50u);
    for (std::size_t i = 1; i < 50; ++i) {
        EXPECT_EQ(shared_out().bars[i].open_time_ms, shared_out().bars[i - 1].close_time_ms + 1);
    }
}

TEST(KlinesCodec, AcceptsExactlyTheMaximumNumberOfRows) {
    ASSERT_EQ(parse(contiguous_rows(hy::kMaxBackfillBars)), KlinesParseError::None);
    EXPECT_EQ(shared_out().count, hy::kMaxBackfillBars);
}

// --- the forming candle ----------------------------------------------------------------------

TEST(KlinesCodec, TheStillFormingLastBarIsDroppedNeverReturned) {
    // Bars 0,1 closed; bar 2 closes at 3*60000-1 = 179999 but now_ms is inside it.
    const std::string body = contiguous_rows(3);
    ASSERT_EQ(parse(body, /*now=*/150'000), KlinesParseError::None);
    EXPECT_EQ(shared_out().count, 2u);
    EXPECT_TRUE(shared_out().dropped_unclosed_tail);
    EXPECT_EQ(shared_out().bars[1].close_time_ms, 119'999);
}

TEST(KlinesCodec, ABarClosesStrictlyBeforeNow) {
    // close_time 59999: closed iff now > 59999. At now == 59999 it is still "forming".
    EXPECT_EQ(parse(contiguous_rows(1), /*now=*/59'999), KlinesParseError::Empty);
    ASSERT_EQ(parse(contiguous_rows(1), /*now=*/60'000), KlinesParseError::None);
    EXPECT_EQ(shared_out().count, 1u);
}

TEST(KlinesCodec, OnlyAFormingCandleIsEmptyNotSuccess) {
    EXPECT_EQ(parse(contiguous_rows(1), /*now=*/1'000), KlinesParseError::Empty);
}

TEST(KlinesCodec, AnUnclosedBarFollowedByAnotherBarIsRefused) {
    // Bar 0 unclosed at now=30000, but bar 1 follows it: an inconsistent response.
    EXPECT_EQ(parse(contiguous_rows(2), /*now=*/30'000), KlinesParseError::UnclosedNotLast);
}

TEST(KlinesCodec, EmptyArrayIsEmpty) { EXPECT_EQ(parse("[]"), KlinesParseError::Empty); }

// --- continuity ------------------------------------------------------------------------------

TEST(KlinesCodec, AMissingBarInsideIsRefused) {
    // bars at 0 and 120000 -- the 60000 bar is absent.
    const std::string body = "[" + row(0, 59'999) + "," + row(120'000, 179'999) + "]";
    EXPECT_EQ(parse(body), KlinesParseError::GapInside);
}

TEST(KlinesCodec, OverlappingOrRepeatedBarsAreRefused) {
    const std::string dup = "[" + row(0, 59'999) + "," + row(0, 59'999) + "]";
    EXPECT_EQ(parse(dup), KlinesParseError::NonMonotonic);
    const std::string reversed = "[" + row(60'000, 119'999) + "," + row(0, 59'999) + "]";
    EXPECT_EQ(parse(reversed), KlinesParseError::NonMonotonic);
}

TEST(KlinesCodec, TooManyRowsIsRefused) {
    EXPECT_EQ(parse(contiguous_rows(hy::kMaxBackfillBars + 1)), KlinesParseError::TooManyRows);
}

// --- per-bar shape ---------------------------------------------------------------------------

TEST(KlinesCodec, SpanMismatchIsRefusedButCanBeSkippedForVariableIntervals) {
    const std::string body = "[" + row(0, 59'998) + "]";  // 59999 ms long, a 1m bar must be 60000
    EXPECT_EQ(parse(body, kFarFuture, kOneMinute), KlinesParseError::BadBarShape);
    // "1M" (span 0 = variable): the same bar passes, since no span is asserted.
    EXPECT_EQ(parse(body, kFarFuture, /*span=*/0), KlinesParseError::None);
}

TEST(KlinesCodec, InconsistentOhlcIsRefused) {
    EXPECT_EQ(parse("[" + row(0, 59'999, "10", "8", "9", "9") + "]"), KlinesParseError::BadBarShape);   // high < low
    EXPECT_EQ(parse("[" + row(0, 59'999, "13", "12", "9", "11") + "]"), KlinesParseError::BadBarShape);  // open > high
    EXPECT_EQ(parse("[" + row(0, 59'999, "10", "12", "9", "8") + "]"), KlinesParseError::BadBarShape);   // close < low
}

TEST(KlinesCodec, CloseTimeNotAfterOpenTimeIsRefused) {
    EXPECT_EQ(parse("[" + row(60'000, 60'000) + "]", kFarFuture, 0), KlinesParseError::BadBarShape);
    EXPECT_EQ(parse("[" + row(60'000, 1) + "]", kFarFuture, 0), KlinesParseError::BadBarShape);
}

TEST(KlinesCodec, BadNumbersAreRefused) {
    EXPECT_EQ(parse("[" + row(0, 59'999, "0", "12", "9", "11") + "]"), KlinesParseError::BadNumber);     // zero price
    EXPECT_EQ(parse("[" + row(0, 59'999, "-10", "12", "9", "11") + "]"), KlinesParseError::BadNumber);   // negative
    EXPECT_EQ(parse("[" + row(0, 59'999, "10", "12", "9", "11", "-1") + "]"), KlinesParseError::BadNumber);  // volume < 0
    EXPECT_EQ(parse("[" + row(0, 59'999, "10abc", "12", "9", "11") + "]"), KlinesParseError::BadNumber);  // trailing garbage
    EXPECT_EQ(parse("[" + row(0, 59'999, "", "12", "9", "11") + "]"), KlinesParseError::BadNumber);       // empty
    EXPECT_EQ(parse("[" + row(0, 59'999, "nan", "12", "9", "11") + "]"), KlinesParseError::BadNumber);
    EXPECT_EQ(parse("[" + row(0, 59'999, "inf", "12", "9", "11") + "]"), KlinesParseError::BadNumber);
}

TEST(KlinesCodec, ZeroVolumeIsAllowed) {
    ASSERT_EQ(parse("[" + row(0, 59'999, "10", "12", "9", "11", "0") + "]"), KlinesParseError::None);
    EXPECT_DOUBLE_EQ(shared_out().bars[0].volume, 0.0);
}

// --- structure -------------------------------------------------------------------------------

TEST(KlinesCodec, MalformedStructureIsRefused) {
    EXPECT_EQ(parse(""), KlinesParseError::MalformedJson);
    EXPECT_EQ(parse("not json"), KlinesParseError::MalformedJson);
    EXPECT_EQ(parse(R"({"code":-1121,"msg":"Invalid symbol."})"), KlinesParseError::MalformedJson);  // error body
    EXPECT_EQ(parse("[1,2,3]"), KlinesParseError::MalformedRow);                                     // rows aren't arrays
    EXPECT_EQ(parse(R"([[0,"10","12","9","11"]])"), KlinesParseError::MalformedRow);                 // too short
    EXPECT_EQ(parse(R"([["0","10","12","9","11","5",59999]])"), KlinesParseError::MalformedRow);     // openTime is a string
    EXPECT_EQ(parse(R"([[0,10,12,9,11,5,59999]])"), KlinesParseError::MalformedRow);                 // prices are numbers
}

// A body cut anywhere before its final byte must never come out as a (shorter) valid backfill. A cut
// that lands on a row boundary looks exactly like "N-1 bars" to a careless parser, and rebuilding
// indicator state from one bar too few, silently, is the failure this codec exists to prevent.
TEST(KlinesCodec, EveryProperPrefixOfAValidResponseIsRefused) {
    const std::string full = contiguous_rows(3);
    ASSERT_EQ(parse(full), KlinesParseError::None);  // control: the whole document is fine
    for (std::size_t cut = 0; cut < full.size(); ++cut) {
        EXPECT_NE(parse(full.substr(0, cut)), KlinesParseError::None)
            << "cut=" << cut << " prefix=" << full.substr(0, cut);
    }
}

// --- interval helpers ------------------------------------------------------------------------

TEST(KlineIntervalSpan, KnownIntervalsHaveTheirExactFixedSpan) {
    EXPECT_EQ(kline_interval_span_ms("1s"), 1'000);
    EXPECT_EQ(kline_interval_span_ms("1m"), 60'000);
    EXPECT_EQ(kline_interval_span_ms("3m"), 180'000);
    EXPECT_EQ(kline_interval_span_ms("15m"), 900'000);
    EXPECT_EQ(kline_interval_span_ms("1h"), 3'600'000);
    EXPECT_EQ(kline_interval_span_ms("4h"), 14'400'000);
    EXPECT_EQ(kline_interval_span_ms("12h"), 43'200'000);
    EXPECT_EQ(kline_interval_span_ms("1d"), 86'400'000);
    EXPECT_EQ(kline_interval_span_ms("3d"), 259'200'000);
    EXPECT_EQ(kline_interval_span_ms("1w"), 604'800'000);
}

TEST(KlineIntervalSpan, MonthIsValidButVariableAndUnknownIsInvalid) {
    EXPECT_TRUE(hy::is_valid_kline_interval("1M"));
    EXPECT_EQ(kline_interval_span_ms("1M"), 0);        // variable: callers skip the span check
    EXPECT_FALSE(hy::is_valid_kline_interval("1x"));
    EXPECT_FALSE(hy::is_valid_kline_interval(""));
    EXPECT_EQ(kline_interval_span_ms("1x"), 0);
    EXPECT_TRUE(hy::is_valid_kline_interval("1m"));    // minute and month are distinct by case
}

// The documented sample bar really does span exactly one "1w" interval -- so the span table and
// Binance's own numbers agree.
TEST(KlineIntervalSpan, DocumentedSampleBarMatchesTheWeekSpan) {
    EXPECT_EQ(1499644799999LL - 1499040000000LL + 1, kline_interval_span_ms("1w"));
}

TEST(KlinesParseErrorName, EveryErrorHasADistinctName) {
    const KlinesParseError all[] = {KlinesParseError::None,        KlinesParseError::MalformedJson,
                                    KlinesParseError::MalformedRow, KlinesParseError::BadNumber,
                                    KlinesParseError::BadBarShape,  KlinesParseError::NonMonotonic,
                                    KlinesParseError::GapInside,    KlinesParseError::UnclosedNotLast,
                                    KlinesParseError::TooManyRows,  KlinesParseError::Empty};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        EXPECT_STRNE(hy::klines_parse_error_name(all[i]), "?");
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            EXPECT_STRNE(hy::klines_parse_error_name(all[i]), hy::klines_parse_error_name(all[j]));
        }
    }
}
