// L4 §3/§4 (docs/BINANCE_PRIVATE_REST_L4_SPEC.md rev 72): binance_private_rest.hpp unit tests.
//
// Pure-logic pieces (parse_balance_decimal_to_ticks/parse_server_time_response/
// parse_account_response) are tested directly against hand-crafted strings/JSON bodies --
// no network involved, same split test_binance_rest_snapshot.cpp already establishes between
// its codec layer and its transport layer.
//
// Network-layer tests use two local TLS fixtures: blackhole_acceptor.hpp's TlsBlackholeAcceptor
// (handshake-then-silent, for timeout-stage tests, same as test_binance_rest_snapshot.cpp) and
// this file's own test_helpers/tls_response_acceptor.hpp's TlsResponseAcceptor (a real minimal
// HTTP responder, needed here because -- unlike the depth-snapshot fetcher's tests -- this file
// also needs to exercise the FULL success path: a real round trip whose response actually
// drives compute_clock_offset()/ClockOffsetPublisher::publish() and
// parse_account_response()/AccountSnapshot end-to-end).
//
// Synthetic test credentials only (written to a temp .env file, matching
// test_binance_environment.cpp's own convention) -- no real API keys, no real network access.
//
// TLS fixture note: EnvironmentBinding::testnet()'s base_host() is hardcoded to
// "testnet.binance.vision" (by design -- see that header's own comment on why host is not a
// runtime parameter), so hostname verification needs a certificate whose SAN actually says
// that, unlike test_binance_rest_snapshot.cpp's tests (which use PrivateRestConfig-equivalent
// RestSnapshotConfig::host as a free parameter and can just point it at the existing
// wrong-san.test.invalid fixture cert). test_leaf_cert_testnet_host.pem/
// test_leaf_key_testnet_host.pem are a dedicated self-signed cert+key pair
// (CN/SAN=testnet.binance.vision) generated for exactly this reason -- not sharing the
// existing wrong-san fixture, which would fail hostname verification here on purpose.

#include <gtest/gtest.h>
#include <hengyuan/binance_private_rest.hpp>

#include "test_helpers/blackhole_acceptor.hpp"
#include "test_helpers/tls_response_acceptor.hpp"

#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using hy::AccountSnapshot;
using hy::AssetBalance;
using hy::BinancePrivateRestClient;
using hy::BoundHmacCredentials;
using hy::ClientOrderId;
using hy::ClockOffsetSnapshot;
using hy::EnvAllowlist;
using hy::EnvironmentBinding;
using hy::OrderExpectation;
using hy::OrderSide;
using hy::OrderState;
using hy::OrderType;
using hy::PrivateRestConfig;
using hy::PrivateRestError;
using hy::QueryOutcome;
using hy::QueryResult;
using hy::QuerySigningError;
using hy::SecureEnvLoader;
using hy::SymbolRules;
using hy::build_exchange_info_target;
using hy::fetch_clock_pair;
using hy::is_snapshot_fresh;
using hy::kBalanceScale;
using hy::kListenKeyLen;
using hy::map_binance_order_status;
using hy::parse_listen_key_response;
using hy::parse_account_response;
using hy::parse_balance_decimal_to_ticks;
using hy::parse_exchange_info_response;
using hy::format_ticks_to_decimal;
using hy::parse_order_query_response;
using hy::parse_server_time_response;
using hy::parse_submit_order_response;
using hy::query_order_adapter;
using hy::SubmitOutcome;
using hy::SubmitResponse;
using hy::submit_order_adapter;

namespace {

std::string fixture_path(const char* filename) {
    return std::string(HY_TEST_FIXTURE_DIR) + "/" + filename;
}

constexpr std::string_view kSyntheticApiKey =
    "TESTKEYabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234";
constexpr std::string_view kSyntheticSecret = "synthetic-test-secret-not-real";

constexpr std::string_view kTestnetAllowedKeys[] = {
    "HENGYUAN_BINANCE_TESTNET_API_KEY",
    "HENGYUAN_BINANCE_TESTNET_SECRET",
};
constexpr EnvAllowlist kTestnetAllowlist{kTestnetAllowedKeys, 2};

// L4 §6: a Binance-realistic (not the intentionally-degenerate 0/8 pair
// test_account_truth.cpp uses) price_scale/qty_scale/quote_scale triple, chosen so exponent =
// price_scale + qty_scale - quote_scale == 0 for the worked success-path example below (keeps
// the hand-verified avg_fill_price_ticks arithmetic simple: a plain integer division, no
// scale-shift to also verify by hand).
OrderExpectation make_btcusdt_expectation(std::string_view coid) {
    SymbolRules rules{};
    std::strncpy(rules.symbol, "BTCUSDT", sizeof(rules.symbol) - 1);
    rules.is_trading = true;
    rules.price_scale = 2;
    rules.qty_scale = 6;
    rules.quote_scale = 8;

    OrderExpectation exp{};
    std::strncpy(exp.client_order_id.id, coid.data(),
                 std::min(coid.size(), sizeof(exp.client_order_id.id) - 1));
    exp.symbol_id = 1;
    exp.side = OrderSide::Buy;
    exp.order_type = OrderType::Limit;
    exp.intended_price_ticks = 5'000'012;  // "50000.12" at price_scale=2
    exp.intended_qty_ticks = 100'000;      // "0.100000" at qty_scale=6
    exp.rules_snapshot_at_submit = rules;
    return exp;
}

// Same price_scale=2/qty_scale=6/quote_scale=8 triple as make_btcusdt_expectation() above,
// for the TODO 1A.3 submit_order()/parse_submit_order_response() tests below, which take a
// SymbolRules directly rather than a captured OrderExpectation (submit_order() is the FIRST
// leg -- there is no prior OrderRecord to have built an OrderExpectation from).
SymbolRules make_btcusdt_rules() {
    SymbolRules rules{};
    std::strncpy(rules.symbol, "BTCUSDT", sizeof(rules.symbol) - 1);
    rules.is_trading = true;
    rules.price_scale = 2;
    rules.qty_scale = 6;
    rules.quote_scale = 8;
    return rules;
}

class BoundCredentialsFixture : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        tmp_path_ =
            std::string(tmp) + "hy_test_privrest_" + std::to_string(GetCurrentProcessId()) + ".env";
#else
        tmp_path_ = "/tmp/hy_test_privrest_" + std::to_string(getpid()) + ".env";
#endif
        std::ofstream f(tmp_path_, std::ios::binary);
        f << "HENGYUAN_BINANCE_TESTNET_API_KEY=" << kSyntheticApiKey << "\n";
        f << "HENGYUAN_BINANCE_TESTNET_SECRET=" << kSyntheticSecret << "\n";
        f.close();
        // env_loader.hpp's Linux path rejects (PermissionTooWide) any file readable/writable
        // by group or other -- Windows has no equivalent check. A freshly created temp file's
        // mode depends on the process umask, not guaranteed to already satisfy this (matches
        // test_binance_environment.cpp's own chmod_owner_only()).
#ifdef __linux__
        chmod(tmp_path_.c_str(), 0600);
#endif
    }

    void TearDown() override { std::remove(tmp_path_.c_str()); }

    std::unique_ptr<BoundHmacCredentials> make_creds() {
        SecureEnvLoader loader;
        EXPECT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status, hy::EnvLoadStatus::Ok);
        auto [err, creds] =
            BoundHmacCredentials::load_and_bind_credentials(EnvironmentBinding::testnet(), loader);
        EXPECT_EQ(err, QuerySigningError::Ok);
        return std::move(creds);
    }
};

}  // namespace

// --- parse_balance_decimal_to_ticks() -- §4.2 lossless conversion ---

TEST(ParseBalanceDecimalToTicks, IntegerOnly) {
    std::int64_t out = 0;
    ASSERT_TRUE(parse_balance_decimal_to_ticks("5", out));
    EXPECT_EQ(out, 500'000'000);  // 5 * 10^8
}

TEST(ParseBalanceDecimalToTicks, ExactlyEightFractionalDigits) {
    std::int64_t out = 0;
    ASSERT_TRUE(parse_balance_decimal_to_ticks("1.23456789", out));
    EXPECT_EQ(out, 123'456'789);
}

TEST(ParseBalanceDecimalToTicks, FewerThanEightFractionalDigitsPadsWithZeros) {
    std::int64_t out = 0;
    ASSERT_TRUE(parse_balance_decimal_to_ticks("1.5", out));
    EXPECT_EQ(out, 150'000'000);
}

TEST(ParseBalanceDecimalToTicks, ZeroVariantsAllParseToZeroTicks) {
    for (std::string_view s : {"0", "0.0", "0.00000000"}) {
        std::int64_t out = -1;
        ASSERT_TRUE(parse_balance_decimal_to_ticks(s, out)) << s;
        EXPECT_EQ(out, 0) << s;
    }
}

TEST(ParseBalanceDecimalToTicks, MoreThanEightFractionalDigitsIsRejectedNotTruncated) {
    // AUDIT: this is the exact failure mode parse_decimal_to_fixed() (binance_json_parser.hpp)
    // has -- it silently truncates instead of rejecting. Pin the opposite (correct) behavior.
    std::int64_t out = 0;
    EXPECT_FALSE(parse_balance_decimal_to_ticks("1.123456789", out));  // 9 fractional digits
}

TEST(ParseBalanceDecimalToTicks, NegativeIsRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(parse_balance_decimal_to_ticks("-1.5", out));
}

TEST(ParseBalanceDecimalToTicks, EmptyStringIsRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(parse_balance_decimal_to_ticks("", out));
}

