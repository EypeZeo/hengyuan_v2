// P2-CORE-IO-01: BinanceJsonParser unit tests with fixture JSON.
#include <gtest/gtest.h>
#include <hengyuan/binance_json_parser.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

using hy::BinanceJsonParser;
using hy::BinanceMarketEvent;
using hy::EventType;
using hy::ParseResult;
using hy::Side;

class BinanceJsonParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(parser_.register_symbol("BTCUSDT", 0));
        ASSERT_TRUE(parser_.register_symbol("ETHUSDT", 1));
    }

    ParseResult parse_one(const char* json, std::uint64_t recv_ns = 99999) {
        count_ = 0;
        return parser_.parse(json, recv_ns, evs_, kMaxEvents, count_);
    }

    BinanceJsonParser parser_;
    static constexpr std::size_t kMaxEvents = 64;
    BinanceMarketEvent evs_[kMaxEvents]{};
    std::size_t count_{0};
};

TEST_F(BinanceJsonParserTest, ParseTrade) {
    const char* json = R"({
        "e": "trade", "E": 1700000000000, "s": "BTCUSDT", "t": 123456,
        "p": "67891.23000000", "q": "0.01000000", "b": 88888, "a": 88889,
        "T": 1700000000000, "m": true, "M": true
    })";
    auto r = parse_one(json);
    EXPECT_EQ(r, ParseResult::Ok);
    EXPECT_EQ(count_, 1u);
    EXPECT_EQ(evs_[0].type, EventType::Trade);
    EXPECT_EQ(evs_[0].event_id, 123456u);
    EXPECT_EQ(evs_[0].price_ticks, 6789123000000LL);
    EXPECT_EQ(evs_[0].qty_lots, 1000000LL);
    EXPECT_EQ(evs_[0].side, Side::Sell);  // m=true -> buyer is maker -> taker is seller
    EXPECT_EQ(evs_[0].symbol_id, 0u);
    EXPECT_EQ(evs_[0].ts_event_ms, 1700000000000ULL);
    EXPECT_EQ(evs_[0].ts_recv_ns, 99999ULL);
}

TEST_F(BinanceJsonParserTest, ParseAggTrade) {
    const char* json = R"({
        "e": "aggTrade", "E": 1700000001000, "s": "ETHUSDT", "a": 789,
        "p": "2345.67800000", "q": "1.50000000",
        "f": 100, "l": 100, "T": 1700000001000, "m": false, "M": true
    })";
    auto r = parse_one(json);
    EXPECT_EQ(r, ParseResult::Ok);
    EXPECT_EQ(count_, 1u);
    EXPECT_EQ(evs_[0].type, EventType::AggTrade);
    EXPECT_EQ(evs_[0].event_id, 789u);
    EXPECT_EQ(evs_[0].price_ticks, 234567800000LL);
    EXPECT_EQ(evs_[0].qty_lots, 150000000LL);
    EXPECT_EQ(evs_[0].side, Side::Buy);  // m=false -> taker is buyer
    EXPECT_EQ(evs_[0].symbol_id, 1u);
}

TEST_F(BinanceJsonParserTest, ParseDepthUpdate) {
    const char* json = R"({
        "e": "depthUpdate", "E": 1700000002000, "s": "BTCUSDT",
        "U": 100, "u": 200,
        "b": [["67890.00000000", "1.00000000"]],
        "a": [["67891.00000000", "0.50000000"]]
    })";
    auto r = parse_one(json);
    EXPECT_EQ(r, ParseResult::Ok);
    EXPECT_EQ(count_, 2u);

    // First event: bid
    EXPECT_EQ(evs_[0].type, EventType::DepthDelta);
    EXPECT_EQ(evs_[0].event_id, 200u);
    EXPECT_EQ(evs_[0].price_ticks, 6789000000000LL);
    EXPECT_EQ(evs_[0].qty_lots, 100000000LL);
    EXPECT_EQ(evs_[0].side, Side::Buy);
    EXPECT_EQ(evs_[0].symbol_id, 0u);
    EXPECT_EQ(evs_[0].ts_event_ms, 1700000002000ULL);

    // Second event: ask
    EXPECT_EQ(evs_[1].type, EventType::DepthDelta);
    EXPECT_EQ(evs_[1].event_id, 200u);
    EXPECT_EQ(evs_[1].price_ticks, 6789100000000LL);
    EXPECT_EQ(evs_[1].qty_lots, 50000000LL);
    EXPECT_EQ(evs_[1].side, Side::Sell);
}

