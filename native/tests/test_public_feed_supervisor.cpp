// 批次 6 6b-0f-2: public_feed_supervisor.hpp tests -- Boost-free, no network, no threads.
//
// The supervisor is generic over the session type, so every test drives it with a FakeSession and an
// explicit fake clock (poll(now_ms)); jitter is off (0%) except in the tests that are about jitter, so
// delays can be asserted exactly. The cross-thread story (a real producer thread feeding one shared
// ring across generations) is in test_public_feed_supervisor_concurrency.cpp.

#include <gtest/gtest.h>
#include <hengyuan/public_feed_policy.hpp>
#include <hengyuan/public_feed_supervisor.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using hy::FeedState;
using hy::FeedSupervisorPolicy;
using hy::FeedTerminalReason;
using hy::PublicFeedSupervisor;
using hy::detail::FeedJitter;
using hy::detail::feed_elapsed_ms;
using hy::detail::feed_sat_add_ms;
using hy::detail::jittered_ms;
using hy::detail::kFeedFallbackBackoffMs;
using hy::detail::saturating_backoff_ms;

namespace {

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::uint32_t kU32Max = std::numeric_limits<std::uint32_t>::max();

// Stands in for BinanceKlineWsSession / BinanceWsSession: the same four calls, the same relaxed
// atomics the supervisor is allowed to read cross-thread.
struct FakeSession {
    explicit FakeSession(std::uint64_t g) : generation(g) {}

    void start() { start_calls.fetch_add(1); }
    void stop() {
        stop_calls.fetch_add(1);
        if (auto_stop) stopped_flag.store(true);  // false = stop() posted, the session is still winding down
    }
    bool stopped() const { return stopped_flag.load(); }
    bool is_connected() const { return connected_flag.load(); }

    void connect() { connected_flag.store(true); }
    void die() { stopped_flag.store(true); }  // fail-stop: any failure ends the session

    const std::uint64_t generation;
    bool auto_stop{true};
    std::atomic<int> start_calls{0};
    std::atomic<int> stop_calls{0};
    std::atomic<bool> connected_flag{false};
    std::atomic<bool> stopped_flag{false};
};

static_assert(hy::SupervisedFeedSession<FakeSession>);

struct MissingIsConnected {
    void start();
    void stop();
    bool stopped() const;
};
static_assert(!hy::SupervisedFeedSession<MissingIsConnected>,
              "a session that cannot report connectedness cannot be supervised");

// initial 1s, doubling, capped at 8s; no jitter; no rollover; never terminal.
FeedSupervisorPolicy quiet_policy() {
    FeedSupervisorPolicy p;
    p.initial_backoff_ms = 1000;
    p.backoff_multiplier = 2;
    p.max_backoff_ms = 8000;
    p.jitter_percent = 0;
    p.connect_deadline_ms = 30'000;
    p.drain_timeout_ms = 5'000;
    p.stable_after_ms = 60'000;
    p.max_consecutive_failures = 0;
    p.max_connection_age_ms = 0;
    return p;
}

struct Rig {
    using Sup = PublicFeedSupervisor<FakeSession>;

    explicit Rig(FeedSupervisorPolicy policy = quiet_policy(), Sup::DrainFn drain = {},
                 Sup::BoundaryFn boundary = {}, bool auto_stop = true)
        : sup(
              [this, auto_stop](std::uint64_t gen) {
                  auto s = std::make_shared<FakeSession>(gen);
                  s->auto_stop = auto_stop;
                  made.push_back(s);
                  return s;
              },
              policy, std::move(drain), std::move(boundary)) {}

    FakeSession& last() { return *made.back(); }

    std::vector<std::shared_ptr<FakeSession>> made;  // declared before sup: the factory appends to it
    Sup sup;
};

}  // namespace

// --- backoff arithmetic ------------------------------------------------------------------------

