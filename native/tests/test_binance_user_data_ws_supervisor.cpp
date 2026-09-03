// TODO 1A.4 batch 2: binance_user_data_ws_supervisor.hpp unit tests.
//
// Deterministic, single-threaded, no real network and no io_context::run() thread anywhere in
// this file -- every test uses UserDataWsSessionConfig{host=""} (or an unpublished
// ListenKeyPublisher), which makes BinanceUserDataWsSession::start() fail SYNCHRONOUSLY inside
// the call (see binance_user_data_ws_session.hpp's own start(): the invalid_config/no_listen_key
// checks return before net::co_spawn() is ever reached), so stopped() is deterministically true
// immediately after start() returns, with no need to drive the io_context at all. This lets the
// supervisor's own retry/backoff state machine be tested in full isolation from real connection
// timing -- the same trade-off test_binance_user_data_ws_session.cpp's own
// IsConnectedIsSafeToPollConcurrentlyAndStaysFalseWithoutAHandshake test documents: a real
// "session actually reaches is_connected()==true, and the supervisor's counter-reset fires"
// integration test needs a real WS server this codebase deliberately does not build as test
// infrastructure for this batch. That specific transition is covered by direct code inspection
// (this file's own header comment) and the harness's real-network exercise, not by this suite.

#include <gtest/gtest.h>
#include <hengyuan/binance_user_data_ws_supervisor.hpp>

using hy::ListenKeyPublisher;
using hy::UserDataJsonParser;
using hy::UserDataWsEventRing;
using hy::UserDataWsReconnectPolicy;
using hy::UserDataWsSessionConfig;
using hy::UserDataWsSessionSupervisor;
using hy::ws_reconnect_backoff_delay_ms;

// --- ws_reconnect_backoff_delay_ms(): direct coverage, mirroring
// test_order_tracker.cpp's BackoffGrowsWithAttemptCount/DegenerateBackoffConfigFailsClosedNotImmediate ---

TEST(WsReconnectBackoffDelay, AttemptZeroReturnsInitialBackoff) {
    UserDataWsReconnectPolicy policy{1000, 2, 60000};
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 0), 1000);
}

TEST(WsReconnectBackoffDelay, GrowsByMultiplierPerAttemptThenSaturates) {
    UserDataWsReconnectPolicy policy{1000, 2, 60000};
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 1), 2000);
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 2), 4000);
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 3), 8000);
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 4), 16000);
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 5), 32000);
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 6), 60000);   // saturates
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 100), 60000);  // stays saturated, no overflow
}

TEST(WsReconnectBackoffDelay, DegenerateInitialBackoffFailsClosedToMaxNotImmediate) {
    UserDataWsReconnectPolicy policy{0, 2, 60000};  // initial_backoff_ms<=0 is degenerate
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 0), 60000);
}

TEST(WsReconnectBackoffDelay, DegenerateMaxBackoffFailsClosedToZeroNotNegative) {
    UserDataWsReconnectPolicy policy{1000, 2, 0};  // max_backoff_ms<=0 is degenerate
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 0), 0);
}

TEST(WsReconnectBackoffDelay, ZeroMultiplierTreatedAsOne) {
    UserDataWsReconnectPolicy policy{1000, 0, 60000};
    EXPECT_EQ(ws_reconnect_backoff_delay_ms(policy, 3), 1000);  // never grows past initial
}

// --- UserDataWsSessionSupervisor ---

namespace {

struct SupervisorFixture {
    SupervisorFixture() : ssl_ctx(boost::asio::ssl::context::tlsv12_client) {}

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx;
    UserDataWsEventRing ring;
    UserDataJsonParser parser;
    ListenKeyPublisher listen_key_pub;
};

// Deterministic synchronous-failure config -- see this file's own header comment.
UserDataWsSessionConfig deterministic_failure_config() {
    UserDataWsSessionConfig cfg;
    cfg.host = "";
    cfg.port = "443";
    return cfg;
}

}  // namespace

TEST(UserDataWsSessionSupervisorTest, FirstPollAttemptsImmediatelyNoBackoffHistory) {
    SupervisorFixture fx;
    UserDataWsSessionSupervisor sup(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, fx.listen_key_pub,
                                     deterministic_failure_config());

    sup.poll(0);

    EXPECT_EQ(sup.stats().total_attempts, 1u);
    EXPECT_EQ(sup.stats().last_attempt_ms, 0);
    EXPECT_EQ(sup.stats().consecutive_failures, 0u);  // failure not yet DETECTED (next poll does)
}