TEST(ParseBalanceDecimalToTicks, TrailingDotWithNoDigitsIsRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(parse_balance_decimal_to_ticks("5.", out));
}

TEST(ParseBalanceDecimalToTicks, NonDigitCharactersAreRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(parse_balance_decimal_to_ticks("1.2x", out));
    EXPECT_FALSE(parse_balance_decimal_to_ticks("abc", out));
}

TEST(ParseBalanceDecimalToTicks, MultipleDecimalPointsAreRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(parse_balance_decimal_to_ticks("1.2.3", out));
}

TEST(ParseBalanceDecimalToTicks, OverflowIsRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(parse_balance_decimal_to_ticks("99999999999999999999.0", out));
}

TEST(ParseBalanceDecimalToTicks, BalanceScaleIsEight) {
    EXPECT_EQ(kBalanceScale, 8);
}

// --- format_ticks_to_decimal() -- TODO 1A.3, the reverse of parse_decimal_to_ticks_with_scale ---

TEST(FormatTicksToDecimal, ScaleZeroFormatsAsBareInteger) {
    char buf[32];
    std::size_t len = 0;
    ASSERT_TRUE(format_ticks_to_decimal(12345, 0, buf, len));
    EXPECT_EQ(std::string_view(buf, len), "12345");
}

TEST(FormatTicksToDecimal, ExactScaleSixFormatsWithZeroPaddedFraction) {
    char buf[32];
    std::size_t len = 0;
    ASSERT_TRUE(format_ticks_to_decimal(100'000, 6, buf, len));  // "0.100000"
    EXPECT_EQ(std::string_view(buf, len), "0.100000");
}

TEST(FormatTicksToDecimal, FractionalPartIsZeroPaddedNotTruncated) {
    char buf[32];
    std::size_t len = 0;
    ASSERT_TRUE(format_ticks_to_decimal(5'000'001, 6, buf, len));  // "5.000001", not "5.1"
    EXPECT_EQ(std::string_view(buf, len), "5.000001");
}

TEST(FormatTicksToDecimal, ScaleEighteenBoundaryAccepted) {
    char buf[32];
    std::size_t len = 0;
    ASSERT_TRUE(format_ticks_to_decimal(1, 18, buf, len));
    EXPECT_EQ(std::string_view(buf, len), "0.000000000000000001");
}

TEST(FormatTicksToDecimal, ScaleNineteenRejected) {
    char buf[32];
    std::size_t len = 0;
    EXPECT_FALSE(format_ticks_to_decimal(1, 19, buf, len));  // pow10_i64's own domain, [0,18]
}

TEST(FormatTicksToDecimal, NegativeTicksRejected) {
    char buf[32];
    std::size_t len = 0;
    EXPECT_FALSE(format_ticks_to_decimal(-1, 2, buf, len));
}

TEST(FormatTicksToDecimal, BufferTooSmallRejectedNotTruncated) {
    char buf[4];  // "50000.12" needs 8 chars + NUL -- 4 is deliberately too small
    std::size_t len = 0;
    EXPECT_FALSE(format_ticks_to_decimal(5'000'012, 2, buf, len));
}

TEST(FormatTicksToDecimal, RoundTripsWithParseDecimalToTicksWithScale) {
    char buf[32];
    std::size_t len = 0;
    ASSERT_TRUE(format_ticks_to_decimal(5'000'012, 2, buf, len));
    std::int64_t round_tripped = 0;
    ASSERT_TRUE(hy::parse_decimal_to_ticks_with_scale(std::string_view(buf, len), 2, round_tripped));
    EXPECT_EQ(round_tripped, 5'000'012);
}

// --- parse_server_time_response() -- §3 ---

TEST(ParseServerTimeResponse, ValidBody) {
    std::int64_t out = 0;
    ASSERT_TRUE(parse_server_time_response(R"({"serverTime":1700000000000})", out));
    EXPECT_EQ(out, 1700000000000LL);
}

TEST(ParseServerTimeResponse, MissingFieldRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(parse_server_time_response(R"({"foo":1})", out));
}

TEST(ParseServerTimeResponse, NegativeValueRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(parse_server_time_response(R"({"serverTime":-1})", out));
}

TEST(ParseServerTimeResponse, MalformedJsonRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(parse_server_time_response("not json", out));
}

// --- parse_account_response() -- §4.1-§4.4 ---

TEST(ParseAccountResponse, ValidResponseWithMultipleAssets) {
    AccountSnapshot out{};
    const auto err = parse_account_response(
        R"({"canTrade":true,"balances":[
            {"asset":"BTC","free":"1.5","locked":"0.5"},
            {"asset":"USDT","free":"1000.12345678","locked":"0"}
        ]})",
        out);
    ASSERT_EQ(err, PrivateRestError::None);
    EXPECT_TRUE(out.can_trade);
    ASSERT_EQ(out.asset_count, 2u);
    const AssetBalance* btc = out.find("BTC");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->free_ticks, 150'000'000);
    EXPECT_EQ(btc->locked_ticks, 50'000'000);
    const AssetBalance* usdt = out.find("USDT");
    ASSERT_NE(usdt, nullptr);
    EXPECT_EQ(usdt->free_ticks, 100'012'345'678LL);
    EXPECT_EQ(usdt->locked_ticks, 0);
}

TEST(ParseAccountResponse, ZeroBalanceAssetsAreSkippedNotSlotted) {
    AccountSnapshot out{};
    const auto err = parse_account_response(
        R"({"canTrade":true,"balances":[
            {"asset":"BTC","free":"0","locked":"0"},
            {"asset":"ETH","free":"1.0","locked":"0"}
        ]})",
        out);
    ASSERT_EQ(err, PrivateRestError::None);
    ASSERT_EQ(out.asset_count, 1u);
    EXPECT_NE(out.find("ETH"), nullptr);
    EXPECT_EQ(out.find("BTC"), nullptr);
}

TEST(ParseAccountResponse, MissingCanTradeLeavesOutUnchanged) {
    AccountSnapshot out{};
    out.can_trade = true;
    out.asset_count = 7;  // sentinel -- must survive untouched on failure (§4.4)
    const auto err = parse_account_response(R"({"balances":[]})", out);
    EXPECT_EQ(err, PrivateRestError::MalformedResponse);
    EXPECT_TRUE(out.can_trade);
    EXPECT_EQ(out.asset_count, 7u);
}

TEST(ParseAccountResponse, MissingBalancesArrayRejected) {
    AccountSnapshot out{};
    const auto err = parse_account_response(R"({"canTrade":true})", out);
    EXPECT_EQ(err, PrivateRestError::MalformedResponse);
}

TEST(ParseAccountResponse, MissingAssetFieldInBalanceEntryRejected) {
    AccountSnapshot out{};
    const auto err = parse_account_response(
        R"({"canTrade":true,"balances":[{"free":"1.0","locked":"0"}]})", out);
    EXPECT_EQ(err, PrivateRestError::MalformedResponse);
}

TEST(ParseAccountResponse, MalformedDecimalInBalanceRejectsWholeFetch) {
    AccountSnapshot out{};
    out.asset_count = 3;  // sentinel
    const auto err = parse_account_response(
        R"({"canTrade":true,"balances":[
            {"asset":"BTC","free":"1.0","locked":"0"},
            {"asset":"ETH","free":"not-a-number","locked":"0"}
        ]})",
        out);
    EXPECT_EQ(err, PrivateRestError::MalformedResponse);
    EXPECT_EQ(out.asset_count, 3u);  // unchanged -- BTC's successful parse must not leak through
}

TEST(ParseAccountResponse, TooManyFractionalDigitsInBalanceRejectsWholeFetch) {
    AccountSnapshot out{};
    const auto err = parse_account_response(
        R"({"canTrade":true,"balances":[{"asset":"BTC","free":"1.123456789","locked":"0"}]})",
        out);
    EXPECT_EQ(err, PrivateRestError::MalformedResponse);
}

TEST(ParseAccountResponse, MoreThanMaxAssetsRejectsWholeFetchNotTruncated) {
    std::string body = R"({"canTrade":true,"balances":[)";
    for (int i = 0; i < 33; ++i) {  // kMaxAssets == 32
        if (i > 0) body += ",";
        body += "{\"asset\":\"A" + std::to_string(i) + "\",\"free\":\"1.0\",\"locked\":\"0\"}";
    }
    body += "]}";

    AccountSnapshot out{};
    out.asset_count = 5;  // sentinel
    const auto err = parse_account_response(body, out);
    EXPECT_EQ(err, PrivateRestError::CapacityExceeded);
    EXPECT_EQ(out.asset_count, 5u);  // unchanged -- not truncated to the first 32
}

TEST(ParseAccountResponse, ExactlyMaxAssetsSucceeds) {
    std::string body = R"({"canTrade":true,"balances":[)";
    for (int i = 0; i < 32; ++i) {
        if (i > 0) body += ",";
        body += "{\"asset\":\"A" + std::to_string(i) + "\",\"free\":\"1.0\",\"locked\":\"0\"}";
    }
    body += "]}";

    AccountSnapshot out{};
    const auto err = parse_account_response(body, out);
    ASSERT_EQ(err, PrivateRestError::None);
    EXPECT_EQ(out.asset_count, 32u);
}

TEST(ParseAccountResponse, AssetNameExactlyAtCapacityFits) {
    // kAssetNameLen == 12 (11 chars + NUL) -- an 11-char symbol fits exactly.
    AccountSnapshot out{};
    const auto err = parse_account_response(
        R"({"canTrade":true,"balances":[{"asset":"ELEVENCHARS","free":"1.0","locked":"0"}]})",
        out);
    ASSERT_EQ(err, PrivateRestError::None);
    ASSERT_EQ(out.asset_count, 1u);
    EXPECT_NE(out.find("ELEVENCHARS"), nullptr);
}

