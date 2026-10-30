// SPDX-License-Identifier: proprietary
// test_binance_private_rest_raw.cpp -- D5 slice R3b-1: the raw signed GETs of BinancePrivateRestClient (fetch_order_raw, fetch_all_orders_raw,
// fetch_open_orders_raw, fetch_account_raw) and read_server_time(), against a local TLS "exchange" that answers each request as a test says.
//
// What these methods are for: the offline operator tool decides "this order does not exist" on the BODY of a 400 ({"code":-2013}), which the
// trading path's reads (fetch_signed_body_coro: any status but 200 is an error, the body is dropped) cannot give it. So the answer comes back
// verbatim -- status and body, whatever the status is -- and everything that makes the trading path's reads safe stays: GET only, signed with the
// client's own fresh clock, the host allowlist, the response caps by kind of answer (order/account: SignedJson 256 KiB; allOrders/openOrders:
// OrderList 1 MiB), the local rate-limit budget on the Reconciliation lane (explicit weights where PrivateRestEndpoint has no slot), the
// per-stage timeouts, and "nothing is sent" for every local refusal.
//
// Synthetic credentials only (the signing example key pair Binance's own docs print); no real network access.

#include <gtest/gtest.h>
#include <hengyuan/binance_private_rest.hpp>
#include <hengyuan/sha256.hpp>

#include "binance_environment_test_hooks.hpp"
#include "binance_private_rest_test_hooks.hpp"
#include "test_helpers/blackhole_acceptor.hpp"
#include "test_helpers/bound_test_credentials.hpp"
#include "test_helpers/server_time_body.hpp"
#include "test_helpers/tls_routing_acceptor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using hy::BinancePrivateRestClient;
using hy::BinancePrivateRestClientTestHooks;
using hy::BoundHmacCredentials;
using hy::EnvironmentBinding;
using hy::PrivateRestConfig;
using hy::PrivateRestError;
using hy::RawHttpAnswer;
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

std::string hmac_hex(std::string_view secret, std::string_view data) {
    const auto as_bytes = [](std::string_view s) { return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()); };
    const hy::crypto::Sha256Digest d = hy::crypto::hmac_sha256(as_bytes(secret), as_bytes(data));
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : d.bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

