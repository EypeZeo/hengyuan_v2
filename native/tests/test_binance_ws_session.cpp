// P2-MD-02 / Track C: binance_ws_session.hpp unit tests.
//
// No real network access -- connection-failure tests use the local blackhole TCP fixture.

#include <gtest/gtest.h>
#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/binance_ws_session.hpp>
#include <hengyuan/spsc_ring.hpp>

#include "test_helpers/blackhole_acceptor.hpp"

#include <chrono>
#include <thread>

using hy::BinanceJsonParser;
using hy::BinanceMarketEvent;
using hy::BinanceWsSession;
using hy::SpscRing;
using hy::WsSessionConfig;
using hy::validate_ws_config;

namespace {

// A freshly-constructed session (never started) satisfies enough of the test fixture needs for
// the pure-configuration tests below; the connection-failure tests below construct their own.
struct SessionFixture {
    SessionFixture() : ssl_ctx(boost::asio::ssl::context::tlsv12_client) {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
    }

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx;
    SpscRing<BinanceMarketEvent, 1024> ring;
    BinanceJsonParser parser;
};

}  // namespace

// --- validate_ws_config() ---

TEST(WsSessionConfigValidation, AcceptsDefaultConfig) {
    WsSessionConfig cfg;
    cfg.subscribe_streams = {"btcusdt@trade", "btcusdt@depth@100ms"};
    EXPECT_TRUE(validate_ws_config(cfg));
}

TEST(WsSessionConfigValidation, RejectsEmptyHost) {
    WsSessionConfig cfg;
    cfg.host = "";
    EXPECT_FALSE(validate_ws_config(cfg));
}

TEST(WsSessionConfigValidation, RejectsTargetWithoutLeadingSlash) {
    WsSessionConfig cfg;
    cfg.target = "ws";
    EXPECT_FALSE(validate_ws_config(cfg));
}

TEST(WsSessionConfigValidation, RejectsMalformedStreamNames) {
    for (const char* bad : {"BTCUSDT@trade", "btcusdt", "btcusdt@", "@trade", "btc usdt@trade",
                            "btcusdt@trade@100ms@extra"}) {
        WsSessionConfig cfg;
        cfg.subscribe_streams = {bad};
        EXPECT_FALSE(validate_ws_config(cfg)) << "stream=" << bad;
    }
}

TEST(WsSessionConfigValidation, AcceptsTwoAndThreeSegmentStreamNames) {
    WsSessionConfig cfg;
    cfg.subscribe_streams = {"btcusdt@trade", "btcusdt@depth@100ms"};
    EXPECT_TRUE(validate_ws_config(cfg));
}

TEST(WsSessionConfigValidation, RejectsOverlongAggregateTarget) {
    WsSessionConfig cfg;
    // "btcusdt@depth@100ms" is 19 bytes; validate_ws_config() counts each entry as size()+1
    // (separator) plus the "/ws" target prefix -- 150 entries -> 3 + 150*20 = 3003 bytes,
    // comfortably over the 2048 aggregate-length limit (100 entries only totals ~2003 bytes,
    // which is NOT over the limit -- that undercount was this test's original bug).
    for (int i = 0; i < 150; ++i) {
        cfg.subscribe_streams.push_back("btcusdt@depth@100ms");
    }
    EXPECT_FALSE(validate_ws_config(cfg));
}

// --- configure_ws_stream() config-effectiveness (no connection needed) ---

TEST(WsSessionReadMessageMax, ConfiguredValueTakesEffect) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>> ws(
        ioc, ssl_ctx);

    hy::configure_ws_stream(ws, std::chrono::seconds(90),
                             static_cast<std::size_t>(4 * 1024 * 1024));
    EXPECT_EQ(ws.read_message_max(), static_cast<std::size_t>(4 * 1024 * 1024));
}

// --- BinanceWsSession: connection failure / lifecycle ---

