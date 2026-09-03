// TODO 1A.4: binance_user_data_ws_session.hpp unit tests.
//
// No real network access -- connection-failure tests use the local blackhole TCP fixture,
// mirroring test_binance_ws_session.cpp's own pattern exactly.

#include <gtest/gtest.h>
#include <hengyuan/binance_listen_key_publisher.hpp>
#include <hengyuan/binance_user_data_ws_session.hpp>

#include "test_helpers/blackhole_acceptor.hpp"

#include <atomic>
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
    EXPECT_TRUE(ev.side_known);
    EXPECT_EQ(ev.side, hy::OrderSide::Buy);
}

// TODO 1A.4 batch 2: full executionReport field set, using Binance's own documented sample
// payload shape (field order matches their docs, not this codebase's own extraction order --
// simdjson on-demand supports out-of-order access, see UserDataJsonParser::parse()'s own
// comment).
TEST(UserDataJsonParser, ExecutionReportExtractsFullFieldSet) {
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    const auto body = R"({
        "e":"executionReport","E":1499405658658,"s":"ETHBTC","c":"HY-A","S":"SELL","o":"LIMIT",
        "f":"GTC","q":"1.00000000","p":"0.10264410","P":"0.00000000","F":"0.00000000","g":-1,
        "C":"","x":"TRADE","X":"PARTIALLY_FILLED","r":"NONE","i":4293153,"l":"0.50000000",
        "z":"0.50000000","L":"0.10264410","n":"0","N":null,"T":1499405658657,"t":-1,
        "I":8641984,"w":true,"m":false,"M":true,"O":1499405658657,"Z":"0.05132205",
        "Y":"0.00000000","Q":"0.00000000"
    })";
    ASSERT_EQ(parser.parse(body, ev), UserDataParseResult::Ok);
    EXPECT_EQ(ev.kind, UserDataEventKind::ExecutionReport);
    EXPECT_EQ(ev.coid.view(), "HY-A");
    EXPECT_EQ(ev.exchange_order_id, 4293153);
    EXPECT_EQ(ev.transaction_time_ms, 1499405658657);
    EXPECT_TRUE(ev.side_known);
    EXPECT_EQ(ev.side, hy::OrderSide::Sell);
    EXPECT_TRUE(ev.order_status_known);
    EXPECT_EQ(ev.order_status, hy::OrderState::PartialFill);
    EXPECT_EQ(ev.exec_type, hy::ExecutionType::Trade);
    EXPECT_TRUE(ev.tif_is_gtc);
    EXPECT_STREQ(ev.last_qty_raw, "0.50000000");
    EXPECT_STREQ(ev.cumulative_filled_qty_raw, "0.50000000");
    EXPECT_STREQ(ev.cumulative_quote_qty_raw, "0.05132205");
    EXPECT_STREQ(ev.last_price_raw, "0.10264410");
    EXPECT_STREQ(ev.order_qty_raw, "1.00000000");
    EXPECT_STREQ(ev.order_price_raw, "0.10264410");
}

TEST(UserDataJsonParser, ExecutionReportWithUnknownSideStatusExecTypeLeavesFlagsUnset) {
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    const auto body =
        R"({"e":"executionReport","E":1,"c":"HY-A","i":1,"S":"BOGUS","X":"BOGUS","x":"BOGUS",
             "f":"IOC"})";
    ASSERT_EQ(parser.parse(body, ev), UserDataParseResult::Ok);
    EXPECT_FALSE(ev.side_known);
    EXPECT_FALSE(ev.order_status_known);
    EXPECT_EQ(ev.exec_type, hy::ExecutionType::Unknown);  // best-effort, not a parse failure
    EXPECT_FALSE(ev.tif_is_gtc);  // only "GTC" is ever true -- this codebase only ever sends GTC
}

TEST(UserDataJsonParser, OverlongDecimalFieldLeftEmptyNotTruncated) {
    UserDataJsonParser parser;
    UserDataWsEvent ev{};
    // 24+ digits -- longer than char[24] can hold; must be rejected, never silently truncated
    // into a shorter, wrong number (copy_raw_decimal_field()'s own "reject, don't truncate"
    // discipline).
    const auto body =
        R"({"e":"executionReport","E":1,"c":"HY-A","i":1,"z":"123456789012345678901234.0"})";
    ASSERT_EQ(parser.parse(body, ev), UserDataParseResult::Ok);
    EXPECT_STREQ(ev.cumulative_filled_qty_raw, "");
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

// TODO 1A.4 batch 2: is_connected() is the one new cross-thread-visible atomic this batch adds
// (see BinanceUserDataWsSession::is_connected()'s own header comment) -- this is its TSan
// coverage. A local blackhole acceptor never completes a real TLS/WS handshake, so
// on_handshake() (the only writer) never runs and this cannot exercise the write side of the
// race directly (that requires a real WS server, which this codebase deliberately does not
// build as test infrastructure for this batch -- same "no speculative infrastructure" principle
// already applied elsewhere, see this batch's own plan). What this DOES prove, under TSan: the
// main thread can safely poll is_connected() throughout the object's full lifecycle --
// construction, an in-flight connection attempt, cancellation via stop(), and destruction via
// the last shared_from_this() reference dropping -- while the IO thread concurrently runs
// resolve/connect/cancel on the strand, with no race reported and no UB.
TEST(BinanceUserDataWsSessionLifecycle, IsConnectedIsSafeToPollConcurrentlyAndStaysFalseWithoutAHandshake) {
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

    std::atomic<bool> keep_polling{true};
    std::atomic<bool> observed_connected{false};
    std::thread poller([&] {
        while (keep_polling.load(std::memory_order_relaxed)) {
            if (session->is_connected()) observed_connected.store(true, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blackhole.accepted_connections() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    session->stop();
    io_thread.join();
    keep_polling.store(false, std::memory_order_relaxed);
    poller.join();

    EXPECT_FALSE(observed_connected.load());  // blackhole never completes a real handshake
    EXPECT_FALSE(session->is_connected());

    session.reset();
    EXPECT_EQ(blackhole.accepted_connections() > 0, true);  // sanity: the attempt did reach TCP
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
    // TODO 1A.4 batch 2: this previously had no real assertions (just `UserDataWsSessionConfig
    // cfg; (void)cfg; SUCCEED();`) -- confirmed via direct read this session that neither this
    // test nor test_binance_ws_session.cpp's own same-named test actually enforced anything a
    // static_assert/field enumeration would (external-review-verified gap). A brace-init with a
    // fixed member count plus EXPECT_EQ on every field IS something that fails to compile (extra
    // positional initializer = error) if a THIRD field with no default is added -- the specific
    // protection this test can actually provide. It cannot catch a new field that DOES have a
    // default value (no portable, reflection-free way to enumerate those), which is why this
    // batch deliberately keeps all new reconnect-related state on the separate
    // UserDataWsReconnectPolicy struct (binance_user_data_ws_supervisor.hpp) instead of adding
    // it here -- "this struct doesn't grow reconnect fields" still holds by construction, this
    // test just cannot single-handedly prove it for a defaulted field.
    UserDataWsSessionConfig cfg{"stream.binance.com", "9443"};
    EXPECT_EQ(cfg.host, "stream.binance.com");
    EXPECT_EQ(cfg.port, "9443");
}