bool is_lower_hex(std::string_view s) {
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// The request as a signed GET of `path` whose business parameters, sorted and percent-encoded, are `canonical` (recvWindow included):
//   <path>?<canonical>&timestamp=<13 digits>&signature=<hex of HMAC-SHA256(secret, "<canonical>&timestamp=<13 digits>")>
void expect_signed_get(const Request& r, std::string_view path, const std::string& canonical) {
    EXPECT_EQ(r.method, "GET");
    EXPECT_EQ(r.api_key_header, BoundTestCredentials::kApiKey);
    EXPECT_EQ(r.host_header, std::string(EnvironmentBinding::testnet().base_host())) << "the logical host, not the loopback address";
    EXPECT_TRUE(r.body.empty());
    const std::string head = std::string(path) + "?" + canonical + "&timestamp=";
    ASSERT_EQ(r.target.compare(0, head.size(), head), 0) << r.target;
    const std::string tail = r.target.substr(head.size());
    ASSERT_EQ(tail.size(), 13u + std::string_view("&signature=").size() + 64u) << tail;
    const std::string timestamp = tail.substr(0, 13);
    for (const char c : timestamp) EXPECT_TRUE(c >= '0' && c <= '9') << timestamp;
    ASSERT_EQ(tail.compare(13, 11, "&signature="), 0) << tail;
    const std::string signature = tail.substr(24);
    EXPECT_TRUE(is_lower_hex(signature)) << signature;
    EXPECT_EQ(signature, hmac_hex(BoundTestCredentials::kSecret, canonical + "&timestamp=" + timestamp))
        << "the signature covers the whole query, and only the secret the client was bound with";
    EXPECT_NEAR(static_cast<double>(std::stoll(timestamp)), static_cast<double>(real_now_ms()), 60'000.0);
}

Reply time_reply() { return Reply::answer(200, hy::test_helpers::current_server_time_body()); }

class RawReadTest : public ::testing::Test {
protected:
    std::string env_path_;
    std::unique_ptr<TlsRoutingAcceptor> server_;
    std::unique_ptr<BinancePrivateRestClient> client_;
    hy::SpotRateLimitTracker tracker_;
    PrivateRestConfig cfg_;
    // The certificate the fixture exchange presents (and the client trusts), and the port the client connects to (empty: the fixture's).
    std::string cert_file_{"test_leaf_cert_testnet_host.pem"};
    std::string key_file_{"test_leaf_key_testnet_host.pem"};
    std::string trust_file_;  // what the client trusts (empty: the certificate the exchange presents, which is self-signed)
    std::string port_override_;

    void SetUp() override {
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        env_path_ = std::string(tmp) + "hy_test_rawread_" + std::to_string(GetCurrentProcessId()) + ".env";
#else
        env_path_ = "/tmp/hy_test_rawread_" + std::to_string(getpid()) + ".env";
#endif
        std::ofstream f(env_path_, std::ios::binary);
        f << "HENGYUAN_BINANCE_TESTNET_API_KEY=" << BoundTestCredentials::kApiKey << "\n";
        f << "HENGYUAN_BINANCE_TESTNET_SECRET=" << BoundTestCredentials::kSecret << "\n";
        f.close();
#ifdef __linux__
        chmod(env_path_.c_str(), 0600);  // the Linux loader refuses a file other users can read
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

    // The fixture exchange answers by `handler`; the client talks to it (and trusts its certificate). `fresh_clock`: publish a clock snapshot
    // through the test hook, so the code behind the clock gate is reachable without a /api/v3/time round trip first.
    // `reconciliation_lane`: if non-zero, a tracker is wired in whose Reconciliation lane holds exactly that much weight (the lane is 10% of the
    // limit; raw requests and ORDERS are ample).
    void wire(TlsRoutingAcceptor::Handler handler, bool fresh_clock = true, std::uint32_t reconciliation_lane = 0,
              EnvironmentBinding binding = EnvironmentBinding::testnet(), bool with_credentials = true) {
        server_ = std::make_unique<TlsRoutingAcceptor>(fixture_path(cert_file_.c_str()), fixture_path(key_file_.c_str()), std::move(handler));
        cfg_.port = port_override_.empty() ? std::to_string(server_->port()) : port_override_;
        hy::SpotRateLimitTracker* tracker = nullptr;
        if (reconciliation_lane != 0) {
            EXPECT_TRUE(tracker_.configure(reconciliation_lane * 10, 0, 100'000, 0, 100'000, 0));
            tracker = &tracker_;
        }
        client_ = std::make_unique<BinancePrivateRestClient>(binding, with_credentials ? make_creds() : nullptr, tracker, hy::EndpointWeightTable{}, cfg_);
        BinancePrivateRestClientTestHooks::use_loopback(*client_, fixture_path((trust_file_.empty() ? cert_file_ : trust_file_).c_str()));
        if (fresh_clock) BinancePrivateRestClientTestHooks::publish_fresh_clock(*client_);
    }

    static TlsRoutingAcceptor::Handler always(Reply reply) {
        return [reply](const Request&) { return reply; };
    }

    // The four raw reads, by name, with valid arguments.
    struct NamedRead {
        const char* name;
        std::uint32_t weight;
        PrivateRestError (*call)(BinancePrivateRestClient&, RawHttpAnswer&);
    };
    static const std::vector<NamedRead>& reads() {
        static const std::vector<NamedRead> all = {
            {"order", 4, [](BinancePrivateRestClient& c, RawHttpAnswer& out) { return c.fetch_order_raw("BTCUSDT", kCoid, out); }},
            {"allOrders", 20, [](BinancePrivateRestClient& c, RawHttpAnswer& out) { return c.fetch_all_orders_raw("BTCUSDT", kStart, kEnd, 1000, out); }},
            {"openOrders", 80, [](BinancePrivateRestClient& c, RawHttpAnswer& out) { return c.fetch_open_orders_raw(out); }},
            {"account", 20, [](BinancePrivateRestClient& c, RawHttpAnswer& out) { return c.fetch_account_raw(out); }},
        };
        return all;
    }

    bool reconciliation_lane_open() const { return tracker_.can_send_order(hy::RateLimitLane::Reconciliation, 1); }
    bool strategy_lane_open() const { return tracker_.can_send_order(hy::RateLimitLane::Strategy, 1); }
};

RawHttpAnswer sentinel() {
    RawHttpAnswer a;
    a.http_status = 7;
    a.body = "sentinel";
    return a;
}

void expect_sentinel(const RawHttpAnswer& a) {
    EXPECT_EQ(a.http_status, 7);
    EXPECT_EQ(a.body, "sentinel") << "`out` is assigned only when the exchange answered";
}

}  // namespace

// --- the answer comes back as it was sent ----------------------------------------------------------------------------------------------------

TEST_F(RawReadTest, EveryStatusAndBodyComesBackVerbatimForEveryRead) {
    struct Row {
        int status;
        std::string body;
    };
    const std::vector<Row> rows = {
        {400, R"({"code":-2013,"msg":"Order does not exist."})"},
        {400, R"({"code":-2026,"msg":"Order was canceled or expired with no executed qty over 90 days ago and has been archived."})"},
        {429, R"({"code":-1003,"msg":"Too many requests."})"},
        {418, R"({"code":-1003,"msg":"IP banned."})"},
        {403, "WAF"},
        {500, "<html><body>Bad gateway</body></html>"},
        {302, ""},
        {200, R"({"orderId":4242})"},
        {200, std::string("\0\xff\xfe[]\r\n", 7)},
    };
    for (const NamedRead& read : reads()) {
        for (const Row& row : rows) {
            wire(always(Reply::answer(row.status, row.body)));
            RawHttpAnswer out = sentinel();
            ASSERT_EQ(read.call(*client_, out), PrivateRestError::None) << read.name << " " << row.status;
            EXPECT_EQ(out.http_status, row.status) << read.name;
            EXPECT_EQ(out.body, row.body) << read.name << " " << row.status << ": the bytes the exchange sent, untouched";
            EXPECT_EQ(server_->request_count(), 1u) << read.name << " " << row.status << ": no redirect is followed, nothing is retried";
        }
    }
}

TEST_F(RawReadTest, ThePointQueryKeepsTheBodyOfTheFourHundredTheTradingPathsReadThrowsAway) {
    wire(always(Reply::answer(400, R"({"code":-2013,"msg":"Order does not exist."})")));
    RawHttpAnswer out;
    ASSERT_EQ(client_->fetch_order_raw("BTCUSDT", kCoid, out), PrivateRestError::None);
    EXPECT_EQ(out.http_status, 400);
    EXPECT_NE(out.body.find("-2013"), std::string::npos);
    // the same answer through the trading path's read: an HttpStatus error and nothing else
    hy::AccountSnapshot snapshot{};
    EXPECT_EQ(client_->fetch_account(snapshot), PrivateRestError::HttpStatus);
}

// --- the request ----------------------------------------------------------------------------------------------------------------------------

TEST_F(RawReadTest, FetchOrderRawSendsASignedGetOfTheOrderByItsClientOrderId) {
    wire(always(Reply::answer(200, "{}")));
    RawHttpAnswer out;
    ASSERT_EQ(client_->fetch_order_raw("BTCUSDT", kCoid, out), PrivateRestError::None);
    const std::vector<Request> seen = server_->requests();
    ASSERT_EQ(seen.size(), 1u);
    expect_signed_get(seen[0], "/api/v3/order",
                      "origClientOrderId=" + std::string(kCoid) + "&recvWindow=" + std::to_string(cfg_.recv_window_ms) + "&symbol=BTCUSDT");
}

TEST_F(RawReadTest, AClientOrderIdWithCharactersTheExchangeAllowsIsPercentEncodedAndStillSigned) {
    wire(always(Reply::answer(200, "{}")));
    RawHttpAnswer out;
    ASSERT_EQ(client_->fetch_order_raw("ETHUSDT", "HY:1/2.a_b-C", out), PrivateRestError::None);
    const std::vector<Request> seen = server_->requests();
    ASSERT_EQ(seen.size(), 1u);
    expect_signed_get(seen[0], "/api/v3/order",
                      "origClientOrderId=HY%3A1%2F2.a_b-C&recvWindow=" + std::to_string(cfg_.recv_window_ms) + "&symbol=ETHUSDT");
}

TEST_F(RawReadTest, FetchAllOrdersRawAsksForOneWindowOfOnePage) {
    wire(always(Reply::answer(200, "[]")));
    RawHttpAnswer out;
    ASSERT_EQ(client_->fetch_all_orders_raw("ETHUSDT", kStart, kEnd, 1000, out), PrivateRestError::None);
    const std::vector<Request> seen = server_->requests();
    ASSERT_EQ(seen.size(), 1u);
    expect_signed_get(seen[0], "/api/v3/allOrders",
                      "endTime=" + std::to_string(kEnd) + "&limit=1000&recvWindow=" + std::to_string(cfg_.recv_window_ms) +
                          "&startTime=" + std::to_string(kStart) + "&symbol=ETHUSDT");
}

TEST_F(RawReadTest, FetchOpenOrdersRawAsksForEveryOpenOrderOfTheAccountWithoutASymbol) {
    wire(always(Reply::answer(200, "[]")));
    RawHttpAnswer out;
    ASSERT_EQ(client_->fetch_open_orders_raw(out), PrivateRestError::None);
    const std::vector<Request> seen = server_->requests();
    ASSERT_EQ(seen.size(), 1u);
    expect_signed_get(seen[0], "/api/v3/openOrders", "recvWindow=" + std::to_string(cfg_.recv_window_ms));
    EXPECT_EQ(seen[0].target.find("symbol"), std::string::npos);
}

TEST_F(RawReadTest, FetchAccountRawOmitsZeroBalances) {
    wire(always(Reply::answer(200, "{}")));
    RawHttpAnswer out;
    ASSERT_EQ(client_->fetch_account_raw(out), PrivateRestError::None);
    const std::vector<Request> seen = server_->requests();
    ASSERT_EQ(seen.size(), 1u);
    expect_signed_get(seen[0], "/api/v3/account", "omitZeroBalances=true&recvWindow=" + std::to_string(cfg_.recv_window_ms));
}

TEST_F(RawReadTest, TheRecvWindowIsTheClientsOwnConfiguration) {
    cfg_.recv_window_ms = 7777;
    wire(always(Reply::answer(200, "{}")));
    RawHttpAnswer out;
    ASSERT_EQ(client_->fetch_open_orders_raw(out), PrivateRestError::None);
    const std::vector<Request> seen = server_->requests();
    ASSERT_EQ(seen.size(), 1u);
    // wire() copies cfg_ into the client when it builds it, after the port is known: the window set above is the one signed
    EXPECT_NE(seen[0].target.find("recvWindow=7777&"), std::string::npos) << seen[0].target;
}

// --- a request that cannot be made locally is not made at all ------------------------------------------------------------------------------------

TEST_F(RawReadTest, WithoutAFreshClockNothingIsSignedOrSentAndNoBudgetIsSpent) {
    // the lane holds exactly one call of each read in turn: if the refused call had burned budget, the next one would be refused too
    for (const NamedRead& read : reads()) {
        wire(always(Reply::answer(200, "{}")), /*fresh_clock=*/false, read.weight);
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::ClockNotFresh) << read.name;
        expect_sentinel(out);
        EXPECT_EQ(server_->request_count(), 0u) << read.name;
        BinancePrivateRestClientTestHooks::publish_fresh_clock(*client_);
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::None) << read.name << ": the refusal cost nothing";
    }
}

