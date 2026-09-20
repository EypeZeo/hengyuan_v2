// 批次 6 6b-0f-1: binance_klines_rest.hpp tests -- fully local, no real network.
//
// TlsResponseAcceptor completes a real TLS handshake, reads a real HTTP request and answers with a
// canned status + body, so the success path (the exact request line/headers we send, status
// handling, body -> codec) runs end to end; the blackhole fixtures cover the timeout stages.
// HY_TEST_FIXTURE_DIR is the CMake-supplied fixtures directory (same as test_binance_rest_snapshot).
//
// The fixture certificate is the dedicated one whose SAN is testnet.binance.vision, so every test
// below verifies and sends the REAL testnet host name while the connection itself is routed to the
// local fixture via connect_host_override -- the same shape production has, minus the network.

#include <gtest/gtest.h>
#include <hengyuan/binance_klines_rest.hpp>

#include "test_helpers/blackhole_acceptor.hpp"
#include "test_helpers/tls_response_acceptor.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <ostream>
#include <string>

// Readable failure output: both enums are uint8_t-backed, which gtest would otherwise print as raw
// bytes. ADL finds these because they live next to the types.
namespace hy {
void PrintTo(PublicRestError e, std::ostream* os) { *os << public_rest_error_name(e); }
void PrintTo(KlinesParseError e, std::ostream* os) { *os << klines_parse_error_name(e); }
}  // namespace hy

using hy::fetch_klines_backfill;
using hy::fetch_public_body;
using hy::KlineBackfill;
using hy::KlinesParseError;
using hy::kKlinesClosedBarMarginMs;
using hy::make_klines_target;
using hy::PublicRestConfig;
using hy::PublicRestError;
using hy::test_helpers::PlainBlackholeAcceptor;
using hy::test_helpers::TlsBlackholeAcceptor;
using hy::test_helpers::TlsResponseAcceptor;

