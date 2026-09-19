// 批次 6 6a-1: binance_kline_ws_session.hpp unit tests.
//
// No real network access -- connection-failure tests use the local blackhole TCP fixture,
// mirroring test_binance_user_data_ws_session.cpp's own pattern exactly.
//
// Coverage map for the two external-review-driven safety requirements (this file's own header
// comment in binance_kline_ws_session.hpp):
//   1. "未收盘更新绝不进 ring" -- proven by KlineJsonParser tests below: "k"."x"==false NEVER
//      returns KlineParseResult::Ok (the only result on_read() ever forwards to
//      handle_closed_bar()/the ring). Combined with on_read()'s own trivial `if (pr == Ok)` glue,
//      this fully establishes the property without needing a live mock WS server -- same
//      "test the parser's classification directly, not full message flow through a real socket"
//      philosophy test_binance_user_data_ws_session.cpp already uses for UserDataJsonParser.
//   2. "跳空检测触发挂起" -- proven directly by the KlineBarGapGuard tests below, which exercise
//      the exact same accept()/is_suspended()/resume() state machine BinanceKlineWsSession's
//      handle_closed_bar()/resume_after_gap() delegate to, with no Boost/Beast/network
//      dependency of its own.

#include <gtest/gtest.h>
#include <hengyuan/binance_kline_ws_session.hpp>

#include "test_helpers/blackhole_acceptor.hpp"

#include <chrono>
#include <thread>

using hy::BinanceKlineWsSession;
using hy::KlineBarGapGuard;
using hy::KlineJsonParser;
using hy::KlineParseResult;
using hy::KlineWsEvent;
using hy::KlineWsEventRing;
using hy::KlineWsSessionConfig;
using hy::validate_kline_ws_config;

namespace {

struct SessionFixture {
    SessionFixture() : ssl_ctx(boost::asio::ssl::context::tlsv12_client) {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
    }

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx;
    KlineWsEventRing ring;
    KlineJsonParser parser;
};

// A real Binance-documented kline payload shape (values taken from Binance's own API docs
// sample), with `x` swapped between calls to cover both the open and closed cases.
std::string kline_body(bool closed, std::int64_t open_time_ms = 123400000,
                        std::int64_t close_time_ms = 123459999) {
    return R"({"e":"kline","E":123456789,"s":"BNBBTC","k":{)"
           R"("t":)" + std::to_string(open_time_ms) + R"(,"T":)" + std::to_string(close_time_ms) +
           R"(,"s":"BNBBTC","i":"1m","f":100,"L":200,)"
           R"("o":"0.0010","c":"0.0020","h":"0.0025","l":"0.0015","v":"1000","n":100,)"
           R"("x":)" + std::string(closed ? "true" : "false") + R"(,)"
           R"("q":"1.0000","V":"500","Q":"0.500","B":"123456"}})";
}

}  // namespace

// --- KlineJsonParser: field extraction + is_closed classification ---

TEST(KlineJsonParser, MalformedJsonRejected) {
    KlineJsonParser parser;
    KlineWsEvent ev{};
    EXPECT_EQ(parser.parse("", ev), KlineParseResult::MalformedJson);
}

TEST(KlineJsonParser, WrongEventTypeRejected) {
    KlineJsonParser parser;
    KlineWsEvent ev{};
    const auto body = R"({"e":"trade","E":1})";
    EXPECT_EQ(parser.parse(body, ev), KlineParseResult::MissingFields);
}

TEST(KlineJsonParser, MissingKlineObjectRejected) {
    KlineJsonParser parser;
    KlineWsEvent ev{};
    const auto body = R"({"e":"kline","E":1})";
    EXPECT_EQ(parser.parse(body, ev), KlineParseResult::MissingFields);
}