TEST_F(BinanceJsonParserTest, ParseDepthMultipleLevels) {
    const char* json = R"({
        "e": "depthUpdate", "E": 1700000003000, "s": "BTCUSDT",
        "U": 300, "u": 350,
        "b": [
            ["67890.00000000", "1.00000000"],
            ["67889.50000000", "2.50000000"],
            ["67888.00000000", "0.75000000"]
        ],
        "a": [
            ["67891.00000000", "0.50000000"],
            ["67892.00000000", "3.00000000"]
        ]
    })";
    auto r = parse_one(json);
    EXPECT_EQ(r, ParseResult::Ok);
    EXPECT_EQ(count_, 5u);

    // 3 bids (Side::Buy)
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(evs_[i].type, EventType::DepthDelta);
        EXPECT_EQ(evs_[i].side, Side::Buy);
        EXPECT_EQ(evs_[i].event_id, 350u);
    }
    EXPECT_EQ(evs_[0].price_ticks, 6789000000000LL);
    EXPECT_EQ(evs_[1].price_ticks, 6788950000000LL);
    EXPECT_EQ(evs_[2].price_ticks, 6788800000000LL);
    EXPECT_EQ(evs_[2].qty_lots, 75000000LL);

    // 2 asks (Side::Sell)
    for (std::size_t i = 3; i < 5; ++i) {
        EXPECT_EQ(evs_[i].type, EventType::DepthDelta);
        EXPECT_EQ(evs_[i].side, Side::Sell);
    }
    EXPECT_EQ(evs_[3].price_ticks, 6789100000000LL);
    EXPECT_EQ(evs_[4].qty_lots, 300000000LL);
}

TEST_F(BinanceJsonParserTest, ParseDepthRemoveLevel) {
    const char* json = R"({
        "e": "depthUpdate", "E": 1700000004000, "s": "BTCUSDT",
        "U": 400, "u": 450,
        "b": [["67890.00000000", "0.00000000"]],
        "a": []
    })";
    auto r = parse_one(json);
    EXPECT_EQ(r, ParseResult::Ok);
    EXPECT_EQ(count_, 1u);
    EXPECT_EQ(evs_[0].price_ticks, 6789000000000LL);
    EXPECT_EQ(evs_[0].qty_lots, 0LL);
    EXPECT_EQ(evs_[0].side, Side::Buy);
}

TEST_F(BinanceJsonParserTest, IgnoresSubscribeAck) {
    const char* json = R"({"result": null, "id": 1})";
    auto r = parse_one(json, 0);
    EXPECT_EQ(r, ParseResult::EventIgnored);
    EXPECT_EQ(count_, 0u);
    EXPECT_EQ(parser_.counters().ignored, 1u);
}

TEST_F(BinanceJsonParserTest, MalformedJson) {
    auto r = parse_one("{not valid json", 0);
    EXPECT_EQ(r, ParseResult::MalformedJson);
    EXPECT_EQ(count_, 0u);
    EXPECT_EQ(parser_.counters().malformed, 1u);
}

TEST_F(BinanceJsonParserTest, UnknownSymbol) {
    const char* json = R"({
        "e": "trade", "E": 1700000000000, "s": "DOGEUSDT", "t": 1,
        "p": "0.10000000", "q": "100.00000000", "m": true, "M": true
    })";
    auto r = parse_one(json, 0);
    EXPECT_EQ(r, ParseResult::UnknownSymbol);
    EXPECT_EQ(count_, 0u);
}

TEST_F(BinanceJsonParserTest, UnknownEventType) {
    const char* json = R"({"e": "kline", "E": 1700000000000, "s": "BTCUSDT"})";
    auto r = parse_one(json, 0);
    EXPECT_EQ(r, ParseResult::UnknownEventType);
    EXPECT_EQ(count_, 0u);
}

// --- parse_decimal_to_fixed tests ---

TEST(DecimalToFixed, WholeNumber) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("12345", 100'000'000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1234500000000LL);
}

TEST(DecimalToFixed, WithDecimals) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("67891.23000000", 100'000'000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 6789123000000LL);
}

TEST(DecimalToFixed, SmallValue) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("0.00000001", 100'000'000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1LL);
}

TEST(DecimalToFixed, Zero) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("0.00000000", 100'000'000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0LL);
}