namespace {

constexpr std::int64_t kHour = 3'600'000;
constexpr std::int64_t kFarFuture = 10'000'000'000'000LL;  // every synthetic bar is "closed" by default

std::string fixture_path(const char* filename) {
    return std::string(HY_TEST_FIXTURE_DIR) + "/" + filename;
}

// One Binance kline row with a sane bar (low 9, high 12, open 10, close 11).
std::string row(std::int64_t open_time, std::int64_t close_time) {
    return "[" + std::to_string(open_time) + ",\"10\",\"12\",\"9\",\"11\",\"5\"," + std::to_string(close_time) +
           ",\"1.0\",3,\"1.0\",\"1.0\",\"0\"]";
}

// n contiguous bars of `span` ms starting at `first_open`.
std::string bars(std::size_t n, std::int64_t span = kHour, std::int64_t first_open = 0) {
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

// The real testnet host (verified against the fixture cert's SAN, sent as Host:) with the socket
// routed to the local fixture.
PublicRestConfig testnet_cfg(unsigned short port) {
    PublicRestConfig cfg;
    cfg.host = "testnet.binance.vision";
    cfg.port = std::to_string(port);
    cfg.extra_trusted_ca_pem_path = fixture_path("test_leaf_cert_testnet_host.pem");
    cfg.connect_host_override = "127.0.0.1";
    return cfg;
}

std::unique_ptr<TlsResponseAcceptor> make_server(int status, std::string body) {
    return std::make_unique<TlsResponseAcceptor>(fixture_path("test_leaf_cert_testnet_host.pem"),
                                                  fixture_path("test_leaf_key_testnet_host.pem"), status,
                                                  std::move(body));
}

std::function<std::int64_t()> clock_at(std::int64_t now) {
    return [now]() -> std::int64_t { return now; };
}

hy::KlinesFetchResult fetch_1h(const PublicRestConfig& cfg, std::int64_t now, int limit = 6) {
    return fetch_klines_backfill("BTCUSDT", "1h", limit, /*symbol_id=*/7, cfg, clock_at(now), shared_out());
}

}  // namespace

// The margin is the safety property, so its size is pinned here rather than only used symbolically
// by the boundary tests below (which would follow any value, including 0).
static_assert(kKlinesClosedBarMarginMs >= 500, "a near-zero margin defeats the point of having one");
static_assert(kKlinesClosedBarMarginMs <= 5000, "a huge margin would discard whole short-interval bars");

// --- config & target: no network ---------------------------------------------------------------

TEST(BinanceKlinesRestTarget, ThereIsNoDefaultHostSoProductionCannotBeReachedByOmission) {
    const PublicRestConfig cfg;
    EXPECT_TRUE(cfg.host.empty());
    EXPECT_EQ(cfg.port, "443");
    EXPECT_TRUE(cfg.extra_trusted_ca_pem_path.empty());
    EXPECT_TRUE(cfg.connect_host_override.empty());
}

TEST(BinanceKlinesRestTarget, BuildsTheExactDocumentedQuery) {
    EXPECT_EQ(make_klines_target("BTCUSDT", "1h", 100), "/api/v3/klines?symbol=BTCUSDT&interval=1h&limit=100");
    EXPECT_EQ(make_klines_target("ETHUSDT", "15m", 1), "/api/v3/klines?symbol=ETHUSDT&interval=15m&limit=1");
    EXPECT_EQ(make_klines_target("BTCUSDT", "1M", 1000), "/api/v3/klines?symbol=BTCUSDT&interval=1M&limit=1000");
}

TEST(BinanceKlinesRestTarget, RejectsEverythingItMustNotSend) {
    for (const char* bad_symbol : {"btcusdt", "BTC/USDT", "BTC USDT", "BTC\r\nUSDT", "BTCUSDT&limit=1", ""}) {
        EXPECT_TRUE(make_klines_target(bad_symbol, "1h", 10).empty()) << "symbol=" << bad_symbol;
    }
    EXPECT_TRUE(make_klines_target(std::string(21, 'A'), "1h", 10).empty());  // > 20 chars

    for (const char* bad_interval : {"", "7m", "1H", "1m&limit=1", "1 h", "1h\r\n"}) {
        EXPECT_TRUE(make_klines_target("BTCUSDT", bad_interval, 10).empty()) << "interval=" << bad_interval;
    }
    for (int bad_limit : {0, -1, 1001, 100000}) {
        EXPECT_TRUE(make_klines_target("BTCUSDT", "1h", bad_limit).empty()) << "limit=" << bad_limit;
    }
}

// --- fetch_public_body(): refuse before touching the network -------------------------------------

TEST(BinanceKlinesRestValidation, MalformedTargetsNeverReachTheWire) {
    PlainBlackholeAcceptor blackhole;
    PublicRestConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());

    // A CR/LF or space in the target would be serialised verbatim into OUR request line/headers.
    for (const char* bad_target : {"", "api/v3/klines", "/api/v3/klines?symbol=A B", "/x\r\nHost: evil",
                                   "/x\nY: z", "/x\x7f"}) {
        const auto r = fetch_public_body(bad_target, cfg);
        EXPECT_EQ(r.error, PublicRestError::InvalidConfig) << "target=" << bad_target;
        EXPECT_TRUE(r.body.empty());
    }
    EXPECT_EQ(blackhole.accepted_connections(), 0u);
}

TEST(BinanceKlinesRestValidation, MalformedHostOrPortNeverReachesTheWire) {
    PlainBlackholeAcceptor blackhole;
    for (const char* bad_host : {"", "bad host", "a/b", "host\r\nX: y", "testnet.binance.vision:443"}) {
        PublicRestConfig cfg;
        cfg.host = bad_host;
        cfg.port = std::to_string(blackhole.port());
        EXPECT_EQ(fetch_public_body("/api/v3/klines?symbol=BTCUSDT&interval=1h&limit=2", cfg).error,
                  PublicRestError::InvalidConfig)
            << "host=" << bad_host;
    }
    PublicRestConfig too_long;
    too_long.host = std::string(254, 'a');
    too_long.port = std::to_string(blackhole.port());
    EXPECT_EQ(fetch_public_body("/x", too_long).error, PublicRestError::InvalidConfig);

    PublicRestConfig no_port;
    no_port.host = "127.0.0.1";
    no_port.port = "";
    EXPECT_EQ(fetch_public_body("/x", no_port).error, PublicRestError::InvalidConfig);

    EXPECT_EQ(blackhole.accepted_connections(), 0u);
}