TEST_F(RawReadTest, AnArgumentTheExchangeCouldNotMeanIsRefusedBeforeAnythingIsSent) {
    wire(always(Reply::answer(200, "{}")));
    RawHttpAnswer out = sentinel();
    const std::string long_symbol(32, 'A');
    const std::string long_coid(hy::kClientOrderIdLen + 1, 'a');
    for (const std::string_view symbol : {std::string_view(""), std::string_view("btcusdt"), std::string_view("BTC-USDT"),
                                          std::string_view("BTCUSDT&x=1"), std::string_view("BTC USDT"), std::string_view(long_symbol)}) {
        EXPECT_EQ(client_->fetch_order_raw(symbol, kCoid, out), PrivateRestError::InvalidConfig) << symbol;
        EXPECT_EQ(client_->fetch_all_orders_raw(symbol, kStart, kEnd, 1000, out), PrivateRestError::InvalidConfig) << symbol;
    }
    EXPECT_EQ(client_->fetch_order_raw("BTCUSDT", "", out), PrivateRestError::InvalidConfig);
    EXPECT_EQ(client_->fetch_order_raw("BTCUSDT", long_coid, out), PrivateRestError::InvalidConfig);
    EXPECT_EQ(client_->fetch_all_orders_raw("BTCUSDT", 0, kEnd, 1000, out), PrivateRestError::InvalidConfig);
    EXPECT_EQ(client_->fetch_all_orders_raw("BTCUSDT", -5, kEnd, 1000, out), PrivateRestError::InvalidConfig);
    EXPECT_EQ(client_->fetch_all_orders_raw("BTCUSDT", kEnd, kStart, 1000, out), PrivateRestError::InvalidConfig) << "end before start";
    EXPECT_EQ(client_->fetch_all_orders_raw("BTCUSDT", kStart, kStart - 1, 1000, out), PrivateRestError::InvalidConfig)
        << "end one millisecond before start";
    EXPECT_EQ(client_->fetch_all_orders_raw("BTCUSDT", kStart, kEnd, 0, out), PrivateRestError::InvalidConfig);
    EXPECT_EQ(client_->fetch_all_orders_raw("BTCUSDT", kStart, kEnd, 1001, out), PrivateRestError::InvalidConfig);
    EXPECT_EQ(server_->request_count(), 0u);
    expect_sentinel(out);
}