TEST(UserDataWsSessionSupervisorTest, RepeatedPollsRespectBackoffWindowBetweenAttempts) {
    SupervisorFixture fx;
    UserDataWsSessionSupervisor sup(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, fx.listen_key_pub,
                                     deterministic_failure_config(),
                                     UserDataWsReconnectPolicy{1000, 2, 60000});

    sup.poll(0);  // attempt 1 -- constructs+starts; fails synchronously inside start()
    ASSERT_EQ(sup.stats().total_attempts, 1u);

    sup.poll(0);  // detects the failure this tick: consecutive_failures_ -> 1, backoff(0)=1000ms
    EXPECT_EQ(sup.stats().consecutive_failures, 1u);
    EXPECT_EQ(sup.stats().total_attempts, 1u);  // no new attempt yet

    sup.poll(500);  // still inside the 1000ms backoff window
    EXPECT_EQ(sup.stats().total_attempts, 1u);

    sup.poll(999);  // one ms short
    EXPECT_EQ(sup.stats().total_attempts, 1u);

    sup.poll(1000);  // backoff elapsed exactly -> attempt 2
    EXPECT_EQ(sup.stats().total_attempts, 2u);

    sup.poll(1000);  // detects 2nd failure: consecutive_failures_ -> 2, backoff(1)=2000ms from now
    EXPECT_EQ(sup.stats().consecutive_failures, 2u);

    sup.poll(2999);  // 1000+2000-1, still short
    EXPECT_EQ(sup.stats().total_attempts, 2u);

    sup.poll(3000);  // elapsed -> attempt 3
    EXPECT_EQ(sup.stats().total_attempts, 3u);
}

TEST(UserDataWsSessionSupervisorTest, ShutdownStopsFurtherAttemptsEvenLongAfterBackoffElapses) {
    SupervisorFixture fx;
    UserDataWsSessionSupervisor sup(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, fx.listen_key_pub,
                                     deterministic_failure_config(),
                                     UserDataWsReconnectPolicy{1000, 2, 60000});

    sup.poll(0);
    ASSERT_EQ(sup.stats().total_attempts, 1u);

    sup.shutdown();

    sup.poll(1);
    sup.poll(1'000'000'000);  // far past any conceivable backoff window
    EXPECT_EQ(sup.stats().total_attempts, 1u);

    sup.shutdown();  // idempotent -- must not crash or change anything
    sup.poll(2'000'000'000);
    EXPECT_EQ(sup.stats().total_attempts, 1u);
}

TEST(UserDataWsSessionSupervisorTest, RotatedListenKeySeqPickedUpOnNextAttempt) {
    SupervisorFixture fx;
    fx.listen_key_pub.publish("key-one", 1000, 2000);  // seq becomes 1

    UserDataWsSessionSupervisor sup(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, fx.listen_key_pub,
                                     deterministic_failure_config(),
                                     UserDataWsReconnectPolicy{1000, 2, 60000});

    sup.poll(0);  // attempt 1 -- reads seq=1 (even though start() itself fails on empty host)
    EXPECT_EQ(sup.stats().last_listen_key_seq, 1u);

    sup.poll(0);  // detects failure, schedules backoff; no new attempt, seq unchanged
    EXPECT_EQ(sup.stats().last_listen_key_seq, 1u);

    fx.listen_key_pub.publish("key-two", 3000, 4000);  // simulates a rotation during the wait

    sup.poll(1000);  // backoff elapsed -> attempt 2, re-reads the CURRENT snapshot
    EXPECT_EQ(sup.stats().total_attempts, 2u);
    EXPECT_EQ(sup.stats().last_listen_key_seq, 2u);
}

TEST(UserDataWsSessionSupervisorTest, NeverPublishedListenKeyStillFollowsTheSameRetryLoop) {
    // Deliberately never calling fx.listen_key_pub.publish() -- start()'s own no_listen_key
    // fail-closed path (binance_user_data_ws_session.hpp) is exercised the same way as any
    // other synchronous failure; the supervisor has no special-cased branch for it (see this
    // file's own header comment: "monitor不需要对失败阶段分支特判").
    SupervisorFixture fx;
    UserDataWsSessionConfig cfg;  // valid host/port defaults, but no listenKey ever published
    UserDataWsSessionSupervisor sup(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, fx.listen_key_pub,
                                     cfg, UserDataWsReconnectPolicy{1000, 2, 60000});

    sup.poll(0);
    EXPECT_EQ(sup.stats().total_attempts, 1u);
    EXPECT_EQ(sup.stats().last_listen_key_seq, 0u);

    sup.poll(0);  // detects failure
    EXPECT_EQ(sup.stats().consecutive_failures, 1u);

    sup.poll(1000);
    EXPECT_EQ(sup.stats().total_attempts, 2u);
}
