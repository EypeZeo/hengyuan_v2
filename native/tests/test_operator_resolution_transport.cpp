// SPDX-License-Identifier: proprietary
// test_operator_resolution_transport.cpp -- D5 slice R3b-1: the offline operator tool's real exchange port (operator_resolution_transport.hpp)
// over the authenticated REST client, against a local TLS "exchange" that answers each request as a test says.
//
// Three layers:
//   1. summarize_account() on its own: the uid the flow cross-checks, and the ONE line of printable ASCII the operator is shown -- composed from
//      parsed fields only, whatever the exchange (or whoever sits in front of it) puts in the answer;
//   2. BinanceExchangeReadPort: the five reads of the port contract, the answer handed back whatever its status, a request that did not complete
//      being a failure and never an empty 200, the fixed gap between two requests (slept through the injected SleepPort), the rate-limit latch
//      (429/418/403 stop the port for good), the single clock refresh;
//   3. the whole procedure -- begin_operator_session(), resolve_order() over REAL audit and control-plane logs -- with the real port speaking
//      real TLS to the local exchange: the one thing replaced is the fence clock (a decorator answers serverTime from a fake world clock the
//      sleeper advances: a 270 s fence cannot be waited out, and the real client refuses to calibrate against an exchange clock that far from
//      the local one).
//
// Synthetic credentials only (the signing example key pair Binance's own docs print); no real network access.

#include <gtest/gtest.h>
#include <hengyuan/operator_resolution_flow.hpp>
#include <hengyuan/operator_resolution_transport.hpp>

#include "binance_private_rest_test_hooks.hpp"
#include "operator_resolution_fakes.hpp"
#include "test_helpers/bound_test_credentials.hpp"
#include "test_helpers/server_time_body.hpp"
#include "test_helpers/tls_routing_acceptor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace hy;
using namespace hy::operator_resolution;
using namespace hy::operator_resolution::testing;
using hy::test_helpers::BoundTestCredentials;
using hy::test_helpers::TlsRoutingAcceptor;
using Reply = TlsRoutingAcceptor::Reply;
using Request = TlsRoutingAcceptor::Request;

