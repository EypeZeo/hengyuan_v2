// 批次 6 6b-0f-3d: the REAL public sessions and the feed supervisor against a LOCAL TLS WebSocket server.
//
// Until now the sessions' positive paths were covered only by an owner running the real testnet:
// "is_connected() turns true after the WebSocket handshake", "a closed bar reaches the ring", "a server
// drop ends the session", "the supervisor reconnects a real session". A mutation check on 6b-0f-3c-2 made
// the gap concrete (deleting `connected_.store(true)` failed no test). WsLoopbackServer closes it.
//
// Everything is local: the server binds 127.0.0.1:0, the clients connect to host "127.0.0.1" and verify its
// certificate (test_leaf_cert_loopback.pem, SAN IP:127.0.0.1) against the same file used as a trust anchor,
// so the REAL TLS + hostname-verification + WebSocket-upgrade code runs, minus the network.
//
// The client side deliberately mirrors the shape the harnesses need: ONE dedicated I/O thread running
// ioc.run(), kept alive by a work guard. WsLoopbackWorkGuard.* shows why the guard is not optional.

#include <gtest/gtest.h>
#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/binance_kline_ws_session.hpp>
#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/binance_ws_session.hpp>
#include <hengyuan/depth_manager.hpp>
#include <hengyuan/public_feed_policy.hpp>
#include <hengyuan/public_feed_supervisor.hpp>

#include "test_helpers/ws_client_rig.hpp"
#include "test_helpers/ws_loopback_server.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using hy::test_helpers::fixture_path;
using hy::test_helpers::steady_ms;
using hy::test_helpers::wait_until;
using hy::test_helpers::WsLoopbackServer;

std::unique_ptr<WsLoopbackServer> make_server(WsLoopbackServer::UpgradeBehavior behavior =
                                                  WsLoopbackServer::UpgradeBehavior::Answer) {
    return std::make_unique<WsLoopbackServer>(fixture_path("test_leaf_cert_loopback.pem"),
                                              fixture_path("test_leaf_key_loopback.pem"), behavior);
}

// The shared harness-shaped client I/O setup (ws_client_rig.hpp). Declare the ring and parser a session
// points at BEFORE the rig: the rig is then destroyed first, and its destructor joins the I/O thread, so
// nothing can still be pushing into a ring that is being torn down.
using ClientRig = hy::test_helpers::WsClientRig;

std::string kline_body(bool closed, std::int64_t open_time_ms, std::int64_t close_time_ms) {
    return R"({"e":"kline","E":123456789,"s":"BTCUSDT","k":{)"
           R"("t":)" + std::to_string(open_time_ms) + R"(,"T":)" + std::to_string(close_time_ms) +
           R"(,"s":"BTCUSDT","i":"1m","f":100,"L":200,)"
           R"("o":"100.0","c":"101.0","h":"102.0","l":"99.0","v":"5.5","n":100,)"
           R"("x":)" + std::string(closed ? "true" : "false") + R"(,)"
           R"("q":"1.0","V":"1.0","Q":"1.0","B":"0"}})";
}

constexpr const char* kDepthUpdate =
    R"({"e":"depthUpdate","E":1700000002000,"s":"BTCUSDT","U":100,"u":200,)"
    R"("b":[["67890.00000000","1.00000000"]],"a":[["67891.00000000","0.50000000"]]})";

hy::KlineWsSessionConfig kline_cfg(const WsLoopbackServer& server) {
    hy::KlineWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(server.port());
    cfg.symbol = "btcusdt";
    cfg.interval = "1m";
    cfg.symbol_id = 5;
    return cfg;
}

hy::WsSessionConfig depth_cfg(const WsLoopbackServer& server) {
    hy::WsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(server.port());
    cfg.subscribe_streams = {"btcusdt@depth@100ms"};
    return cfg;
}

}  // namespace

// --- the depth session ---------------------------------------------------------------------------------

