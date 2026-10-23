// Private-stream migration M3b: the sweep's transport between the hot thread and the reconcile thread (barrier_sweep.hpp, sweep_pump.hpp), and the one
// addition M3b made to the barrier itself (a request that never left the hot thread is rescinded).
//
// What is pinned here:
//   * SweepJob (the reconcile thread's side): the NEWEST request wins, a repeated / older / zero epoch is ignored, the start time is the tick that took
//     the request, a finished sweep stays finished until a higher epoch arrives;
//   * the request ring is the plain SPSC primitive with four slots; the sink and the account probe fail closed when unwired;
//   * on_sweep_request_undelivered: only the outstanding request of the current epoch can be rescinded, and then the same epoch is offered again at once;
//   * SweepPump (the hot thread's side): a request is posted exactly when the barrier wants one, a full ring rescinds it, an incomplete probe does
//     nothing, and a SweepDone marker completes the barrier only for the current epoch, with nothing unresolved AND an account refreshed since the
//     request went out -- every other marker leaves the barrier Sweeping.
// The reconcile thread's half of the protocol (poll_once) is in test_order_tracker_sweep.cpp; both threads together in test_sweep_concurrency.cpp.
#include <gtest/gtest.h>

#include <hengyuan/barrier_sweep.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/reconnect_barrier.hpp>
#include <hengyuan/startup_recovery.hpp>
#include <hengyuan/submit_readiness.hpp>
#include <hengyuan/sweep_pump.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

using namespace hy;

// --- SweepJob ------------------------------------------------------------------------------------------------------------------

namespace {

class SweepJobTest : public ::testing::Test {
protected:
    SweepRequestRing requests_;
    SweepJob job_{requests_};

    void post(std::uint64_t epoch) { ASSERT_TRUE(requests_.try_push(SweepRequest{epoch})); }
};

}  // namespace

TEST_F(SweepJobTest, NothingRunsBeforeARequest) {
    EXPECT_FALSE(job_.running());
    EXPECT_EQ(job_.epoch(), 0U);
    EXPECT_FALSE(job_.take_requests(100));
    EXPECT_FALSE(job_.running());
    EXPECT_EQ(job_.stats().requests_taken, 0U);
}

TEST_F(SweepJobTest, ARequestStartsASweepAtTheTickThatTookIt) {
    post(7);
    EXPECT_TRUE(job_.take_requests(1234));
    EXPECT_TRUE(job_.running());
    EXPECT_EQ(job_.epoch(), 7U);
    EXPECT_EQ(job_.started_ms(), 1234);
    EXPECT_EQ(job_.stats().sweeps_started, 1U);
    EXPECT_EQ(job_.stats().requests_taken, 1U);
}

TEST_F(SweepJobTest, TheNewestRequestWinsWhateverTheOrder) {
    post(3);
    post(5);
    post(4);  // not how the barrier hands them out, but the job must not depend on it
    EXPECT_TRUE(job_.take_requests(10));
    EXPECT_EQ(job_.epoch(), 5U);
    EXPECT_EQ(job_.stats().sweeps_started, 2U) << "3 and then 5 each began a sweep; 4 was already below 5";
    EXPECT_EQ(job_.stats().requests_stale, 1U);
}

TEST_F(SweepJobTest, ZeroRepeatedAndOlderEpochsAreIgnored) {
    post(2);
    ASSERT_TRUE(job_.take_requests(10));
    post(0);
    post(2);  // the same epoch again
    post(1);  // older
    EXPECT_FALSE(job_.take_requests(20));
    EXPECT_EQ(job_.epoch(), 2U);
    EXPECT_EQ(job_.started_ms(), 10) << "an ignored request must not move the start of the sweep in progress";
    EXPECT_EQ(job_.stats().requests_stale, 3U);
    EXPECT_EQ(job_.stats().sweeps_started, 1U);
}

TEST_F(SweepJobTest, AHigherEpochRestartsARunningSweepAndMovesItsStart) {
    post(1);
    ASSERT_TRUE(job_.take_requests(100));
    post(2);
    EXPECT_TRUE(job_.take_requests(250));
    EXPECT_TRUE(job_.running());
    EXPECT_EQ(job_.epoch(), 2U);
    EXPECT_EQ(job_.started_ms(), 250);
}