TEST(BinanceKlinesRestValidation, FetchWithABadSymbolIntervalOrLimitNeverReachesTheWire) {
    PlainBlackholeAcceptor blackhole;
    PublicRestConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());

    const auto now = clock_at(kFarFuture);
    EXPECT_EQ(fetch_klines_backfill("btcusdt", "1h", 5, 1, cfg, now, shared_out()).transport,
              PublicRestError::InvalidConfig);
    EXPECT_EQ(fetch_klines_backfill("BTCUSDT", "7m", 5, 1, cfg, now, shared_out()).transport,
              PublicRestError::InvalidConfig);
    EXPECT_EQ(fetch_klines_backfill("BTCUSDT", "1h", 0, 1, cfg, now, shared_out()).transport,
              PublicRestError::InvalidConfig);
    EXPECT_EQ(fetch_klines_backfill("BTCUSDT", "1h", 1001, 1, cfg, now, shared_out()).transport,
              PublicRestError::InvalidConfig);
    EXPECT_EQ(blackhole.accepted_connections(), 0u);
}

TEST(BinanceKlinesRestValidation, AnUnusableClockIsRefusedNotTreatedAsEverythingIsForming) {
    PlainBlackholeAcceptor blackhole;
    PublicRestConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());

    // 0 is what an unsynchronised clock reports; the margin itself is the exact boundary (the
    // subtraction would land on 0, which no real bar can be older than); INT64_MIN would overflow it.
    for (std::int64_t bad_now : {std::int64_t{0}, std::int64_t{-5}, kKlinesClosedBarMarginMs,
                                  std::numeric_limits<std::int64_t>::min()}) {
        const auto r = fetch_1h(cfg, bad_now);
        EXPECT_EQ(r.transport, PublicRestError::InvalidConfig) << "now=" << bad_now;
        EXPECT_FALSE(r.ok());
    }
    const auto no_clock = fetch_klines_backfill("BTCUSDT", "1h", 5, 1, cfg, std::function<std::int64_t()>{},
                                                 shared_out());
    EXPECT_EQ(no_clock.transport, PublicRestError::InvalidConfig);
    EXPECT_EQ(blackhole.accepted_connections(), 0u);
}

// --- success path over a real TLS round trip -----------------------------------------------------

TEST(BinanceKlinesRestFetch, HappyPathParsesBarsAndSendsExactlyTheExpectedPublicRequest) {
    auto server = make_server(200, bars(5));
    const auto r = fetch_1h(testnet_cfg(server->port()), kFarFuture, /*limit=*/7);

    ASSERT_TRUE(r.ok()) << "transport=" << hy::public_rest_error_name(r.transport)
                        << " parse=" << hy::klines_parse_error_name(r.parse);
    EXPECT_EQ(r.http_status, 200);
    ASSERT_EQ(shared_out().count, 5u);
    EXPECT_FALSE(shared_out().dropped_unclosed_tail);
    EXPECT_EQ(shared_out().bars[0].open_time_ms, 0);
    EXPECT_EQ(shared_out().bars[4].close_time_ms, 5 * kHour - 1);
    EXPECT_EQ(shared_out().bars[2].symbol_id, 7u);  // stamped by the caller, not parsed
    EXPECT_TRUE(shared_out().bars[2].is_closed);

    const auto reqs = server->requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target, "/api/v3/klines?symbol=BTCUSDT&interval=1h&limit=7");
    // Host: is the VERIFIED host, not the connect override (which is 127.0.0.1 here).
    EXPECT_EQ(reqs[0].host_header, "testnet.binance.vision");
    // PUBLIC REST: no credential header may ever ride along.
    EXPECT_TRUE(reqs[0].api_key_header.empty());
}

TEST(BinanceKlinesRestFetch, ForwardsTheRequestedSymbolIntervalAndLimit) {
    constexpr std::int64_t k15m = 900'000;
    auto server = make_server(200, bars(1000, k15m));
    const auto r = fetch_klines_backfill("ETHUSDT", "15m", 1000, 9, testnet_cfg(server->port()),
                                          clock_at(kFarFuture), shared_out());
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(shared_out().count, 1000u);
    const auto reqs = server->requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].target, "/api/v3/klines?symbol=ETHUSDT&interval=15m&limit=1000");
}