TEST_F(RawReadTest, TheEdgesOfWhatIsAcceptedAreAccepted) {
    wire(always(Reply::answer(200, "{}")));
    RawHttpAnswer out;
    const std::string longest_coid(hy::kClientOrderIdLen, 'a');
    EXPECT_EQ(client_->fetch_order_raw("BTCUSDT", longest_coid, out), PrivateRestError::None);
    EXPECT_EQ(client_->fetch_order_raw("BTCUSDT", "a", out), PrivateRestError::None);
    EXPECT_EQ(client_->fetch_all_orders_raw("BTCUSDT", kStart, kStart, 1, out), PrivateRestError::None) << "start == end, one entry";
    EXPECT_EQ(client_->fetch_all_orders_raw("BTCUSDT", 1, kEnd, 1000, out), PrivateRestError::None);
    EXPECT_EQ(server_->request_count(), 4u);
}

TEST_F(RawReadTest, AHostThePolicyDoesNotAllowIsRefusedBeforeAnythingIsSent) {
    hy::EndpointAllowlist al{};
    al.hosts[0] = "not-the-bound-host.example";
    al.count = 1;
    wire(always(Reply::answer(200, "{}")), true, 0, hy::EnvironmentBindingTestHooks::with_allowlist(EnvironmentBinding::testnet(), al));
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::InvalidConfig) << read.name;
        expect_sentinel(out);
    }
    std::int64_t ms = 11;
    EXPECT_EQ(client_->read_server_time(ms), PrivateRestError::InvalidConfig);
    EXPECT_EQ(ms, 11);
    EXPECT_EQ(server_->request_count(), 0u);
}