namespace {

std::string fixture_path(const char* filename) { return std::string(HY_TEST_FIXTURE_DIR) + "/" + filename; }

std::int64_t real_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

constexpr std::string_view kCoid = "HY-1790000000000-7-0A1B";
constexpr std::int64_t kStart = 1'789'999'700'000;
constexpr std::int64_t kEnd = 1'790'000'270'000;

std::string path_of(const Request& r) { return r.target.substr(0, r.target.find('?')); }

Reply time_reply() { return Reply::answer(200, hy::test_helpers::current_server_time_body()); }

// Binance's documented sample answer of GET /api/v3/account.
constexpr std::string_view kAccountBody =
    R"({"makerCommission":15,"takerCommission":15,"buyerCommission":0,"sellerCommission":0,)"
    R"("commissionRates":{"maker":"0.00150000","taker":"0.00150000","buyer":"0.00000000","seller":"0.00000000"},)"
    R"("canTrade":true,"canWithdraw":true,"canDeposit":true,"brokered":false,"requireSelfTradePrevention":false,)"
    R"("preventSor":false,"updateTime":123456789,"accountType":"SPOT","balances":[)"
    R"({"asset":"BTC","free":"4723846.89208129","locked":"0.00000000"},)"
    R"({"asset":"LTC","free":"4763368.68006011","locked":"0.00000000"}],"permissions":["SPOT"],"uid":354937868})";

std::string account_with(std::string_view uid_member, std::string_view rest = R"("accountType":"SPOT","permissions":["SPOT"],"balances":[])") {
    std::string s = "{";
    if (!uid_member.empty()) {
        s += uid_member;
        s += ",";
    }
    s += rest;
    s += "}";
    return s;
}

bool printable_ascii_line(std::string_view s) {
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

}  // namespace

// ============================================================ 1. summarize_account ====================================================================

TEST(SummarizeAccount, TheDocumentedAnswerGivesItsUidAndAShortLine) {
    const AccountView v = summarize_account(kAccountBody);
    ASSERT_TRUE(v.uid.has_value());
    EXPECT_EQ(*v.uid, 354937868u);
    EXPECT_EQ(v.summary,
              "account type SPOT; permissions SPOT; balances (free/locked): BTC 4723846.89208129/0.00000000, LTC 4763368.68006011/0.00000000");
    EXPECT_TRUE(printable_ascii_line(v.summary));
}

TEST(SummarizeAccount, TheUidIsExactlyOnePositiveIntegerOrNothing) {
    struct Row {
        const char* member;
        std::optional<std::uint64_t> want;
    };
    const std::vector<Row> rows = {
        {R"("uid":354937868)", 354937868u},
        {R"("uid":1)", 1u},
        {R"("uid":18446744073709551615)", std::numeric_limits<std::uint64_t>::max()},
        {R"("uid":9007199254740993)", 9007199254740993u},  // above 2^53: still exact (no round trip through a double)
        {R"("uid":0)", std::nullopt},
        {R"("uid":-5)", std::nullopt},
        {R"("uid":1.5)", std::nullopt},
        {R"("uid":354937868.0)", std::nullopt},
        {R"("uid":3.54937868e8)", std::nullopt},
        {R"("uid":"354937868")", std::nullopt},
        {R"("uid":null)", std::nullopt},
        {R"("uid":true)", std::nullopt},
        {R"("uid":[354937868])", std::nullopt},
        {R"("uid":{"value":354937868})", std::nullopt},
        {R"("uid":354937868,"uid":354937868)", std::nullopt},  // twice, even if equal: which one is meant?
        {R"("uid":354937868,"uid":5)", std::nullopt},
        {R"("uid":18446744073709551616)", std::nullopt},  // 2^64
        {R"("UID":354937868)", std::nullopt},              // the key is exactly "uid"
        {"", std::nullopt},
    };
    for (const Row& row : rows) {
        const AccountView v = summarize_account(account_with(row.member));
        EXPECT_EQ(v.uid, row.want) << row.member;
    }
}

TEST(SummarizeAccount, ADuplicateKeyIsNotResolvedByPickingOne) {
    // even when the other member comes first, or is nested somewhere else
    EXPECT_FALSE(summarize_account(R"({"uid":5,"accountType":"SPOT","uid":5})").uid.has_value());
    EXPECT_EQ(summarize_account(R"({"commissionRates":{"uid":5},"accountType":"SPOT","uid":7})").uid, std::optional<std::uint64_t>(7u))
        << "a member of the same name inside another object is not this member";
}

TEST(SummarizeAccount, AnythingButOneJsonObjectIsEmpty) {
    const std::vector<std::string> bodies = {
        "",
        " ",
        "null",
        "5",
        "\"uid\"",
        "[]",
        R"([{"uid":5}])",
        "{",
        R"({"uid":5)",
        R"({"uid":5} trailing)",
        R"({"uid":5}{"uid":5})",
        std::string("\xEF\xBB\xBF") + R"({"uid":5})",  // a byte order mark: not what the exchange sends
        "<html><body>502 Bad Gateway</body></html>",
        std::string("{\"uid\":5,\"x\":\"\xC3\x28\"}"),  // invalid UTF-8 inside the document
    };
    for (const std::string& body : bodies) {
        const AccountView v = summarize_account(body);
        EXPECT_FALSE(v.uid.has_value()) << body;
        EXPECT_TRUE(v.summary.empty()) << body;
    }
}

TEST(SummarizeAccount, AnOversizedBodyIsEmpty) {
    const std::string big = R"({"uid":5,"pad":")" + std::string(kMaxEvidenceBodyBytes, 'x') + R"("})";
    const AccountView v = summarize_account(big);
    EXPECT_FALSE(v.uid.has_value());
    EXPECT_TRUE(v.summary.empty());
}

TEST(SummarizeAccount, AMissingOrMalformedPartIsAQuestionMarkAndNeverStopsTheOthers) {
    EXPECT_EQ(summarize_account(R"({"uid":5})").summary, "account type ?; permissions ?; balances (free/locked): ?");
    EXPECT_EQ(summarize_account(R"({"uid":5,"accountType":5,"permissions":"SPOT","balances":{}})").summary,
              "account type ?; permissions ?; balances (free/locked): ?");
    EXPECT_EQ(summarize_account(R"({"uid":5,"accountType":"SPOT","permissions":[],"balances":[]})").summary,
              "account type SPOT; permissions none; balances (free/locked): none");
    // two members of one name: that part is unknown, the rest is unaffected
    EXPECT_EQ(summarize_account(R"({"uid":5,"accountType":"A","accountType":"B","permissions":["SPOT"],"balances":[]})").summary,
              "account type ?; permissions SPOT; balances (free/locked): none");
    EXPECT_EQ(summarize_account(R"({"uid":5,"accountType":"SPOT","permissions":["A"],"permissions":["B"],"balances":[]})").summary,
              "account type SPOT; permissions ?; balances (free/locked): none");
    EXPECT_EQ(summarize_account(R"({"uid":5,"accountType":"SPOT","permissions":["SPOT"],"balances":[],"balances":[]})").summary,
              "account type SPOT; permissions SPOT; balances (free/locked): ?");
    // a permission that is not a string, and a list in which it sits among others
    EXPECT_EQ(summarize_account(R"({"uid":5,"accountType":"SPOT","permissions":[5,"SPOT",null],"balances":[]})").summary,
              "account type SPOT; permissions ?,SPOT,?; balances (free/locked): none");
    // the uid survives a summary that is mostly missing
    EXPECT_EQ(summarize_account(R"({"uid":5})").uid, std::optional<std::uint64_t>(5u));
}

TEST(SummarizeAccount, PermissionsAndBalancesAreListedUpToEightAndTheRestIsCounted) {
    std::string perms = "[";
    std::string balances = "[";
    for (int i = 0; i < 11; ++i) {
        if (i != 0) {
            perms += ",";
            balances += ",";
        }
        perms += "\"P" + std::to_string(i) + "\"";
        balances += "{\"asset\":\"A" + std::to_string(i) + "\",\"free\":\"" + std::to_string(i) + ".5\",\"locked\":\"0\"}";
    }
    perms += "]";
    balances += "]";
    const AccountView v = summarize_account(R"({"uid":5,"accountType":"SPOT","permissions":)" + perms + R"(,"balances":)" + balances + "}");
    EXPECT_EQ(v.summary,
              "account type SPOT; permissions P0,P1,P2,P3,P4,P5,P6,P7 and 3 more; balances (free/locked): "
              "A0 0.5/0, A1 1.5/0, A2 2.5/0, A3 3.5/0, A4 4.5/0, A5 5.5/0, A6 6.5/0, A7 7.5/0 and 3 more");

    // exactly eight is not "and 0 more"
    const AccountView eight = summarize_account(
        R"({"uid":5,"accountType":"SPOT","permissions":["A","B","C","D","E","F","G","H"],"balances":[]})");
    EXPECT_EQ(eight.summary, "account type SPOT; permissions A,B,C,D,E,F,G,H; balances (free/locked): none");
}

TEST(SummarizeAccount, AnEntryThatIsNotABalanceIsAQuestionMarkInItsPlace) {
    const AccountView v = summarize_account(
        R"({"uid":5,"accountType":"SPOT","permissions":["SPOT"],"balances":[)"
        R"({"asset":"BTC","free":"1","locked":"2"},5,{"asset":"ETH","free":"3"},{"asset":"X","free":"1","locked":"2","asset":"Y"},)"
        R"({"asset":"LTC","free":1,"locked":"2"},{"asset":"BNB","free":"4","locked":"5"}]})");
    EXPECT_EQ(v.summary, "account type SPOT; permissions SPOT; balances (free/locked): BTC 1/2, ?, ?, ?, ?, BNB 4/5");
}

TEST(SummarizeAccount, WhatIsShownIsCutAndFilteredAndNeverARawPieceOfTheAnswer) {
    // names: letters, digits, '.', '_', '-' stay, every other byte of the (UTF-8 decoded) string becomes '?', at most 16 of them
    EXPECT_EQ(summarize_account(R"({"uid":5,"accountType":"SPOT AND MORE THAN 16 CHARACTERS LONG","permissions":[],"balances":[]})").summary,
              "account type SPOT?AND?MORE?TH; permissions none; balances (free/locked): none");
    EXPECT_EQ(summarize_account(R"({"uid":5,"accountType":"a.b_c-D9","permissions":["x/y"],"balances":[]})").summary,
              "account type a.b_c-D9; permissions x?y; balances (free/locked): none");
    EXPECT_EQ(summarize_account(R"({"uid":5,"accountType":"\u001b[31mRED\u0007\n","permissions":[],"balances":[]})").summary,
              "account type ??31mRED??; permissions none; balances (free/locked): none");
    // an amount: digits with at most one inner point, at most 24 characters; anything else is one '?'
    const AccountView v = summarize_account(
        R"({"uid":5,"accountType":"SPOT","permissions":[],"balances":[)"
        R"({"asset":"A","free":"123456789012345678901234","locked":"1234567890123456789012345"},)"
        R"({"asset":"B","free":".5","locked":"5."},{"asset":"C","free":"1.2.3","locked":"-1"},)"
        R"({"asset":"D","free":"1e5","locked":""},{"asset":"E","free":"0x10","locked":"1 2"},)"
        R"({"asset":"F","free":"\u001b[2J","locked":"0.00000000"}]})");
    EXPECT_EQ(v.summary,
              "account type SPOT; permissions none; balances (free/locked): A 123456789012345678901234/?, B ?/?, C ?/?, D ?/?, E ?/?, F ?/0.00000000");
}

TEST(SummarizeAccount, AccountTypeAssetAndPermissionNamesAreCutAtSixteenCharactersAndAmountsAtTwentyFour) {
    const AccountView v = summarize_account(
        R"({"uid":5,"accountType":"ABCDEFGHIJKLMNOPQRSTUVWXYZ","permissions":["abcdefghijklmnopqrstuvwxyz"],"balances":[)"
        R"({"asset":"0123456789ABCDEFGHIJ","free":"1","locked":"2"}]})");
    EXPECT_EQ(v.summary, "account type ABCDEFGHIJKLMNOP; permissions abcdefghijklmnop; balances (free/locked): 0123456789ABCDEF 1/2");
}

TEST(SummarizeAccount, EveryCharacterTheAnswerCanCarryComesOutAsPrintableAscii) {
    // each code point 0..255 as an asset name, a permission, an account type and an amount: the line is always printable ASCII, one line
    for (int cp = 0; cp < 256; ++cp) {
        std::string s;  // the code point, JSON-escaped
        char esc[8];
        std::snprintf(esc, sizeof(esc), "\\u%04x", cp);
        s = esc;
        const std::string body = R"({"uid":5,"accountType":")" + s + R"(","permissions":[")" + s + R"("],"balances":[{"asset":")" + s +
                                 R"(","free":")" + s + R"(","locked":")" + s + R"("}]})";
        const AccountView v = summarize_account(body);
        ASSERT_FALSE(v.summary.empty()) << cp;
        EXPECT_TRUE(printable_ascii_line(v.summary)) << cp << ": " << v.summary;
        EXPECT_LE(v.summary.size(), 640u);
    }
    // and the whole line is capped even when every part is at its largest
    std::string balances = "[";
    for (int i = 0; i < 8; ++i) {
        if (i != 0) balances += ",";
        balances += R"({"asset":"AAAAAAAAAAAAAAAAAAAA","free":"123456789012345678901234","locked":"123456789012345678901234"})";
    }
    balances += "]";
    const AccountView big = summarize_account(
        R"({"uid":5,"accountType":"TTTTTTTTTTTTTTTTTTTT","permissions":["PPPPPPPPPPPPPPPPPPPP","PPPPPPPPPPPPPPPPPPPP","PPPPPPPPPPPPPPPPPPPP",)"
        R"("PPPPPPPPPPPPPPPPPPPP","PPPPPPPPPPPPPPPPPPPP","PPPPPPPPPPPPPPPPPPPP","PPPPPPPPPPPPPPPPPPPP","PPPPPPPPPPPPPPPPPPPP"],"balances":)" +
        balances + "}");
    EXPECT_EQ(big.summary.size(), 640u) << "cut at the cap: the parts alone would be longer";
    EXPECT_TRUE(printable_ascii_line(big.summary));
}