TEST(DecimalToFixed, LargePrice) {
    // BTC at $100,000
    auto r = BinanceJsonParser::parse_decimal_to_fixed("100000.00000000", 100'000'000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 10'000'000'000'000LL);
}

TEST(DecimalToFixed, NegativeValue) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("-1.50000000", 100'000'000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, -150000000LL);
}

TEST(DecimalToFixed, EmptyString) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("", 100'000'000);
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalToFixed, InvalidChars) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("12.3abc", 100'000'000);
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalToFixed, Overflow) {
    // Huge number that would overflow int64
    auto r = BinanceJsonParser::parse_decimal_to_fixed("99999999999999999", 100'000'000);
    EXPECT_FALSE(r.has_value());
}

// --- P2-MD-02 / Track C: signed-overflow UB fix regression tests ---
// These specifically target the fractional-digit accumulation path, which previously had no
// overflow guard at all (unlike the integer-part path, which already did).

TEST(DecimalToFixed, ZeroMultiplierRejected) {
    // multiplier<=0 previously reached an integer division by zero (UB/SIGFPE).
    auto r = BinanceJsonParser::parse_decimal_to_fixed("1.5", 0);
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalToFixed, NegativeMultiplierRejected) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("1.5", -100'000'000);
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalToFixed, ExcessiveFractionalDigitsOverflowRejected) {
    // 25 fractional digits: frac_value accumulation would overflow int64 (UB) without the
    // fix's per-digit guard on the fractional path.
    auto r = BinanceJsonParser::parse_decimal_to_fixed("1.1234567890123456789012345", 100'000'000);
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalToFixed, FractionalDivisorOverflowRejected) {
    // Enough fractional digits (~19) for frac_divisor *= 10 alone to overflow, even if the
    // digits happen to be zero (frac_value itself would stay small).
    auto r = BinanceJsonParser::parse_decimal_to_fixed("1.0000000000000000000000000", 100'000'000);
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalToFixed, Int64MaxBoundary) {
    // integer part exactly at the edge the existing integer-path guard is meant to allow.
    auto r = BinanceJsonParser::parse_decimal_to_fixed("92233720368", 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 92233720368LL);
}

TEST(DecimalToFixed, NegativeWithManyFractionalDigitsStillGuarded) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("-0.1234567890123456789012345", 100'000'000);
    EXPECT_FALSE(r.has_value());
}

// --- register_symbol() input validation (audit API-SYM-020) ---
//
// symbol_id used to be an unchecked uint32. Every downstream consumer indexes a
// fixed 64-entry table with it and bounds-checks, so there was no out-of-range
// access -- but HotThread::book() falls back to books_[0] for an out-of-range id,
// so an event with symbol_id >= 64 fired on_tob_ carrying ITS id and symbol ZERO's
// prices. This is the only place that can make that unrepresentable.

TEST(RegisterSymbol, RejectsSymbolIdBeyondThePerSymbolTables) {
    BinanceJsonParser p;
    EXPECT_TRUE(p.register_symbol("BTCUSDT", BinanceJsonParser::kMaxSymbolId));
    EXPECT_FALSE(p.register_symbol("ETHUSDT", BinanceJsonParser::kMaxSymbolId + 1));
    EXPECT_FALSE(p.register_symbol("SOLUSDT", 1000));
    EXPECT_FALSE(p.register_symbol("ADAUSDT", 0xFFFFFFFFu));
}

TEST(RegisterSymbol, RejectsEmptyAndOverlongNames) {
    BinanceJsonParser p;
    EXPECT_FALSE(p.register_symbol("", 0));
    // SymbolEntry::name is 24 bytes, so 23 chars is the longest storable name.
    EXPECT_TRUE(p.register_symbol("ABCDEFGHIJKLMNOPQRSTUVW", 0));
    EXPECT_FALSE(p.register_symbol("ABCDEFGHIJKLMNOPQRSTUVWX", 1))
        << "silent truncation would make two distinct symbols collide in find_symbol()";
}

TEST(RegisterSymbol, RejectsNonPositiveMultipliers) {
    BinanceJsonParser p;
    EXPECT_FALSE(p.register_symbol("BTCUSDT", 0, 0, 100'000'000));
    EXPECT_FALSE(p.register_symbol("BTCUSDT", 0, 100'000'000, -1));
    EXPECT_TRUE(p.register_symbol("BTCUSDT", 0, 100'000'000, 100'000'000));
}

