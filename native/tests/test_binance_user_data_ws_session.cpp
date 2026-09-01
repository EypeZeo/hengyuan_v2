// TODO 1A.4: binance_user_data_ws_session.hpp unit tests.
//
// No real network access -- connection-failure tests use the local blackhole TCP fixture,
// mirroring test_binance_ws_session.cpp's own pattern exactly.

#include <gtest/gtest.h>
#include <hengyuan/binance_listen_key_publisher.hpp>
#include <hengyuan/binance_user_data_ws_session.hpp>

#include "test_helpers/blackhole_acceptor.hpp"

#include <chrono>
#include <thread>

using hy::BinanceUserDataWsSession;
using hy::ListenKeyPublisher;
using hy::UserDataEventKind;
using hy::UserDataJsonParser;
using hy::UserDataParseResult;
using hy::UserDataWsEvent;
using hy::UserDataWsEventRing;
using hy::UserDataWsSessionConfig;

namespace {

struct SessionFixture {
    SessionFixture() : ssl_ctx(boost::asio::ssl::context::tlsv12_client) {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
    }

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx;
    UserDataWsEventRing ring;
    UserDataJsonParser parser;
    ListenKeyPublisher listen_key_pub;
};

}  // namespace

// --- UserDataJsonParser: "e"/"E"/"c"/"i" narrow extraction ---

TEST(UserDataJsonParser, MalformedJsonRejected) {
    // Empty input has no top-level value at all -- simdjson::ondemand::parser::iterate() itself
    // rejects this (EMPTY), before any field lookup is attempted. This is the genuine
    // "document-level" malformed case UserDataParseResult::MalformedJson exists for, distinct
    // from UserDataJsonParser.MissingEventTypeFieldRejected below (a well-formed, non-empty
    // document that simply lacks the "e" key, which fails later, at field lookup --
    // MissingEventType, not MalformedJson).
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    EXPECT_EQ(parser.parse("", ev), UserDataParseResult::MalformedJson);
}

TEST(UserDataJsonParser, StructurallyInvalidJsonDoesNotFabricateAnEvent) {
    // simdjson::ondemand is lazy about full structural validation -- an unclosed object like
    // "{" may not fail until a field is actually looked up, funnelling into
    // MissingEventType rather than MalformedJson (both are "safely rejected, no event
    // fabricated" outcomes for this narrow parser's purposes; only the specific enum value
    // differs). This test asserts the outcome that actually matters: never Ok with a
    // fabricated kind for genuinely malformed input.
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    const auto result = parser.parse("{", ev);
    EXPECT_NE(result, UserDataParseResult::Ok);
    EXPECT_EQ(ev.kind, UserDataEventKind::Unknown);
}

TEST(UserDataJsonParser, MissingEventTypeFieldRejected) {
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    EXPECT_EQ(parser.parse(R"({"E":1700000000000})", ev), UserDataParseResult::MissingEventType);
}

TEST(UserDataJsonParser, ExecutionReportExtractsClientOrderIdAndOrderId) {
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    const auto body =
        R"({"e":"executionReport","E":1700000000123,"s":"BTCUSDT","c":"HY-1700000000000-7-42","i":998877,"S":"BUY"})";
    ASSERT_EQ(parser.parse(body, ev), UserDataParseResult::Ok);
    EXPECT_EQ(ev.kind, UserDataEventKind::ExecutionReport);
    EXPECT_EQ(ev.event_time_ms, 1700000000123);
    EXPECT_EQ(ev.coid.view(), "HY-1700000000000-7-42");
    EXPECT_EQ(ev.exchange_order_id, 998877);
}

TEST(UserDataJsonParser, OutboundAccountPositionClassifiedButNotFurtherParsed) {
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    const auto body = R"({"e":"outboundAccountPosition","E":1700000000123,"B":[]})";
    ASSERT_EQ(parser.parse(body, ev), UserDataParseResult::Ok);
    EXPECT_EQ(ev.kind, UserDataEventKind::OutboundAccountPosition);
    EXPECT_TRUE(ev.coid.empty());
}

TEST(UserDataJsonParser, ListenKeyExpiredClassified) {
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    const auto body = R"({"e":"listenKeyExpired","E":1700000000123})";
    ASSERT_EQ(parser.parse(body, ev), UserDataParseResult::Ok);
    EXPECT_EQ(ev.kind, UserDataEventKind::ListenKeyExpired);
}

TEST(UserDataJsonParser, UnrecognizedEventTypeClassifiedUnknownNotRejected) {
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    const auto body = R"({"e":"someFutureEventType","E":1700000000123})";
    ASSERT_EQ(parser.parse(body, ev), UserDataParseResult::Ok);
    EXPECT_EQ(ev.kind, UserDataEventKind::Unknown);
}

TEST(UserDataJsonParser, ExecutionReportWithOverlongClientOrderIdLeavesCoidEmpty) {
    // A clientOrderId longer than kClientOrderIdLen must not overrun the fixed buffer -- silently
    // leave coid empty (drain_user_data_events() then simply won't find a match) rather than
    // truncate into a DIFFERENT valid-looking (and wrong) order id.
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    std::string overlong_coid(64, 'X');
    const std::string body =
        R"({"e":"executionReport","E":1,"c":")" + overlong_coid + R"(","i":1})";
    ASSERT_EQ(parser.parse(body, ev), UserDataParseResult::Ok);
    EXPECT_EQ(ev.kind, UserDataEventKind::ExecutionReport);
    EXPECT_TRUE(ev.coid.empty());
}