TEST(RealSleeper, SleepsForAtLeastTheTimeAskedAndNotAtAllForANonPositiveOne) {
    RealSleeper sleeper;
    const auto elapsed_ms = [](auto since) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
    };
    const auto t0 = std::chrono::steady_clock::now();
    sleeper.sleep_for(std::chrono::milliseconds{80});
    EXPECT_GE(elapsed_ms(t0), 75);
    EXPECT_LT(elapsed_ms(t0), 3000);
    const auto t1 = std::chrono::steady_clock::now();
    sleeper.sleep_for(std::chrono::milliseconds{0});
    sleeper.sleep_for(std::chrono::milliseconds{-5000});
    EXPECT_LT(elapsed_ms(t1), 1000) << "a non-positive wait returns at once";
}

// ============================================================ 2. the port ============================================================================

namespace {

class TransportFixture : public ::testing::Test {
protected:
    std::string env_path_;
    std::unique_ptr<TlsRoutingAcceptor> server_;
    std::unique_ptr<BinancePrivateRestClient> client_;
    hy::SpotRateLimitTracker tracker_;
    PrivateRestConfig cfg_;

    void SetUp() override {
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        env_path_ = std::string(tmp) + "hy_test_optransport_" + std::to_string(GetCurrentProcessId()) + ".env";
#else
        env_path_ = "/tmp/hy_test_optransport_" + std::to_string(getpid()) + ".env";
#endif
        std::ofstream f(env_path_, std::ios::binary);
        f << "HENGYUAN_BINANCE_TESTNET_API_KEY=" << BoundTestCredentials::kApiKey << "\n";
        f << "HENGYUAN_BINANCE_TESTNET_SECRET=" << BoundTestCredentials::kSecret << "\n";
        f.close();
#ifdef __linux__
        chmod(env_path_.c_str(), 0600);
#endif
    }

    void TearDown() override {
        client_.reset();
        server_.reset();
        std::remove(env_path_.c_str());
    }

    std::unique_ptr<BoundHmacCredentials> make_creds() {
        static constexpr std::string_view kAllowedKeys[] = {"HENGYUAN_BINANCE_TESTNET_API_KEY", "HENGYUAN_BINANCE_TESTNET_SECRET"};
        static constexpr hy::EnvAllowlist kAllowlist{kAllowedKeys, 2};
        hy::SecureEnvLoader loader;
        EXPECT_EQ(loader.load(env_path_.c_str(), kAllowlist).status, hy::EnvLoadStatus::Ok);
        auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(EnvironmentBinding::testnet(), loader);
        EXPECT_EQ(err, hy::QuerySigningError::Ok);
        return std::move(creds);
    }

    // `reconciliation_lane`: if non-zero, a tracker is wired in whose Reconciliation lane holds exactly that much weight.
    void wire(TlsRoutingAcceptor::Handler handler, bool fresh_clock = true, bool with_credentials = true, std::uint32_t reconciliation_lane = 0) {
        client_.reset();
        server_ = std::make_unique<TlsRoutingAcceptor>(fixture_path("test_leaf_cert_testnet_host.pem"),
                                                       fixture_path("test_leaf_key_testnet_host.pem"), std::move(handler));
        cfg_.port = std::to_string(server_->port());
        hy::SpotRateLimitTracker* tracker = nullptr;
        if (reconciliation_lane != 0) {
            EXPECT_TRUE(tracker_.configure(reconciliation_lane * 10, 0, 100'000, 0, 100'000, 0));
            tracker = &tracker_;
        }
        client_ = std::make_unique<BinancePrivateRestClient>(EnvironmentBinding::testnet(), with_credentials ? make_creds() : nullptr, tracker,
                                                             hy::EndpointWeightTable{}, cfg_);
        BinancePrivateRestClientTestHooks::use_loopback(*client_, fixture_path("test_leaf_cert_testnet_host.pem"));
        if (fresh_clock) BinancePrivateRestClientTestHooks::publish_fresh_clock(*client_);
    }
};

// A handler that answers by the path of the request; anything it does not know is a 404.
struct Routes {
    std::optional<Reply> time;  // unset: every /api/v3/time request is answered with the exchange time of that moment
    Reply order = Reply::answer(400, R"({"code":-2013,"msg":"Order does not exist."})");
    Reply all_orders = Reply::answer(200, "[]");
    Reply open_orders = Reply::answer(200, "[]");
    Reply account = Reply::answer(200, std::string(kAccountBody));

    TlsRoutingAcceptor::Handler handler() const {
        const Routes r = *this;
        return [r](const Request& req) {
            const std::string path = path_of(req);
            if (path == "/api/v3/time") return r.time ? *r.time : time_reply();
            if (path == "/api/v3/order") return r.order;
            if (path == "/api/v3/allOrders") return r.all_orders;
            if (path == "/api/v3/openOrders") return r.open_orders;
            if (path == "/api/v3/account") return r.account;
            return Reply::answer(404, "{}");
        };
    }
};

class PortTest : public TransportFixture {
protected:
    FakeWorld world_;
    FakeSleeper sleeper_{world_};
    std::unique_ptr<BinanceExchangeReadPort> port_;