TEST(ParseAccountResponse, OverlongAssetNameRejectedNotTruncated) {
    AccountSnapshot out{};
    // kAssetNameLen == 12 (11 chars + NUL) -- a 12-char symbol overruns it.
    const auto err = parse_account_response(
        R"({"canTrade":true,"balances":[{"asset":"TWELVELETTRS","free":"1.0","locked":"0"}]})",
        out);
    EXPECT_EQ(err, PrivateRestError::MalformedResponse);
}

TEST(ParseAccountResponse, EmptyBodyRejectedAtTheIterateStage) {
    // simdjson::ondemand is lazy: iterate() itself only fails on a genuinely empty document --
    // verified directly (an empty string is the only input among several malformed candidates
    // tried that reaches this path; everything else, including syntactically invalid JSON like
    // "not json" or "{", gets past iterate() and only fails later at field access, see the next
    // test). Both paths are failure returns that leave `out` untouched either way -- the
    // distinction matters for diagnostics, not for correctness.
    AccountSnapshot out{};
    const auto err = parse_account_response("", out);
    EXPECT_EQ(err, PrivateRestError::JsonParse);
}

TEST(ParseAccountResponse, SyntacticallyInvalidJsonRejectedAtFieldAccess) {
    AccountSnapshot out{};
    const auto err = parse_account_response("not json", out);
    EXPECT_EQ(err, PrivateRestError::MalformedResponse);
}

// --- map_binance_order_status() -- L4 §6.5's status-string table ---

TEST(MapBinanceOrderStatus, EveryKnownStatusMapsToItsDocumentedState) {
    const std::pair<std::string_view, OrderState> kTable[] = {
        {"NEW", OrderState::Accepted},
        {"PARTIALLY_FILLED", OrderState::PartialFill},
        {"FILLED", OrderState::Filled},
        {"PENDING_CANCEL", OrderState::CancelRequested},
        {"CANCELED", OrderState::Cancelled},
        {"REJECTED", OrderState::Rejected},
        {"EXPIRED", OrderState::Expired},
        {"EXPIRED_IN_MATCH", OrderState::Expired},
    };
    for (const auto& [status, expected] : kTable) {
        OrderState out{};
        ASSERT_TRUE(map_binance_order_status(status, out)) << status;
        EXPECT_EQ(out, expected) << status;
    }
}

TEST(MapBinanceOrderStatus, UnrecognizedStatusIsRejectedNotGuessed) {
    OrderState out = OrderState::Filled;  // sentinel
    EXPECT_FALSE(map_binance_order_status("SOME_FUTURE_STATUS", out));
    EXPECT_EQ(out, OrderState::Filled);  // untouched
}

// --- parse_order_query_response() -- L4 §6.1.2 schema + field-match, §6.1.1 fill derivation ---

TEST(ParseOrderQueryResponse, FullSuccessPathFindsAndDerivesAvgFillPrice) {
    const auto expected = make_btcusdt_expectation("test-coid-001");
    const auto result = parse_order_query_response(
        R"({"symbol":"BTCUSDT","orderId":12345,"clientOrderId":"test-coid-001",)"
        R"("price":"50000.12","origQty":"0.100000","executedQty":"0.050000",)"
        R"("cummulativeQuoteQty":"2500.00000000","status":"PARTIALLY_FILLED",)"
        R"("side":"BUY","type":"LIMIT","timeInForce":"GTC"})",
        expected);
    ASSERT_EQ(result.outcome, QueryOutcome::Found);
    EXPECT_EQ(result.confirmed_state, OrderState::PartialFill);
    EXPECT_EQ(result.exchange_order_id, 12345);
    EXPECT_EQ(result.filled_qty_ticks, 50'000);          // "0.050000" at qty_scale=6
    EXPECT_EQ(result.avg_fill_price_ticks, 5'000'000);   // 2500.00000000/0.050000 = 50000.00
}

TEST(ParseOrderQueryResponse, UnfilledOrderReportsZeroFillFieldsNotDivideByZero) {
    const auto expected = make_btcusdt_expectation("test-coid-002");
    const auto result = parse_order_query_response(
        R"({"symbol":"BTCUSDT","orderId":1,"clientOrderId":"test-coid-002",)"
        R"("price":"50000.12","origQty":"0.100000","executedQty":"0.000000",)"
        R"("cummulativeQuoteQty":"0.00000000","status":"NEW",)"
        R"("side":"BUY","type":"LIMIT","timeInForce":"GTC"})",
        expected);
    ASSERT_EQ(result.outcome, QueryOutcome::Found);
    EXPECT_EQ(result.confirmed_state, OrderState::Accepted);
    EXPECT_EQ(result.filled_qty_ticks, 0);
    EXPECT_EQ(result.avg_fill_price_ticks, 0);
}

TEST(ParseOrderQueryResponse, MalformedJsonIsInconclusive) {
    const auto expected = make_btcusdt_expectation("test-coid-003");
    EXPECT_EQ(parse_order_query_response("not json", expected).outcome, QueryOutcome::Inconclusive);
}

TEST(ParseOrderQueryResponse, EachMissingRequiredFieldIsInconclusive) {
    const auto expected = make_btcusdt_expectation("test-coid-004");
    // One full valid body, then each required field independently removed.
    const std::string_view kFields[] = {
        "symbol", "orderId", "clientOrderId", "price", "origQty", "executedQty",
        "cummulativeQuoteQty", "status", "side", "type", "timeInForce",
    };
    for (auto field : kFields) {
        std::string body =
            R"({"symbol":"BTCUSDT","orderId":1,"clientOrderId":"test-coid-004",)"
            R"("price":"50000.12","origQty":"0.100000","executedQty":"0.000000",)"
            R"("cummulativeQuoteQty":"0.00000000","status":"NEW",)"
            R"("side":"BUY","type":"LIMIT","timeInForce":"GTC"})";
        // Rename the target field's key so it's absent under its expected name (cheaper than
        // building 11 bespoke bodies, and just as precise -- the parser looks up by name).
        const std::string needle = "\"" + std::string(field) + "\":";
        const auto pos = body.find(needle);
        ASSERT_NE(pos, std::string::npos) << field;
        body.replace(pos, needle.size(), "\"_absent_\":");
        EXPECT_EQ(parse_order_query_response(body, expected).outcome, QueryOutcome::Inconclusive)
            << "field=" << field;
    }
}

TEST(ParseOrderQueryResponse, EachFieldMismatchIsInconclusive) {
    const auto expected = make_btcusdt_expectation("test-coid-005");
    auto body_with = [](std::string_view coid, std::string_view symbol, std::string_view side,
                         std::string_view type, std::string_view tif, std::string_view price,
                         std::string_view qty) {
        return std::string(R"({"symbol":")") + std::string(symbol) +
               R"(","orderId":1,"clientOrderId":")" + std::string(coid) + R"(",)" + R"("price":")" +
               std::string(price) + R"(","origQty":")" + std::string(qty) +
               R"(","executedQty":"0.000000","cummulativeQuoteQty":"0.00000000",)" +
               R"("status":"NEW","side":")" + std::string(side) + R"(","type":")" +
               std::string(type) + R"(","timeInForce":")" + std::string(tif) + R"("})";
    };
    EXPECT_EQ(parse_order_query_response(
                  body_with("WRONG-COID", "BTCUSDT", "BUY", "LIMIT", "GTC", "50000.12", "0.100000"),
                  expected)
                  .outcome,
              QueryOutcome::Inconclusive)
        << "coid mismatch";
    EXPECT_EQ(parse_order_query_response(
                  body_with("test-coid-005", "ETHUSDT", "BUY", "LIMIT", "GTC", "50000.12",
                            "0.100000"),
                  expected)
                  .outcome,
              QueryOutcome::Inconclusive)
        << "symbol mismatch";
    EXPECT_EQ(parse_order_query_response(
                  body_with("test-coid-005", "BTCUSDT", "SELL", "LIMIT", "GTC", "50000.12",
                            "0.100000"),
                  expected)
                  .outcome,
              QueryOutcome::Inconclusive)
        << "side mismatch";
    EXPECT_EQ(parse_order_query_response(
                  body_with("test-coid-005", "BTCUSDT", "BUY", "MARKET", "GTC", "50000.12",
                            "0.100000"),
                  expected)
                  .outcome,
              QueryOutcome::Inconclusive)
        << "type mismatch";
    EXPECT_EQ(parse_order_query_response(
                  body_with("test-coid-005", "BTCUSDT", "BUY", "LIMIT", "IOC", "50000.12",
                            "0.100000"),
                  expected)
                  .outcome,
              QueryOutcome::Inconclusive)
        << "timeInForce mismatch";
    EXPECT_EQ(parse_order_query_response(
                  body_with("test-coid-005", "BTCUSDT", "BUY", "LIMIT", "GTC", "1.00", "0.100000"),
                  expected)
                  .outcome,
              QueryOutcome::Inconclusive)
        << "price mismatch";
    EXPECT_EQ(parse_order_query_response(
                  body_with("test-coid-005", "BTCUSDT", "BUY", "LIMIT", "GTC", "50000.12",
                            "9.000000"),
                  expected)
                  .outcome,
              QueryOutcome::Inconclusive)
        << "qty mismatch";
}