TEST_F(SweepJobTest, AFinishedSweepStaysFinishedUntilAHigherEpochArrives) {
    post(4);
    ASSERT_TRUE(job_.take_requests(10));
    job_.finished();
    EXPECT_FALSE(job_.running());
    EXPECT_EQ(job_.stats().sweeps_completed, 1U);
    EXPECT_EQ(job_.epoch(), 4U) << "the last epoch is remembered: the same request must not start it again";
    post(4);
    EXPECT_FALSE(job_.take_requests(20));
    EXPECT_FALSE(job_.running());
    post(5);
    EXPECT_TRUE(job_.take_requests(30));
    EXPECT_TRUE(job_.running());
}

TEST_F(SweepJobTest, AMarkerThatCouldNotBePushedIsCountedAndTheSweepKeepsRunning) {
    post(1);
    ASSERT_TRUE(job_.take_requests(10));
    job_.marker_push_failed();
    job_.marker_push_failed();
    EXPECT_TRUE(job_.running());
    EXPECT_EQ(job_.stats().marker_push_failures, 2U);
    EXPECT_EQ(job_.stats().sweeps_completed, 0U);
}

// --- the ring, the sink, the probe ---------------------------------------------------------------------------------------------

TEST(SweepTransportTypes, TheRequestRingIsTheSpscPrimitiveWithFourSlots) {
    EXPECT_EQ(SweepRequestRing::capacity(), 4U);
    EXPECT_EQ(kSweepRequestRingCapacity, 4U);
    SweepRequestRing ring;
    for (std::uint64_t e = 1; e <= 4; ++e) EXPECT_TRUE(ring.try_push(SweepRequest{e}));
    EXPECT_FALSE(ring.try_push(SweepRequest{5})) << "a full ring refuses; the pump rescinds the request";
    SweepRequest r{};
    ASSERT_TRUE(ring.try_pop(r));
    EXPECT_EQ(r.epoch, 1U) << "FIFO";
    EXPECT_TRUE(std::is_trivially_copyable_v<SweepRequest>);
    EXPECT_TRUE(std::is_trivially_copyable_v<SweepDone>);
}

namespace {

struct SinkLog {
    int calls{0};
    SweepDone last{};
};
void record_done(void* user, const SweepDone& d) noexcept {
    auto* log = static_cast<SinkLog*>(user);
    ++log->calls;
    log->last = d;
}

}  // namespace

TEST(SweepTransportTypes, AnUnwiredSinkDropsAndAWiredOneDelivers) {
    const SweepSink unwired{};
    EXPECT_FALSE(unwired.is_valid());
    unwired.deliver(SweepDone{1, 0});  // must not crash

    SinkLog log;
    const SweepSink wired{&record_done, &log};
    EXPECT_TRUE(wired.is_valid());
    wired.deliver(SweepDone{9, 3});
    EXPECT_EQ(log.calls, 1);
    EXPECT_EQ(log.last.epoch, 9U);
    EXPECT_EQ(log.last.unresolved, 3U);
}

namespace {

struct ProbeLog {
    int calls{0};
    std::int64_t since{0};
    bool answer{true};
};
bool probe_answer(void* user, std::int64_t since) noexcept {
    auto* log = static_cast<ProbeLog*>(user);
    ++log->calls;
    log->since = since;
    return log->answer;
}

}  // namespace

TEST(SweepTransportTypes, AnUnwiredAccountProbeIsNeverFresh) {
    const SweepAccountProbe unwired{};
    EXPECT_FALSE(unwired.fresh_since(0)) << "nothing refreshes the account yet: fail closed";
    ProbeLog log;
    const SweepAccountProbe wired{&probe_answer, &log};
    EXPECT_TRUE(wired.fresh_since(77));
    EXPECT_EQ(log.since, 77);
    log.answer = false;
    EXPECT_FALSE(wired.fresh_since(78));
}

// --- the barrier: a request that never left ------------------------------------------------------------------------------------

namespace {

FeedObservation live(std::uint64_t generation, std::int64_t now) {
    FeedObservation o{};
    o.generation = generation;
    o.connected = true;
    o.last_activity_ms = now;
    o.sampled_at_ms = now;
    return o;
}

}  // namespace