TEST(KlineJsonParser, ClosedCandleExtractsFullFieldSet) {
    KlineJsonParser parser;
    KlineWsEvent ev{};
    const auto body = kline_body(/*closed=*/true, 123400000, 123459999);
    ASSERT_EQ(parser.parse(body, ev), KlineParseResult::Ok);
    EXPECT_TRUE(ev.is_closed);
    EXPECT_EQ(ev.open_time_ms, 123400000);
    EXPECT_EQ(ev.close_time_ms, 123459999);
    EXPECT_DOUBLE_EQ(ev.open, 0.0010);
    EXPECT_DOUBLE_EQ(ev.high, 0.0025);
    EXPECT_DOUBLE_EQ(ev.low, 0.0015);
    EXPECT_DOUBLE_EQ(ev.close, 0.0020);
    EXPECT_DOUBLE_EQ(ev.volume, 1000.0);
    EXPECT_EQ(ev.symbol_id, 0u);  // never parsed from JSON -- stamped by the session, not here
}

// This is the load-bearing test for external-review-driven safety requirement 1 ("就地过滤，
// 绝不能指望消费端事后过滤"): an in-progress (unclosed) candle update must NEVER classify as
// Ok, the only result on_read() forwards to the ring.
TEST(KlineJsonParser, UnclosedCandleNeverReturnsOk) {
    KlineJsonParser parser;
    KlineWsEvent ev{};
    const auto body = kline_body(/*closed=*/false);
    ASSERT_EQ(parser.parse(body, ev), KlineParseResult::Unclosed);
    EXPECT_FALSE(ev.is_closed);  // OHLCV may still be populated (forward-scan parse), but
                                  // is_closed itself -- the only field on_read() branches on --
                                  // stays false, and the result is never Ok either way.
}

TEST(KlineJsonParser, MissingRequiredNumericFieldRejected) {
    KlineJsonParser parser;
    KlineWsEvent ev{};
    const auto body =
        R"({"e":"kline","k":{"t":1,"T":2,"o":"1.0","h":"1.0","l":"1.0","x":true}})";  // "c"/"v" missing
    EXPECT_EQ(parser.parse(body, ev), KlineParseResult::MissingFields);
}

TEST(KlineJsonParser, NonNumericDecimalStringRejected) {
    KlineJsonParser parser;
    KlineWsEvent ev{};
    const auto body =
        R"({"e":"kline","k":{"t":1,"T":2,"o":"not-a-number","h":"1","l":"1","c":"1","v":"1","x":true}})";
    EXPECT_EQ(parser.parse(body, ev), KlineParseResult::MissingFields);
}

TEST(KlineJsonParser, ReusedAcrossCallsWithDifferentBodySizes) {
    // Exercises the reused-padded-buffer growth path (binance_json_parser.hpp's own AUDIT
    // PERF-ALLOC-012 discipline), same test shape as
    // UserDataJsonParser.ReusedAcrossCallsWithDifferentBodySizes.
    KlineJsonParser parser;
    KlineWsEvent ev{};
    ASSERT_EQ(parser.parse(kline_body(false), ev), KlineParseResult::Unclosed);

    std::string padding(8192, ' ');
    const std::string body =
        R"({"e":"kline","pad":")" + padding + R"(","k":{"t":1,"T":2,"o":"1","h":"1","l":"1",)"
        R"("c":"1","v":"1","x":true}})";
    ASSERT_EQ(parser.parse(body, ev), KlineParseResult::Ok);
    EXPECT_TRUE(ev.is_closed);
}

// --- validate_kline_ws_config ---

TEST(ValidateKlineWsConfig, AcceptsWellFormedConfig) {
    KlineWsSessionConfig cfg;
    cfg.symbol = "btcusdt";
    cfg.interval = "1m";
    EXPECT_TRUE(validate_kline_ws_config(cfg));
}

TEST(ValidateKlineWsConfig, RejectsUppercaseSymbol) {
    KlineWsSessionConfig cfg;
    cfg.symbol = "BTCUSDT";  // Binance stream names are lowercase-only
    cfg.interval = "1m";
    EXPECT_FALSE(validate_kline_ws_config(cfg));
}

TEST(ValidateKlineWsConfig, RejectsEmptySymbol) {
    KlineWsSessionConfig cfg;
    cfg.symbol = "";
    cfg.interval = "1m";
    EXPECT_FALSE(validate_kline_ws_config(cfg));
}

TEST(ValidateKlineWsConfig, RejectsUnknownInterval) {
    KlineWsSessionConfig cfg;
    cfg.symbol = "btcusdt";
    cfg.interval = "1x";
    EXPECT_FALSE(validate_kline_ws_config(cfg));
}