TEST(BinanceKlinesRestFetch, StaleBarsFromAnEarlierFetchNeverSurviveAFailedOne) {
    auto ok_server = make_server(200, bars(5));
    ASSERT_TRUE(fetch_1h(testnet_cfg(ok_server->port()), kFarFuture).ok());
    ASSERT_EQ(shared_out().count, 5u);

    auto failing_server = make_server(503, bars(5));
    const auto r = fetch_1h(testnet_cfg(failing_server->port()), kFarFuture);
    EXPECT_EQ(r.transport, PublicRestError::HttpStatus);
    EXPECT_EQ(shared_out().count, 0u);
    EXPECT_FALSE(shared_out().dropped_unclosed_tail);
}

// --- which bars are closed ---------------------------------------------------------------------------

TEST(BinanceKlinesRestClosedBars, TheStillFormingLastBarIsDroppedAndReported) {
    // 5 hourly bars; now sits 30 minutes into the last one.
    auto server = make_server(200, bars(5));
    const auto r = fetch_1h(testnet_cfg(server->port()), 4 * kHour + kHour / 2);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(shared_out().count, 4u);
    EXPECT_TRUE(shared_out().dropped_unclosed_tail);
    EXPECT_EQ(shared_out().bars[3].close_time_ms, 4 * kHour - 1);
}

TEST(BinanceKlinesRestClosedBars, ABarThatJustClosedInsideTheSafetyMarginIsStillTreatedAsForming) {
    // Three hourly bars; the last one's close_time is C = 3h - 1. It counts as closed only once
    // now - margin > C, i.e. now >= C + margin + 1.
    constexpr std::int64_t C = 3 * kHour - 1;
    auto server = make_server(200, bars(3));
    const PublicRestConfig cfg = testnet_cfg(server->port());

    // now == C + margin  ->  exactly at the boundary: still dropped.
    const auto at_boundary = fetch_1h(cfg, C + kKlinesClosedBarMarginMs);
    ASSERT_TRUE(at_boundary.ok());
    EXPECT_EQ(shared_out().count, 2u);
    EXPECT_TRUE(shared_out().dropped_unclosed_tail);

    // one millisecond later it is accepted.
    const auto past_boundary = fetch_1h(cfg, C + kKlinesClosedBarMarginMs + 1);
    ASSERT_TRUE(past_boundary.ok());
    EXPECT_EQ(shared_out().count, 3u);
    EXPECT_FALSE(shared_out().dropped_unclosed_tail);
}

// The clock must be sampled BEFORE the request leaves. Sampled after the response, a bar that was
// still forming (partial OHLCV) when the server built the response but closed during the transfer
// would be accepted as final -- silent, unrepairable indicator corruption. Sampled before, the worst
// case is dropping a bar that had in fact just closed, which the recovery protocol re-joins.
TEST(BinanceKlinesRestClosedBars, TheClockIsReadExactlyOnceAndBeforeTheRequestGoesOut) {
    auto server = make_server(200, bars(3));
    int calls = 0;
    std::size_t requests_seen_at_call = 999;
    const auto clock = [&]() -> std::int64_t {
        ++calls;
        requests_seen_at_call = server->requests().size();
        return kFarFuture;
    };

    const auto r = fetch_klines_backfill("BTCUSDT", "1h", 4, 7, testnet_cfg(server->port()), clock, shared_out());
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(requests_seen_at_call, 0u) << "the clock was read after the request had already been served";
    EXPECT_EQ(server->requests().size(), 1u);
}

TEST(BinanceKlinesRestClosedBars, OnlyAFormingCandleIsAParseErrorNotASilentEmptySuccess) {
    auto server = make_server(200, bars(1));
    const auto r = fetch_1h(testnet_cfg(server->port()), kHour / 2);  // inside the only bar
    EXPECT_EQ(r.transport, PublicRestError::None);
    EXPECT_EQ(r.parse, KlinesParseError::Empty);
    EXPECT_FALSE(r.ok());
}

