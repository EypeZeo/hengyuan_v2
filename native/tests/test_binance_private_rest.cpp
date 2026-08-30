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
using hy::ClockOffsetSnapshot;
using hy::EnvAllowlist;
using hy::EnvironmentBinding;
using hy::PrivateRestConfig;
using hy::PrivateRestError;
using hy::QuerySigningError;
using hy::SecureEnvLoader;
using hy::build_exchange_info_target;
using hy::fetch_clock_pair;
using hy::is_snapshot_fresh;
using hy::kBalanceScale;
using hy::parse_account_response;
using hy::parse_balance_decimal_to_ticks;
using hy::parse_exchange_info_response;
using hy::parse_server_time_response;

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