TEST(ParseOrderQueryResponse, UnrecognizedStatusIsInconclusive) {
    const auto expected = make_btcusdt_expectation("test-coid-006");
    const auto result = parse_order_query_response(
        R"({"symbol":"BTCUSDT","orderId":1,"clientOrderId":"test-coid-006",)"
        R"("price":"50000.12","origQty":"0.100000","executedQty":"0.000000",)"
        R"("cummulativeQuoteQty":"0.00000000","status":"SOME_FUTURE_STATUS",)"
        R"("side":"BUY","type":"LIMIT","timeInForce":"GTC"})",
        expected);
    EXPECT_EQ(result.outcome, QueryOutcome::Inconclusive);
}

// --- parse_submit_order_response() -- TODO 1A.3 / SUBMITPORT spec §4.2 ---

TEST(ParseSubmitOrderResponse, FullSuccessPathAcceptedNoFill) {
    const auto rules = make_btcusdt_rules();
    const auto resp = parse_submit_order_response(
        R"({"symbol":"BTCUSDT","orderId":777,"clientOrderId":"submit-coid-001",)"
        R"("transactTime":1700000000000,"price":"50000.12","origQty":"0.100000",)"
        R"("executedQty":"0.000000","cummulativeQuoteQty":"0.00000000","status":"NEW",)"
        R"("side":"BUY","type":"LIMIT","timeInForce":"GTC"})",
        "submit-coid-001", rules, OrderSide::Buy, OrderType::Limit, 5'000'012, 100'000);
    ASSERT_EQ(resp.outcome, SubmitOutcome::Accepted);
    EXPECT_EQ(resp.exchange_status, OrderState::Accepted);
    EXPECT_EQ(resp.exchange_order_id, 777);
    EXPECT_EQ(resp.filled_qty_ticks, 0);
    EXPECT_EQ(resp.avg_fill_price_ticks, 0);
}

TEST(ParseSubmitOrderResponse, FullSuccessPathImmediatePartialFillDerivesAvgPrice) {
    const auto rules = make_btcusdt_rules();
    const auto resp = parse_submit_order_response(
        R"({"symbol":"BTCUSDT","orderId":778,"clientOrderId":"submit-coid-002",)"
        R"("transactTime":1700000000000,"price":"50000.12","origQty":"0.100000",)"
        R"("executedQty":"0.050000","cummulativeQuoteQty":"2500.00000000",)"
        R"("status":"PARTIALLY_FILLED","side":"BUY","type":"LIMIT","timeInForce":"GTC"})",
        "submit-coid-002", rules, OrderSide::Buy, OrderType::Limit, 5'000'012, 100'000);
    ASSERT_EQ(resp.outcome, SubmitOutcome::Accepted);
    EXPECT_EQ(resp.exchange_status, OrderState::PartialFill);
    EXPECT_EQ(resp.filled_qty_ticks, 50'000);
    EXPECT_EQ(resp.avg_fill_price_ticks, 5'000'000);  // 2500.00000000 / 0.050000 = 50000.00
}

TEST(ParseSubmitOrderResponse, FullSuccessPathImmediateFullFill) {
    const auto rules = make_btcusdt_rules();
    const auto resp = parse_submit_order_response(
        R"({"symbol":"BTCUSDT","orderId":779,"clientOrderId":"submit-coid-003",)"
        R"("transactTime":1700000000000,"price":"50000.12","origQty":"0.100000",)"
        R"("executedQty":"0.100000","cummulativeQuoteQty":"5000.01200000",)"
        R"("status":"FILLED","side":"BUY","type":"LIMIT","timeInForce":"GTC"})",
        "submit-coid-003", rules, OrderSide::Buy, OrderType::Limit, 5'000'012, 100'000);
    ASSERT_EQ(resp.outcome, SubmitOutcome::Accepted);
    EXPECT_EQ(resp.exchange_status, OrderState::Filled);
    EXPECT_EQ(resp.filled_qty_ticks, 100'000);
    EXPECT_EQ(resp.avg_fill_price_ticks, 5'000'012);  // 5000.012 / 0.1 = 50000.12
}

TEST(ParseSubmitOrderResponse, MalformedJsonIsNetworkError) {
    const auto rules = make_btcusdt_rules();
    const auto resp = parse_submit_order_response("not json", "submit-coid-004", rules,
                                                    OrderSide::Buy, OrderType::Limit, 5'000'012,
                                                    100'000);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

TEST(ParseSubmitOrderResponse, EachMissingRequiredFieldIsNetworkError) {
    const auto rules = make_btcusdt_rules();
    const std::string_view kFields[] = {
        "symbol", "orderId", "clientOrderId", "transactTime", "price", "origQty",
        "executedQty", "cummulativeQuoteQty", "status", "timeInForce", "type", "side",
    };
    for (auto field : kFields) {
        std::string body =
            R"({"symbol":"BTCUSDT","orderId":1,"clientOrderId":"submit-coid-005",)"
            R"("transactTime":1700000000000,"price":"50000.12","origQty":"0.100000",)"
            R"("executedQty":"0.000000","cummulativeQuoteQty":"0.00000000","status":"NEW",)"
            R"("side":"BUY","type":"LIMIT","timeInForce":"GTC"})";
        const std::string needle = "\"" + std::string(field) + "\":";
        const auto pos = body.find(needle);
        ASSERT_NE(pos, std::string::npos) << field;
        body.replace(pos, needle.size(), "\"_absent_\":");
        const auto resp = parse_submit_order_response(body, "submit-coid-005", rules,
                                                        OrderSide::Buy, OrderType::Limit,
                                                        5'000'012, 100'000);
        EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError) << "field=" << field;
    }
}

TEST(ParseSubmitOrderResponse, EachFieldMismatchIsNetworkError) {
    const auto rules = make_btcusdt_rules();
    auto body_with = [](std::string_view coid, std::string_view symbol, std::string_view side,
                         std::string_view type, std::string_view tif, std::string_view price,
                         std::string_view qty) {
        return std::string(R"({"symbol":")") + std::string(symbol) +
               R"(","orderId":1,"clientOrderId":")" + std::string(coid) +
               R"(","transactTime":1700000000000,"price":")" + std::string(price) +
               R"(","origQty":")" + std::string(qty) +
               R"(","executedQty":"0.000000","cummulativeQuoteQty":"0.00000000",)" +
               R"("status":"NEW","side":")" + std::string(side) + R"(","type":")" +
               std::string(type) + R"(","timeInForce":")" + std::string(tif) + R"("})";
    };
    const std::pair<std::string, const char*> kCases[] = {
        {body_with("WRONG-COID", "BTCUSDT", "BUY", "LIMIT", "GTC", "50000.12", "0.100000"),
         "coid mismatch"},
        {body_with("submit-coid-006", "ETHUSDT", "BUY", "LIMIT", "GTC", "50000.12", "0.100000"),
         "symbol mismatch"},
        {body_with("submit-coid-006", "BTCUSDT", "SELL", "LIMIT", "GTC", "50000.12", "0.100000"),
         "side mismatch"},
        {body_with("submit-coid-006", "BTCUSDT", "BUY", "MARKET", "GTC", "50000.12", "0.100000"),
         "type mismatch"},
        {body_with("submit-coid-006", "BTCUSDT", "BUY", "LIMIT", "IOC", "50000.12", "0.100000"),
         "timeInForce mismatch"},
        {body_with("submit-coid-006", "BTCUSDT", "BUY", "LIMIT", "GTC", "1.00", "0.100000"),
         "price mismatch"},
        {body_with("submit-coid-006", "BTCUSDT", "BUY", "LIMIT", "GTC", "50000.12", "9.000000"),
         "qty mismatch"},
    };
    for (const auto& [body, label] : kCases) {
        const auto resp = parse_submit_order_response(body, "submit-coid-006", rules,
                                                        OrderSide::Buy, OrderType::Limit,
                                                        5'000'012, 100'000);
        EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError) << label;
    }
}

TEST(ParseSubmitOrderResponse, UnrecognizedStatusIsNetworkError) {
    const auto rules = make_btcusdt_rules();
    const auto resp = parse_submit_order_response(
        R"({"symbol":"BTCUSDT","orderId":1,"clientOrderId":"submit-coid-007",)"
        R"("transactTime":1700000000000,"price":"50000.12","origQty":"0.100000",)"
        R"("executedQty":"0.000000","cummulativeQuoteQty":"0.00000000",)"
        R"("status":"SOME_FUTURE_STATUS","side":"BUY","type":"LIMIT","timeInForce":"GTC"})",
        "submit-coid-007", rules, OrderSide::Buy, OrderType::Limit, 5'000'012, 100'000);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

// --- build_exchange_info_target() -- L4 §5's `symbols` filter query construction ---

TEST(BuildExchangeInfoTarget, EmptyFilterReturnsUnfilteredPath) {
    std::string target;
    ASSERT_TRUE(build_exchange_info_target(std::span<const std::string_view>{}, target));
    EXPECT_EQ(target, "/api/v3/exchangeInfo");
}

TEST(BuildExchangeInfoTarget, SingleSymbolUsesShortFormNoEncoding) {
    std::string target;
    const std::string_view symbols[] = {"BTCUSDT"};
    ASSERT_TRUE(build_exchange_info_target(symbols, target));
    EXPECT_EQ(target, "/api/v3/exchangeInfo?symbol=BTCUSDT");
}

TEST(BuildExchangeInfoTarget, MultipleSymbolsArePercentEncodedExactly) {
    std::string target;
    const std::string_view symbols[] = {"BTCUSDT", "ETHUSDT"};
    ASSERT_TRUE(build_exchange_info_target(symbols, target));
    // Exact byte-for-byte target, not a "contains the encoded form" check -- a single wrong
    // hex digit or a swapped %5B/%5D would still pass a substring check but not this one.
    EXPECT_EQ(target,
              "/api/v3/exchangeInfo?symbols=%5B%22BTCUSDT%22%2C%22ETHUSDT%22%5D");
}

TEST(BuildExchangeInfoTarget, ThreeSymbolsEncodeCommasBetweenEach) {
    std::string target;
    const std::string_view symbols[] = {"BTCUSDT", "ETHUSDT", "BNBUSDT"};
    ASSERT_TRUE(build_exchange_info_target(symbols, target));
    EXPECT_EQ(target,
              "/api/v3/exchangeInfo?symbols=%5B%22BTCUSDT%22%2C%22ETHUSDT%22%2C%22BNBUSDT%22%5D");
}

TEST(BuildExchangeInfoTarget, LowercaseSymbolIsRejected) {
    std::string target;
    const std::string_view symbols[] = {"btcusdt"};
    EXPECT_FALSE(build_exchange_info_target(symbols, target));
}

TEST(BuildExchangeInfoTarget, EmptySymbolStringIsRejected) {
    std::string target;
    const std::string_view symbols[] = {""};
    EXPECT_FALSE(build_exchange_info_target(symbols, target));
}

TEST(BuildExchangeInfoTarget, OverlongSymbolIsRejected) {
    std::string target;
    const std::string_view symbols[] = {"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};  // >= kSymbolNameLen
    EXPECT_FALSE(build_exchange_info_target(symbols, target));
}

TEST(BuildExchangeInfoTarget, SymbolWithInjectionCharactersIsRejected) {
    std::string target;
    const std::string_view symbols[] = {"BTC\"USDT"};
    EXPECT_FALSE(build_exchange_info_target(symbols, target));
}

// --- parse_exchange_info_response() -- L4 §5 schema validation + scale derivation ---

TEST(ParseExchangeInfoResponse, ValidResponseWithMultipleSymbolsAndUnknownFilterSkipped) {
    constexpr std::string_view body = R"({
        "serverTime": 1700000000000,
        "symbols": [
            {
                "symbol": "BTCUSDT",
                "status": "TRADING",
                "quoteAssetPrecision": 8,
                "filters": [
                    {"filterType": "PRICE_FILTER", "minPrice": "0.01000000",
                     "maxPrice": "1000000.00000000", "tickSize": "0.01000000"},
                    {"filterType": "LOT_SIZE", "minQty": "0.00001000",
                     "maxQty": "9000.00000000", "stepSize": "0.00001000"},
                    {"filterType": "MIN_NOTIONAL", "minNotional": "10.00000000"},
                    {"filterType": "MAX_NUM_ORDERS", "maxNumOrders": 200}
                ]
            },
            {
                "symbol": "ETHUSDT",
                "status": "BREAK",
                "quoteAssetPrecision": 8,
                "filters": [
                    {"filterType": "PRICE_FILTER", "minPrice": "0.01000000",
                     "maxPrice": "100000.00000000", "tickSize": "0.01000000"},
                    {"filterType": "LOT_SIZE", "minQty": "0.00010000",
                     "maxQty": "9000.00000000", "stepSize": "0.00010000"},
                    {"filterType": "NOTIONAL", "minNotional": "5.00000000"}
                ]
            }
        ]
    })";

    hy::ParsedExchangeInfo out{};
    ASSERT_EQ(parse_exchange_info_response(body, out), PrivateRestError::None);
    EXPECT_EQ(out.server_time_ms, 1700000000000);
    ASSERT_EQ(out.symbol_count, 2u);

    const auto& btc = out.symbols[0];
    EXPECT_EQ(std::string_view(btc.symbol), "BTCUSDT");
    EXPECT_TRUE(btc.is_trading);
    EXPECT_EQ(btc.quote_scale, 8);
    EXPECT_EQ(btc.price_scale, 2);
    EXPECT_EQ(btc.min_price_ticks, 1);
    EXPECT_EQ(btc.max_price_ticks, 100000000);
    EXPECT_EQ(btc.tick_size_ticks, 1);
    EXPECT_EQ(btc.qty_scale, 5);
    EXPECT_EQ(btc.min_qty_ticks, 1);
    EXPECT_EQ(btc.max_qty_ticks, 900000000);
    EXPECT_EQ(btc.step_size_ticks, 1);
    EXPECT_EQ(btc.min_notional_ticks, 1000000000);
    EXPECT_EQ(btc.rules_version, 0u);  // not this parser's job to assign -- SymbolRegistry's

    const auto& eth = out.symbols[1];
    EXPECT_EQ(std::string_view(eth.symbol), "ETHUSDT");
    EXPECT_FALSE(eth.is_trading);  // "BREAK", not "TRADING"
    EXPECT_EQ(eth.qty_scale, 4);
    EXPECT_EQ(eth.min_qty_ticks, 1);
    EXPECT_EQ(eth.max_qty_ticks, 90000000);
    EXPECT_EQ(eth.min_notional_ticks, 500000000);  // "NOTIONAL", not "MIN_NOTIONAL" -- same field
}

TEST(ParseExchangeInfoResponse, MissingServerTimeRejected) {
    hy::ParsedExchangeInfo out{};
    const auto err = parse_exchange_info_response(R"({"symbols":[]})", out);
    EXPECT_EQ(err, PrivateRestError::MalformedResponse);
}

TEST(ParseExchangeInfoResponse, MissingSymbolsArrayRejected) {
    hy::ParsedExchangeInfo out{};
    const auto err = parse_exchange_info_response(R"({"serverTime":1700000000000})", out);
    EXPECT_EQ(err, PrivateRestError::MalformedResponse);
}

TEST(ParseExchangeInfoResponse, EmptySymbolsArraySucceedsWithZeroCount) {
    hy::ParsedExchangeInfo out{};
    ASSERT_EQ(parse_exchange_info_response(R"({"serverTime":1700000000000,"symbols":[]})", out),
              PrivateRestError::None);
    EXPECT_EQ(out.symbol_count, 0u);
}

TEST(ParseExchangeInfoResponse, MissingFiltersArrayRejected) {
    constexpr std::string_view body = R"({
        "serverTime": 1700000000000,
        "symbols": [{"symbol": "BTCUSDT", "status": "TRADING", "quoteAssetPrecision": 8}]
    })";
    hy::ParsedExchangeInfo out{};
    EXPECT_EQ(parse_exchange_info_response(body, out), PrivateRestError::MalformedResponse);
}

TEST(ParseExchangeInfoResponse, OverlongSymbolNameRejectedNotTruncated) {
    const std::string body =
        R"({"serverTime":1700000000000,"symbols":[{"symbol":")" +
        std::string(30, 'A') +  // >= kSymbolNameLen
        R"(","status":"TRADING","quoteAssetPrecision":8,"filters":[]}]})";
    hy::ParsedExchangeInfo out{};
    EXPECT_EQ(parse_exchange_info_response(body, out), PrivateRestError::MalformedResponse);
}