    void start(const Routes& routes, bool fresh_clock = true, std::chrono::milliseconds spacing = kDefaultRequestSpacing,
               bool with_credentials = true, std::uint32_t reconciliation_lane = 0) {
        wire(routes.handler(), fresh_clock, with_credentials, reconciliation_lane);
        port_ = std::make_unique<BinanceExchangeReadPort>(*client_, sleeper_, spacing);
        sleeper_.sleeps.clear();
    }

    std::vector<std::string> paths() const {
        std::vector<std::string> out;
        for (const Request& r : server_->requests()) out.push_back(path_of(r));
        return out;
    }

    static void expect_not_completed(const HttpRead& r) {
        EXPECT_FALSE(r.transport_ok);
        EXPECT_EQ(r.http_status, 0);
        EXPECT_TRUE(r.body.empty());
    }

    void TearDown() override {
        port_.reset();
        TransportFixture::TearDown();
    }
};

}  // namespace

TEST_F(PortTest, TheFiveReadsGoWhereThePortContractSaysAndTheirAnswersComeBack) {
    start(Routes{});
    const std::optional<ExchangeMs> t = port_->server_time();
    ASSERT_TRUE(t.has_value());
    EXPECT_NEAR(static_cast<double>(t->count()), static_cast<double>(real_now_ms()), 5000.0);

    const HttpRead order = port_->query_order("BTCUSDT", kCoid);
    EXPECT_TRUE(order.transport_ok);
    EXPECT_EQ(order.http_status, 400);
    EXPECT_EQ(order.body, R"({"code":-2013,"msg":"Order does not exist."})");

    const HttpRead page = port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1000);
    EXPECT_TRUE(page.transport_ok);
    EXPECT_EQ(page.http_status, 200);
    EXPECT_EQ(page.body, "[]");

    const HttpRead open = port_->open_orders();
    EXPECT_TRUE(open.transport_ok);
    EXPECT_EQ(open.http_status, 200);

    const AccountView account = port_->account();
    ASSERT_TRUE(account.uid.has_value());
    EXPECT_EQ(*account.uid, 354937868u);
    EXPECT_EQ(account.summary,
              "account type SPOT; permissions SPOT; balances (free/locked): BTC 4723846.89208129/0.00000000, LTC 4763368.68006011/0.00000000");

    EXPECT_EQ(paths(), (std::vector<std::string>{"/api/v3/time", "/api/v3/order", "/api/v3/allOrders", "/api/v3/openOrders", "/api/v3/account"}));
    for (const Request& r : server_->requests()) EXPECT_EQ(r.method, "GET");
    const std::vector<Request> seen = server_->requests();
    EXPECT_NE(seen[1].target.find("symbol=BTCUSDT"), std::string::npos);
    EXPECT_NE(seen[1].target.find("origClientOrderId=" + std::string(kCoid)), std::string::npos);
    EXPECT_NE(seen[2].target.find("startTime=" + std::to_string(kStart)), std::string::npos);
    EXPECT_NE(seen[2].target.find("endTime=" + std::to_string(kEnd)), std::string::npos);
    EXPECT_NE(seen[2].target.find("limit=1000"), std::string::npos);
    EXPECT_NE(seen[2].target.find("symbol=BTCUSDT"), std::string::npos);
    EXPECT_TRUE(seen[0].api_key_header.empty()) << "the clock reading is not signed and carries no key";
    for (std::size_t i = 1; i < seen.size(); ++i) EXPECT_EQ(seen[i].api_key_header, BoundTestCredentials::kApiKey) << i;
}

TEST_F(PortTest, TheLimitOfAnAllOrdersPageIsPassedOn) {
    start(Routes{});
    (void)port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 500);
    (void)port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1);
    const std::vector<Request> seen = server_->requests();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_NE(seen[0].target.find("limit=500&"), std::string::npos) << seen[0].target;
    EXPECT_NE(seen[1].target.find("limit=1&"), std::string::npos) << seen[1].target;
}

TEST_F(PortTest, AClientWithoutCredentialsFailsEverySignedReadAndNoneOfThemPacesTheNext) {
    start(Routes{}, true, kDefaultRequestSpacing, /*with_credentials=*/false);
    expect_not_completed(port_->query_order("BTCUSDT", kCoid));
    expect_not_completed(port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1000));
    expect_not_completed(port_->open_orders());
    EXPECT_FALSE(port_->account().uid.has_value());
    EXPECT_EQ(server_->request_count(), 0u);
    EXPECT_TRUE(sleeper_.sleeps.empty()) << "nothing reached the network, so nothing needed a gap";
}

TEST_F(PortTest, ARequestTheLocalBudgetRefusesIsNotAnAnswerAndPacesNothing) {
    start(Routes{}, true, kDefaultRequestSpacing, true, /*reconciliation_lane=*/1);  // room for one clock reading (weight 1), nothing else
    EXPECT_TRUE(port_->server_time().has_value());
    expect_not_completed(port_->query_order("BTCUSDT", kCoid));  // weight 4
    expect_not_completed(port_->open_orders());                    // weight 80
    EXPECT_EQ(server_->request_count(), 1u);
    EXPECT_EQ(sleeper_.sleeps, (std::vector<std::int64_t>{500})) << "the gap after the one request that went out, and none for the refusals";
    EXPECT_FALSE(port_->server_time().has_value()) << "the lane is spent: the clock reading is refused too";
}

TEST_F(PortTest, ServerTimeIsTheExchangesOwnReadingExactly) {
    const auto served = std::make_shared<std::atomic<std::int64_t>>(0);
    wire(
        [served](const Request&) {
            const std::int64_t now = real_now_ms() + 1234;
            served->store(now);
            return Reply::answer(200, "{\"serverTime\":" + std::to_string(now) + "}");
        },
        false);
    port_ = std::make_unique<BinanceExchangeReadPort>(*client_, sleeper_);
    const std::optional<ExchangeMs> t = port_->server_time();
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->count(), served->load());
}

TEST_F(PortTest, ARequestThatDoesNotCompleteIsNeverAnEmpty200) {
    cfg_.read_timeout_ms = 300;
    struct Fault {
        const char* name;
        Reply reply;
    };
    const std::vector<Fault> faults = {
        {"silent", Reply::silent()},
        {"cut short", Reply::cut_short(R"([{"symbol":"BTCUSDT","orderId":1,"clientOrderId":"HY-1"},{"symbol":"BTCUSDT"})")},
        {"dropped", Reply::drop()},
    };
    for (const Fault& fault : faults) {
        Routes routes;
        routes.order = routes.all_orders = routes.open_orders = routes.account = fault.reply;
        start(routes);
        expect_not_completed(port_->query_order("BTCUSDT", kCoid));
        expect_not_completed(port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1000));
        expect_not_completed(port_->open_orders());
        const AccountView a = port_->account();
        EXPECT_FALSE(a.uid.has_value()) << fault.name;
        EXPECT_TRUE(a.summary.empty()) << fault.name;
        EXPECT_FALSE(port_->halted()) << fault.name << ": a transport failure is not the exchange saying stop";
        EXPECT_EQ(server_->request_count(), 4u) << fault.name << ": each failed request was sent once and never repeated";
    }
    // and the clock reading
    Routes timeless;
    timeless.time = Reply::answer(500, "oops");
    start(timeless, /*fresh_clock=*/false);
    EXPECT_FALSE(port_->server_time().has_value());
    timeless.time = Reply::answer(200, "not json");
    start(timeless, false);
    EXPECT_FALSE(port_->server_time().has_value());
}