TEST(WsLoopbackDepthSession, ReportsConnectedOnlyAfterTheHandshakeAndDeliversParsedEvents) {
    auto server = make_server();
    hy::BinanceJsonParser parser;
    ASSERT_TRUE(parser.register_symbol("BTCUSDT", 0));
    hy::SpscRing<hy::BinanceMarketEvent, 1024> ring;
    ClientRig rig;

    auto session = std::make_shared<hy::BinanceWsSession<1024>>(rig.ioc, rig.ssl_ctx, ring, parser, depth_cfg(*server));
    EXPECT_FALSE(session->is_connected()) << "never started";
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }))
        << "the session never completed a WebSocket handshake (server: tls=" << server->tls_handshakes()
        << " upgrades=" << server->upgrades() << ")";
    EXPECT_FALSE(session->stopped());
    ASSERT_EQ(server->targets().size(), 1U);
    EXPECT_EQ(server->targets()[0], "/ws/btcusdt@depth@100ms");
    EXPECT_EQ(server->upgrades(), 1U);

    server->send_text(kDepthUpdate);
    std::vector<hy::BinanceMarketEvent> events;
    ASSERT_TRUE(wait_until([&] {
        hy::BinanceMarketEvent ev{};
        while (ring.try_pop(ev)) events.push_back(ev);
        return events.size() >= 2;
    }));
    EXPECT_EQ(events[0].type, hy::EventType::DepthDelta);
    EXPECT_EQ(events[0].event_id, 200U);
    EXPECT_EQ(events[0].price_ticks, 6789000000000LL);
    EXPECT_EQ(events[0].side, hy::Side::Buy);
    EXPECT_EQ(events[1].side, hy::Side::Sell);
    EXPECT_EQ(events[1].price_ticks, 6789100000000LL);

    session->stop();
    ASSERT_TRUE(wait_until([&] { return session->stopped(); }));
    EXPECT_EQ(session->stats_snapshot().errors, 0U) << "a requested stop is not an error";
}