TEST(BinanceKlinesRestClosedBars, TheSmallestAcceptedClockStillReachesTheWire) {
    // kKlinesClosedBarMarginMs + 1 is the first non-refused reading: the request must actually be
    // made (and, with nothing older than 1 ms, simply find no closed bar).
    auto server = make_server(200, bars(1));
    const auto r = fetch_1h(testnet_cfg(server->port()), kKlinesClosedBarMarginMs + 1);
    EXPECT_EQ(r.transport, PublicRestError::None);
    EXPECT_EQ(r.parse, KlinesParseError::Empty);
    EXPECT_EQ(server->requests().size(), 1u);
}

// --- HTTP status ---------------------------------------------------------------------------------------

TEST(BinanceKlinesRestHttp, AnyNon200IsRefusedWithItsCodeAndTheBodyIsNeverParsed) {
    // 418/429 are Binance's rate-limit/ban codes; a supervisor must be able to tell them from a
    // plain 5xx, so the code has to survive. The body is a perfectly valid backfill on purpose.
    for (int status : {301, 302, 400, 403, 404, 418, 429, 500, 502, 503}) {
        auto server = make_server(status, bars(5));
        const auto r = fetch_1h(testnet_cfg(server->port()), kFarFuture);
        EXPECT_EQ(r.transport, PublicRestError::HttpStatus) << "status=" << status;
        EXPECT_EQ(r.http_status, status);
        EXPECT_EQ(r.parse, KlinesParseError::None);  // never reached
        EXPECT_FALSE(r.ok());
        EXPECT_EQ(shared_out().count, 0u) << "status=" << status;
    }
}

TEST(BinanceKlinesRestHttp, FetchPublicBodyReturnsTheBodyOnlyOnSuccess) {
    auto ok_server = make_server(200, R"({"hello":"world"})");
    const auto ok = fetch_public_body("/x", testnet_cfg(ok_server->port()));
    EXPECT_EQ(ok.error, PublicRestError::None);
    EXPECT_EQ(ok.http_status, 200);
    EXPECT_EQ(ok.body, R"({"hello":"world"})");

    auto bad_server = make_server(429, R"({"code":-1003,"msg":"Too many requests"})");
    const auto bad = fetch_public_body("/x", testnet_cfg(bad_server->port()));
    EXPECT_EQ(bad.error, PublicRestError::HttpStatus);
    EXPECT_EQ(bad.http_status, 429);
    EXPECT_TRUE(bad.body.empty());  // an error body is not returned as if it were data
}

// --- a 200 that is not klines --------------------------------------------------------------------------

TEST(BinanceKlinesRestParse, ABody200ThatIsNotKlinesIsAParseErrorNotATransportError) {
    struct Case {
        const char* what;
        std::string body;
        KlinesParseError expected;
    };
    const std::string gap = "[" + row(0, kHour - 1) + "," + row(2 * kHour, 3 * kHour - 1) + "]";  // 1h hole
    const Case cases[] = {
        {"a CDN error page served as 200", "<html><body>502 Bad Gateway</body></html>", KlinesParseError::MalformedJson},
        {"a JSON object instead of an array", R"({"code":-1121,"msg":"Invalid symbol."})",
         KlinesParseError::MalformedJson},
        {"an empty array", "[]", KlinesParseError::Empty},
        {"a hole between two bars", gap, KlinesParseError::GapInside},
    };
    for (const Case& c : cases) {
        auto server = make_server(200, c.body);
        const auto r = fetch_1h(testnet_cfg(server->port()), kFarFuture);
        EXPECT_EQ(r.transport, PublicRestError::None) << c.what;
        EXPECT_EQ(r.parse, c.expected) << c.what;
        EXPECT_FALSE(r.ok()) << c.what;
    }
}

// A server-side cut with a self-consistent Content-Length (so the transport is content) must not
// come out as a shorter, apparently complete backfill. The exhaustive every-prefix property lives
// in test_binance_klines_codec.cpp; this pins that the wiring surfaces it.
TEST(BinanceKlinesRestParse, ATruncatedBodyIsNeverAcceptedAsAShorterBackfill) {
    const std::string full = bars(3);
    const std::size_t after_first_row = full.find("],[") + 2;  // "[row1]," -- cut exactly on a row boundary
    for (std::size_t cut : {std::size_t{60}, after_first_row, full.size() - 1}) {
        auto server = make_server(200, full.substr(0, cut));
        const auto r = fetch_1h(testnet_cfg(server->port()), kFarFuture);
        EXPECT_EQ(r.transport, PublicRestError::None) << "cut=" << cut;
        EXPECT_NE(r.parse, KlinesParseError::None) << "cut=" << cut;
        EXPECT_FALSE(r.ok()) << "cut=" << cut;
    }
}