TEST(RegisterSymbol, RejectedRegistrationLeavesTheTableUsable) {
    BinanceJsonParser p;
    ASSERT_FALSE(p.register_symbol("BTCUSDT", 9999));
    ASSERT_TRUE(p.register_symbol("BTCUSDT", 0));

    BinanceMarketEvent evs[4]{};
    std::size_t count = 0;
    const char* json =
        R"({"e":"trade","E":1,"s":"BTCUSDT","t":1,"p":"100.0","q":"1.0","m":false})";
    EXPECT_EQ(p.parse(json, 0, evs, 4, count), ParseResult::Ok);
    EXPECT_EQ(evs[0].symbol_id, 0u);
}

// --- depthUpdate truncation emits a resync marker (audit MD-TRUNC-015) ---
//
// This used to `break` out of the level loop and return Ok with whatever fit,
// silently diverging the local book from the exchange. Because the bids loop runs
// first, a large enough message could consume every slot and leave the asks side
// completely unrepresented -- still reported as a clean parse.

TEST(DepthTruncation, OverlongDepthUpdateYieldsSingleResyncMarker) {
    BinanceJsonParser p;
    ASSERT_TRUE(p.register_symbol("BTCUSDT", 0));

    // 8 bid levels into a 4-slot batch.
    std::string json = R"({"e":"depthUpdate","E":123,"s":"BTCUSDT","U":10,"u":15,"b":[)";
    for (int i = 0; i < 8; ++i) {
        if (i) json += ",";
        json += "[\"" + std::to_string(50000 - i) + ".0\",\"1.0\"]";
    }
    json += R"(],"a":[["50010.0","1.0"]]})";

    BinanceMarketEvent evs[4]{};
    std::size_t count = 0;
    auto r = p.parse(json, 999, evs, 4, count);

    EXPECT_EQ(r, ParseResult::TruncatedResync);
    ASSERT_EQ(count, 1u) << "the partial delta must be discarded, not handed back";
    EXPECT_EQ(evs[0].type, EventType::DepthDelta);
    EXPECT_EQ(evs[0].symbol_id, 0u);
    EXPECT_EQ(evs[0].event_id, 15u);
    EXPECT_EQ(evs[0].aux_id, 10u);
    EXPECT_NE(evs[0].flags & hy::event_flag::kResyncRequired, 0)
        << "the marker must be what drives DepthManager back to Buffering";
    EXPECT_EQ(p.counters().truncated_resync, 1u);
    EXPECT_EQ(p.counters().parsed_ok, 0u) << "truncation must not be counted as a clean parse";
}

TEST(DepthTruncation, ExactlyFittingDepthUpdateIsNotTruncated) {
    // Boundary: as many levels as slots must still be an ordinary Ok.
    BinanceJsonParser p;
    ASSERT_TRUE(p.register_symbol("BTCUSDT", 0));
    std::string json =
        R"({"e":"depthUpdate","E":123,"s":"BTCUSDT","U":10,"u":15,)"
        R"("b":[["50000.0","1.0"],["49999.0","2.0"]],"a":[["50010.0","1.0"],["50011.0","2.0"]]})";

    BinanceMarketEvent evs[4]{};
    std::size_t count = 0;
    auto r = p.parse(json, 999, evs, 4, count);

    EXPECT_EQ(r, ParseResult::Ok);
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(p.counters().truncated_resync, 0u);
}

TEST(DepthTruncation, TruncationInTheAsksLoopIsAlsoCaught) {
    // Bids fit exactly; the asks side is what overflows. Under the old code this was
    // the worst case -- the book would apply bid-only updates forever.
    BinanceJsonParser p;
    ASSERT_TRUE(p.register_symbol("BTCUSDT", 0));
    std::string json =
        R"({"e":"depthUpdate","E":123,"s":"BTCUSDT","U":10,"u":15,)"
        R"("b":[["50000.0","1.0"],["49999.0","2.0"]],)"
        R"("a":[["50010.0","1.0"],["50011.0","2.0"],["50012.0","3.0"]]})";

    BinanceMarketEvent evs[4]{};
    std::size_t count = 0;
    auto r = p.parse(json, 999, evs, 4, count);

    EXPECT_EQ(r, ParseResult::TruncatedResync);
    ASSERT_EQ(count, 1u);
    EXPECT_NE(evs[0].flags & hy::event_flag::kResyncRequired, 0);
}