TEST(ParseExchangeInfoResponse, MalformedPriceFilterDecimalRejectsWholeFetch) {
    constexpr std::string_view body = R"({
        "serverTime": 1700000000000,
        "symbols": [{
            "symbol": "BTCUSDT", "status": "TRADING", "quoteAssetPrecision": 8,
            "filters": [{"filterType": "PRICE_FILTER", "minPrice": "not-a-number",
                         "maxPrice": "1.0", "tickSize": "0.01"}]
        }]
    })";
    hy::ParsedExchangeInfo out{};
    EXPECT_EQ(parse_exchange_info_response(body, out), PrivateRestError::MalformedResponse);
}

TEST(ParseExchangeInfoResponse, MoreThanMaxSymbolsRejectsWholeFetchNotTruncated) {
    std::string body = R"({"serverTime":1700000000000,"symbols":[)";
    for (std::size_t i = 0; i < hy::kMaxSymbols + 1; ++i) {
        if (i > 0) body += ',';
        char sym[16];
        std::snprintf(sym, sizeof(sym), "SYM%zuUSDT", i);
        body += R"({"symbol":")";
        body += sym;
        body += R"(","status":"TRADING","quoteAssetPrecision":8,"filters":[]})";
    }
    body += "]}";

    hy::ParsedExchangeInfo out{};
    out.symbol_count = 999;  // sentinel -- must be left untouched on this failure path
    EXPECT_EQ(parse_exchange_info_response(body, out), PrivateRestError::CapacityExceeded);
    EXPECT_EQ(out.symbol_count, 999u);
}

// --- BinancePrivateRestClient::init() -- §8 trust-store check ---

// binance_tls_trust_store_populated()'s own correctness is tested deterministically here,
// independent of any particular machine's ambient default CA paths (see the diagnostic test
// below for why that ambient state cannot be asserted true/false portably).
TEST(BinanceTlsTrustStorePopulated, TrueAfterLoadingAKnownGoodCertFile) {
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tlsv12_client);
    ctx.load_verify_file(fixture_path("test_leaf_cert_testnet_host.pem"));
    EXPECT_TRUE(hy::binance_tls_trust_store_populated(ctx));
}

TEST(BinanceTlsTrustStorePopulated, FalseWithNoTrustAnchorsLoadedAtAll) {
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tlsv12_client);
    // Deliberately skip configure_binance_ssl_context()/set_default_verify_paths() entirely.
    EXPECT_FALSE(hy::binance_tls_trust_store_populated(ctx));
}