TEST(WsLoopbackDepthSession, IsConnectedStaysFalseWhileTlsIsUpButTheUpgradeIsNeverAnswered) {
    auto server = make_server(WsLoopbackServer::UpgradeBehavior::Stall);
    hy::BinanceJsonParser parser;
    hy::SpscRing<hy::BinanceMarketEvent, 1024> ring;
    ClientRig rig;
    auto session = std::make_shared<hy::BinanceWsSession<1024>>(rig.ioc, rig.ssl_ctx, ring, parser, depth_cfg(*server));
    session->start();

    // TLS completes, so the client is now inside its WebSocket handshake -- the stage no blackhole fixture
    // can reach. "Connected" must mean the handshake finished, not that TCP/TLS came up.
    ASSERT_TRUE(wait_until([&] { return server->tls_handshakes() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(session->is_connected());
    EXPECT_FALSE(session->stopped()) << "a pending handshake is not a failure yet";
    EXPECT_EQ(server->upgrades(), 0U);

    session->stop();
    ASSERT_TRUE(wait_until([&] { return session->stopped(); }));
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->stats_snapshot().errors, 0U);
}

// --- the kline session ---------------------------------------------------------------------------------

TEST(WsLoopbackKlineSession, OnlyClosedBarsReachTheRingAndSymbolIdIsStamped) {
    auto server = make_server();
    hy::KlineWsEventRing ring;
    hy::KlineJsonParser parser;
    ClientRig rig;
    auto session = std::make_shared<hy::BinanceKlineWsSession>(rig.ioc, rig.ssl_ctx, ring, parser, kline_cfg(*server));
    EXPECT_FALSE(session->is_connected());
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    ASSERT_EQ(server->targets().size(), 1U);
    EXPECT_EQ(server->targets()[0], "/ws/btcusdt@kline_1m");

    // Binance pushes an in-progress update every ~2s and only one of them per bar is the closed one.
    server->send_text(kline_body(false, 0, 59'999));
    server->send_text(kline_body(true, 0, 59'999));
    server->send_text(kline_body(false, 60'000, 119'999));
    server->send_text(kline_body(true, 60'000, 119'999));

    std::vector<hy::KlineWsEvent> bars;
    ASSERT_TRUE(wait_until([&] {
        hy::KlineWsEvent ev{};
        while (ring.try_pop(ev)) bars.push_back(ev);
        return bars.size() >= 2;
    }));
    EXPECT_EQ(bars[0].open_time_ms, 0);
    EXPECT_EQ(bars[1].open_time_ms, 60'000);
    for (const auto& b : bars) {
        EXPECT_TRUE(b.is_closed);
        EXPECT_EQ(b.symbol_id, 5U);
        EXPECT_DOUBLE_EQ(b.close, 101.0);
    }

    session->stop();
    ASSERT_TRUE(wait_until([&] { return session->stopped(); }));
    const auto stats = session->stats_snapshot();
    EXPECT_EQ(stats.push_ok, 2U);
    EXPECT_EQ(stats.unclosed_skipped, 2U);
    EXPECT_EQ(stats.gap_detected_count, 0U);
    EXPECT_EQ(stats.errors, 0U);
}

TEST(WsLoopbackKlineSession, AGapBetweenTwoClosedBarsSuspendsTheSessionAndDropsTheGapBar) {
    auto server = make_server();
    hy::KlineWsEventRing ring;
    hy::KlineJsonParser parser;
    ClientRig rig;
    auto session = std::make_shared<hy::BinanceKlineWsSession>(rig.ioc, rig.ssl_ctx, ring, parser, kline_cfg(*server));
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));

    server->send_text(kline_body(true, 0, 59'999));
    server->send_text(kline_body(true, 120'000, 179'999));  // the 60'000 bar never arrived
    ASSERT_TRUE(wait_until([&] { return session->is_suspended(); }));

    std::vector<hy::KlineWsEvent> bars;
    hy::KlineWsEvent ev{};
    while (ring.try_pop(ev)) bars.push_back(ev);
    ASSERT_EQ(bars.size(), 1U) << "only the bar before the hole may have been delivered";
    EXPECT_EQ(bars[0].open_time_ms, 0);
    EXPECT_EQ(session->stats_snapshot().gap_detected_count, 1U);

    // resume_after_gap() (which the feed driver calls) re-baselines: the next bar flows again.
    session->resume_after_gap();
    ASSERT_TRUE(wait_until([&] { return !session->is_suspended(); }));
    server->send_text(kline_body(true, 180'000, 239'999));
    ASSERT_TRUE(wait_until([&] { return ring.try_pop(ev); }));
    EXPECT_EQ(ev.open_time_ms, 180'000);

    session->stop();
    ASSERT_TRUE(wait_until([&] { return session->stopped(); }));
}

TEST(WsLoopbackKlineSession, AServerSideDropEndsTheSessionFailStop) {
    auto server = make_server();
    hy::KlineWsEventRing ring;
    hy::KlineJsonParser parser;
    ClientRig rig;
    auto session = std::make_shared<hy::BinanceKlineWsSession>(rig.ioc, rig.ssl_ctx, ring, parser, kline_cfg(*server));
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));

    server->drop_all();  // what a network drop looks like from the client: the connection just ends
    ASSERT_TRUE(wait_until([&] { return session->stopped(); })) << "the session did not notice the drop";
    const auto stats = session->stats_snapshot();
    EXPECT_EQ(stats.errors, 1U);
    EXPECT_EQ(stats.last_error_stage, "read");
}

TEST(WsLoopbackKlineSession, AGracefulServerCloseAlsoEndsTheSession) {
    auto server = make_server();
    hy::KlineWsEventRing ring;
    hy::KlineJsonParser parser;
    ClientRig rig;
    auto session = std::make_shared<hy::BinanceKlineWsSession>(rig.ioc, rig.ssl_ctx, ring, parser, kline_cfg(*server));
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));

    server->close_all();
    ASSERT_TRUE(wait_until([&] { return session->stopped(); }));
    EXPECT_EQ(session->stats_snapshot().errors, 1U);
}

// --- the supervisor with REAL sessions -----------------------------------------------------------------------

namespace {

hy::FeedSupervisorPolicy fast_policy() {
    hy::FeedSupervisorPolicy p;
    p.initial_backoff_ms = 1;
    p.max_backoff_ms = 5;
    p.jitter_percent = 0;
    p.connect_deadline_ms = 5000;
    p.drain_timeout_ms = 200;
    return p;
}

}  // namespace

TEST(WsLoopbackSupervisor, AReconnectedRealSessionResumesDeliveringBars) {
    auto server = make_server();
    hy::KlineWsEventRing ring;
    hy::KlineJsonParser parser;
    ClientRig rig;
    const auto cfg = kline_cfg(*server);
    hy::PublicFeedSupervisor<hy::BinanceKlineWsSession> sup(
        [&](std::uint64_t) {
            return std::make_shared<hy::BinanceKlineWsSession>(rig.ioc, rig.ssl_ctx, ring, parser, cfg);
        },
        fast_policy());

    auto poll_until = [&](const std::function<bool()>& pred) {
        return wait_until([&] {
            sup.poll(steady_ms());
            return pred();
        });
    };

    ASSERT_TRUE(poll_until([&] { return sup.healthy(); }));
    EXPECT_EQ(sup.generation(), 1U);
    server->send_text(kline_body(true, 0, 59'999));
    hy::KlineWsEvent ev{};
    ASSERT_TRUE(poll_until([&] { return ring.try_pop(ev); }));
    EXPECT_EQ(ev.open_time_ms, 0);

    server->drop_all();  // the network drops the connection
    ASSERT_TRUE(poll_until([&] { return sup.generation() == 2U && sup.healthy(); }))
        << "state=" << hy::feed_state_name(sup.state()) << " generation=" << sup.generation();
    EXPECT_EQ(sup.stats().total_failures, 1U);
    EXPECT_EQ(server->upgrades(), 2U) << "the second generation must be a fresh, real connection";

    server->send_text(kline_body(true, 60'000, 119'999));
    ASSERT_TRUE(poll_until([&] { return ring.try_pop(ev); }));
    EXPECT_EQ(ev.open_time_ms, 60'000) << "bars from the new generation must reach the same ring";

    const auto last = sup.current();
    sup.shutdown();
    ASSERT_TRUE(wait_until([&] { return last->stopped(); }));
}

TEST(WsLoopbackSupervisor, APlannedRolloverReplacesAHealthyRealSessionWithoutCountingAFailure) {
    auto server = make_server();
    hy::KlineWsEventRing ring;
    hy::KlineJsonParser parser;
    ClientRig rig;
    const auto cfg = kline_cfg(*server);
    hy::FeedSupervisorPolicy policy = fast_policy();
    policy.max_connection_age_ms = 300;
    hy::PublicFeedSupervisor<hy::BinanceKlineWsSession> sup(
        [&](std::uint64_t) {
            return std::make_shared<hy::BinanceKlineWsSession>(rig.ioc, rig.ssl_ctx, ring, parser, cfg);
        },
        policy);

    ASSERT_TRUE(wait_until(
        [&] {
            sup.poll(steady_ms());
            return sup.stats().rollovers >= 2 && sup.healthy();
        },
        15000))
        << "state=" << hy::feed_state_name(sup.state()) << " rollovers=" << sup.stats().rollovers
        << " upgrades=" << server->upgrades();
    EXPECT_EQ(sup.stats().total_failures, 0U);
    EXPECT_GE(server->upgrades(), 3U) << "every rollover is a genuinely new connection";
    EXPECT_GE(sup.generation(), 3U);

    const auto last = sup.current();
    sup.shutdown();
    ASSERT_TRUE(wait_until([&] { return last->stopped(); }));
}

TEST(WsLoopbackSupervisor, ASessionStuckInTheUpgradeIsStoppedAtTheConnectDeadlineAndCountedFailed) {
    auto server = make_server(WsLoopbackServer::UpgradeBehavior::Stall);
    hy::KlineWsEventRing ring;
    hy::KlineJsonParser parser;
    ClientRig rig;
    const auto cfg = kline_cfg(*server);
    hy::FeedSupervisorPolicy policy = fast_policy();
    policy.connect_deadline_ms = 400;
    hy::PublicFeedSupervisor<hy::BinanceKlineWsSession> sup(
        [&](std::uint64_t) {
            return std::make_shared<hy::BinanceKlineWsSession>(rig.ioc, rig.ssl_ctx, ring, parser, cfg);
        },
        policy);

    // Without is_connected() meaning "handshake done", this session would look healthy the moment TLS came
    // up; with it, the supervisor sees a session that never connects and enforces the deadline.
    ASSERT_TRUE(wait_until([&] {
        sup.poll(steady_ms());
        return sup.stats().connect_timeouts >= 1 && sup.generation() >= 2;
    }));
    EXPECT_FALSE(sup.healthy());
    EXPECT_GE(sup.stats().total_failures, 1U);
    EXPECT_EQ(server->upgrades(), 0U);
    sup.shutdown();
}

// A minimal, valid single-level snapshot -- enough to move DepthManager out of Buffering so the
// test can observe it going back there on the next generation.
hy::DepthSnapshot make_snapshot(std::uint64_t last_update_id, std::int64_t bid_price) {
    hy::DepthSnapshot snap;
    snap.last_update_id = last_update_id;
    snap.bids[0] = hy::PriceLevel{bid_price, 100};
    snap.bid_count = 1;
    snap.asks[0] = hy::PriceLevel{bid_price + 100, 100};
    snap.ask_count = 1;
    return snap;
}

// 批次 6 6b-0f-4: wrapping the depth session in a real PublicFeedSupervisor makes it reconnect --
// but a fresh generation's update ids do not continue the previous generation's sequence, so the
// consumer MUST reset DepthManager to Buffering exactly once per new generation (FeedGenerationTracker,
// public_feed_policy.hpp) before trusting any of that generation's events. This proves the wiring, not
// just the tracker's own pure logic (already covered in test_public_feed_supervisor.cpp): a real
// reconnect against a real server, with a real DepthManager on the other end.
TEST(WsLoopbackSupervisor, EachNewGenerationResetsDepthManagerToBufferingExactlyOnce) {
    auto server = make_server();
    hy::BinanceJsonParser parser;
    ASSERT_TRUE(parser.register_symbol("BTCUSDT", 0));
    hy::SpscRing<hy::BinanceMarketEvent, 1024> ring;
    ClientRig rig;
    const auto cfg = depth_cfg(*server);
    hy::PublicFeedSupervisor<hy::BinanceWsSession<1024>> sup(
        [&](std::uint64_t) {
            return std::make_shared<hy::BinanceWsSession<1024>>(rig.ioc, rig.ssl_ctx, ring, parser, cfg);
        },
        fast_policy());

    hy::DepthManager depth_mgr;
    hy::FeedGenerationTracker gen_tracker;
    int resets = 0;
    auto tick_once = [&] {
        sup.poll(steady_ms());
        if (gen_tracker.observe(sup.generation())) {
            depth_mgr.start_buffering();
            ++resets;
        }
    };
    auto poll_until = [&](const std::function<bool()>& pred) {
        return wait_until([&] {
            tick_once();
            return pred();
        });
    };

    // Generation 1: connects, resets exactly once, and a snapshot + one event is enough to reach
    // Tracking -- proving the reset leaves the manager genuinely usable, not just flipped and ignored.
    ASSERT_TRUE(poll_until([&] { return sup.healthy(); }));
    EXPECT_EQ(resets, 1);
    EXPECT_EQ(depth_mgr.state(), hy::DepthState::Buffering);
    ASSERT_TRUE(depth_mgr.apply_snapshot(make_snapshot(100, 67890'00000000LL)));
    EXPECT_TRUE(depth_mgr.on_depth_event(hy::BinanceMarketEvent{}, 101, 101));
    ASSERT_EQ(depth_mgr.state(), hy::DepthState::Tracking);
    EXPECT_EQ(depth_mgr.stats().events_applied, 1U);

    // The connection drops; the supervisor starts a genuinely new generation (a fresh TCP+TLS+WS
    // handshake against the server, not a resumed session with continuing update ids).
    server->drop_all();
    ASSERT_TRUE(poll_until([&] { return sup.generation() == 2U && sup.healthy(); }));
    EXPECT_EQ(resets, 2) << "a second generation must reset the book exactly once, not zero and not twice";
    EXPECT_EQ(depth_mgr.state(), hy::DepthState::Buffering)
        << "must not still read Tracking against generation 1's now-meaningless update-id baseline";

    // A snapshot + event using update ids that would make NO sense as a continuation of generation
    // 1's sequence (100/101 above) must still be accepted cleanly -- proof the reset actually took,
    // rather than the manager silently carrying generation 1's last_applied_u_ forward.
    ASSERT_TRUE(depth_mgr.apply_snapshot(make_snapshot(5, 67891'00000000LL)));
    EXPECT_TRUE(depth_mgr.on_depth_event(hy::BinanceMarketEvent{}, 6, 6));
    EXPECT_EQ(depth_mgr.state(), hy::DepthState::Tracking);
    EXPECT_EQ(depth_mgr.stats().resyncs, 0U) << "a proper reset is not a gap: it must not count as one";

    sup.shutdown();
}

// --- why the work guard is not optional ---------------------------------------------------------------------------

// io_context::run() returns as soon as there is no work. The harnesses start their I/O thread and only
// THEN let a supervisor's poll() create the sessions, so without a work guard the thread would already
// have exited and every session would sit in a queue nobody runs -- a harness with all feeds silently
// dead. (The pre-existing harnesses got away with it only because a session happened to be started
// before the thread was.) This documents the trap; ClientRig above is the pattern that avoids it.
TEST(WsLoopbackWorkGuard, WithoutAGuardTheIoThreadExitsAtOnceAndASessionStartedLaterNeverRuns) {
    auto server = make_server();
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    hy::configure_binance_ssl_context(ssl_ctx);
    ssl_ctx.load_verify_file(fixture_path("test_leaf_cert_loopback.pem"));
    boost::asio::io_context ioc;

    std::thread io_thread([&ioc] { ioc.run(); });
    io_thread.join();  // nothing to do: run() has already returned
    ASSERT_TRUE(ioc.stopped());

    hy::KlineWsEventRing ring;
    hy::KlineJsonParser parser;
    auto session = std::make_shared<hy::BinanceKlineWsSession>(ioc, ssl_ctx, ring, parser, kline_cfg(*server));
    session->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(server->tls_handshakes(), 0U) << "a session on a dead I/O thread must not have reached the server";

    // Clean up: run the queued start and the stop on this thread so nothing is left pending. Bounded, so
    // that a session which did connect (the failure this test guards against) fails the test rather than
    // blocking run() forever on a read that will never complete.
    session->stop();
    ioc.restart();
    ioc.run_for(std::chrono::seconds(2));
    EXPECT_TRUE(session->stopped());
}