// --- Reusable padded buffer (audit PERF-ALLOC-012) ---

TEST(ParserPaddedBuffer, RepeatedParsesOfVaryingSizesStayCorrect) {
    // The buffer is reused and only grows, so a short message parsed after a long
    // one must not see stale bytes from the previous document.
    BinanceJsonParser p;
    ASSERT_TRUE(p.register_symbol("BTCUSDT", 0));

    std::string big = R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,"b":[)";
    for (int i = 0; i < 200; ++i) {
        if (i) big += ",";
        big += "[\"" + std::to_string(50000 - i) + ".0\",\"1.0\"]";
    }
    big += R"(],"a":[["60000.0","1.0"]]})";

    const std::string small =
        R"({"e":"trade","E":7,"s":"BTCUSDT","t":42,"p":"100.5","q":"2.25","m":false})";

    BinanceMarketEvent evs[512]{};
    std::size_t count = 0;

    ASSERT_EQ(p.parse(big, 1, evs, 512, count), ParseResult::Ok);
    ASSERT_EQ(count, 201u);

    ASSERT_EQ(p.parse(small, 2, evs, 512, count), ParseResult::Ok);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(evs[0].type, EventType::Trade);
    EXPECT_EQ(evs[0].event_id, 42u);
    EXPECT_EQ(evs[0].price_ticks, 10050000000LL);
    EXPECT_EQ(evs[0].qty_lots, 225000000LL);

    // ...and back up again, to exercise growth after shrink.
    ASSERT_EQ(p.parse(big, 3, evs, 512, count), ParseResult::Ok);
    EXPECT_EQ(count, 201u);
}

TEST(ParserPaddedBuffer, EmptyInputIsRejectedNotUndefined) {
    BinanceJsonParser p;
    ASSERT_TRUE(p.register_symbol("BTCUSDT", 0));
    BinanceMarketEvent evs[4]{};
    std::size_t count = 0;
    EXPECT_EQ(p.parse("", 1, evs, 4, count), ParseResult::MalformedJson);
    EXPECT_EQ(count, 0u);
}

// --- Combined-contribution overflow (audit MD-NUM-001) ---
//
// Each of the three pre-existing guards proves ONE step doesn't overflow: the
// integer-digit loop, the fractional-digit loop, and integer_part*multiplier.
// None of them constrains their SUM. `max/multiplier` truncates, so integer_part
// is admitted right up to floor(max/multiplier), leaving only `max % multiplier`
// of headroom -- 54,775,807 for the default 1e8 multiplier -- while frac_contrib
// reaches multiplier-1 = 99,999,999. UBSan reports
// "signed integer overflow: 99999999 + 9223372036800000000" on these inputs
// without the final guard. Reachable straight from an untrusted WS "p"/"q" string
// and from binance_depth_snapshot_codec.hpp's REST path.
TEST(DecimalToFixed, CombinedContributionOverflowRejected) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("92233720368.99999999", 100'000'000);
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalToFixed, CombinedContributionOverflowRejectedNegative) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("-92233720368.99999999", 100'000'000);
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalToFixed, CombinedContributionJustUnderMaxAccepted) {
    // The largest value that genuinely fits: floor(max/1e8) whole units plus the
    // exact remaining headroom (max % 1e8 = 54,775,807). Proves the new guard
    // rejects only what actually overflows, rather than clipping the top of the
    // representable range.
    auto r = BinanceJsonParser::parse_decimal_to_fixed("92233720368.54775807", 100'000'000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::numeric_limits<std::int64_t>::max());
}

TEST(DecimalToFixed, CombinedContributionOneTickOverMaxRejected) {
    auto r = BinanceJsonParser::parse_decimal_to_fixed("92233720368.54775808", 100'000'000);
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalToFixed, CombinedContributionOverflowOtherMultiplier) {
    // Same class of input at a 3-decimal multiplier, so the fix isn't tied to 1e8.
    // max/1000 = 9223372036854775, remaining headroom = 807.
    auto over = BinanceJsonParser::parse_decimal_to_fixed("9223372036854775.808", 1000);
    EXPECT_FALSE(over.has_value());
    auto ok = BinanceJsonParser::parse_decimal_to_fixed("9223372036854775.807", 1000);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, std::numeric_limits<std::int64_t>::max());
}