TEST(ReconnectBarrierUndelivered, OnlyTheOutstandingRequestOfTheCurrentEpochCanBeRescinded) {
    ReconnectBarrier barrier;
    constexpr std::int64_t kNow = 10'000;
    barrier.observe(live(1, kNow));
    ASSERT_EQ(barrier.state(), BarrierState::Sweeping);

    barrier.on_sweep_request_undelivered(barrier.epoch());  // nothing outstanding yet
    EXPECT_EQ(barrier.stats().sweeps_undelivered, 0U);

    const std::optional<std::uint64_t> epoch = barrier.take_sweep_request(kNow);
    ASSERT_TRUE(epoch.has_value());
    EXPECT_TRUE(barrier.sweep_outstanding());

    barrier.on_sweep_request_undelivered(*epoch + 1);  // some other epoch
    EXPECT_TRUE(barrier.sweep_outstanding());
    EXPECT_EQ(barrier.stats().sweeps_undelivered, 0U);

    barrier.on_sweep_request_undelivered(*epoch);
    EXPECT_FALSE(barrier.sweep_outstanding());
    EXPECT_EQ(barrier.stats().sweeps_undelivered, 1U);

    barrier.on_sweep_request_undelivered(*epoch);  // already rescinded
    EXPECT_EQ(barrier.stats().sweeps_undelivered, 1U);
}

TEST(ReconnectBarrierUndelivered, TheSameEpochIsOfferedAgainAtOnceWithoutWaitingForTheTimeout) {
    ReconnectBarrier barrier;
    constexpr std::int64_t kNow = 10'000;
    barrier.observe(live(1, kNow));
    const std::optional<std::uint64_t> first = barrier.take_sweep_request(kNow);
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(barrier.take_sweep_request(kNow).has_value()) << "outstanding: not offered twice";

    barrier.on_sweep_request_undelivered(*first);
    const std::optional<std::uint64_t> again = barrier.take_sweep_request(kNow);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, *first) << "nothing was sent, so there is no reason for a new epoch";
    EXPECT_EQ(barrier.stats().sweep_timeouts, 0U);
}

TEST(ReconnectBarrierUndelivered, ItDoesNotBreakTheReportThatFollowsTheRealRequest) {
    ReconnectBarrier barrier;
    constexpr std::int64_t kNow = 10'000;
    barrier.observe(live(1, kNow));
    const std::optional<std::uint64_t> first = barrier.take_sweep_request(kNow);
    barrier.on_sweep_request_undelivered(*first);
    const std::optional<std::uint64_t> second = barrier.take_sweep_request(kNow);
    ASSERT_TRUE(second.has_value());
    barrier.on_sweep_report(SweepReport{*second, 0U, true}, kNow);
    EXPECT_EQ(barrier.state(), BarrierState::Reconciled);
}

// --- the pump --------------------------------------------------------------------------------------------------------------

namespace {

struct Script {
    FeedObservation feed{};
    RecoveryObservation recovery{};
    bool clock_ready{true};
};

FeedObservation read_feed(void* p) noexcept { return static_cast<Script*>(p)->feed; }
RecoveryObservation read_recovery(void* p) noexcept { return static_cast<Script*>(p)->recovery; }
bool read_clock(void* p) noexcept { return static_cast<Script*>(p)->clock_ready; }

class SweepPumpTest : public ::testing::Test {
protected:
    static constexpr std::int64_t kNow = 5'000'000;

    KillSwitch kill_switch_;
    StartupRecovery recovery_{60'000};
    Script script_;
    SubmitReadiness readiness_{recovery_, kill_switch_, ReadinessProbe{&read_feed, &read_recovery, &read_clock, &script_}};
    SweepRequestRing requests_;
    ProbeLog account_;
    SweepPump pump_{readiness_, requests_, SweepAccountProbe{&probe_answer, &account_}};

    void SetUp() override { script_.feed = live(1, kNow); }

    std::optional<SweepRequest> pop_request() {
        SweepRequest r{};
        if (!requests_.try_pop(r)) return std::nullopt;
        return r;
    }

    // Put the barrier into Sweeping and post its request; returns the epoch posted.
    std::uint64_t request_posted() {
        pump_.service();
        const std::optional<SweepRequest> r = pop_request();
        EXPECT_TRUE(r.has_value());
        return r ? r->epoch : 0U;
    }
};

}  // namespace

TEST_F(SweepPumpTest, NothingIsPostedWhileTheBarrierIsDownOrReconciled) {
    script_.feed = FeedObservation{};  // generation 0: no session at all
    pump_.service();
    EXPECT_FALSE(pop_request().has_value());
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Down);