TEST_F(RawReadTest, AZeroPolicyCeilingRefusesEveryRawReadBeforeAnythingIsSent) {
    wire(always(Reply::answer(200, "{}")), true, 0, hy::EnvironmentBindingTestHooks::with_max_response_bytes(EnvironmentBinding::testnet(), 0));
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::InvalidConfig) << read.name;
        expect_sentinel(out);
    }
    EXPECT_EQ(server_->request_count(), 0u);
}

TEST_F(RawReadTest, AClientWithoutCredentialsSignsAndSendsNothing) {
    wire(always(Reply::answer(200, "{}")), true, 0, EnvironmentBinding::testnet(), /*with_credentials=*/false);
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::SigningFailed) << read.name;
        expect_sentinel(out);
    }
    EXPECT_EQ(server_->request_count(), 0u);
}

// --- the local rate-limit budget -----------------------------------------------------------------------------------------------------------------
//
// The tracker's Reconciliation lane holds exactly `lane` weight. A lane one short of a read's weight refuses its first call; a lane of exactly its
// weight serves one call and refuses the next; the Strategy lane is never touched. allOrders (20) and openOrders (80) have no PrivateRestEndpoint
// slot, so their weights are the explicit ones in binance_private_rest.hpp.

TEST_F(RawReadTest, EachReadIsChargedItsWeightOnTheReconciliationLane) {
    for (const NamedRead& read : reads()) {
        {
            wire(always(Reply::answer(200, "{}")), true, read.weight - 1);
            RawHttpAnswer out = sentinel();
            EXPECT_EQ(read.call(*client_, out), PrivateRestError::RateLimitExhausted) << read.name << ": one short of its weight";
            EXPECT_EQ(server_->request_count(), 0u) << read.name;
            expect_sentinel(out);
        }
        {
            wire(always(Reply::answer(200, "{}")), true, read.weight);
            RawHttpAnswer out;
            EXPECT_EQ(read.call(*client_, out), PrivateRestError::None) << read.name << ": exactly its weight";
            EXPECT_FALSE(reconciliation_lane_open()) << read.name;
            EXPECT_EQ(read.call(*client_, out), PrivateRestError::RateLimitExhausted) << read.name << ": the lane is spent";
            EXPECT_EQ(server_->request_count(), 1u) << read.name << ": the refused call never reached the network";
            EXPECT_TRUE(strategy_lane_open()) << read.name;
        }
        {
            wire(always(Reply::answer(200, "{}")), true, read.weight * 2);
            RawHttpAnswer out;
            EXPECT_EQ(read.call(*client_, out), PrivateRestError::None) << read.name;
            EXPECT_EQ(read.call(*client_, out), PrivateRestError::None) << read.name;
            EXPECT_EQ(read.call(*client_, out), PrivateRestError::RateLimitExhausted) << read.name;
            EXPECT_EQ(server_->request_count(), 2u) << read.name;
        }
    }
}

TEST_F(RawReadTest, EachReadAlsoCountsAsOneRawRequest) {
    EXPECT_TRUE(tracker_.configure(100'000, 0, 0, 0, 100'000, 0));  // weight ample, raw requests none
    wire(always(Reply::answer(200, "{}")));
    client_ = std::make_unique<BinancePrivateRestClient>(EnvironmentBinding::testnet(), make_creds(), &tracker_, hy::EndpointWeightTable{}, cfg_);
    BinancePrivateRestClientTestHooks::use_loopback(*client_, fixture_path("test_leaf_cert_testnet_host.pem"));
    BinancePrivateRestClientTestHooks::publish_fresh_clock(*client_);
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::RateLimitExhausted) << read.name;
    }
    EXPECT_EQ(server_->request_count(), 0u);
}

// --- how much a read may be answered with -----------------------------------------------------------------------------------------------------