// "1M" (calendar month) vs "1m" (minute) -- Binance's own distinct-case convention, both valid.
TEST(ValidateKlineWsConfig, DistinguishesMinuteAndMonthIntervalsByCase) {
    KlineWsSessionConfig cfg;
    cfg.symbol = "btcusdt";
    cfg.interval = "1M";
    EXPECT_TRUE(validate_kline_ws_config(cfg));
    cfg.interval = "1m";
    EXPECT_TRUE(validate_kline_ws_config(cfg));
}

// --- KlineBarGapGuard: bar-gap (跳空) detection + suspend/resume ---
// Load-bearing coverage for external-review-driven safety requirement 2.

TEST(KlineBarGapGuard, FirstBarAlwaysAcceptedNoBaselineYet) {
    KlineBarGapGuard guard;
    KlineWsEvent ev{};
    ev.open_time_ms = 1000;
    ev.close_time_ms = 1999;
    EXPECT_TRUE(guard.accept(ev));
    EXPECT_FALSE(guard.is_suspended());
}

TEST(KlineBarGapGuard, ContiguousSecondBarAccepted) {
    KlineBarGapGuard guard;
    KlineWsEvent first{};
    first.open_time_ms = 1000;
    first.close_time_ms = 1999;
    ASSERT_TRUE(guard.accept(first));

    KlineWsEvent second{};
    second.open_time_ms = 2000;  // == prev_close_time_ms (1999) + 1
    second.close_time_ms = 2999;
    EXPECT_TRUE(guard.accept(second));
    EXPECT_FALSE(guard.is_suspended());
}

TEST(KlineBarGapGuard, GapRejectsTheTriggeringBarAndSuspends) {
    KlineBarGapGuard guard;
    KlineWsEvent first{};
    first.open_time_ms = 1000;
    first.close_time_ms = 1999;
    ASSERT_TRUE(guard.accept(first));

    KlineWsEvent gapped{};
    gapped.open_time_ms = 4000;  // two bars missing -- not prev_close_time_ms(1999) + 1
    gapped.close_time_ms = 4999;
    EXPECT_FALSE(guard.accept(gapped));  // the gap-triggering bar itself is NOT accepted
    EXPECT_TRUE(guard.is_suspended());
}

TEST(KlineBarGapGuard, SubsequentBarsWhileSuspendedAreAlsoRejected) {
    KlineBarGapGuard guard;
    KlineWsEvent first{};
    first.open_time_ms = 1000;
    first.close_time_ms = 1999;
    ASSERT_TRUE(guard.accept(first));
    KlineWsEvent gapped{};
    gapped.open_time_ms = 4000;
    gapped.close_time_ms = 4999;
    ASSERT_FALSE(guard.accept(gapped));
    ASSERT_TRUE(guard.is_suspended());

    KlineWsEvent nextAfterGap{};
    nextAfterGap.open_time_ms = 5000;  // would be contiguous with `gapped`, but the guard must
    nextAfterGap.close_time_ms = 5999; // stay frozen -- it never adopted `gapped` as a baseline
    EXPECT_FALSE(guard.accept(nextAfterGap));
    EXPECT_TRUE(guard.is_suspended());
}

TEST(KlineBarGapGuard, ResumeClearsSuspensionAndResetsBaseline) {
    KlineBarGapGuard guard;
    KlineWsEvent first{};
    first.open_time_ms = 1000;
    first.close_time_ms = 1999;
    ASSERT_TRUE(guard.accept(first));
    KlineWsEvent gapped{};
    gapped.open_time_ms = 9000;
    gapped.close_time_ms = 9999;
    ASSERT_FALSE(guard.accept(gapped));
    ASSERT_TRUE(guard.is_suspended());

    guard.resume();
    EXPECT_FALSE(guard.is_suspended());

    // After resume(), the very next bar becomes the new baseline unconditionally (no stale
    // continuity check against the pre-gap value) -- this is exactly the "REST 补齐历史后重新
    // 起算" recovery path the plan describes: the caller backfills history out-of-band, then the
    // next observed bar is simply trusted as correct.
    KlineWsEvent afterResume{};
    afterResume.open_time_ms = 50000;  // arbitrary -- not contiguous with anything before
    afterResume.close_time_ms = 50999;
    EXPECT_TRUE(guard.accept(afterResume));
    EXPECT_FALSE(guard.is_suspended());
}

