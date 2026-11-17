// P2-MD-02 / Track C: binance_depth_snapshot_codec.hpp unit tests.
//
// Deliberately zero Boost/OpenSSL dependency (this test file only includes the codec header),
// so it builds and runs under the default HY_BUILD_TESTS configuration without needing
// HY_BUILD_DEMO -- see native/CMakeLists.txt's registration for this target.

#include <gtest/gtest.h>
#include <hengyuan/binance_depth_snapshot_codec.hpp>

#include <string>
#include <variant>

using hy::DepthSnapshot;
using hy::DepthSnapshotParseError;
using hy::parse_depth_response;

namespace {

constexpr std::int64_t kPriceMult = 100'000'000;
constexpr std::int64_t kQtyMult = 100'000'000;

bool is_error(const std::variant<DepthSnapshot, DepthSnapshotParseError>& result,
              DepthSnapshotParseError expected) {
    return std::holds_alternative<DepthSnapshotParseError>(result) &&
           std::get<DepthSnapshotParseError>(result) == expected;
}

}  // namespace

TEST(DepthSnapshotCodec, ParsesValidResponse) {
    const char* body = R"({
        "lastUpdateId": 12345,
        "bids": [["100.50000000", "1.00000000"], ["100.00000000", "2.00000000"]],
        "asks": [["101.00000000", "1.50000000"], ["101.50000000", "0.50000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    ASSERT_TRUE(std::holds_alternative<DepthSnapshot>(result));
    const auto& snap = std::get<DepthSnapshot>(result);
    EXPECT_EQ(snap.last_update_id, 12345u);
    ASSERT_EQ(snap.bid_count, 2u);
    ASSERT_EQ(snap.ask_count, 2u);
    EXPECT_EQ(snap.bids[0].price_ticks, 10050000000LL);
    EXPECT_EQ(snap.bids[1].price_ticks, 10000000000LL);
    EXPECT_EQ(snap.asks[0].price_ticks, 10100000000LL);
    EXPECT_EQ(snap.asks[1].price_ticks, 10150000000LL);
}

TEST(DepthSnapshotCodec, MissingLastUpdateIdRejected) {
    const char* body = R"({
        "bids": [["100.00000000", "1.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::InvalidLastUpdateId));
}

TEST(DepthSnapshotCodec, ZeroLastUpdateIdRejected) {
    const char* body = R"({
        "lastUpdateId": 0,
        "bids": [["100.00000000", "1.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::InvalidLastUpdateId));
}

TEST(DepthSnapshotCodec, WrongTypeLastUpdateIdRejected) {
    const char* body = R"({
        "lastUpdateId": "not-a-number",
        "bids": [["100.00000000", "1.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::InvalidLastUpdateId));
}

TEST(DepthSnapshotCodec, BidsNotArrayRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": "not-an-array",
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::MalformedLevel));
}

TEST(DepthSnapshotCodec, LevelMissingQtyRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [["100.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::MalformedLevel));
}

TEST(DepthSnapshotCodec, LevelPriceNotStringRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [[100.5, "1.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::MalformedLevel));
}

TEST(DepthSnapshotCodec, InvalidRestPriceLevelsAreRejected) {
    struct Case {
        const char* name;
        const char* bids;
        const char* asks;
    };
    const Case cases[] = {
        {"bid_level_has_extra_field", R"(["100","1","extra"])", R"(["101","1"])"},
        {"ask_level_has_extra_field", R"(["100","1"])", R"(["101","1","extra"])"},
        {"zero_bid_price", R"(["0","1"])", R"(["101","1"])"},
        {"negative_bid_price", R"(["-1","1"])", R"(["101","1"])"},
        {"zero_ask_price", R"(["100","1"])", R"(["0","1"])"},
        {"negative_ask_price", R"(["100","1"])", R"(["-1","1"])"},
        {"valid_and_invalid_bid_levels", R"(["100","1"],["0","1"])", R"(["101","1"])"},
        {"valid_and_invalid_ask_levels", R"(["100","1"])", R"(["0","1"],["101","1"])"},
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        const std::string body = std::string(R"({"lastUpdateId":1,"bids":[)") + test_case.bids +
                                 R"(],"asks":[)" + test_case.asks + R"(]})";
        const auto result = parse_depth_response(body, kPriceMult, kQtyMult);
        EXPECT_TRUE(is_error(result, DepthSnapshotParseError::MalformedLevel));
    }
}

TEST(DepthSnapshotCodec, UnparseableDecimalRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [["not-a-decimal", "1.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::MalformedLevel));
}

TEST(DepthSnapshotCodec, ZeroQtyRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [["100.00000000", "0.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::NonPositiveQty));
}

TEST(DepthSnapshotCodec, NegativeQtyRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [["100.00000000", "-1.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::NonPositiveQty));
}

TEST(DepthSnapshotCodec, BidsNotStrictlyDescendingRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [["100.00000000", "1.00000000"], ["100.50000000", "1.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::OrderingViolation));
}

TEST(DepthSnapshotCodec, DuplicateBidPriceRejected) {
    // Strictly descending means no ties either.
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [["100.00000000", "1.00000000"], ["100.00000000", "2.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::OrderingViolation));
}

TEST(DepthSnapshotCodec, AsksNotStrictlyAscendingRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [["100.00000000", "1.00000000"]],
        "asks": [["101.50000000", "1.00000000"], ["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::OrderingViolation));
}

TEST(DepthSnapshotCodec, CrossedBookRejected) {
    // best_bid >= best_ask
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [["101.00000000", "1.00000000"]],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::OrderingViolation));
}

TEST(DepthSnapshotCodec, EmptyBidsRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [],
        "asks": [["101.00000000", "1.00000000"]]
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::EmptySide));
}

TEST(DepthSnapshotCodec, EmptyAsksRejected) {
    const char* body = R"({
        "lastUpdateId": 1,
        "bids": [["100.00000000", "1.00000000"]],
        "asks": []
    })";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::EmptySide));
}

TEST(DepthSnapshotCodec, BothSidesEmptyRejected) {
    const char* body = R"({"lastUpdateId": 1, "bids": [], "asks": []})";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    EXPECT_TRUE(is_error(result, DepthSnapshotParseError::EmptySide));
}

TEST(DepthSnapshotCodec, MalformedJsonRejected) {
    const char* body = "{not valid json";
    auto result = parse_depth_response(body, kPriceMult, kQtyMult);
    ASSERT_TRUE(std::holds_alternative<DepthSnapshotParseError>(result));
}