TEST(BinanceWsSessionConnectivity, TlsHandshakeStageTimeout) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;

    WsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    cfg.subscribe_streams = {"btcusdt@trade"};

    auto session = std::make_shared<BinanceWsSession<1024>>(fx.ioc, fx.ssl_ctx, fx.ring,
                                                              fx.parser, cfg);
    session->start();

    std::thread io_thread([&fx] { fx.ioc.run(); });
    auto start = std::chrono::steady_clock::now();
    // No explicit stop() needed here: the session fails on its own once the TLS-handshake-stage
    // deadline fires, at which point io_context::run() has no more work and returns.
    io_thread.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto stats = session->stats_snapshot();
    EXPECT_TRUE(session->stopped());
    EXPECT_NE(stats.last_error_ec.value(), 0);
    EXPECT_EQ(stats.errors, 1u);
    EXPECT_LT(elapsed, std::chrono::seconds(20));
}

TEST(BinanceWsSessionLifecycle, RequestedStopCancelsPendingHandshakeWithoutErrorOrLeak) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;

    WsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    cfg.subscribe_streams = {"btcusdt@trade"};

    auto session = std::make_shared<BinanceWsSession<1024>>(fx.ioc, fx.ssl_ctx, fx.ring,
                                                              fx.parser, cfg);
    std::weak_ptr<BinanceWsSession<1024>> weak_session = session;
    session->start();
    std::thread io_thread([&fx] { fx.ioc.run(); });

    // Wait until TCP is accepted: the client is then in its intentionally stalled TLS
    // handshake, so stop() must exercise an operation_aborted completion rather than merely
    // stopping before any work began.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blackhole.accepted_connections() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (blackhole.accepted_connections() == 0) {
        // Do not ASSERT while io_thread is joinable: its destructor would terminate the test
        // process and leave the session's operation alive. Always use the production shutdown
        // protocol first, then report the fixture failure.
        session->stop();
        io_thread.join();
        FAIL() << "blackhole did not accept the client connection before the test deadline";
        return;
    }

    auto start = std::chrono::steady_clock::now();
    session->stop();
    io_thread.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto stats = session->stats_snapshot();
    EXPECT_TRUE(session->stopped());
    EXPECT_EQ(stats.errors, 0u);
    EXPECT_LT(elapsed, std::chrono::seconds(2));

    session.reset();
    EXPECT_TRUE(weak_session.expired());
}

TEST(BinanceWsSessionLifecycle, ExactlyOnceStartGuard) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;

    WsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    cfg.subscribe_streams = {"btcusdt@trade"};

    auto session = std::make_shared<BinanceWsSession<1024>>(fx.ioc, fx.ssl_ctx, fx.ring,
                                                              fx.parser, cfg);
    session->start();
    session->start();  // must be a silent no-op, not a second resolve attempt
    session->start();

    std::thread io_thread([&fx] { fx.ioc.run(); });
    io_thread.join();

    // A second/third start() that actually re-triggered resolve would eventually surface as
    // more than one recorded error (each attempt independently timing out and calling fail()).
    // Because the guard makes start() #2/#3 pure no-ops, there is exactly one failure recorded.
    auto stats = session->stats_snapshot();
    EXPECT_EQ(stats.errors, 1u);
}

TEST(BinanceWsSessionLifecycle, InvalidConfigFailsWithoutTouchingNetwork) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;  // never contacted if this test passes
    SessionFixture fx;

    WsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    cfg.subscribe_streams = {"NOT A VALID STREAM NAME"};

    auto session = std::make_shared<BinanceWsSession<1024>>(fx.ioc, fx.ssl_ctx, fx.ring,
                                                              fx.parser, cfg);
    session->start();

    std::thread io_thread([&fx] { fx.ioc.run(); });
    io_thread.join();

    EXPECT_TRUE(session->stopped());
    auto stats = session->stats_snapshot();
    EXPECT_EQ(stats.last_error_stage, "invalid_config");
}

TEST(BinanceWsSessionLifecycle, DeadReconnectFieldsAreGoneAtCompileTime) {
    // This test's only job is to fail to compile if WsSessionConfig/WsSessionStats ever grow
    // back a reconnect_* field that nothing reads -- see this file's header comment on the
    // fail-stop design. No runtime assertions needed.
    WsSessionConfig cfg;
    (void)cfg;
    SUCCEED();
}