TEST(FeedBackoff, GrowsGeometricallyAndSaturates) {
    const std::int64_t expected[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
    for (std::uint32_t attempt = 0; attempt < std::size(expected); ++attempt) {
        EXPECT_EQ(saturating_backoff_ms(1000, 2, 60'000, attempt), expected[attempt]) << "attempt=" << attempt;
    }
}

TEST(FeedBackoff, NeverOverflowsWhateverTheInputs) {
    EXPECT_EQ(saturating_backoff_ms(1, 2, kI64Max, 200), kI64Max);
    EXPECT_EQ(saturating_backoff_ms(kI64Max / 2, 3, kI64Max, 5), kI64Max);
    EXPECT_EQ(saturating_backoff_ms(1000, kU32Max, 60'000, 3), 60'000);
    EXPECT_EQ(saturating_backoff_ms(1000, 2, 60'000, kU32Max), 60'000);
}

TEST(FeedBackoff, AMultiplierOfOneNeverGrowsAndNeverSpins) {
    // Without the early return this would loop 4 billion times.
    EXPECT_EQ(saturating_backoff_ms(1500, 1, 60'000, kU32Max), 1500);
    EXPECT_EQ(saturating_backoff_ms(1500, 0, 60'000, 7), 1500);  // 0 is treated as 1
}

TEST(FeedBackoff, InitialAboveMaxIsClampedToMax) {
    EXPECT_EQ(saturating_backoff_ms(90'000, 2, 60'000, 0), 60'000);
}

TEST(FeedBackoff, ADegenerateCurveFailsClosedAndNeverToZero) {
    EXPECT_EQ(saturating_backoff_ms(0, 2, 5000, 0), 5000);
    EXPECT_EQ(saturating_backoff_ms(-5, 2, 5000, 3), 5000);
    EXPECT_EQ(saturating_backoff_ms(1000, 2, 0, 0), kFeedFallbackBackoffMs);
    EXPECT_EQ(saturating_backoff_ms(1000, 2, -1, 0), kFeedFallbackBackoffMs);
    EXPECT_EQ(saturating_backoff_ms(0, 0, 0, 0), kFeedFallbackBackoffMs);
    EXPECT_GT(kFeedFallbackBackoffMs, 0);
}

// --- jitter -------------------------------------------------------------------------------------

TEST(FeedJitter, StaysInsideTheBandAndActuallyUsesIt) {
    FeedJitter rng(12345);
    std::int64_t lo = kI64Max;
    std::int64_t hi = 0;
    bool in_band = true;
    for (int i = 0; i < 20'000; ++i) {
        const std::int64_t d = jittered_ms(1000, 20, rng.next());
        in_band = in_band && d >= 800 && d <= 1200;
        lo = d < lo ? d : lo;
        hi = d > hi ? d : hi;
    }
    EXPECT_TRUE(in_band);
    EXPECT_LE(lo, 810) << "the low end of the band is never reached -- jitter is not spreading";
    EXPECT_GE(hi, 1190) << "the high end of the band is never reached -- jitter is not spreading";
}

TEST(FeedJitter, ZeroPercentIsExactlyTheBase) {
    FeedJitter rng(7);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(jittered_ms(1234, 0, rng.next()), 1234);
}

TEST(FeedJitter, PercentAbove100ClampsTo100) {
    FeedJitter rng(99);
    bool in_band = true;
    std::int64_t lo = kI64Max;
    std::int64_t hi = 0;
    for (int i = 0; i < 20'000; ++i) {
        const std::int64_t d = jittered_ms(1000, 5000, rng.next());
        in_band = in_band && d >= 1 && d <= 2000;
        lo = d < lo ? d : lo;
        hi = d > hi ? d : hi;
    }
    EXPECT_TRUE(in_band);
    EXPECT_LE(lo, 100);
    EXPECT_GE(hi, 1900);
}

TEST(FeedJitter, NeverBelowOneMillisecondForAPositiveBase) {
    FeedJitter rng(3);
    for (int i = 0; i < 2000; ++i) EXPECT_GE(jittered_ms(1, 100, rng.next()), 1);
    EXPECT_GE(jittered_ms(2, 100, 0), 1);  // the band's floor is 0; it must be lifted to 1
}

TEST(FeedJitter, ADayLongBaseCannotOverflow) {
    constexpr std::int64_t kDay = hy::detail::kFeedMaxBackoffCapMs;
    FeedJitter rng(5);
    for (int i = 0; i < 2000; ++i) {
        const std::int64_t d = jittered_ms(kDay, 100, rng.next());
        ASSERT_GE(d, 1);
        ASSERT_LE(d, 2 * kDay);
    }
}

TEST(FeedJitter, DeterministicPerSeedAndDifferentAcrossSeeds) {
    FeedJitter a(42);
    FeedJitter b(42);
    FeedJitter c(43);
    bool differs = false;
    for (int i = 0; i < 16; ++i) {
        const std::uint64_t x = a.next();
        EXPECT_EQ(x, b.next());
        differs = differs || x != c.next();
    }
    EXPECT_TRUE(differs);
}

TEST(FeedJitter, AZeroSeedIsNotADegenerateGenerator) {
    FeedJitter rng(0);  // xorshift with an all-zero state would emit 0 forever
    const std::uint64_t first = rng.next();
    EXPECT_NE(first, 0U);
    EXPECT_NE(rng.next(), first);
}

// --- time arithmetic ----------------------------------------------------------------------------

TEST(FeedTime, ElapsedIsClampedAndOverflowSafe) {
    EXPECT_EQ(feed_elapsed_ms(10, 5), 5);
    EXPECT_EQ(feed_elapsed_ms(5, 5), 0);
    EXPECT_EQ(feed_elapsed_ms(5, 10), 0);  // a clock that went backwards: nothing has elapsed
    EXPECT_EQ(feed_elapsed_ms(kI64Max, -1), kI64Max);
    EXPECT_EQ(feed_elapsed_ms(0, kI64Min), kI64Max);
    EXPECT_EQ(feed_elapsed_ms(kI64Max, kI64Min), kI64Max);
}

TEST(FeedTime, AdditionSaturatesInsteadOfWrapping) {
    EXPECT_EQ(feed_sat_add_ms(5, 7), 12);
    EXPECT_EQ(feed_sat_add_ms(kI64Max, 0), kI64Max);
    EXPECT_EQ(feed_sat_add_ms(kI64Max, 1), kI64Max);
    EXPECT_EQ(feed_sat_add_ms(kI64Max - 3, 10), kI64Max);
    EXPECT_EQ(feed_sat_add_ms(kI64Min, -1), kI64Min);
}

TEST(FeedStateName, EveryStateHasADistinctName) {
    const FeedState all[] = {FeedState::Idle,     FeedState::Connecting, FeedState::Connected,
                             FeedState::Draining, FeedState::Backoff,    FeedState::Terminal};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        EXPECT_STRNE(hy::feed_state_name(all[i]), "?");
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            EXPECT_STRNE(hy::feed_state_name(all[i]), hy::feed_state_name(all[j]));
        }
    }
}

// --- starting, connecting, failing -----------------------------------------------------------------

TEST(PublicFeedSupervisor, TheFirstPollStartsGenerationOneImmediately) {
    Rig rig;
    EXPECT_EQ(rig.sup.state(), FeedState::Idle);
    EXPECT_EQ(rig.sup.generation(), 0U);
    EXPECT_EQ(rig.sup.current(), nullptr);

    rig.sup.poll(1000);
    ASSERT_EQ(rig.made.size(), 1U);
    EXPECT_EQ(rig.last().generation, 1U);
    EXPECT_EQ(rig.last().start_calls.load(), 1);
    EXPECT_EQ(rig.sup.state(), FeedState::Connecting);
    EXPECT_EQ(rig.sup.generation(), 1U);
    EXPECT_FALSE(rig.sup.healthy());
    EXPECT_EQ(rig.sup.current().get(), &rig.last());
    EXPECT_EQ(rig.sup.stats().total_attempts, 1U);
}

TEST(PublicFeedSupervisor, ObservingTheConnectionMakesTheFeedHealthy) {
    Rig rig;
    rig.sup.poll(100);
    rig.last().connect();
    rig.sup.poll(150);
    EXPECT_EQ(rig.sup.state(), FeedState::Connected);
    EXPECT_TRUE(rig.sup.healthy());
    EXPECT_EQ(rig.sup.stats().last_connected_ms, 150);

    for (std::int64_t t = 200; t < 5000; t += 300) rig.sup.poll(t);
    EXPECT_EQ(rig.sup.state(), FeedState::Connected);
    EXPECT_EQ(rig.last().start_calls.load(), 1);  // steady state touches nothing
    EXPECT_EQ(rig.last().stop_calls.load(), 0);
    EXPECT_EQ(rig.made.size(), 1U);
}

TEST(PublicFeedSupervisor, ASessionThatDiesBeforeConnectingBacksOffAndIsNeverRestartedEarly) {
    Rig rig;
    rig.sup.poll(100);
    rig.last().die();
    rig.sup.poll(200);  // observes the stop; no drain hook, so it goes straight to Backoff

    EXPECT_EQ(rig.sup.state(), FeedState::Backoff);
    EXPECT_FALSE(rig.sup.healthy());
    EXPECT_EQ(rig.sup.current(), nullptr);  // the dead session is released
    EXPECT_EQ(rig.sup.stats().consecutive_failures, 1U);
    EXPECT_EQ(rig.sup.stats().total_failures, 1U);
    EXPECT_EQ(rig.sup.stats().next_attempt_ms, 1200);

    rig.sup.poll(1199);
    EXPECT_EQ(rig.made.size(), 1U);
    rig.sup.poll(1200);
    ASSERT_EQ(rig.made.size(), 2U);
    EXPECT_EQ(rig.last().generation, 2U);
    EXPECT_EQ(rig.sup.state(), FeedState::Connecting);
}

TEST(PublicFeedSupervisor, BackoffEscalatesAcrossConsecutiveFailuresThenSaturates) {
    Rig rig;  // 1s, x2, cap 8s
    const std::int64_t expected_delay[] = {1000, 2000, 4000, 8000, 8000, 8000};
    std::int64_t now = 0;
    rig.sup.poll(now);
    for (std::size_t i = 0; i < std::size(expected_delay); ++i) {
        rig.last().die();
        rig.sup.poll(now);
        ASSERT_EQ(rig.sup.state(), FeedState::Backoff) << "failure " << i;
        EXPECT_EQ(rig.sup.stats().next_attempt_ms - now, expected_delay[i]) << "failure " << i;
        EXPECT_EQ(rig.sup.stats().consecutive_failures, i + 1);
        now = rig.sup.stats().next_attempt_ms;
        rig.sup.poll(now);
        ASSERT_EQ(rig.made.size(), i + 2);
    }
}

TEST(PublicFeedSupervisor, ABriefConnectionDoesNotResetTheFailureStreak) {
    // A peer that completes the handshake and drops seconds later must keep escalating, or it would
    // be retried every initial_backoff_ms forever.
    Rig rig;  // stable_after_ms = 60s
    rig.sup.poll(0);
    rig.last().die();
    rig.sup.poll(0);  // failure 1 -> next at 1000
    rig.sup.poll(1000);
    ASSERT_EQ(rig.made.size(), 2U);

    rig.last().connect();
    rig.sup.poll(1010);  // connected at 1010
    rig.last().die();
    rig.sup.poll(6010);  // 5s later: far below stable_after
    EXPECT_EQ(rig.sup.stats().consecutive_failures, 2U);
    EXPECT_EQ(rig.sup.stats().next_attempt_ms - 6010, 2000);
}

TEST(PublicFeedSupervisor, ALongHealthyRunThatEndsStartsAFreshFailureStreak) {
    Rig rig;
    rig.sup.poll(0);
    rig.last().die();
    rig.sup.poll(0);  // failure 1
    rig.sup.poll(1000);
    ASSERT_EQ(rig.made.size(), 2U);

    rig.last().connect();
    rig.sup.poll(1010);
    rig.last().die();
    rig.sup.poll(1010 + 60'000);  // exactly stable_after_ms later
    EXPECT_EQ(rig.sup.stats().consecutive_failures, 1U) << "the streak should have restarted";
    EXPECT_EQ(rig.sup.stats().next_attempt_ms - (1010 + 60'000), 1000);
    EXPECT_EQ(rig.sup.stats().total_failures, 2U);  // the lifetime total still counts both
}

// --- connect deadline & winding down -------------------------------------------------------------------

TEST(PublicFeedSupervisor, ASessionStuckConnectingIsStoppedOnceAndCountedFailed) {
    Rig rig(quiet_policy(), {}, {}, /*auto_stop=*/false);
    rig.sup.poll(0);
    rig.sup.poll(29'999);
    EXPECT_EQ(rig.last().stop_calls.load(), 0);

    rig.sup.poll(30'000);  // connect_deadline_ms reached
    EXPECT_EQ(rig.last().stop_calls.load(), 1);
    EXPECT_EQ(rig.sup.stats().connect_timeouts, 1U);

    for (std::int64_t t = 30'500; t <= 40'000; t += 500) rig.sup.poll(t);
    EXPECT_EQ(rig.last().stop_calls.load(), 1) << "stop() must be requested once, not every tick";
    EXPECT_EQ(rig.sup.state(), FeedState::Connecting);  // still winding down
    EXPECT_EQ(rig.sup.stats().total_failures, 0U);      // not a failure until it has actually stopped

    rig.last().die();
    rig.sup.poll(41'000);
    EXPECT_EQ(rig.sup.stats().total_failures, 1U);
    EXPECT_EQ(rig.sup.state(), FeedState::Backoff);
}

TEST(PublicFeedSupervisor, NoSuccessorStartsWhileTheOldSessionIsStillWindingDown) {
    Rig rig(quiet_policy(), {}, {}, /*auto_stop=*/false);
    rig.sup.poll(0);
    rig.sup.poll(30'000);  // deadline: stop() requested, but the fake keeps running
    ASSERT_EQ(rig.last().stop_calls.load(), 1);

    for (std::int64_t t = 30'001; t < 5'000'000; t += 49'999) rig.sup.poll(t);
    EXPECT_EQ(rig.made.size(), 1U) << "a new generation started while the old one had not stopped";

    rig.last().die();
    rig.sup.poll(5'000'000);
    rig.sup.poll(5'001'000);
    EXPECT_EQ(rig.made.size(), 2U);
}

TEST(PublicFeedSupervisor, TheConnectDeadlineIsMeasuredFromTheStartNotFromTheFirstPoll) {
    Rig rig;
    rig.sup.poll(100'000);
    rig.sup.poll(100'000 + 29'999);
    EXPECT_EQ(rig.last().stop_calls.load(), 0);
    rig.sup.poll(100'000 + 30'000);
    EXPECT_EQ(rig.last().stop_calls.load(), 1);
}

TEST(PublicFeedSupervisor, AClockThatGoesBackwardsNeverFiresADeadline) {
    Rig rig;
    rig.sup.poll(100'000);
    rig.sup.poll(50);  // non-monotonic input
    EXPECT_EQ(rig.last().stop_calls.load(), 0);
    EXPECT_EQ(rig.sup.state(), FeedState::Connecting);
    rig.sup.poll(100'000 + 29'999);
    EXPECT_EQ(rig.last().stop_calls.load(), 0);
}

// --- the drain ack -------------------------------------------------------------------------------------

TEST(PublicFeedSupervisor, TheNextGenerationWaitsForTheConsumersDrainAck) {
    int calls = 0;
    Rig rig(quiet_policy(), [&calls] { return ++calls >= 3; });
    rig.sup.poll(0);
    rig.last().die();

    rig.sup.poll(10);  // drain call 1: not yet
    EXPECT_EQ(rig.sup.state(), FeedState::Draining);
    EXPECT_EQ(calls, 1);
    rig.sup.poll(20);  // call 2: not yet
    EXPECT_EQ(rig.sup.state(), FeedState::Draining);
    EXPECT_EQ(calls, 2);
    rig.sup.poll(30);  // call 3: acked -> Backoff, delay counted from the ack
    EXPECT_EQ(rig.sup.state(), FeedState::Backoff);
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(rig.sup.stats().next_attempt_ms, 30 + 1000);
    EXPECT_EQ(rig.sup.stats().drain_timeouts, 0U);

    rig.sup.poll(31);
    rig.sup.poll(1029);
    EXPECT_EQ(calls, 3) << "the drain hook must only be called while draining";
    EXPECT_EQ(rig.made.size(), 1U);
    rig.sup.poll(1030);
    EXPECT_EQ(rig.made.size(), 2U);
}

TEST(PublicFeedSupervisor, ADrainThatNeverAcksTimesOutAndProceedsAnyway) {
    int calls = 0;
    Rig rig(quiet_policy(), [&calls] {
        ++calls;
        return false;
    });
    rig.sup.poll(0);
    rig.last().die();
    rig.sup.poll(100);  // Draining from t=100, drain_timeout_ms = 5000
    EXPECT_EQ(rig.sup.state(), FeedState::Draining);

    rig.sup.poll(5099);
    EXPECT_EQ(rig.sup.state(), FeedState::Draining);
    EXPECT_EQ(rig.sup.stats().drain_timeouts, 0U);

    rig.sup.poll(5100);
    EXPECT_EQ(rig.sup.state(), FeedState::Backoff);
    EXPECT_EQ(rig.sup.stats().drain_timeouts, 1U);
    EXPECT_EQ(rig.sup.stats().next_attempt_ms, 5100 + 1000);
}

TEST(PublicFeedSupervisor, TheDrainHookIsNotConsultedWhileASessionIsRunning) {
    int calls = 0;
    Rig rig(quiet_policy(), [&calls] {
        ++calls;
        return true;
    });
    rig.sup.poll(0);
    rig.last().connect();
    for (std::int64_t t = 10; t < 100'000; t += 997) rig.sup.poll(t);
    EXPECT_EQ(calls, 0);
}

// --- planned rollover ---------------------------------------------------------------------------------

TEST(PublicFeedSupervisor, AHealthySessionIsRolledOverWithoutBackoffOrAFailure) {
    FeedSupervisorPolicy p = quiet_policy();
    p.max_connection_age_ms = 1000;
    Rig rig(p);

    rig.sup.poll(0);
    rig.last().connect();
    rig.sup.poll(10);  // connected at 10
    rig.sup.poll(1009);
    EXPECT_EQ(rig.last().stop_calls.load(), 0);

    rig.sup.poll(1010);  // age == max_connection_age_ms
    ASSERT_EQ(rig.last().stop_calls.load(), 1);
    EXPECT_EQ(rig.made.size(), 1U);  // the successor waits for the old session to stop

    rig.sup.poll(1011);  // sees stopped(); a planned end starts the successor in the same tick
    ASSERT_EQ(rig.made.size(), 2U);
    EXPECT_EQ(rig.last().generation, 2U);
    EXPECT_EQ(rig.sup.state(), FeedState::Connecting);
    EXPECT_EQ(rig.sup.stats().rollovers, 1U);
    EXPECT_EQ(rig.sup.stats().total_failures, 0U);
    EXPECT_EQ(rig.sup.stats().consecutive_failures, 0U);
}

TEST(PublicFeedSupervisor, ARolloverWaitsForTheConsumersBoundary) {
    FeedSupervisorPolicy p = quiet_policy();
    p.max_connection_age_ms = 1000;
    bool boundary_ok = false;
    int asked = 0;
    Rig rig(p, {}, [&](std::int64_t) {
        ++asked;
        return boundary_ok;
    });

    rig.sup.poll(0);
    rig.last().connect();
    rig.sup.poll(10);
    for (std::int64_t t = 1010; t <= 3000; t += 250) rig.sup.poll(t);
    EXPECT_GT(asked, 0);
    EXPECT_EQ(rig.last().stop_calls.load(), 0) << "rolled over without the consumer's approval";

    boundary_ok = true;
    rig.sup.poll(3100);
    EXPECT_EQ(rig.last().stop_calls.load(), 1);
}

TEST(PublicFeedSupervisor, TheBoundaryIsNotAskedBeforeARolloverIsDue) {
    FeedSupervisorPolicy p = quiet_policy();
    p.max_connection_age_ms = 1000;
    int asked = 0;
    Rig rig(p, {}, [&](std::int64_t) {
        ++asked;
        return true;
    });
    rig.sup.poll(0);
    rig.last().connect();
    rig.sup.poll(10);
    rig.sup.poll(1009);
    EXPECT_EQ(asked, 0);
}

TEST(PublicFeedSupervisor, ConnectionAgeIsMeasuredFromTheConnectNotFromTheStart) {
    FeedSupervisorPolicy p = quiet_policy();
    p.max_connection_age_ms = 1000;
    Rig rig(p);
    rig.sup.poll(0);
    rig.sup.poll(5000);  // still connecting, older than max_connection_age_ms: not a rollover candidate
    EXPECT_EQ(rig.last().stop_calls.load(), 0);

    rig.last().connect();
    rig.sup.poll(6000);  // connected at 6000
    rig.sup.poll(6999);
    EXPECT_EQ(rig.last().stop_calls.load(), 0);
    rig.sup.poll(7000);
    EXPECT_EQ(rig.last().stop_calls.load(), 1);
}

TEST(PublicFeedSupervisor, RolloverOnlyRequestsTheStopOnce) {
    FeedSupervisorPolicy p = quiet_policy();
    p.max_connection_age_ms = 1000;
    Rig rig(p, {}, {}, /*auto_stop=*/false);
    rig.sup.poll(0);
    rig.last().connect();
    rig.sup.poll(10);
    for (std::int64_t t = 1010; t < 9000; t += 400) rig.sup.poll(t);
    EXPECT_EQ(rig.last().stop_calls.load(), 1);
    EXPECT_EQ(rig.made.size(), 1U);
}

// --- terminal & shutdown -----------------------------------------------------------------------------------

TEST(PublicFeedSupervisor, GivesUpAfterTheConfiguredNumberOfConsecutiveFailures) {
    FeedSupervisorPolicy p = quiet_policy();
    p.max_consecutive_failures = 3;
    Rig rig(p);

    std::int64_t now = 0;
    rig.sup.poll(now);
    for (int failure = 1; failure <= 2; ++failure) {
        rig.last().die();
        rig.sup.poll(now);
        ASSERT_EQ(rig.sup.state(), FeedState::Backoff) << "failure " << failure;
        now = rig.sup.stats().next_attempt_ms;
        rig.sup.poll(now);
    }
    ASSERT_EQ(rig.made.size(), 3U);

    rig.last().die();
    rig.sup.poll(now);  // the third in a row
    EXPECT_EQ(rig.sup.state(), FeedState::Terminal);
    EXPECT_TRUE(rig.sup.terminal());
    EXPECT_EQ(rig.sup.terminal_reason(), FeedTerminalReason::TooManyFailures);
    EXPECT_FALSE(rig.sup.healthy());
    EXPECT_EQ(rig.sup.current(), nullptr);

    for (std::int64_t t = now; t < now + 1'000'000; t += 50'000) rig.sup.poll(t);
    EXPECT_EQ(rig.made.size(), 3U) << "a terminal supervisor must never start another session";
}

TEST(PublicFeedSupervisor, ZeroMeansNeverGiveUp) {
    Rig rig;  // max_consecutive_failures = 0
    std::int64_t now = 0;
    rig.sup.poll(now);
    for (int i = 0; i < 50; ++i) {
        rig.last().die();
        rig.sup.poll(now);
        ASSERT_EQ(rig.sup.state(), FeedState::Backoff);
        now = rig.sup.stats().next_attempt_ms;
        rig.sup.poll(now);
    }
    EXPECT_EQ(rig.made.size(), 51U);
    EXPECT_FALSE(rig.sup.terminal());
    EXPECT_EQ(rig.sup.stats().consecutive_failures, 50U);
}

TEST(PublicFeedSupervisor, ARolloverNeverCountsTowardsTheFailureLimit) {
    FeedSupervisorPolicy p = quiet_policy();
    p.max_consecutive_failures = 2;
    p.max_connection_age_ms = 1000;
    Rig rig(p);

    std::int64_t now = 0;
    rig.sup.poll(now);
    for (int i = 0; i < 10; ++i) {
        rig.last().connect();
        rig.sup.poll(now + 10);
        now += 10 + 1000;
        rig.sup.poll(now);      // rollover due: stop() requested (auto_stop -> stopped)
        rig.sup.poll(now + 1);  // observed; the successor starts in the same tick
        now += 1;
    }
    EXPECT_FALSE(rig.sup.terminal());
    EXPECT_EQ(rig.sup.stats().rollovers, 10U);
    EXPECT_EQ(rig.sup.stats().total_failures, 0U);
    EXPECT_EQ(rig.made.size(), 11U);
}

TEST(PublicFeedSupervisor, ShutdownStopsTheCurrentSessionAndIsIdempotent) {
    Rig rig;
    rig.sup.poll(0);
    rig.last().connect();
    rig.sup.poll(10);
    ASSERT_TRUE(rig.sup.healthy());

    rig.sup.shutdown();
    EXPECT_EQ(rig.last().stop_calls.load(), 1);
    EXPECT_EQ(rig.sup.state(), FeedState::Terminal);
    EXPECT_EQ(rig.sup.terminal_reason(), FeedTerminalReason::Shutdown);
    EXPECT_EQ(rig.sup.current(), nullptr);
    EXPECT_FALSE(rig.sup.healthy());

    rig.sup.shutdown();
    rig.sup.shutdown();
    EXPECT_EQ(rig.last().stop_calls.load(), 1);

    for (std::int64_t t = 100; t < 1'000'000; t += 10'000) rig.sup.poll(t);
    EXPECT_EQ(rig.made.size(), 1U) << "no restart after shutdown";
}

TEST(PublicFeedSupervisor, ShutdownBeforeTheFirstPollStartsNothing) {
    Rig rig;
    rig.sup.shutdown();
    rig.sup.poll(0);
    rig.sup.poll(100'000);
    EXPECT_TRUE(rig.made.empty());
    EXPECT_EQ(rig.sup.terminal_reason(), FeedTerminalReason::Shutdown);
}

TEST(PublicFeedSupervisor, ShutdownAfterGivingUpKeepsTheOriginalReason) {
    FeedSupervisorPolicy p = quiet_policy();
    p.max_consecutive_failures = 1;
    Rig rig(p);
    rig.sup.poll(0);
    rig.last().die();
    rig.sup.poll(0);
    ASSERT_EQ(rig.sup.terminal_reason(), FeedTerminalReason::TooManyFailures);
    rig.sup.shutdown();
    EXPECT_EQ(rig.sup.terminal_reason(), FeedTerminalReason::TooManyFailures);
}

TEST(PublicFeedSupervisor, ShutdownWhileWindingDownDoesNotWaitForTheSession) {
    Rig rig(quiet_policy(), {}, {}, /*auto_stop=*/false);
    rig.sup.poll(0);
    rig.sup.poll(30'000);  // stop() already requested by the connect deadline
    rig.sup.shutdown();    // does not block, does not start anything
    EXPECT_EQ(rig.sup.state(), FeedState::Terminal);
    EXPECT_EQ(rig.made.size(), 1U);
}

// --- generations -----------------------------------------------------------------------------------------

TEST(PublicFeedSupervisor, FactoryGenerationsStrictlyIncreaseAndCurrentIsAlwaysTheNewest) {
    Rig rig;
    std::int64_t now = 0;
    rig.sup.poll(now);
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(rig.sup.current().get(), &rig.last());
        rig.last().die();
        rig.sup.poll(now);
        EXPECT_EQ(rig.sup.current(), nullptr) << "between generations there is no session";
        now = rig.sup.stats().next_attempt_ms;
        rig.sup.poll(now);
    }
    ASSERT_EQ(rig.made.size(), 6U);
    for (std::size_t i = 0; i < rig.made.size(); ++i) {
        EXPECT_EQ(rig.made[i]->generation, i + 1);
        EXPECT_EQ(rig.made[i]->start_calls.load(), 1);
    }
    EXPECT_EQ(rig.sup.generation(), 6U);
    EXPECT_EQ(rig.sup.stats().total_attempts, 6U);
}

TEST(PublicFeedSupervisor, AStaleSessionReferenceCannotAffectTheSupervisor) {
    Rig rig;
    rig.sup.poll(0);
    std::shared_ptr<FakeSession> old_session = rig.made[0];
    old_session->die();
    rig.sup.poll(0);
    rig.sup.poll(rig.sup.stats().next_attempt_ms);
    ASSERT_EQ(rig.made.size(), 2U);
    ASSERT_EQ(rig.sup.state(), FeedState::Connecting);
    const auto failures = rig.sup.stats().total_failures;

    // The old session's late completions land after its replacement started.
    old_session->connect();
    old_session->die();
    for (std::int64_t t = 5000; t < 9000; t += 500) rig.sup.poll(t);
    EXPECT_EQ(rig.sup.state(), FeedState::Connecting) << "the old generation's flags leaked into the new one";
    EXPECT_EQ(rig.sup.stats().total_failures, failures);
    EXPECT_EQ(rig.made.size(), 2U);

    rig.last().connect();
    rig.sup.poll(9500);
    EXPECT_EQ(rig.sup.state(), FeedState::Connected);
}

TEST(PublicFeedSupervisor, ANullSessionFromTheFactoryIsAFailureNotACrashOrASpin) {
    int calls = 0;
    PublicFeedSupervisor<FakeSession> sup(
        [&calls](std::uint64_t) {
            ++calls;
            return std::shared_ptr<FakeSession>{};
        },
        quiet_policy());

    sup.poll(0);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(sup.state(), FeedState::Backoff);
    EXPECT_EQ(sup.stats().total_failures, 1U);
    EXPECT_EQ(sup.stats().next_attempt_ms, 1000);

    sup.poll(999);
    EXPECT_EQ(calls, 1);
    sup.poll(1000);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(sup.stats().consecutive_failures, 2U);
    EXPECT_EQ(sup.stats().next_attempt_ms, 1000 + 2000);
}

TEST(PublicFeedSupervisor, ANullFactoryAtTheEndOfTimeStillReturnsPromptly) {
    // now == INT64_MAX makes every "next attempt" saturate onto now itself; a start -> fail -> start
    // recursion here would overflow the stack rather than fail an assertion.
    PublicFeedSupervisor<FakeSession> sup([](std::uint64_t) { return std::shared_ptr<FakeSession>{}; },
                                          quiet_policy());
    sup.poll(kI64Max);
    sup.poll(kI64Max);
    sup.poll(kI64Max);
    EXPECT_EQ(sup.stats().total_attempts, 3U);  // one attempt per poll, not a recursion
}

TEST(PublicFeedSupervisor, WithoutAFactoryEveryAttemptIsAFailure) {
    PublicFeedSupervisor<FakeSession> sup(PublicFeedSupervisor<FakeSession>::Factory{}, quiet_policy());
    sup.poll(0);
    EXPECT_EQ(sup.state(), FeedState::Backoff);
    EXPECT_EQ(sup.stats().total_failures, 1U);
}

// --- jitter inside the supervisor ---------------------------------------------------------------------------

TEST(PublicFeedSupervisor, TheJitterBandIsAppliedAroundTheBackoffCurve) {
    FeedSupervisorPolicy p = quiet_policy();
    p.jitter_percent = 20;
    p.max_backoff_ms = 60'000;
    p.jitter_seed = 777;
    Rig rig(p);

    bool saw_below = false;
    bool saw_above = false;
    std::int64_t now = 0;
    rig.sup.poll(now);
    for (int i = 0; i < 60; ++i) {
        // Always the FIRST failure of a streak, so the un-jittered delay is exactly 1000ms: connect
        // and stay up past stable_after so each drop restarts the streak.
        rig.last().connect();
        rig.sup.poll(now + 1);
        rig.last().die();
        now += 61'000;
        rig.sup.poll(now);
        ASSERT_EQ(rig.sup.state(), FeedState::Backoff);
        const std::int64_t delay = rig.sup.stats().next_attempt_ms - now;
        ASSERT_GE(delay, 800);
        ASSERT_LE(delay, 1200);
        saw_below = saw_below || delay < 1000;
        saw_above = saw_above || delay > 1000;
        now = rig.sup.stats().next_attempt_ms;
        rig.sup.poll(now);
    }
    EXPECT_TRUE(saw_below && saw_above) << "60 draws never straddled the un-jittered delay";
}

TEST(PublicFeedSupervisor, TheSameSeedReproducesTheSameDelaysAndADifferentSeedDoesNot) {
    auto delays_for = [](std::uint64_t seed) {
        FeedSupervisorPolicy p = quiet_policy();
        p.jitter_percent = 50;
        p.jitter_seed = seed;
        Rig rig(p);
        std::vector<std::int64_t> out;
        std::int64_t now = 0;
        rig.sup.poll(now);
        for (int i = 0; i < 12; ++i) {
            rig.last().die();
            rig.sup.poll(now);
            out.push_back(rig.sup.stats().next_attempt_ms - now);
            now = rig.sup.stats().next_attempt_ms;
            rig.sup.poll(now);
        }
        return out;
    };
    EXPECT_EQ(delays_for(5), delays_for(5));
    EXPECT_NE(delays_for(5), delays_for(6));
}

// --- configuration hygiene -------------------------------------------------------------------------------------

TEST(PublicFeedSupervisor, ADegenerateBackoffConfigNeverRetriesOnEveryPoll) {
    FeedSupervisorPolicy p = quiet_policy();
    p.initial_backoff_ms = 0;
    p.max_backoff_ms = 0;
    Rig rig(p);
    rig.sup.poll(0);
    rig.last().die();
    rig.sup.poll(0);
    EXPECT_EQ(rig.sup.state(), FeedState::Backoff);
    EXPECT_GE(rig.sup.stats().next_attempt_ms, kFeedFallbackBackoffMs);
    rig.sup.poll(1);
    EXPECT_EQ(rig.made.size(), 1U);
}

TEST(PublicFeedSupervisor, AnAbsurdBackoffIsCappedAtADay) {
    FeedSupervisorPolicy p = quiet_policy();
    p.initial_backoff_ms = kI64Max / 4;
    p.max_backoff_ms = kI64Max / 2;
    Rig rig(p);
    rig.sup.poll(0);
    rig.last().die();
    rig.sup.poll(0);
    EXPECT_EQ(rig.sup.stats().next_attempt_ms, hy::detail::kFeedMaxBackoffCapMs);
}

// --- the harness policy (public_feed_policy.hpp) ---------------------------------------------------------------

TEST(PublicFeedPolicy, TheRolloverFiresWellInsideBinancesTwentyFourHourConnectionLimit) {
    static_assert(hy::kBinanceWsConnectionLimitMs == 24LL * 60 * 60 * 1000);
    const FeedSupervisorPolicy p = hy::make_public_feed_policy(42);
    EXPECT_GT(p.max_connection_age_ms, 0) << "0 would mean no planned rollover at all";
    EXPECT_LT(p.max_connection_age_ms, hy::kBinanceWsConnectionLimitMs);
    // Slack for a slow reconnect (backoff + handshake + a backfill after it) to still land in time.
    EXPECT_GE(hy::kBinanceWsConnectionLimitMs - p.max_connection_age_ms, 30LL * 60 * 1000);
}

TEST(PublicFeedPolicy, ADeadFeedEndsTheRunInsteadOfLeavingItBlind) {
    const FeedSupervisorPolicy p = hy::make_public_feed_policy(1);
    EXPECT_EQ(p.max_consecutive_failures, hy::kPublicFeedMaxConsecutiveFailures);
    EXPECT_GT(p.max_consecutive_failures, 0U) << "0 means never give up";
}

TEST(PublicFeedPolicy, TheSeedIsCarriedAndEverythingElseKeepsTheSupervisorDefaults) {
    const FeedSupervisorPolicy defaults;
    const FeedSupervisorPolicy p = hy::make_public_feed_policy(7);
    EXPECT_EQ(p.jitter_seed, 7U);
    EXPECT_EQ(p.initial_backoff_ms, defaults.initial_backoff_ms);
    EXPECT_EQ(p.backoff_multiplier, defaults.backoff_multiplier);
    EXPECT_EQ(p.max_backoff_ms, defaults.max_backoff_ms);
    EXPECT_EQ(p.jitter_percent, defaults.jitter_percent);
    EXPECT_EQ(p.connect_deadline_ms, defaults.connect_deadline_ms);
    EXPECT_EQ(p.drain_timeout_ms, defaults.drain_timeout_ms);
    EXPECT_EQ(p.stable_after_ms, defaults.stable_after_ms);
}

// The policy against the real supervisor: what the harness actually gets.
TEST(PublicFeedPolicy, ASessionIsRolledOverAtTwentyThreeHoursAndNotBefore) {
    Rig rig(hy::make_public_feed_policy(1));
    rig.sup.poll(0);
    rig.last().connect();
    rig.sup.poll(10);  // connected at 10

    rig.sup.poll(10 + hy::kPublicFeedMaxConnectionAgeMs - 1);
    EXPECT_EQ(rig.last().stop_calls.load(), 0);
    rig.sup.poll(10 + hy::kPublicFeedMaxConnectionAgeMs);
    EXPECT_EQ(rig.last().stop_calls.load(), 1);
}

TEST(PublicFeedPolicy, TenConsecutiveFailuresMakeTheFeedTerminal) {
    Rig rig(hy::make_public_feed_policy(1));
    std::int64_t now = 0;
    rig.sup.poll(now);
    for (std::uint32_t failure = 1; failure <= hy::kPublicFeedMaxConsecutiveFailures; ++failure) {
        ASSERT_FALSE(rig.sup.terminal()) << "gave up early, at failure " << failure - 1;
        rig.last().die();
        rig.sup.poll(now);
        if (failure < hy::kPublicFeedMaxConsecutiveFailures) {
            ASSERT_EQ(rig.sup.state(), FeedState::Backoff);
            now = rig.sup.stats().next_attempt_ms;
            rig.sup.poll(now);
        }
    }
    EXPECT_TRUE(rig.sup.terminal());
    EXPECT_EQ(rig.sup.terminal_reason(), FeedTerminalReason::TooManyFailures);
}

TEST(PublicFeedPolicy, TheKlineRolloverWindowOpensOnlyRightAfterABarCloses) {
    constexpr std::int64_t kHourSpan = 3'600'000;
    constexpr std::int64_t last = 1'700'000'000'000;  // an arbitrary bar close
    // `live`/`last_close` are never varied across the calls below, so they are not lambda
    // parameters at all -- a default argument cannot name a local of the enclosing scope
    // (not even a constexpr one: [dcl.fct.default]p9, "Local variables shall not be used in a
    // default argument"). MSVC accepts it as a non-conformant extension; GCC correctly rejects
    // it ("local variable ... may not appear in this context"), caught by the WSL2/tokyo-vps
    // validation tier this header's tests hadn't run under before.
    auto open = [&](std::int64_t now, std::int64_t span) {
        return hy::kline_rollover_window_open(true, last, now, span);
    };

    // 1h bars: the first 60s after the close, exactly.
    EXPECT_TRUE(open(last, kHourSpan));
    EXPECT_TRUE(open(last + 59'999, kHourSpan));
    EXPECT_FALSE(open(last + 60'000, kHourSpan));
    EXPECT_FALSE(open(last + kHourSpan / 2, kHourSpan));

    // 1m bars: a quarter of the span (15s), not the full 60s.
    EXPECT_TRUE(open(last + 14'999, 60'000));
    EXPECT_FALSE(open(last + 15'000, 60'000));
    // 1s bars: 250ms.
    EXPECT_TRUE(open(last + 249, 1'000));
    EXPECT_FALSE(open(last + 250, 1'000));
    // "1M" has no fixed span: 60s.
    EXPECT_TRUE(open(last + 59'999, 0));
    EXPECT_FALSE(open(last + 60'000, 0));
}

TEST(PublicFeedPolicy, TheKlineRolloverWindowStaysShutWhenTheFeedIsNotInSyncOrTheInputsAreNonsense) {
    constexpr std::int64_t last = 1'700'000'000'000;
    EXPECT_FALSE(hy::kline_rollover_window_open(false, last, last + 10, 3'600'000)) << "not live: leave a recovering feed alone";
    EXPECT_FALSE(hy::kline_rollover_window_open(true, 0, 10, 3'600'000)) << "no bar consumed yet";
    EXPECT_FALSE(hy::kline_rollover_window_open(true, -5, 10, 3'600'000));
    EXPECT_FALSE(hy::kline_rollover_window_open(true, last, last - 1, 3'600'000)) << "the clock is behind the bar";
    EXPECT_FALSE(hy::kline_rollover_window_open(true, last, last + 10, 3));  // a span so small its quarter is zero
    // Extreme timestamps must not overflow.
    EXPECT_FALSE(hy::kline_rollover_window_open(true, 1, kI64Max, 3'600'000));
    EXPECT_TRUE(hy::kline_rollover_window_open(true, kI64Max - 5, kI64Max, 3'600'000));
}

// --- FeedGenerationTracker (public_feed_policy.hpp) -------------------------------------------------------------

TEST(FeedGenerationTracker, FiresOnceForTheFirstRealGenerationButNeverForTheZeroSentinel) {
    hy::FeedGenerationTracker t;
    EXPECT_FALSE(t.observe(0)) << "0 = PublicFeedSupervisor's own 'no session yet': nothing to reset from";
    EXPECT_TRUE(t.observe(1));
    EXPECT_FALSE(t.observe(1)) << "same generation again: not a new connection";
    EXPECT_FALSE(t.observe(1));
}

TEST(FeedGenerationTracker, FiresExactlyOnceForEachSubsequentGenerationChange) {
    hy::FeedGenerationTracker t;
    EXPECT_TRUE(t.observe(1));
    EXPECT_TRUE(t.observe(2));
    EXPECT_FALSE(t.observe(2));
    EXPECT_FALSE(t.observe(2));
    EXPECT_TRUE(t.observe(3));
    EXPECT_FALSE(t.observe(3));
}

TEST(FeedGenerationTracker, ADroppedThenLaterObservedZeroNeverFiresEvenAfterARealGeneration) {
    hy::FeedGenerationTracker t;
    EXPECT_TRUE(t.observe(1));
    // 0 never fires, whether it's the first value seen or comes after a real one -- current()
    // only ever returns null between generations, never generation 0 itself.
    EXPECT_FALSE(t.observe(0));
    EXPECT_FALSE(t.observe(0));
    EXPECT_TRUE(t.observe(2));
}

TEST(FeedGenerationTracker, GenerationNumbersNeedNotBeConsecutive) {
    hy::FeedGenerationTracker t;
    EXPECT_TRUE(t.observe(5));
    EXPECT_FALSE(t.observe(5));
    EXPECT_TRUE(t.observe(9));  // supervisors never skip generations in practice, but the tracker
                                // only ever compares for CHANGE, not for a +1 step
    EXPECT_FALSE(t.observe(9));
}