TEST_F(PortTest, ARequestTheClientRefusesLocallyIsNotAnAnswerEither) {
    start(Routes{});
    expect_not_completed(port_->query_order("btcusdt", kCoid));
    expect_not_completed(port_->query_order("BTCUSDT", ""));
    expect_not_completed(port_->all_orders("BTCUSDT", ExchangeMs{kEnd}, ExchangeMs{kStart}, 1000));
    expect_not_completed(port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1001));
    EXPECT_EQ(server_->request_count(), 0u);
    EXPECT_TRUE(sleeper_.sleeps.empty());
}

TEST_F(PortTest, AnAccountAnswerThatIsNotA200IsNotAnAccountWhateverItSays) {
    Routes routes;
    routes.account = Reply::answer(400, std::string(kAccountBody));  // the body of a real account, with an error status
    start(routes);
    const AccountView a = port_->account();
    EXPECT_FALSE(a.uid.has_value());
    EXPECT_TRUE(a.summary.empty());

    routes.account = Reply::answer(200, R"({"code":-2015,"msg":"Invalid API-key, IP, or permissions for action."})");
    start(routes);
    const AccountView b = port_->account();
    EXPECT_FALSE(b.uid.has_value()) << "a 200 without a uid is an account without a uid";
    EXPECT_FALSE(b.summary.empty()) << "and its summary is still composed from what is there";
}

// --- the gap between two requests -----------------------------------------------------------------------------------------------------------------

TEST_F(PortTest, NoGapBeforeTheFirstRequestAndOneBeforeEveryOtherAnsweredOne) {
    start(Routes{});
    EXPECT_TRUE(port_->server_time().has_value());
    EXPECT_TRUE(sleeper_.sleeps.empty()) << "nothing to wait for before the first request";
    (void)port_->query_order("BTCUSDT", kCoid);
    EXPECT_EQ(sleeper_.sleeps, (std::vector<std::int64_t>{500}));
    (void)port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1000);
    (void)port_->open_orders();
    (void)port_->account();
    (void)port_->server_time();
    EXPECT_EQ(sleeper_.sleeps, (std::vector<std::int64_t>{500, 500, 500, 500, 500}));
    EXPECT_EQ(server_->request_count(), 6u);
}

TEST_F(PortTest, TheGapIsWhatTheCallerAskedFor) {
    start(Routes{}, true, std::chrono::milliseconds{1234});
    (void)port_->open_orders();
    (void)port_->open_orders();
    EXPECT_EQ(sleeper_.sleeps, (std::vector<std::int64_t>{1234}));
}

TEST_F(PortTest, AFailedRequestStillSpacesTheNextOneBecauseItDidReachTheNetwork) {
    cfg_.read_timeout_ms = 300;
    Routes routes;
    routes.order = Reply::drop();
    start(routes);
    expect_not_completed(port_->query_order("BTCUSDT", kCoid));
    (void)port_->open_orders();
    EXPECT_EQ(sleeper_.sleeps, (std::vector<std::int64_t>{500}));
}

TEST_F(PortTest, ARequestRefusedLocallyIsNeitherPacedNorPacesTheNext) {
    start(Routes{});
    expect_not_completed(port_->query_order("btcusdt", kCoid));  // refused before the network
    EXPECT_TRUE(port_->server_time().has_value());                // the first request that goes out
    EXPECT_TRUE(sleeper_.sleeps.empty()) << "the refused one did not count";
    expect_not_completed(port_->query_order("btcusdt", kCoid));
    (void)port_->open_orders();
    EXPECT_EQ(sleeper_.sleeps, (std::vector<std::int64_t>{500})) << "one gap, for the one request that went out before it";
}

// --- the latch -------------------------------------------------------------------------------------------------------------------------------

TEST_F(PortTest, AnAnswerThatSaysStopIsHandedBackAndThenTheAnswererIsLeftAlone) {
    struct Case {
        int status;
        const char* body;
    };
    for (const Case c : {Case{429, R"({"code":-1003,"msg":"Too many requests."})"}, Case{418, R"({"code":-1003,"msg":"IP banned."})"},
                         Case{403, "Forbidden by WAF"}}) {
        Routes routes;
        routes.all_orders = Reply::answer(c.status, c.body);
        start(routes);
        ASSERT_TRUE(port_->server_time().has_value());
        const HttpRead stop = port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1000);
        EXPECT_TRUE(stop.transport_ok) << c.status;
        EXPECT_EQ(stop.http_status, c.status);
        EXPECT_EQ(stop.body, c.body) << "the answer itself is not swallowed";
        EXPECT_TRUE(port_->halted()) << c.status;

        const std::size_t sent = server_->request_count();
        const std::size_t slept = sleeper_.sleeps.size();
        expect_not_completed(port_->query_order("BTCUSDT", kCoid));
        expect_not_completed(port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1000));
        expect_not_completed(port_->open_orders());
        EXPECT_FALSE(port_->account().uid.has_value());
        EXPECT_FALSE(port_->server_time().has_value());
        EXPECT_EQ(server_->request_count(), sent) << c.status << ": nothing more was sent";
        EXPECT_EQ(sleeper_.sleeps.size(), slept) << c.status << ": and nothing was waited for";
    }
}

TEST_F(PortTest, EveryReadCanTripTheLatchAndEveryOtherStatusLeavesItAlone) {
    // each of the four signed reads, answered 429
    for (int which = 0; which < 4; ++which) {
        Routes routes;
        const Reply stop = Reply::answer(429, R"({"code":-1003})");
        if (which == 0) routes.order = stop;
        if (which == 1) routes.all_orders = stop;
        if (which == 2) routes.open_orders = stop;
        if (which == 3) routes.account = stop;
        start(routes);
        if (which == 0) (void)port_->query_order("BTCUSDT", kCoid);
        if (which == 1) (void)port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1000);
        if (which == 2) (void)port_->open_orders();
        if (which == 3) (void)port_->account();
        EXPECT_TRUE(port_->halted()) << which;
        EXPECT_EQ(server_->request_count(), 1u) << which;
    }
    // statuses that are NOT the exchange saying stop, on every read
    for (const int status : {200, 400, 401, 404, 417, 419, 428, 430, 500, 502, 503}) {
        Routes routes;
        routes.order = routes.all_orders = routes.open_orders = routes.account = Reply::answer(status, "{}");
        start(routes);
        (void)port_->query_order("BTCUSDT", kCoid);
        (void)port_->all_orders("BTCUSDT", ExchangeMs{kStart}, ExchangeMs{kEnd}, 1000);
        (void)port_->open_orders();
        (void)port_->account();
        EXPECT_FALSE(port_->halted()) << status;
        EXPECT_EQ(server_->request_count(), 4u) << status;
    }
}