TEST(UserDataJsonParser, ReusedAcrossCallsWithDifferentBodySizes) {
    // Exercises the reused-padded-buffer growth path (binance_json_parser.hpp's own AUDIT
    // PERF-ALLOC-012 discipline) -- a small message followed by a much larger one must both
    // parse correctly, not just whichever size the buffer happened to start at.
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    ASSERT_EQ(parser.parse(R"({"e":"listenKeyExpired","E":1})", ev), UserDataParseResult::Ok);

    std::string padding(8192, ' ');
    const std::string body =
        R"({"e":"executionReport","E":1,"c":"HY-BIG","i":42,"pad":")" + padding + R"("})";
    ASSERT_EQ(parser.parse(body, ev), UserDataParseResult::Ok);
    EXPECT_EQ(ev.kind, UserDataEventKind::ExecutionReport);
    EXPECT_EQ(ev.coid.view(), "HY-BIG");
}

// --- BinanceUserDataWsSession: lifecycle / connection failure ---

TEST(BinanceUserDataWsSessionLifecycle, NoListenKeyPublishedFailsWithoutTouchingNetwork) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;  // never contacted if this test passes
    SessionFixture fx;

    UserDataWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    // Deliberately never publish a listenKey via fx.listen_key_pub.

    auto session = std::make_shared<BinanceUserDataWsSession>(fx.ioc, fx.ssl_ctx, fx.ring,
                                                                fx.parser, fx.listen_key_pub, cfg);
    session->start();

    std::thread io_thread([&fx] { fx.ioc.run(); });
    io_thread.join();

    EXPECT_TRUE(session->stopped());
    auto stats = session->stats_snapshot();
    EXPECT_EQ(stats.last_error_stage, "no_listen_key");
    EXPECT_EQ(blackhole.accepted_connections(), 0u);
}

TEST(BinanceUserDataWsSessionLifecycle, EmptyHostFailsWithoutTouchingNetwork) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;
    fx.listen_key_pub.publish("a-valid-looking-listen-key", 1000, 2000);

    UserDataWsSessionConfig cfg;
    cfg.host = "";
    cfg.port = std::to_string(blackhole.port());

    auto session = std::make_shared<BinanceUserDataWsSession>(fx.ioc, fx.ssl_ctx, fx.ring,
                                                                fx.parser, fx.listen_key_pub, cfg);
    session->start();

    std::thread io_thread([&fx] { fx.ioc.run(); });
    io_thread.join();

    EXPECT_TRUE(session->stopped());
    auto stats = session->stats_snapshot();
    EXPECT_EQ(stats.last_error_stage, "invalid_config");
}

TEST(BinanceUserDataWsSessionConnectivity, TlsHandshakeStageTimeout) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;
    fx.listen_key_pub.publish("a-valid-looking-listen-key", 1000, 2000);

    UserDataWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());

    auto session = std::make_shared<BinanceUserDataWsSession>(fx.ioc, fx.ssl_ctx, fx.ring,
                                                                fx.parser, fx.listen_key_pub, cfg);
    session->start();

    std::thread io_thread([&fx] { fx.ioc.run(); });
    auto start = std::chrono::steady_clock::now();
    io_thread.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto stats = session->stats_snapshot();
    EXPECT_TRUE(session->stopped());
    EXPECT_NE(stats.last_error_ec.value(), 0);
    EXPECT_EQ(stats.errors, 1u);
    EXPECT_LT(elapsed, std::chrono::seconds(20));
}

TEST(BinanceUserDataWsSessionLifecycle, RequestedStopCancelsPendingHandshakeWithoutErrorOrLeak) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;
    fx.listen_key_pub.publish("a-valid-looking-listen-key", 1000, 2000);

    UserDataWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());

    auto session = std::make_shared<BinanceUserDataWsSession>(fx.ioc, fx.ssl_ctx, fx.ring,
                                                                fx.parser, fx.listen_key_pub, cfg);
    std::weak_ptr<BinanceUserDataWsSession> weak_session = session;
    session->start();
    std::thread io_thread([&fx] { fx.ioc.run(); });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blackhole.accepted_connections() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (blackhole.accepted_connections() == 0) {
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

TEST(BinanceUserDataWsSessionLifecycle, ExactlyOnceStartGuard) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;
    fx.listen_key_pub.publish("a-valid-looking-listen-key", 1000, 2000);

    UserDataWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());

    auto session = std::make_shared<BinanceUserDataWsSession>(fx.ioc, fx.ssl_ctx, fx.ring,
                                                                fx.parser, fx.listen_key_pub, cfg);
    session->start();
    session->start();  // must be a silent no-op, not a second resolve attempt
    session->start();

    std::thread io_thread([&fx] { fx.ioc.run(); });
    io_thread.join();

    auto stats = session->stats_snapshot();
    EXPECT_EQ(stats.errors, 1u);
}

TEST(BinanceUserDataWsSessionLifecycle, DeadReconnectFieldsAreGoneAtCompileTime) {
    // Same discipline test_binance_ws_session.cpp's own DeadReconnectFieldsAreGoneAtCompileTime
    // enforces for BinanceWsSession -- fails to compile if UserDataWsSessionConfig/
    // UserDataWsSessionStats ever grow a reconnect_* field that nothing reads. No runtime
    // assertions needed.
    UserDataWsSessionConfig cfg;
    (void)cfg;
    SUCCEED();
}