// --- ingest_closed_bar(): ring-full is a gap too (外部复核 P0-04) ---
// The bug this group is the regression for: KlineBarGapGuard::accept() advances its continuity
// baseline BEFORE the ring push, so a failed push used to leave the guard believing "no gap"
// while the consumer was one bar short -- every later contiguous bar then sailed through and
// silently corrupted SMA/EMA/RSI state. These tests exercise the real SpscRing (no network).

namespace {

// Contiguous 1-second-ish bars: bar i spans [1000*i, 1000*i + 999], so bar i+1 opens at
// (close of bar i) + 1 exactly -- the continuity rule KlineBarGapGuard enforces.
KlineWsEvent contiguous_bar(std::int64_t i) {
    KlineWsEvent ev{};
    ev.open_time_ms = 1000 * i;
    ev.close_time_ms = 1000 * i + 999;
    ev.is_closed = true;
    return ev;
}

}  // namespace

TEST(IngestClosedBar, ContiguousBarsArePushed) {
    KlineBarGapGuard guard;
    KlineWsEventRing ring;
    EXPECT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(0)), hy::KlineIngestResult::Pushed);
    EXPECT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(1)), hy::KlineIngestResult::Pushed);
    EXPECT_FALSE(guard.is_suspended());
}

TEST(IngestClosedBar, GapIsDetectedAndTheGapBarIsNotPushed) {
    KlineBarGapGuard guard;
    KlineWsEventRing ring;
    ASSERT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(0)), hy::KlineIngestResult::Pushed);
    EXPECT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(5)),  // bars 1-4 missing
              hy::KlineIngestResult::GapDetected);
    EXPECT_TRUE(guard.is_suspended());

    KlineWsEvent popped{};
    ASSERT_TRUE(ring.try_pop(popped));  // bar 0 only
    EXPECT_EQ(popped.open_time_ms, 0);
    EXPECT_FALSE(ring.try_pop(popped));  // the gap bar never reached the ring
}

// THE regression for P0-04: fill the ring with no consumer, lose one closed bar, then let the
// consumer drain EVERYTHING and offer the very next contiguous bar. Before the fix that bar was
// accepted (the guard's baseline had advanced past the lost bar) and the consumer was silently
// one bar short from then on. It must now be refused until resume().
TEST(IngestClosedBar, RingFullSuspendsAndLaterContiguousBarsAreRefusedEvenAfterTheConsumerDrains) {
    KlineBarGapGuard guard;
    KlineWsEventRing ring;
    const std::int64_t capacity = static_cast<std::int64_t>(KlineWsEventRing::capacity());

    for (std::int64_t i = 0; i < capacity; ++i) {
        ASSERT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(i)), hy::KlineIngestResult::Pushed)
            << "bar " << i;
    }
    // The ring is now genuinely full: this closed bar is lost.
    EXPECT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(capacity)),
              hy::KlineIngestResult::RingFull);
    EXPECT_TRUE(guard.is_suspended());

    // Consumer catches up completely -- the ring now has plenty of room.
    KlineWsEvent popped{};
    std::int64_t drained = 0;
    while (ring.try_pop(popped)) ++drained;
    ASSERT_EQ(drained, capacity);

    // The next bar is contiguous with the LOST bar, so the old baseline would have accepted it.
    EXPECT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(capacity + 1)),
              hy::KlineIngestResult::SuspendedEarlier);
    EXPECT_FALSE(ring.try_pop(popped));  // nothing leaked into the ring
}