TEST_F(RawReadTest, AnOrderListPageMayBeAMebibyteAndAPointOrAccountAnswerMayNotBeMoreThanTheSignedJsonCap) {
    const std::size_t signed_json_cap = hy::response_cap_bytes(hy::ResponseKind::SignedJson);
    const std::size_t order_list_cap = hy::response_cap_bytes(hy::ResponseKind::OrderList);
    ASSERT_LT(signed_json_cap, order_list_cap);
    const std::string between(signed_json_cap + 1, 'x');  // too much for an order or an account, fine for a page of orders

    wire(always(Reply::answer(200, between)));
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out = sentinel();
        const PrivateRestError e = read.call(*client_, out);
        if (std::string_view(read.name) == "allOrders" || std::string_view(read.name) == "openOrders") {
            EXPECT_EQ(e, PrivateRestError::None) << read.name << ": a page of orders has the larger cap";
            EXPECT_EQ(out.body.size(), between.size()) << read.name;
        } else {
            EXPECT_EQ(e, PrivateRestError::Read) << read.name << ": over the SignedJson cap is a failed read, not a truncated answer";
            expect_sentinel(out);
        }
    }
}

TEST_F(RawReadTest, APageOfExactlyTheOrderListCapIsReadAndOneByteMoreIsAFailedRead) {
    const std::size_t cap = hy::response_cap_bytes(hy::ResponseKind::OrderList);
    {
        wire(always(Reply::answer(200, std::string(cap, 'x'))));
        RawHttpAnswer out;
        ASSERT_EQ(client_->fetch_all_orders_raw("BTCUSDT", kStart, kEnd, 1000, out), PrivateRestError::None);
        EXPECT_EQ(out.body.size(), cap);
        RawHttpAnswer open;
        EXPECT_EQ(client_->fetch_open_orders_raw(open), PrivateRestError::None);
        EXPECT_EQ(open.body.size(), cap);
    }
    {
        wire(always(Reply::answer(200, std::string(cap + 1, 'x'))));
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(client_->fetch_all_orders_raw("BTCUSDT", kStart, kEnd, 1000, out), PrivateRestError::Read);
        expect_sentinel(out);
        RawHttpAnswer open = sentinel();
        EXPECT_EQ(client_->fetch_open_orders_raw(open), PrivateRestError::Read);
        expect_sentinel(open);
    }
}

TEST_F(RawReadTest, AnAnswerOfExactlyTheSignedJsonCapIsReadAndOneByteMoreIsNot) {
    const std::size_t cap = hy::response_cap_bytes(hy::ResponseKind::SignedJson);
    {
        wire(always(Reply::answer(200, std::string(cap, 'x'))));
        RawHttpAnswer out;
        ASSERT_EQ(client_->fetch_order_raw("BTCUSDT", kCoid, out), PrivateRestError::None);
        EXPECT_EQ(out.body.size(), cap);
        RawHttpAnswer account;
        EXPECT_EQ(client_->fetch_account_raw(account), PrivateRestError::None);
    }
    {
        wire(always(Reply::answer(200, std::string(cap + 1, 'x'))));
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(client_->fetch_order_raw("BTCUSDT", kCoid, out), PrivateRestError::Read);
        expect_sentinel(out);
        RawHttpAnswer account = sentinel();
        EXPECT_EQ(client_->fetch_account_raw(account), PrivateRestError::Read);
        expect_sentinel(account);
    }
}

TEST_F(RawReadTest, ABindingWithASmallerResponseCeilingShrinksTheCapOfEveryKind) {
    // the permit takes the smaller of the kind's cap and the policy's ceiling
    constexpr std::size_t kCeiling = 4096;
    wire(always(Reply::answer(200, std::string(kCeiling + 1, 'x'))), true, 0,
         hy::EnvironmentBindingTestHooks::with_max_response_bytes(EnvironmentBinding::testnet(), kCeiling));
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::Read) << read.name;
        expect_sentinel(out);
    }
    wire(always(Reply::answer(200, std::string(kCeiling, 'x'))), true, 0,
         hy::EnvironmentBindingTestHooks::with_max_response_bytes(EnvironmentBinding::testnet(), kCeiling));
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out;
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::None) << read.name;
        EXPECT_EQ(out.body.size(), kCeiling) << read.name;
    }
}

// --- a request that did not complete is a failure, never an answer ----------------------------------------------------------------------------------

TEST_F(RawReadTest, AnExchangeThatNeverAnswersEndsInAReadTimeout) {
    cfg_.read_timeout_ms = 400;
    for (const NamedRead& read : reads()) {
        wire(always(Reply::silent()));
        RawHttpAnswer out = sentinel();
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::Read) << read.name;
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_GE(waited, 300) << read.name << ": it waited for the answer";
        EXPECT_LT(waited, 3000) << read.name << ": and gave up at the read timeout, not at the (longer) write or connect one";
        expect_sentinel(out);
        EXPECT_EQ(server_->request_count(), 1u) << read.name;
    }
}

