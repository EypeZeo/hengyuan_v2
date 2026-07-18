// P2-EXEC-LIVE-01 D12-1: env_parser unit tests — L1 synthetic key=value parser.
// All tests use synthetic data only, no real secrets.
#include <gtest/gtest.h>
#include <hengyuan/env_parser.hpp>

using hy::EnvAllowlist;
using hy::EnvParseResult;
using hy::EnvParseStatus;
using hy::parse_env_buffer;

static constexpr std::string_view kAllowedKeys[] = {
    "BINANCE_API_KEY",
    "BINANCE_API_SECRET",
    "ENABLE_LIVE_TRADING",
    "MAX_NOTIONAL_USD",
};

static constexpr EnvAllowlist kAllowlist{kAllowedKeys, 4};

TEST(EnvParser, ParsesBasicKeyValue) {
    auto r = parse_env_buffer("BINANCE_API_KEY=test123\n", kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.entry_count, 1u);
    EXPECT_EQ(r.get("BINANCE_API_KEY"), "test123");
}

TEST(EnvParser, ParsesMultipleEntries) {
    auto r = parse_env_buffer(
        "BINANCE_API_KEY=key1\n"
        "BINANCE_API_SECRET=secret1\n"
        "MAX_NOTIONAL_USD=100\n",
        kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.entry_count, 3u);
    EXPECT_EQ(r.get("BINANCE_API_KEY"), "key1");
    EXPECT_EQ(r.get("BINANCE_API_SECRET"), "secret1");
    EXPECT_EQ(r.get("MAX_NOTIONAL_USD"), "100");
}

TEST(EnvParser, SkipsComments) {
    auto r = parse_env_buffer(
        "# This is a comment\n"
        "BINANCE_API_KEY=val\n"
        "# Another comment\n",
        kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.entry_count, 1u);
    EXPECT_EQ(r.get("BINANCE_API_KEY"), "val");
}

TEST(EnvParser, SkipsEmptyLines) {
    auto r = parse_env_buffer(
        "\n"
        "BINANCE_API_KEY=val\n"
        "\n"
        "\n",
        kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.entry_count, 1u);
}

TEST(EnvParser, HandlesWindowsLineEndings) {
    auto r = parse_env_buffer(
        "BINANCE_API_KEY=val\r\n"
        "BINANCE_API_SECRET=sec\r\n",
        kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.entry_count, 2u);
    EXPECT_EQ(r.get("BINANCE_API_KEY"), "val");
    EXPECT_EQ(r.get("BINANCE_API_SECRET"), "sec");
}

TEST(EnvParser, StripsQuotedValues) {
    auto r = parse_env_buffer(
        "BINANCE_API_KEY=\"quoted_val\"\n"
        "BINANCE_API_SECRET='single_quoted'\n",
        kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.get("BINANCE_API_KEY"), "quoted_val");
    EXPECT_EQ(r.get("BINANCE_API_SECRET"), "single_quoted");
}

TEST(EnvParser, TrimsWhitespaceAroundKeyAndValue) {
    auto r = parse_env_buffer(
        "  BINANCE_API_KEY  =  val  \n",
        kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.get("BINANCE_API_KEY"), "val");
}

TEST(EnvParser, RejectsUnknownKey) {
    auto r = parse_env_buffer("UNKNOWN_KEY=val\n", kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::UnknownKey);
    EXPECT_EQ(r.line_number, 1u);
}

TEST(EnvParser, RejectsMalformedLineNoEquals) {
    auto r = parse_env_buffer("BINANCE_API_KEY\n", kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::MalformedLine);
    EXPECT_EQ(r.line_number, 1u);
}

TEST(EnvParser, RejectsMalformedLineEmptyKey) {
    auto r = parse_env_buffer("=value\n", kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::MalformedLine);
    EXPECT_EQ(r.line_number, 1u);
}

TEST(EnvParser, RejectsDuplicateKey) {
    auto r = parse_env_buffer(
        "BINANCE_API_KEY=val1\n"
        "BINANCE_API_KEY=val2\n",
        kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::DuplicateKey);
    EXPECT_EQ(r.line_number, 2u);
}

TEST(EnvParser, HasReturnsFalseForMissingKey) {
    auto r = parse_env_buffer("BINANCE_API_KEY=val\n", kAllowlist);
    EXPECT_TRUE(r.has("BINANCE_API_KEY"));
    EXPECT_FALSE(r.has("BINANCE_API_SECRET"));
}

TEST(EnvParser, GetReturnsEmptyForMissingKey) {
    auto r = parse_env_buffer("BINANCE_API_KEY=val\n", kAllowlist);
    EXPECT_TRUE(r.get("BINANCE_API_SECRET").empty());
}

TEST(EnvParser, EmptyValueIsValid) {
    auto r = parse_env_buffer("BINANCE_API_KEY=\n", kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.entry_count, 1u);
    EXPECT_TRUE(r.get("BINANCE_API_KEY").empty());
}

TEST(EnvParser, ValueWithEqualsSign) {
    auto r = parse_env_buffer("BINANCE_API_KEY=abc=def\n", kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.get("BINANCE_API_KEY"), "abc=def");
}

TEST(EnvParser, EmptyBufferIsOk) {
    auto r = parse_env_buffer("", kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.entry_count, 0u);
}

TEST(EnvParser, NoTrailingNewline) {
    auto r = parse_env_buffer("BINANCE_API_KEY=val", kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::Ok);
    EXPECT_EQ(r.entry_count, 1u);
    EXPECT_EQ(r.get("BINANCE_API_KEY"), "val");
}

TEST(EnvParser, ReportsCorrectLineNumberOnError) {
    auto r = parse_env_buffer(
        "BINANCE_API_KEY=ok\n"
        "# comment\n"
        "BAD_KEY=val\n",
        kAllowlist);
    EXPECT_EQ(r.status, EnvParseStatus::UnknownKey);
    EXPECT_EQ(r.line_number, 3u);
}

TEST(EnvParser, EmptyAllowlistRejectsEverything) {
    EnvAllowlist empty{nullptr, 0};
    auto r = parse_env_buffer("BINANCE_API_KEY=val\n", empty);
    EXPECT_EQ(r.status, EnvParseStatus::UnknownKey);
}

TEST(EnvParser, KeyTooLong) {
    std::string long_key(hy::kMaxKeyLen + 1, 'A');
    std::string_view long_keys[] = {std::string_view(long_key)};
    EnvAllowlist al{long_keys, 1};
    std::string line = long_key + "=val\n";
    auto r = parse_env_buffer(line, al);
    EXPECT_EQ(r.status, EnvParseStatus::KeyTooLong);
}