TEST(IngestClosedBar, ResumeAfterRingFullRestoresIngestionFromTheNextBar) {
    KlineBarGapGuard guard;
    KlineWsEventRing ring;
    const std::int64_t capacity = static_cast<std::int64_t>(KlineWsEventRing::capacity());
    for (std::int64_t i = 0; i < capacity; ++i) {
        ASSERT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(i)), hy::KlineIngestResult::Pushed);
    }
    ASSERT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(capacity)),
              hy::KlineIngestResult::RingFull);
    KlineWsEvent popped{};
    while (ring.try_pop(popped)) {}

    guard.resume();  // the caller's explicit "I have rebuilt state" acknowledgement
    EXPECT_EQ(hy::ingest_closed_bar(guard, ring, contiguous_bar(capacity + 10)),
              hy::KlineIngestResult::Pushed);  // new baseline, no continuity check against the past
    EXPECT_FALSE(guard.is_suspended());
}

TEST(KlineBarGapGuard, ForceSuspendRejectsEverythingUntilResume) {
    KlineBarGapGuard guard;
    ASSERT_TRUE(guard.accept(contiguous_bar(0)));
    guard.force_suspend();
    EXPECT_TRUE(guard.is_suspended());
    EXPECT_FALSE(guard.accept(contiguous_bar(1)));  // would have been contiguous
    guard.resume();
    EXPECT_TRUE(guard.accept(contiguous_bar(1)));
}

// --- BinanceKlineWsSession: lifecycle / connection failure (blackhole fixture, no real network) ---

TEST(BinanceKlineWsSessionLifecycle, InvalidConfigFailsWithoutTouchingNetwork) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;  // never contacted if this test passes
    SessionFixture fx;

    KlineWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    cfg.symbol = "BTCUSDT";  // invalid: uppercase
    cfg.interval = "1m";

    auto session =
        std::make_shared<BinanceKlineWsSession>(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, cfg);
    session->start();

    std::thread io_thread([&fx] { fx.ioc.run(); });
    io_thread.join();

    EXPECT_TRUE(session->stopped());
    auto stats = session->stats_snapshot();
    EXPECT_EQ(stats.last_error_stage, "invalid_config");
    EXPECT_EQ(blackhole.accepted_connections(), 0u);
}

TEST(BinanceKlineWsSessionConnectivity, TlsHandshakeStageTimeout) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;

    KlineWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    cfg.symbol = "btcusdt";
    cfg.interval = "1m";

    auto session =
        std::make_shared<BinanceKlineWsSession>(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, cfg);
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

TEST(BinanceKlineWsSessionLifecycle, RequestedStopCancelsPendingHandshakeWithoutErrorOrLeak) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;

    KlineWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    cfg.symbol = "btcusdt";
    cfg.interval = "1m";

    auto session =
        std::make_shared<BinanceKlineWsSession>(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, cfg);
    std::weak_ptr<BinanceKlineWsSession> weak_session = session;
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

TEST(BinanceKlineWsSessionLifecycle, ExactlyOnceStartGuard) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;

    KlineWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    cfg.symbol = "btcusdt";
    cfg.interval = "1m";

    auto session =
        std::make_shared<BinanceKlineWsSession>(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, cfg);
    session->start();
    session->start();  // must be a silent no-op, not a second resolve attempt
    session->start();

    std::thread io_thread([&fx] { fx.ioc.run(); });
    io_thread.join();

    auto stats = session->stats_snapshot();
    EXPECT_EQ(stats.errors, 1u);
}

TEST(BinanceKlineWsSessionLifecycle, ResumeAfterGapAndIsSuspendedAreSafeBeforeAndAfterShutdown) {
    // A never-started (or already-stopped) session's resume_after_gap() posts onto a strand
    // whose io_context either never ran or already finished -- must not crash, hang, or throw.
    // is_suspended() defaults false (no gap has ever been observed).
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    SessionFixture fx;

    KlineWsSessionConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = std::to_string(blackhole.port());
    cfg.symbol = "btcusdt";
    cfg.interval = "1m";

    auto session =
        std::make_shared<BinanceKlineWsSession>(fx.ioc, fx.ssl_ctx, fx.ring, fx.parser, cfg);
    EXPECT_FALSE(session->is_suspended());
    session->resume_after_gap();  // posted work never runs (io_context never started) -- fine

    session->start();
    std::thread io_thread([&fx] { fx.ioc.run(); });
    session->stop();
    io_thread.join();

    EXPECT_FALSE(session->is_suspended());
}