// --- the clock --------------------------------------------------------------------------------------------------------------------------------

TEST_F(PortTest, ASignedReadWithoutAFreshClockTakesOneNewReadingAndRepeatsItselfOnce) {
    start(Routes{}, /*fresh_clock=*/false);
    const HttpRead r = port_->query_order("BTCUSDT", kCoid);
    EXPECT_TRUE(r.transport_ok);
    EXPECT_EQ(r.http_status, 400);
    EXPECT_EQ(paths(), (std::vector<std::string>{"/api/v3/time", "/api/v3/order"})) << "nothing had been sent before the reading";
    EXPECT_EQ(sleeper_.sleeps, (std::vector<std::int64_t>{500})) << "the gap between the reading and the repeated request";
    // from now on the snapshot is there: no more readings
    (void)port_->open_orders();
    EXPECT_EQ(server_->request_count(), 3u);
}

TEST_F(PortTest, IfTheNewReadingFailsTheReadFailsAndTheRequestIsNeverSent) {
    Routes routes;
    routes.time = Reply::answer(500, "oops");
    start(routes, false);
    expect_not_completed(port_->query_order("BTCUSDT", kCoid));
    EXPECT_EQ(paths(), (std::vector<std::string>{"/api/v3/time"}));
    EXPECT_TRUE(sleeper_.sleeps.empty()) << "a failed refresh ends the read at once: no repeat, and no gap waited for one";
    expect_not_completed(port_->open_orders());
    EXPECT_FALSE(port_->account().uid.has_value());
}

TEST_F(PortTest, AClockReadingThatIsFarFromTheLocalClockIsNotUsedAndTheReadFails) {
    Routes routes;
    routes.time = Reply::answer(200, hy::test_helpers::server_time_body_offset_by(600'000));
    start(routes, false);
    EXPECT_FALSE(port_->server_time().has_value());
    expect_not_completed(port_->open_orders());
    EXPECT_EQ(paths(), (std::vector<std::string>{"/api/v3/time", "/api/v3/time"}));
}

// ============================================================ 3. the whole procedure over TLS ======================================================

namespace {

// The flow's clock, without the real client's calibration: serverTime comes from the fake world (which the sleeper advances), while the real
// port still makes its real request for every reading -- so the traffic on the wire is what a production run produces, and the client's own
// clock snapshot stays fresh -- and every other read goes through the real port unchanged. Records what the flow asked, at what world time and
// how many sleeps had happened by then.
class WorldClockPort final : public ExchangeReadPort {
public:
    struct Call {
        std::string kind;
        std::int64_t world_ms;
        std::size_t sleeps_before;
    };

    WorldClockPort(BinanceExchangeReadPort& real, FakeWorld& world, FakeSleeper& sleeper) : real_(real), world_(world), sleeper_(sleeper) {}

    std::optional<ExchangeMs> server_time() override {
        note("time");
        const std::optional<ExchangeMs> actual = real_.server_time();
        if (!actual) return std::nullopt;
        return ExchangeMs{world_.now_ms};
    }
    HttpRead query_order(std::string_view symbol, std::string_view client_order_id) override {
        note("order");
        return real_.query_order(symbol, client_order_id);
    }
    HttpRead all_orders(std::string_view symbol, ExchangeMs start, ExchangeMs end, std::size_t limit) override {
        note("allOrders");
        return real_.all_orders(symbol, start, end, limit);
    }
    HttpRead open_orders() override {
        note("openOrders");
        return real_.open_orders();
    }
    AccountView account() override {
        note("account");
        return real_.account();
    }

    std::vector<Call> calls;

    std::size_t count(std::string_view kind) const {
        std::size_t n = 0;
        for (const Call& c : calls) {
            if (c.kind == kind) ++n;
        }
        return n;
    }

private:
    void note(const char* kind) { calls.push_back({kind, world_.now_ms, sleeper_.sleeps.size()}); }

    BinanceExchangeReadPort& real_;
    FakeWorld& world_;
    FakeSleeper& sleeper_;
};

std::vector<std::byte> file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

constexpr std::uint32_t kKeyId = 1;
constexpr std::uint64_t kUid = kFakeAccountUid;

class WholeProcedureTest : public TransportFixture {
protected:
    std::int64_t t0_{0};  // the exchange time at which the lock is taken (the real time of the test: the client's clock must agree with it)
    std::string coid_;
    std::string stem_;
    std::string audit_path_;
    std::string cp_path_;
    std::unique_ptr<KeyRing> ring_;
    std::unique_ptr<DurableAuditSink> audit_;
    std::unique_ptr<ControlPlaneLogSink> names_;
    std::vector<std::byte> audit_before_;
    std::vector<std::byte> cp_before_;

    FakeWorld world_;
    FakeSleeper sleeper_{world_};
    FakeConsole console_;
    FakeStore store_;
    std::unique_ptr<BinanceExchangeReadPort> real_port_;
    std::unique_ptr<WorldClockPort> port_;
    std::unique_ptr<OperatorPorts> ports_;

    void SetUp() override {
        TransportFixture::SetUp();
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        stem_ = std::string(tmp) + "hy_optransport_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_optransport_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        audit_path_ = stem_ + ".audit.log";
        cp_path_ = stem_ + ".cp.log";
        remove_logs();
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x40 + i);
        ring_ = std::make_unique<KeyRing>(kek);
        static const char kKey[] = "operator-resolution-transport-test-key";
        std::vector<std::byte> key(sizeof(kKey) - 1);
        std::memcpy(key.data(), kKey, key.size());
        WrappedKeyRecord rec{};
        ASSERT_EQ(ring_->add_key(kKeyId, key, rec), KeyRingAddStatus::Ok);
        t0_ = real_now_ms();
        world_.now_ms = t0_;
        coid_ = "HY-" + std::to_string(t0_ - 601'000) + "-1-0A0A";
    }

    void TearDown() override {
        ports_.reset();
        port_.reset();
        real_port_.reset();
        names_.reset();
        audit_.reset();
        remove_logs();
        TransportFixture::TearDown();
    }

    void remove_logs() {
        for (const std::string& base : {audit_path_, cp_path_}) {
            for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".cp.lock", ".cp.tip", ".cp.tip.tmp", ".keyrotations",
                                       ".keyrotations.lock", ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid", ".storeid.lock",
                                       ".storeid.tip", ".storeid.tip.tmp"}) {
                std::remove((base + suffix).c_str());
            }
        }
    }

    AuditRecord record(AuditEventType ev, OrderState st) const {
        AuditRecord r{};
        r.timestamp_ms = t0_ - 600'000;
        r.event_type = ev;
        r.mode = ExecutionMode::Live;
        r.symbol_id = 0;
        r.set_client_order_id(coid_);
        r.price_ticks = 500'000;
        r.qty_ticks = 100;
        r.resulting_state = st;
        r.side = OrderSide::Sell;
        return r;
    }

    // One ambiguous order in the audit log, one symbol in the control-plane log, both opened the way the tool opens them, and a client whose
    // exchange answers by `routes` (set BEFORE the server starts: the handler is not touched afterwards).
    void prepare(const Routes& routes) {
        {
            DurableAuditSink sink(audit_path_, *ring_, kKeyId);
            ASSERT_TRUE(sink.is_open());
            const std::int64_t send_at = t0_ - 599'000;
            ASSERT_TRUE(sink.append_durable(record(AuditEventType::OrderIntentCreated, OrderState::Intent), send_at - 1000).acked());
            ASSERT_TRUE(sink.append_durable(record(AuditEventType::OrderSubmitPrepared, OrderState::Submitting), send_at).acked());
            ASSERT_TRUE(sink.append_durable(record(AuditEventType::OrderAmbiguous, OrderState::Ambiguous), send_at + 1000).acked());
        }
        {
            ControlPlaneLogSink sink(cp_path_, *ring_, kKeyId);
            ASSERT_TRUE(sink.is_open());
            SymbolRules rules{};
            std::memcpy(rules.symbol, "BTCUSDT", 7);
            rules.is_trading = true;
            rules.rules_version = 1;
            SymbolRegistrySnapshotPayload snap{};
            snap.timestamp_ms = t0_ - 3'600'000;
            snap.rules_version = 1;
            snap.symbol_count = 1;
            ASSERT_TRUE(sink.append_snapshot(snap, std::span<const SymbolRules>(&rules, 1)).acked());
        }
        audit_before_ = file_bytes(audit_path_);
        cp_before_ = file_bytes(cp_path_);
        audit_ = std::make_unique<DurableAuditSink>(audit_path_, *ring_, kKeyId, kMaxIntentOrphans, AuditSinkOpenMode::OperatorOpen);
        names_ = std::make_unique<ControlPlaneLogSink>(cp_path_, *ring_, kKeyId);
        ASSERT_TRUE(audit_->is_open());
        ASSERT_FALSE(audit_->fenced());
        ASSERT_TRUE(names_->is_open());

        wire(routes.handler(), /*fresh_clock=*/false);
        real_port_ = std::make_unique<BinanceExchangeReadPort>(*client_, sleeper_);
        port_ = std::make_unique<WorldClockPort>(*real_port_, world_, sleeper_);
        ports_ = std::make_unique<OperatorPorts>(OperatorPorts{*port_, sleeper_, console_, store_});
    }

    OperatorResolutionRequest request() const {
        OperatorResolutionRequest r;
        r.client_order_id = coid_;
        r.environment = OperatorEnvironment::Testnet;
        r.account_uid = kUid;
        r.operator_label = "alice";
        return r;
    }

    bool logs_untouched() {
        ports_.reset();
        port_.reset();
        real_port_.reset();
        names_.reset();
        audit_.reset();
        return file_bytes(audit_path_) == audit_before_ && file_bytes(cp_path_) == cp_before_;
    }

    std::vector<std::string> wire_paths() const {
        std::vector<std::string> out;
        for (const Request& r : server_->requests()) out.push_back(path_of(r));
        return out;
    }
};

}  // namespace