TEST_F(RawReadTest, ABodyThatIsCutOffInTransitIsNotAnAnswerAndNeitherIsAConnectionThatIsDropped) {
    for (const NamedRead& read : reads()) {
        wire(always(Reply::cut_short(R"([{"symbol":"BTCUSDT","orderId":1,"clientOrderId":"HY-1"},{"symbol":"BTCUSDT"})")));
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::Read) << read.name << ": cut short";
        expect_sentinel(out);

        wire(always(Reply::drop()));
        RawHttpAnswer dropped = sentinel();
        EXPECT_EQ(read.call(*client_, dropped), PrivateRestError::Read) << read.name << ": dropped";
        expect_sentinel(dropped);
    }
}

TEST_F(RawReadTest, NothingListeningIsAConnectFailureAndNotAnAnswer) {
    wire(always(Reply::answer(200, "{}")));
    cfg_.port = "1";  // nothing listens there
    cfg_.connect_timeout_ms = 1500;
    client_ = std::make_unique<BinancePrivateRestClient>(EnvironmentBinding::testnet(), make_creds(), nullptr, hy::EndpointWeightTable{}, cfg_);
    BinancePrivateRestClientTestHooks::use_loopback(*client_, fixture_path("test_leaf_cert_testnet_host.pem"));
    BinancePrivateRestClientTestHooks::publish_fresh_clock(*client_);
    RawHttpAnswer out = sentinel();
    const PrivateRestError e = client_->fetch_open_orders_raw(out);
    EXPECT_EQ(e, PrivateRestError::Connect);
    expect_sentinel(out);
}

TEST_F(RawReadTest, AServerThatNeverCompletesTheHandshakeEndsInATlsHandshakeFailure) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    port_override_ = std::to_string(blackhole.port());
    cfg_.handshake_timeout_ms = 300;
    wire(always(Reply::answer(200, "{}")));
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out = sentinel();
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::TlsHandshake) << read.name;
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_GE(waited, 200) << read.name;
        EXPECT_LT(waited, 3000) << read.name << ": it gave up at the handshake timeout, not at the (longer) connect one";
        expect_sentinel(out);
    }
    EXPECT_EQ(server_->request_count(), 0u);
}

TEST_F(RawReadTest, ACertificateForAnotherHostIsRefusedEvenIfTheClientTrustsItsIssuer) {
    // the fixture's other leaf certificate is issued by the fixture's root CA for wrong-san.test.invalid: with that root trusted the chain is
    // fine, and the only thing wrong is the name -- so a client that does not verify it would go on and send the request
    cert_file_ = "test_leaf_cert.pem";
    key_file_ = "test_leaf_key.pem";
    trust_file_ = "test_ca_cert.pem";
    wire(always(Reply::answer(200, "{}")));
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::TlsHandshake) << read.name;
        expect_sentinel(out);
    }
    EXPECT_EQ(server_->request_count(), 0u) << "the request is only written after the name was verified";
}

TEST_F(RawReadTest, ACertificateThatIsNotTrustedIsRefused) {
    wire(always(Reply::answer(200, "{}")));
    // the same exchange, a client that trusts nothing extra
    client_ = std::make_unique<BinancePrivateRestClient>(EnvironmentBinding::testnet(), make_creds(), nullptr, hy::EndpointWeightTable{}, cfg_);
    BinancePrivateRestClientTestHooks::use_loopback(*client_);
    BinancePrivateRestClientTestHooks::publish_fresh_clock(*client_);
    RawHttpAnswer out = sentinel();
    EXPECT_EQ(client_->fetch_open_orders_raw(out), PrivateRestError::TlsHandshake);
    expect_sentinel(out);
    EXPECT_EQ(server_->request_count(), 0u);
}

TEST_F(RawReadTest, TheHeaderSectionOfTheAnswerIsBoundedToo) {
    wire(always(Reply::answer_with_header_padding(200, "{}", 4096)));
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out;
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::None) << read.name << ": a header section of a few KiB is fine";
    }
    wire(always(Reply::answer_with_header_padding(200, "{}", 32 * 1024)));
    for (const NamedRead& read : reads()) {
        RawHttpAnswer out = sentinel();
        EXPECT_EQ(read.call(*client_, out), PrivateRestError::Read) << read.name << ": 32 KiB of headers is not (the cap is 8 KiB; Beast itself cannot build a single field above 64 KiB)";
        expect_sentinel(out);
    }
}

// --- read_server_time(): the clock sync, plus the exchange's own reading -----------------------------------------------------------------------------