    script_.feed = live(1, kNow);
    const std::uint64_t epoch = request_posted();
    readiness_.barrier().on_sweep_report(SweepReport{epoch, 0U, true}, kNow);
    ASSERT_EQ(readiness_.barrier().state(), BarrierState::Reconciled);
    pump_.service();
    EXPECT_FALSE(pop_request().has_value()) << "a reconciled barrier asks for nothing";
}

TEST_F(SweepPumpTest, ARequestIsPostedOnceAndRemembersWhenItWentOut) {
    pump_.service();
    const std::optional<SweepRequest> r = pop_request();
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(r->epoch, 0U);
    EXPECT_EQ(pump_.requested_epoch(), r->epoch);
    EXPECT_EQ(pump_.requested_at_ms(), kNow) << "the hot thread's steady reading, from the same observation the barrier saw";
    EXPECT_EQ(pump_.stats().requests_sent, 1U);

    pump_.service();
    pump_.service();
    EXPECT_FALSE(pop_request().has_value()) << "outstanding: the barrier does not offer a second one";
    EXPECT_EQ(pump_.stats().requests_sent, 1U);
}

TEST_F(SweepPumpTest, AFullRingRescindsTheRequestAndTheSameEpochGoesOutOnceThereIsRoom) {
    for (std::uint64_t e = 1; e <= kSweepRequestRingCapacity; ++e) ASSERT_TRUE(requests_.try_push(SweepRequest{e + 100}));
    pump_.service();
    EXPECT_EQ(pump_.stats().request_push_failures, 1U);
    EXPECT_EQ(pump_.stats().requests_sent, 0U);
    EXPECT_EQ(pump_.requested_epoch(), 0U) << "nothing went out, so nothing is remembered";
    EXPECT_FALSE(readiness_.barrier().sweep_outstanding());

    const std::uint64_t barrier_epoch = readiness_.barrier().epoch();
    SweepRequest r{};
    while (requests_.try_pop(r)) {}  // the reconcile thread caught up
    pump_.service();
    const std::optional<SweepRequest> posted = pop_request();
    ASSERT_TRUE(posted.has_value());
    EXPECT_EQ(posted->epoch, barrier_epoch);
    EXPECT_EQ(pump_.stats().requests_sent, 1U);
}

TEST_F(SweepPumpTest, AnIncompleteProbeObservesAndPostsNothing) {
    SubmitReadiness blind{recovery_, kill_switch_, ReadinessProbe{}};
    SweepRequestRing ring;
    SweepPump pump{blind, ring, SweepAccountProbe{}};
    pump.service();
    SweepRequest r{};
    EXPECT_FALSE(ring.try_pop(r));
    EXPECT_EQ(pump.stats().requests_sent, 0U);
    // a marker with nothing to observe against is ignored too
    pump.sink().deliver(SweepDone{1, 0});
    EXPECT_EQ(pump.stats().markers_received, 1U);
    EXPECT_EQ(blind.barrier().stats().sweeps_completed, 0U);
}

TEST_F(SweepPumpTest, ACleanMarkerOfTheCurrentEpochWithAFreshAccountCompletesTheBarrier) {
    const std::uint64_t epoch = request_posted();
    account_.answer = true;
    pump_.sink().deliver(SweepDone{epoch, 0});
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Reconciled);
    EXPECT_EQ(readiness_.barrier().reconciled_generation(), 1U);
    EXPECT_EQ(account_.calls, 1);
    EXPECT_EQ(account_.since, kNow) << "the account is judged against the moment the request went out";
    EXPECT_EQ(pump_.stats().markers_received, 1U);
    EXPECT_EQ(pump_.stats().reports_account_stale, 0U);
}

TEST_F(SweepPumpTest, TheAccountIsJudgedAgainstTheRequestAndNotAgainstTheMarkersArrival) {
    const std::uint64_t epoch = request_posted();
    // The reconcile thread spent a while asking the exchange; the account was refreshed in the meantime, after the request and before the marker.
    script_.feed = live(1, kNow + 700);
    account_.answer = true;
    pump_.sink().deliver(SweepDone{epoch, 0});
    EXPECT_EQ(account_.since, kNow) << "the bound is the moment the request went out, not the moment the answer got back";
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Reconciled);
}