// AUDIT (empirically confirmed on this exact MSVC 19.51 + vcpkg OpenSSL local dev environment,
// 2026-08-29, via a throwaway diagnostic probe before writing this test): after
// set_default_verify_paths(), X509_get_default_cert_file()/_dir() resolve to
// "C:\Program Files\Common Files\SSL\cert.pem" / "...\certs" -- NEITHER of which exists on this
// machine (confirmed directly) -- so binance_tls_trust_store_populated() correctly reports
// false here. This is real production risk for a Windows-hosted deployment of this client
// without an explicitly bundled/loaded CA file -- exactly the platform gap spec §8 calls out
// ("may or may not automatically bridge to the Windows Certificate Store"), not a bug in the
// check. It cannot be asserted true/false as a portable pass/fail condition (a properly
// provisioned machine, or the GCC-14/Ubuntu production target with its usual system CA bundle,
// would legitimately see true) -- recorded here so a run has an unambiguous, actionable signal
// instead of silence either way.
TEST(BinanceTlsTrustStorePopulated, DiagnoseAmbientSystemDefaultPaths) {
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tlsv12_client);
    hy::configure_binance_ssl_context(ctx);
    const bool populated = hy::binance_tls_trust_store_populated(ctx);
    std::fprintf(stderr, "[binance_tls] ambient default trust store populated: %s\n",
                 populated ? "true" : "false");
}

TEST(BinancePrivateRestClientInit, DoesNotThrowAndReturnsABool) {
    // Wiring smoke test only -- the underlying check's own true/false correctness is covered
    // deterministically above; init()'s ambient result is platform-dependent (see
    // DiagnoseAmbientSystemDefaultPaths), so this does not assert which value it returns.
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), nullptr);
    EXPECT_NO_THROW({ (void)client.init(); });
}

// --- BinancePrivateRestClient::sync_clock() -- §3, network-layer ---

TEST_F(BoundCredentialsFixture, SyncClockTlsHandshakeStageTimeout) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(blackhole.port());
    cfg.connect_host_override = "127.0.0.1";
    // No extra_trusted_ca_pem_path / no leaf cert served -- PlainBlackholeAcceptor never
    // completes a TLS handshake at all, so this must fail at that stage regardless of trust.
    EXPECT_EQ(client.sync_clock(cfg), PrivateRestError::TlsHandshake);
}

TEST_F(BoundCredentialsFixture, SyncClockReadStageTimeoutAfterSuccessfulHandshake) {
    hy::test_helpers::TlsBlackholeAcceptor tls_blackhole(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"));
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(tls_blackhole.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";
    // Handshake succeeds (hostname matches, CA is trusted); the fixture never sends an HTTP
    // response, so this proves the read-stage deadline actually fires rather than hanging.
    EXPECT_EQ(client.sync_clock(cfg), PrivateRestError::Read);
}

TEST_F(BoundCredentialsFixture, SyncClockRealSuccessPathPublishesOffset) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(server.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";

    EXPECT_EQ(client.sync_clock(cfg), PrivateRestError::None);

    const ClockOffsetSnapshot snap = client.clock_publisher().load();
    EXPECT_NE(snap.seq, 0u);  // published at least once
    EXPECT_TRUE(is_snapshot_fresh(snap, fetch_clock_pair()));

    const auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target, "/api/v3/time");
    EXPECT_TRUE(reqs[0].api_key_header.empty());  // §3 is unauthenticated -- no signing header
}

// --- BinancePrivateRestClient::fetch_account() -- §4, network-layer ---

TEST_F(BoundCredentialsFixture, FetchAccountFailsClosedWithoutPriorClockSync) {
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    AccountSnapshot out{};
    out.can_trade = true;  // sentinel
    // No sync_clock() call at all -- clock_publisher() has never published anything (seq==0),
    // so is_snapshot_fresh() must reject it and fetch_account() must never attempt to sign or
    // send anything.
    EXPECT_EQ(client.fetch_account(out), PrivateRestError::ClockNotFresh);
    EXPECT_TRUE(out.can_trade);  // untouched
}

TEST_F(BoundCredentialsFixture, FetchAccountRealSuccessPathSignsAndParses) {
    // First, a real sync_clock() round trip so the clock is fresh (matches how a real caller
    // would sequence these two calls -- fetch_account() itself never falls back to
    // uncalibrated local time on a stale clock).
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    hy::test_helpers::TlsResponseAcceptor account_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"canTrade":true,"balances":[{"asset":"BTC","free":"1.5","locked":"0.5"}]})");
    PrivateRestConfig fetch_cfg;
    fetch_cfg.port = std::to_string(account_server.port());
    fetch_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    fetch_cfg.connect_host_override = "127.0.0.1";

    AccountSnapshot out{};
    ASSERT_EQ(client.fetch_account(out, fetch_cfg), PrivateRestError::None);
    EXPECT_TRUE(out.can_trade);
    ASSERT_EQ(out.asset_count, 1u);
    EXPECT_EQ(out.find("BTC")->free_ticks, 150'000'000);
    EXPECT_GT(out.timestamp_ms, 0);

    const auto reqs = account_server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target.substr(0, 16), "/api/v3/account?");
    EXPECT_EQ(reqs[0].api_key_header, kSyntheticApiKey);
    // §2.1's fixed construction: timestamp then signature, always appended last.
    EXPECT_NE(reqs[0].target.find("&timestamp="), std::string::npos);
    EXPECT_NE(reqs[0].target.find("&signature="), std::string::npos);
}

TEST_F(BoundCredentialsFixture, FetchAccountHttpErrorStatusRejected) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    hy::test_helpers::TlsResponseAcceptor error_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 401, R"({"code":-2015,"msg":"denied"})");
    PrivateRestConfig fetch_cfg;
    fetch_cfg.port = std::to_string(error_server.port());
    fetch_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    fetch_cfg.connect_host_override = "127.0.0.1";

    AccountSnapshot out{};
    out.asset_count = 9;  // sentinel
    EXPECT_EQ(client.fetch_account(out, fetch_cfg), PrivateRestError::HttpStatus);
    EXPECT_EQ(out.asset_count, 9u);  // untouched
}

// --- BinancePrivateRestClient::query_order() -- L4 §6, network-layer ---

TEST_F(BoundCredentialsFixture, QueryOrderFailsClosedWithoutPriorClockSync) {
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    const auto expected = make_btcusdt_expectation("coid-no-clock");
    // No sync_clock() call -- must never attempt to sign or send anything.
    EXPECT_EQ(client.query_order(expected).outcome, QueryOutcome::Inconclusive);
}

TEST_F(BoundCredentialsFixture, QueryOrderRealSuccessPathSignsAndParses) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    const auto expected = make_btcusdt_expectation("coid-success-001");
    hy::test_helpers::TlsResponseAcceptor order_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"symbol":"BTCUSDT","orderId":777,"clientOrderId":"coid-success-001",)"
        R"("price":"50000.12","origQty":"0.100000","executedQty":"0.050000",)"
        R"("cummulativeQuoteQty":"2500.00000000","status":"PARTIALLY_FILLED",)"
        R"("side":"BUY","type":"LIMIT","timeInForce":"GTC"})");
    PrivateRestConfig fetch_cfg;
    fetch_cfg.port = std::to_string(order_server.port());
    fetch_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    fetch_cfg.connect_host_override = "127.0.0.1";

    const auto result = client.query_order(expected, fetch_cfg);
    ASSERT_EQ(result.outcome, QueryOutcome::Found);
    EXPECT_EQ(result.confirmed_state, OrderState::PartialFill);
    EXPECT_EQ(result.exchange_order_id, 777);
    EXPECT_EQ(result.filled_qty_ticks, 50'000);
    EXPECT_EQ(result.avg_fill_price_ticks, 5'000'000);

    const auto reqs = order_server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target.substr(0, 14), "/api/v3/order?");
    EXPECT_EQ(reqs[0].api_key_header, kSyntheticApiKey);
    // §2.1 alphabetical order: origClientOrderId, recvWindow, symbol, then timestamp/signature
    // always appended last.
    const auto& target = reqs[0].target;
    const auto pos_coid = target.find("origClientOrderId=");
    const auto pos_recv = target.find("&recvWindow=");
    const auto pos_symbol = target.find("&symbol=");
    const auto pos_ts = target.find("&timestamp=");
    const auto pos_sig = target.find("&signature=");
    ASSERT_NE(pos_coid, std::string::npos);
    ASSERT_NE(pos_recv, std::string::npos);
    ASSERT_NE(pos_symbol, std::string::npos);
    ASSERT_NE(pos_ts, std::string::npos);
    ASSERT_NE(pos_sig, std::string::npos);
    EXPECT_LT(pos_coid, pos_recv);
    EXPECT_LT(pos_recv, pos_symbol);
    EXPECT_LT(pos_symbol, pos_ts);
    EXPECT_LT(pos_ts, pos_sig);
}

// --- query_order() rate-limit gate (Batch H, H1) ---
//
// rate_limiter_ defaults to nullptr on every constructor call in this file that doesn't
// pass one explicitly (all 30+ of them, including every test above this point) -- these
// two tests are the only ones that actually wire a SpotRateLimitTracker in, covering both
// directions of the guard added to query_order().

TEST_F(BoundCredentialsFixture, QueryOrderExhaustedRateLimiterReturnsInconclusiveWithoutNetworkAttempt) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    hy::SpotRateLimitTracker limiter;
    ASSERT_TRUE(limiter.configure(0, 0, 0, 0, 0, 0));  // zero budget in every lane -- exhausted
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds(), &limiter);
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    // No order-endpoint server is even started -- if query_order() attempted a network
    // call despite the exhausted budget, it would have nothing to connect to and this
    // test's own setup would be wrong, not just the assertion below.
    const auto expected = make_btcusdt_expectation("coid-rate-limited");
    EXPECT_EQ(client.query_order(expected).outcome, QueryOutcome::Inconclusive);
}