namespace {

// The exchange of a whole-procedure test: its account answer carries `uid` (none: no uid member at all).
Routes routes_for_uid(std::optional<std::uint64_t> uid) {
    Routes r;
    const std::string rest = R"("accountType":"SPOT","permissions":["SPOT"],"balances":[{"asset":"USDT","free":"1000.0","locked":"0"}])";
    r.account = Reply::answer(200, uid ? account_with("\"uid\":" + std::to_string(*uid), rest) : account_with("", rest));
    return r;
}

}  // namespace

TEST_F(WholeProcedureTest, AnAbsentOrderIsResolvedWithTheRealPortOverRealTls) {
    prepare(routes_for_uid(kUid));
    OperatorSession session = begin_operator_session(*audit_, *names_, *ports_, FenceParams{});
    ASSERT_EQ(session.status, SessionStatus::Ready);
    EXPECT_EQ(session.lock_ref.count(), t0_);
    const OperatorResolutionOutcome o = resolve_order(session, request());
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status) << " / " << conclusion_name(o.conclusion);
    EXPECT_EQ(o.conclusion, AbsentConclusionStatus::Absent);
    EXPECT_EQ(o.append.status, OperatorAppendStatus::Appended);
    ASSERT_TRUE(o.exchange_account_uid.has_value());
    EXPECT_EQ(*o.exchange_account_uid, kUid);

    // on the wire: GETs only; the session's clock reading, then the account (before the wait), clock readings while the fence ran, the
    // evidence (point query, allOrders, openOrders) and the clock reading of the frame
    for (const Request& r : server_->requests()) EXPECT_EQ(r.method, "GET");
    const std::vector<std::string> paths = wire_paths();
    ASSERT_GE(paths.size(), 8u);
    EXPECT_EQ(paths[0], "/api/v3/time");
    EXPECT_EQ(paths[1], "/api/v3/account");
    const std::size_t order_at = static_cast<std::size_t>(std::find(paths.begin(), paths.end(), "/api/v3/order") - paths.begin());
    ASSERT_LT(order_at + 3, paths.size());
    for (std::size_t i = 2; i < order_at; ++i) EXPECT_EQ(paths[i], "/api/v3/time") << i << ": only the clock until the fence is over";
    EXPECT_EQ(paths[order_at + 1], "/api/v3/allOrders");
    EXPECT_EQ(paths[order_at + 2], "/api/v3/openOrders");
    EXPECT_EQ(paths[order_at + 3], "/api/v3/time");
    EXPECT_EQ(paths.size(), order_at + 4);
    EXPECT_EQ(std::count(paths.begin(), paths.end(), "/api/v3/account"), 1);

    // in the flow's time: no evidence request before lock + 270 s, and a 500 ms gap between two evidence requests
    EXPECT_EQ(port_->count("account"), 1u);
    const WorldClockPort::Call* previous = nullptr;
    for (const WorldClockPort::Call& c : port_->calls) {
        const bool evidence = c.kind == "order" || c.kind == "allOrders" || c.kind == "openOrders";
        if (!evidence) continue;
        EXPECT_GE(c.world_ms, t0_ + kDefaultEvidenceWaitMs) << c.kind << " came before the fence was over";
        if (previous != nullptr) {
            EXPECT_EQ(c.sleeps_before, previous->sleeps_before + 1) << c.kind << ": the gap after the request before it, and nothing else";
            EXPECT_EQ(sleeper_.sleeps[previous->sleeps_before], 500) << c.kind;
        }
        previous = &c;
    }
    EXPECT_NE(previous, nullptr);

    // the evidence file holds what the real exchange answered: the 400 and its code, verbatim, and the uid it reported
    ASSERT_EQ(store_.stored.size(), 1u);
    const std::string& bundle = store_.stored[0].second;
    EXPECT_NE(bundle.find(R"("name":"order","params":"symbol=BTCUSDT&origClientOrderId=)" + coid_), std::string::npos) << bundle;
    EXPECT_NE(bundle.find(R"("http_status":400,"body":"{\"code\":-2013,\"msg\":\"Order does not exist.\"}")"), std::string::npos) << bundle;
    EXPECT_NE(bundle.find("\"account_uid_claimed\":" + std::to_string(kUid)), std::string::npos);
    EXPECT_NE(bundle.find("\"account_uid_reported\":" + std::to_string(kUid)), std::string::npos);
    EXPECT_NE(bundle.find(R"("account_summary":"account type SPOT; permissions SPOT; balances (free/locked): USDT 1000.0/0")"), std::string::npos);
    EXPECT_EQ(bundle.find("signature"), std::string::npos);
    EXPECT_EQ(bundle.find(std::string(BoundTestCredentials::kApiKey)), std::string::npos);
    EXPECT_EQ(bundle.find(std::string(BoundTestCredentials::kSecret)), std::string::npos);
    for (const std::string& line : console_.lines) {
        EXPECT_EQ(line.find(std::string(BoundTestCredentials::kApiKey)), std::string::npos);
        EXPECT_EQ(line.find(std::string(BoundTestCredentials::kSecret)), std::string::npos);
    }
}

TEST_F(WholeProcedureTest, AKeyOfAnotherAccountIsRefusedBeforeAnyWaitOrEvidenceRequest) {
    prepare(routes_for_uid(kUid + 1));
    OperatorSession session = begin_operator_session(*audit_, *names_, *ports_, FenceParams{});
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request());
    EXPECT_EQ(o.status, ResolveStatus::AccountUidMismatch) << resolve_status_name(o.status);
    ASSERT_TRUE(o.exchange_account_uid.has_value());
    EXPECT_EQ(*o.exchange_account_uid, kUid + 1);
    EXPECT_EQ(wire_paths(), (std::vector<std::string>{"/api/v3/time", "/api/v3/account"}));
    EXPECT_EQ(sleeper_.sleeps, (std::vector<std::int64_t>{500})) << "only the gap between those two requests";
    EXPECT_TRUE(store_.attempts.empty());
    EXPECT_EQ(console_.reads, 0);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(WholeProcedureTest, AnAccountAnswerWithoutAUidIsRefusedBeforeAnyWaitOrEvidenceRequest) {
    prepare(routes_for_uid(std::nullopt));
    OperatorSession session = begin_operator_session(*audit_, *names_, *ports_, FenceParams{});
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request());
    EXPECT_EQ(o.status, ResolveStatus::AccountUnverified) << resolve_status_name(o.status);
    EXPECT_FALSE(o.exchange_account_uid.has_value());
    EXPECT_EQ(wire_paths(), (std::vector<std::string>{"/api/v3/time", "/api/v3/account"}));
    EXPECT_TRUE(store_.attempts.empty());
    EXPECT_EQ(console_.reads, 0);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(WholeProcedureTest, AnExchangeThatSaysStopEndsTheRunAndIsNotAskedAgain) {
    Routes routes = routes_for_uid(kUid);
    routes.order = Reply::answer(429, R"({"code":-1003,"msg":"Too many requests."})");
    prepare(routes);
    OperatorSession session = begin_operator_session(*audit_, *names_, *ports_, FenceParams{});
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request());
    EXPECT_EQ(o.status, ResolveStatus::EvidenceReadFailed) << resolve_status_name(o.status);
    const std::vector<std::string> paths = wire_paths();
    ASSERT_FALSE(paths.empty());
    EXPECT_EQ(paths.back(), "/api/v3/order") << "the 429 was the last thing the exchange was asked: no allOrders, no openOrders, nothing";
    EXPECT_EQ(std::count(paths.begin(), paths.end(), "/api/v3/order"), 1);
    EXPECT_TRUE(real_port_->halted());
    EXPECT_TRUE(store_.attempts.empty()) << "no evidence was built";
    EXPECT_EQ(console_.reads, 0);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(WholeProcedureTest, ABodyThatIsCutOffInTransitIsNotEvidenceAndNothingIsResolved) {
    Routes routes = routes_for_uid(kUid);
    routes.all_orders = Reply::cut_short(R"([{"symbol":"BTCUSDT","orderId":1,"clientOrderId":"HY-1"},{"symbol":"BTCUSDT","orderId":2)");
    prepare(routes);
    OperatorSession session = begin_operator_session(*audit_, *names_, *ports_, FenceParams{});
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request());
    EXPECT_EQ(o.status, ResolveStatus::EvidenceReadFailed) << resolve_status_name(o.status);
    EXPECT_TRUE(store_.attempts.empty());
    EXPECT_EQ(console_.reads, 0);
    EXPECT_FALSE(real_port_->halted()) << "a body cut off is a broken connection, not the exchange saying stop";
    EXPECT_TRUE(logs_untouched());
}

TEST_F(WholeProcedureTest, AnOrderTheExchangeKnowsIsRefusedWithItsEvidenceAndNobodyIsAsked) {
    Routes routes = routes_for_uid(kUid);
    routes.order = Reply::answer(200, R"({"symbol":"BTCUSDT","orderId":4242,"clientOrderId":"x","status":"NEW"})");
    prepare(routes);
    OperatorSession session = begin_operator_session(*audit_, *names_, *ports_, FenceParams{});
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request());
    EXPECT_EQ(o.status, ResolveStatus::EvidenceNotAbsent) << resolve_status_name(o.status);
    EXPECT_EQ(o.conclusion, AbsentConclusionStatus::PointQueryNotAbsent);
    EXPECT_EQ(store_.attempts.size(), 1u) << "the evidence of a refusal is kept too";
    EXPECT_EQ(console_.reads, 0);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(WholeProcedureTest, NoRealStatusOtherThanTheDocumentedFourHundredIsTakenForAbsence) {
    struct Row {
        int status;
        const char* body;
    };
    for (const Row row : {Row{401, R"({"code":-2015,"msg":"Invalid API-key, IP, or permissions for action."})"},
                          Row{400, R"({"code":-1021,"msg":"Timestamp for this request is outside of the recvWindow."})"},
                          Row{400, R"({"code":-2026,"msg":"Order was canceled or expired with no executed qty over 90 days ago and has been archived."})"},
                          Row{404, R"({"code":-2013,"msg":"Order does not exist."})"},
                          Row{500, "<html>Bad gateway</html>"}, Row{502, ""}}) {
        Routes routes = routes_for_uid(kUid);
        routes.order = Reply::answer(row.status, row.body);
        // a fresh world for each row: new logs, new session
        TearDown();
        SetUp();
        prepare(routes);
        OperatorSession session = begin_operator_session(*audit_, *names_, *ports_, FenceParams{});
        ASSERT_EQ(session.status, SessionStatus::Ready) << row.status;
        const OperatorResolutionOutcome o = resolve_order(session, request());
        EXPECT_EQ(o.status, ResolveStatus::EvidenceNotAbsent) << row.status << " " << row.body << " -> " << resolve_status_name(o.status);
        EXPECT_EQ(console_.reads, 0) << row.status;
    }
}