TEST_F(SweepPumpTest, AnAccountNotRefreshedSinceTheRequestKeepsTheBarrierSweeping) {
    const std::uint64_t epoch = request_posted();
    account_.answer = false;
    pump_.sink().deliver(SweepDone{epoch, 0});
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Sweeping);
    EXPECT_EQ(pump_.stats().reports_account_stale, 1U);
    EXPECT_EQ(readiness_.barrier().stats().sweeps_unresolved, 1U) << "the barrier saw an incomplete report and will ask again";
}

TEST_F(SweepPumpTest, AnUnwiredAccountProbeNeverLetsTheBarrierComplete) {
    SweepRequestRing ring;
    SweepPump pump{readiness_, ring, SweepAccountProbe{}};
    pump.service();
    SweepRequest r{};
    ASSERT_TRUE(ring.try_pop(r));
    pump.sink().deliver(SweepDone{r.epoch, 0});
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Sweeping);
}

TEST_F(SweepPumpTest, UnresolvedOrdersKeepTheBarrierSweepingAndTheNextRequestIsANewEpoch) {
    const std::uint64_t epoch = request_posted();
    account_.answer = true;
    pump_.sink().deliver(SweepDone{epoch, 2});
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Sweeping);
    EXPECT_EQ(readiness_.barrier().stats().sweeps_unresolved, 1U);

    // the barrier asks again after its pause, under a new epoch
    script_.feed = live(1, kNow + readiness_.barrier().policy().resweep_after_ms + 1);
    pump_.service();
    const std::optional<SweepRequest> again = pop_request();
    ASSERT_TRUE(again.has_value());
    EXPECT_GT(again->epoch, epoch);
}

TEST_F(SweepPumpTest, AMarkerOfAnOlderEpochChangesNothingAndNeverAsksTheAccount) {
    const std::uint64_t epoch = request_posted();
    pump_.sink().deliver(SweepDone{epoch - 1, 0});
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Sweeping);
    EXPECT_EQ(readiness_.barrier().stats().reports_ignored, 1U);
    EXPECT_EQ(account_.calls, 0) << "freshness is only judged for the request that is outstanding";
}

TEST_F(SweepPumpTest, AMarkerForASweepThatAGenerationChangeMadeObsoleteChangesNothing) {
    const std::uint64_t epoch = request_posted();
    script_.feed = live(2, kNow + 10);  // the supervisor started generation 2 meanwhile
    account_.answer = true;
    pump_.sink().deliver(SweepDone{epoch, 0});  // the pump observes first: the barrier restarts for generation 2
    EXPECT_NE(readiness_.barrier().state(), BarrierState::Reconciled);
    EXPECT_EQ(readiness_.barrier().generation(), 2U);
    EXPECT_EQ(readiness_.barrier().stats().reports_ignored, 1U);
}

TEST_F(SweepPumpTest, AMarkerAfterTheBarrierWentDownChangesNothing) {
    const std::uint64_t epoch = request_posted();
    script_.feed = FeedObservation{};  // the session is gone
    pump_.sink().deliver(SweepDone{epoch, 0});
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Down);
    EXPECT_EQ(readiness_.barrier().stats().sweeps_completed, 0U);
}

TEST_F(SweepPumpTest, ADuplicateMarkerCannotCompleteTheBarrierTwiceOrResurrectIt) {
    const std::uint64_t epoch = request_posted();
    account_.answer = true;
    pump_.sink().deliver(SweepDone{epoch, 0});
    ASSERT_EQ(readiness_.barrier().state(), BarrierState::Reconciled);
    // the generation is lost; the barrier goes Down; the old marker shows up again
    script_.feed = FeedObservation{};
    readiness_.observe();
    ASSERT_EQ(readiness_.barrier().state(), BarrierState::Down);
    script_.feed = live(1, kNow + 20);
    pump_.sink().deliver(SweepDone{epoch, 0});
    EXPECT_NE(readiness_.barrier().state(), BarrierState::Reconciled) << "an old epoch's answer is not an answer to the new sweep";
}