TEST_F(BoundCredentialsFixture, QueryOrderRateLimiterWithBudgetStillSendsRealRequest) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    hy::SpotRateLimitTracker limiter;
    ASSERT_TRUE(limiter.configure(1000, 0, 1000, 0, 1000, 0));  // ample budget every lane
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds(), &limiter);
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    const auto expected = make_btcusdt_expectation("coid-rate-ok");
    hy::test_helpers::TlsResponseAcceptor order_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"symbol":"BTCUSDT","orderId":777,"clientOrderId":"coid-rate-ok",)"
        R"("price":"50000.12","origQty":"0.100000","executedQty":"0.050000",)"
        R"("cummulativeQuoteQty":"2500.00000000","status":"PARTIALLY_FILLED",)"
        R"("side":"BUY","type":"LIMIT","timeInForce":"GTC"})");
    PrivateRestConfig fetch_cfg;
    fetch_cfg.port = std::to_string(order_server.port());
    fetch_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    fetch_cfg.connect_host_override = "127.0.0.1";

    EXPECT_EQ(client.query_order(expected, fetch_cfg).outcome, QueryOutcome::Found);
    EXPECT_EQ(order_server.requests().size(), 1u);  // budget was available -- real request sent
}

TEST_F(BoundCredentialsFixture, QueryOrderHttpErrorStatusIsInconclusiveNotFailure) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    hy::test_helpers::TlsResponseAcceptor error_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 429,
        R"({"code":-1003,"msg":"Too many requests"})");
    PrivateRestConfig fetch_cfg;
    fetch_cfg.port = std::to_string(error_server.port());
    fetch_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    fetch_cfg.connect_host_override = "127.0.0.1";

    const auto expected = make_btcusdt_expectation("coid-http-error");
    // Never Rejected/"does not exist" -- collapses to the same Inconclusive as every other
    // untrustworthy outcome (L4 §6.1.2's exhaustive list).
    EXPECT_EQ(client.query_order(expected, fetch_cfg).outcome, QueryOutcome::Inconclusive);
}

TEST_F(BoundCredentialsFixture, QueryOrderMalformedJsonIsInconclusive) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    hy::test_helpers::TlsResponseAcceptor bad_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200, "not json");
    PrivateRestConfig fetch_cfg;
    fetch_cfg.port = std::to_string(bad_server.port());
    fetch_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    fetch_cfg.connect_host_override = "127.0.0.1";

    const auto expected = make_btcusdt_expectation("coid-bad-json");
    EXPECT_EQ(client.query_order(expected, fetch_cfg).outcome, QueryOutcome::Inconclusive);
}

TEST(QueryOrderEntryGuards, NullCredentialsFoldsIntoInconclusiveWithoutNetworkAttempt) {
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), nullptr);
    const auto expected = make_btcusdt_expectation("coid-null-creds");
    EXPECT_EQ(client.query_order(expected).outcome, QueryOutcome::Inconclusive);
}

// --- query_order_adapter() -- production QueryPort::QueryFn wiring ---

TEST(QueryOrderAdapter, NullUserDataFoldsIntoInconclusive) {
    OrderExpectation expected{};
    EXPECT_EQ(query_order_adapter(expected, nullptr).outcome, QueryOutcome::Inconclusive);
}

// --- BinancePrivateRestClient::submit_order() -- TODO 1A.3, network-layer ---

TEST_F(BoundCredentialsFixture, SubmitOrderFailsClosedWithoutPriorClockSync) {
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    const auto rules = make_btcusdt_rules();
    // No sync_clock() call -- must never attempt to sign or send anything.
    const auto resp = client.submit_order("coid-no-clock", 1, OrderSide::Buy, OrderType::Limit,
                                           5'000'012, 100'000, rules);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

TEST_F(BoundCredentialsFixture, SubmitOrderRealSuccessPathSignsAndParses) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    const auto rules = make_btcusdt_rules();
    hy::test_helpers::TlsResponseAcceptor order_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"symbol":"BTCUSDT","orderId":888,"clientOrderId":"submit-net-001",)"
        R"("transactTime":1700000000000,"price":"50000.12","origQty":"0.100000",)"
        R"("executedQty":"0.050000","cummulativeQuoteQty":"2500.00000000",)"
        R"("status":"PARTIALLY_FILLED","side":"BUY","type":"LIMIT","timeInForce":"GTC"})");
    PrivateRestConfig fetch_cfg;
    fetch_cfg.port = std::to_string(order_server.port());
    fetch_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    fetch_cfg.connect_host_override = "127.0.0.1";

    const auto resp = client.submit_order("submit-net-001", 1, OrderSide::Buy, OrderType::Limit,
                                           5'000'012, 100'000, rules, fetch_cfg);
    ASSERT_EQ(resp.outcome, SubmitOutcome::Accepted);
    EXPECT_EQ(resp.exchange_status, OrderState::PartialFill);
    EXPECT_EQ(resp.exchange_order_id, 888);
    EXPECT_EQ(resp.filled_qty_ticks, 50'000);
    EXPECT_EQ(resp.avg_fill_price_ticks, 5'000'000);

    const auto reqs = order_server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target.substr(0, 14), "/api/v3/order?");
    EXPECT_EQ(reqs[0].api_key_header, kSyntheticApiKey);
    // §2.1's fixed construction: newClientOrderId/price/quantity/etc all present, timestamp
    // then signature always appended last, same shape query_order()'s own request-shape test
    // already pins for the GET side.
    EXPECT_NE(reqs[0].target.find("newClientOrderId=submit-net-001"), std::string::npos);
    EXPECT_NE(reqs[0].target.find("&timestamp="), std::string::npos);
    EXPECT_NE(reqs[0].target.find("&signature="), std::string::npos);
}

TEST_F(BoundCredentialsFixture, SubmitOrderHttpErrorStatusIsNetworkErrorNotRejected) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    hy::test_helpers::TlsResponseAcceptor error_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 400,
        R"({"code":-2010,"msg":"Account has insufficient balance"})");
    PrivateRestConfig fetch_cfg;
    fetch_cfg.port = std::to_string(error_server.port());
    fetch_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    fetch_cfg.connect_host_override = "127.0.0.1";

    const auto rules = make_btcusdt_rules();
    // Never Rejected directly from a POST error status this batch (no §4.4.1 400-allowlist) --
    // collapses to the same NetworkError every other untrustworthy outcome does; the
    // orchestrator's existing Ambiguous->reconcile path resolves it correctly via the
    // already-real GET /api/v3/order query_order().
    const auto resp = client.submit_order("coid-http-error", 1, OrderSide::Buy, OrderType::Limit,
                                           5'000'012, 100'000, rules, fetch_cfg);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

TEST_F(BoundCredentialsFixture, SubmitOrderMalformedJsonIsNetworkError) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    sync_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    sync_cfg.connect_host_override = "127.0.0.1";
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    hy::test_helpers::TlsResponseAcceptor bad_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200, "not json");
    PrivateRestConfig fetch_cfg;
    fetch_cfg.port = std::to_string(bad_server.port());
    fetch_cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    fetch_cfg.connect_host_override = "127.0.0.1";

    const auto rules = make_btcusdt_rules();
    const auto resp = client.submit_order("coid-bad-json", 1, OrderSide::Buy, OrderType::Limit,
                                           5'000'012, 100'000, rules, fetch_cfg);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

TEST(SubmitOrderEntryGuards, NullCredentialsFoldsIntoNetworkErrorWithoutNetworkAttempt) {
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), nullptr);
    const auto rules = make_btcusdt_rules();
    const auto resp = client.submit_order("coid-null-creds", 1, OrderSide::Buy, OrderType::Limit,
                                           5'000'012, 100'000, rules);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

TEST(SubmitOrderEntryGuards, NonPositivePriceOrQtyRejected) {
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), nullptr);
    const auto rules = make_btcusdt_rules();
    EXPECT_EQ(client.submit_order("coid-a", 1, OrderSide::Buy, OrderType::Limit, 0, 100'000, rules)
                  .outcome,
              SubmitOutcome::NetworkError);
    EXPECT_EQ(client.submit_order("coid-b", 1, OrderSide::Buy, OrderType::Limit, 5'000'012, 0, rules)
                  .outcome,
              SubmitOutcome::NetworkError);
}

// --- submit_order_adapter() -- production SubmitPort::SubmitFn wiring ---