TEST_F(RawReadTest, ReadServerTimeHandsOutTheExchangesReadingAndPublishesTheClockLikeSyncClock) {
    const auto served_ptr = std::make_shared<std::atomic<std::int64_t>>(0);
    std::atomic<std::int64_t>& served = *served_ptr;
    wire(
        [served_ptr](const Request& r) {
            if (r.target != "/api/v3/time") return Reply::answer(200, "{}");
            const std::int64_t now = real_now_ms() + 1500;  // the exchange's clock runs 1.5 s ahead
            served_ptr->store(now);
            return Reply::answer(200, "{\"serverTime\":" + std::to_string(now) + "}");
        },
        /*fresh_clock=*/false);

    RawHttpAnswer out = sentinel();
    EXPECT_EQ(client_->fetch_order_raw("BTCUSDT", kCoid, out), PrivateRestError::ClockNotFresh) << "no snapshot yet";

    std::int64_t ms = 11;
    ASSERT_EQ(client_->read_server_time(ms), PrivateRestError::None);
    EXPECT_NE(served.load(), 0);
    EXPECT_EQ(ms, served.load()) << "the exchange's own reading, exactly";

    EXPECT_EQ(client_->fetch_order_raw("BTCUSDT", kCoid, out), PrivateRestError::None) << "the snapshot read_server_time() published";
    const std::vector<Request> seen = server_->requests();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].method, "GET");
    EXPECT_EQ(seen[0].target, "/api/v3/time");
    EXPECT_TRUE(seen[0].api_key_header.empty()) << "the clock reading is unsigned and carries no key";
    // the order query was signed with the offset that reading measured: about the exchange's clock, 1.5 s ahead of the local one
    const std::string& target = seen[1].target;
    const std::size_t at = target.find("&timestamp=");
    ASSERT_NE(at, std::string::npos) << target;
    EXPECT_NEAR(static_cast<double>(std::stoll(target.substr(at + 11, 13))), static_cast<double>(real_now_ms() + 1500), 1000.0);
}

TEST_F(RawReadTest, ReadServerTimeLeavesItsOutputAloneOnEveryFailure) {
    {
        wire(always(Reply::answer(500, "oops")), false);
        std::int64_t ms = 11;
        EXPECT_EQ(client_->read_server_time(ms), PrivateRestError::HttpStatus);
        EXPECT_EQ(ms, 11);
    }
    {
        wire(always(Reply::answer(200, R"({"serverTime":"soon"})")), false);
        std::int64_t ms = 11;
        EXPECT_EQ(client_->read_server_time(ms), PrivateRestError::JsonParse);
        EXPECT_EQ(ms, 11);
    }
    {
        // an exchange clock ten minutes away from the local one is not something to calibrate against (SAFE-01 slice 4)
        wire(always(Reply::answer(200, hy::test_helpers::server_time_body_offset_by(600'000))), false);
        std::int64_t ms = 11;
        EXPECT_EQ(client_->read_server_time(ms), PrivateRestError::ClockSkewTooLarge);
        EXPECT_EQ(ms, 11);
        RawHttpAnswer out;
        EXPECT_EQ(client_->fetch_order_raw("BTCUSDT", kCoid, out), PrivateRestError::ClockNotFresh) << "and nothing was published";
    }
    {
        wire(always(Reply::drop()), false);
        std::int64_t ms = 11;
        EXPECT_NE(client_->read_server_time(ms), PrivateRestError::None);
        EXPECT_EQ(ms, 11);
    }
}

TEST_F(RawReadTest, ReadServerTimeCostsOneWeightLikeSyncClock) {
    wire(always(time_reply()), false, 2);
    std::int64_t ms = 0;
    EXPECT_EQ(client_->read_server_time(ms), PrivateRestError::None);
    EXPECT_EQ(client_->read_server_time(ms), PrivateRestError::None);
    EXPECT_EQ(client_->read_server_time(ms), PrivateRestError::RateLimitExhausted);
    EXPECT_EQ(server_->request_count(), 2u);
}

TEST_F(RawReadTest, SyncClockStillWorksAsBeforeAndNeedsNoOutput) {
    wire(always(time_reply()), false);
    EXPECT_EQ(client_->sync_clock(), PrivateRestError::None);
    RawHttpAnswer out;
    EXPECT_EQ(client_->fetch_open_orders_raw(out), PrivateRestError::None) << "sync_clock() published the snapshot";
}

// --- the raw reads and the trading path's reads do not share a failure mode ---------------------------------------------------------------------------

TEST_F(RawReadTest, TheTradingPathsReadStillTurnsANon200IntoAnErrorWithoutABody) {
    wire(always(Reply::answer(400, R"({"code":-2013})")));
    hy::AccountSnapshot snapshot{};
    EXPECT_EQ(client_->fetch_account(snapshot), PrivateRestError::HttpStatus);
}