// --- response size cap ---------------------------------------------------------------------------------

TEST(BinanceKlinesRestLimits, ABodyOverOneMiBIsRefusedAtTheReadStageAndExactlyOneMiBIsNot) {
    constexpr std::size_t kMiB = 1024 * 1024;

    auto at_cap = make_server(200, std::string(kMiB, ' '));
    const auto ok = fetch_public_body("/x", testnet_cfg(at_cap->port()));
    EXPECT_EQ(ok.error, PublicRestError::None);
    EXPECT_EQ(ok.body.size(), kMiB);

    auto over_cap = make_server(200, std::string(kMiB + 1, ' '));
    const auto over = fetch_public_body("/x", testnet_cfg(over_cap->port()));
    EXPECT_EQ(over.error, PublicRestError::Read);
    EXPECT_TRUE(over.body.empty());
}

// --- transport failure stages --------------------------------------------------------------------------

TEST(BinanceKlinesRestConnectivity, MismatchedHostIsRejectedEvenThoughTheChainIsTrusted) {
    // Negative control for hostname verification: the CA is trusted, so the ONLY thing wrong is that
    // the host we claim (api.binance.com) is not the one in the certificate (testnet.binance.vision).
    auto server = make_server(200, bars(3));
    PublicRestConfig cfg = testnet_cfg(server->port());
    cfg.host = "api.binance.com";

    const auto r = fetch_public_body("/x", cfg);
    EXPECT_EQ(r.error, PublicRestError::TlsHandshake);
    EXPECT_EQ(server->requests().size(), 0u) << "no HTTP request may be sent over an unverified channel";
}

TEST(BinanceKlinesRestConnectivity, AnUntrustedCertificateIsRejected) {
    // Same server, but WITHOUT the extra trusted CA: the self-signed fixture cert must not verify
    // against the system store. (The matching-host success path above is the positive control.)
    auto server = make_server(200, bars(3));
    PublicRestConfig cfg = testnet_cfg(server->port());
    cfg.extra_trusted_ca_pem_path.clear();

    const auto r = fetch_public_body("/x", cfg);
    EXPECT_EQ(r.error, PublicRestError::TlsHandshake);
    EXPECT_EQ(server->requests().size(), 0u);
}

TEST(BinanceKlinesRestConnectivity, ConnectionRefusedIsTheConnectStage) {
    unsigned short dead_port = 0;
    {
        PlainBlackholeAcceptor listener;  // borrow a free port, then release it
        dead_port = listener.port();
    }
    PublicRestConfig cfg = testnet_cfg(dead_port);
    const auto start = std::chrono::steady_clock::now();
    const auto r = fetch_public_body("/x", cfg);
    EXPECT_EQ(r.error, PublicRestError::Connect);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(15));
}

TEST(BinanceKlinesRestConnectivity, TlsHandshakeStageTimeout) {
    PlainBlackholeAcceptor blackhole;
    PublicRestConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());

    const auto start = std::chrono::steady_clock::now();
    const auto r = fetch_public_body("/x", cfg);
    EXPECT_EQ(r.error, PublicRestError::TlsHandshake);
    // Bounded by the ~5s handshake-stage budget, well under the ~30s all-stages worst case.
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(15));
}

TEST(BinanceKlinesRestConnectivity, ReadStageTimeoutAfterASuccessfulHandshake) {
    TlsBlackholeAcceptor tls_blackhole(fixture_path("test_leaf_cert_testnet_host.pem"),
                                        fixture_path("test_leaf_key_testnet_host.pem"));
    const PublicRestConfig cfg = testnet_cfg(tls_blackhole.port());

    const auto start = std::chrono::steady_clock::now();
    const auto r = fetch_public_body("/x", cfg);
    // If this regresses to TlsHandshake the trust/hostname wiring broke, not the read deadline.
    EXPECT_EQ(r.error, PublicRestError::Read);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(20));
}