TEST(SubmitOrderAdapter, NullUserDataFoldsIntoNetworkError) {
    const auto rules = make_btcusdt_rules();
    const auto resp = submit_order_adapter("coid", 1, OrderSide::Buy, OrderType::Limit, 5'000'012,
                                            100'000, rules, nullptr);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

TEST(SubmitOrderAdapter, NullClientOrderIdFoldsIntoNetworkError) {
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), nullptr);
    const auto rules = make_btcusdt_rules();
    const auto resp = submit_order_adapter(nullptr, 1, OrderSide::Buy, OrderType::Limit,
                                            5'000'012, 100'000, rules, &client);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

// --- BinancePrivateRestClient::fetch_exchange_info() -- §5, network-layer ---

TEST_F(BoundCredentialsFixture, FetchExchangeInfoRealSuccessPathParsesAndUsesNoApiKey) {
    constexpr std::string_view body = R"({
        "serverTime": 1700000000000,
        "symbols": [
            {
                "symbol": "BTCUSDT",
                "status": "TRADING",
                "quoteAssetPrecision": 8,
                "filters": [
                    {"filterType": "PRICE_FILTER", "minPrice": "0.01000000",
                     "maxPrice": "1000000.00000000", "tickSize": "0.01000000"},
                    {"filterType": "LOT_SIZE", "minQty": "0.00001000",
                     "maxQty": "9000.00000000", "stepSize": "0.00001000"},
                    {"filterType": "MIN_NOTIONAL", "minNotional": "10.00000000"},
                    {"filterType": "MAX_NUM_ORDERS", "maxNumOrders": 200}
                ]
            },
            {
                "symbol": "ETHUSDT",
                "status": "TRADING",
                "quoteAssetPrecision": 8,
                "filters": [
                    {"filterType": "PRICE_FILTER", "minPrice": "0.01000000",
                     "maxPrice": "100000.00000000", "tickSize": "0.01000000"},
                    {"filterType": "LOT_SIZE", "minQty": "0.00010000",
                     "maxQty": "9000.00000000", "stepSize": "0.00010000"},
                    {"filterType": "NOTIONAL", "minNotional": "5.00000000"}
                ]
            }
        ]
    })";
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200, std::string(body));
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(server.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";

    hy::ParsedExchangeInfo out{};
    const std::string_view symbols[] = {"BTCUSDT", "ETHUSDT"};
    ASSERT_EQ(client.fetch_exchange_info(out, symbols, cfg), PrivateRestError::None);
    EXPECT_EQ(out.server_time_ms, 1700000000000);
    ASSERT_EQ(out.symbol_count, 2u);
    EXPECT_EQ(std::string_view(out.symbols[0].symbol), "BTCUSDT");
    EXPECT_EQ(out.symbols[0].min_notional_ticks, 1000000000);
    EXPECT_EQ(std::string_view(out.symbols[1].symbol), "ETHUSDT");
    EXPECT_EQ(out.symbols[1].min_notional_ticks, 500000000);

    const auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target,
              "/api/v3/exchangeInfo?symbols=%5B%22BTCUSDT%22%2C%22ETHUSDT%22%5D");
    EXPECT_TRUE(reqs[0].api_key_header.empty());  // §5 is unauthenticated -- no signing header,
                                                   // same posture as §3's /time
}

TEST_F(BoundCredentialsFixture, FetchExchangeInfoSingleSymbolUsesShortFormTarget) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000,"symbols":[]})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(server.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";

    hy::ParsedExchangeInfo out{};
    const std::string_view symbols[] = {"BTCUSDT"};
    ASSERT_EQ(client.fetch_exchange_info(out, symbols, cfg), PrivateRestError::None);

    const auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target, "/api/v3/exchangeInfo?symbol=BTCUSDT");
}

TEST_F(BoundCredentialsFixture, FetchExchangeInfoInvalidSymbolFailsClosedWithoutNetworkAttempt) {
    // No TlsResponseAcceptor at all -- build_exchange_info_target() must reject this before any
    // connection is even attempted.
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig cfg;
    cfg.connect_host_override = "127.0.0.1";

    hy::ParsedExchangeInfo out{};
    out.symbol_count = 7;  // sentinel
    const std::string_view symbols[] = {"not-a-valid-symbol"};
    EXPECT_EQ(client.fetch_exchange_info(out, symbols, cfg), PrivateRestError::InvalidConfig);
    EXPECT_EQ(out.symbol_count, 7u);  // untouched
}

TEST_F(BoundCredentialsFixture, FetchExchangeInfoHttpErrorStatusRejected) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 503, R"({"msg":"unavailable"})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(server.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";

    hy::ParsedExchangeInfo out{};
    out.symbol_count = 7;  // sentinel
    const std::string_view symbols[] = {"BTCUSDT"};
    EXPECT_EQ(client.fetch_exchange_info(out, symbols, cfg), PrivateRestError::HttpStatus);
    EXPECT_EQ(out.symbol_count, 7u);  // untouched
}

// --- parse_listen_key_response() -- TODO 1A.4 ---

TEST(ParseListenKeyResponse, ValidResponseExtractsKey) {
    char buf[kListenKeyLen]{};
    std::size_t len = 0;
    ASSERT_TRUE(parse_listen_key_response(R"({"listenKey":"abc123def456"})", buf, len));
    EXPECT_EQ(std::string_view(buf, len), "abc123def456");
}

TEST(ParseListenKeyResponse, MissingListenKeyFieldRejected) {
    char buf[kListenKeyLen]{};
    std::size_t len = 0;
    EXPECT_FALSE(parse_listen_key_response(R"({"somethingElse":"x"})", buf, len));
    EXPECT_EQ(len, 0u);
}

TEST(ParseListenKeyResponse, MalformedJsonRejected) {
    char buf[kListenKeyLen]{};
    std::size_t len = 0;
    EXPECT_FALSE(parse_listen_key_response("not json", buf, len));
}

TEST(ParseListenKeyResponse, EmptyKeyRejected) {
    char buf[kListenKeyLen]{};
    std::size_t len = 0;
    EXPECT_FALSE(parse_listen_key_response(R"({"listenKey":""})", buf, len));
    EXPECT_EQ(len, 0u);
}

TEST(ParseListenKeyResponse, KeyTooLargeForBufferRejectedNotTruncated) {
    char buf[8]{};  // deliberately tiny -- key won't fit
    std::size_t len = 0;
    EXPECT_FALSE(parse_listen_key_response(R"({"listenKey":"way-too-long-for-this-buffer"})", buf,
                                            len));
    EXPECT_EQ(len, 0u);
}

// --- BinancePrivateRestClient::create_listen_key()/keepalive_listen_key()/close_listen_key()
// -- TODO 1A.4, network-layer ---

TEST_F(BoundCredentialsFixture, CreateListenKeySucceedsWithoutPriorClockSync) {
    // No sync_clock() call anywhere in this test -- these three endpoints are USER_STREAM-type
    // (API-key header only, no HMAC signature/timestamp), so they must not depend on a fresh
    // clock-offset snapshot the way fetch_account()/query_order()/submit_order() do.
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"listenKey":"pqia91ma19a5s61cv6a81va65sdf19v8a65a1a5s61cv6a81va65sdf19v8a65a1"})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(server.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";

    char buf[kListenKeyLen]{};
    std::size_t len = 0;
    ASSERT_EQ(client.create_listen_key(buf, len, cfg), PrivateRestError::None);
    EXPECT_EQ(std::string_view(buf, len),
              "pqia91ma19a5s61cv6a81va65sdf19v8a65a1a5s61cv6a81va65sdf19v8a65a1");

    const auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target, "/api/v3/userDataStream");
    EXPECT_EQ(reqs[0].api_key_header, kSyntheticApiKey);
    // Never a signed query -- no timestamp/signature params on this endpoint.
    EXPECT_EQ(reqs[0].target.find("signature="), std::string::npos);
    EXPECT_EQ(reqs[0].target.find("timestamp="), std::string::npos);
}

TEST_F(BoundCredentialsFixture, CreateListenKeyHttpErrorIsHttpStatus) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 429,
        R"({"code":-1003,"msg":"Too many requests"})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(server.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";

    char buf[kListenKeyLen]{};
    std::size_t len = 7;  // sentinel
    EXPECT_EQ(client.create_listen_key(buf, len, cfg), PrivateRestError::HttpStatus);
    EXPECT_EQ(len, 7u);  // untouched on failure
}

TEST_F(BoundCredentialsFixture, CreateListenKeyMalformedJsonIsMalformedResponse) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200, "not json");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(server.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";

    char buf[kListenKeyLen]{};
    std::size_t len = 0;
    EXPECT_EQ(client.create_listen_key(buf, len, cfg), PrivateRestError::MalformedResponse);
}

TEST_F(BoundCredentialsFixture, KeepaliveListenKeySucceedsAndPercentEncodesTheKeyInTheQuery) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200, R"({})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(server.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";

    EXPECT_EQ(client.keepalive_listen_key("abc123def456", cfg), PrivateRestError::None);

    const auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target, "/api/v3/userDataStream?listenKey=abc123def456");
}

TEST_F(BoundCredentialsFixture, CloseListenKeySucceeds) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200, R"({})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());

    PrivateRestConfig cfg;
    cfg.port = std::to_string(server.port());
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";

    EXPECT_EQ(client.close_listen_key("abc123def456", cfg), PrivateRestError::None);

    const auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target, "/api/v3/userDataStream?listenKey=abc123def456");
}

TEST(ListenKeyEntryGuards, NullCredentialsFoldsIntoSigningFailedWithoutNetworkAttempt) {
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), nullptr);
    char buf[kListenKeyLen]{};
    std::size_t len = 0;
    EXPECT_EQ(client.create_listen_key(buf, len), PrivateRestError::SigningFailed);
    EXPECT_EQ(client.keepalive_listen_key("some-key"), PrivateRestError::SigningFailed);
    EXPECT_EQ(client.close_listen_key("some-key"), PrivateRestError::SigningFailed);
}

TEST_F(BoundCredentialsFixture, KeepaliveListenKeyEmptyKeyIsInvalidConfigWithoutNetworkAttempt) {
    // No TlsResponseAcceptor at all -- an empty listenKey must be rejected before any
    // connection is even attempted.
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    PrivateRestConfig cfg;
    cfg.connect_host_override = "127.0.0.1";
    EXPECT_EQ(client.keepalive_listen_key("", cfg), PrivateRestError::InvalidConfig);
    EXPECT_EQ(client.close_listen_key("", cfg), PrivateRestError::InvalidConfig);
}
