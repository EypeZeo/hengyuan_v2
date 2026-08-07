// P2-CORE-IO-01: BinanceJsonParser unit tests with fixture JSON.
#include <gtest/gtest.h>
#include <hengyuan/binance_json_parser.hpp>

using hy::BinanceJsonParser;
using hy::BinanceMarketEvent;
using hy::EventType;
using hy::ParseResult;
using hy::Side;

class BinanceJsonParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_.register_symbol("BTCUSDT", 0);
        parser_.register_symbol("ETHUSDT", 1);
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
