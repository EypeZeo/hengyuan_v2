// Private-stream migration M3a: SubmitReadiness / SubmitReadyToken (submit_readiness.hpp), the adapter that turns a PublicFeedSupervisor into the
// barrier's input (feed_observation.hpp) and the synthetic readiness drills and tests use (synthetic_readiness.hpp).
//
// What is pinned here:
//   * the token is issued only when ALL of its conditions hold, and a refusal names the FIRST one that does not (one fixed priority order);
//   * the facts are read at the moment of the decision -- authorize() brings the barrier up to date itself, whether or not the loop ran;
//   * FI-043 end to end with a real supervisor: after a rotation / reconnect nothing is authorized until the NEW generation is swept;
//   * FI-020 end to end: a silent connection is refused, the loop is told to stop it, and the next generation starts from "not swept";
//   * the token is a move-only value with one issuer, and the synthetic readiness is the same machinery driven by scripted observations.
#include <gtest/gtest.h>

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/feed_observation.hpp>
#include <hengyuan/public_feed_supervisor.hpp>
#include <hengyuan/submit_readiness.hpp>
#include <hengyuan/synthetic_readiness.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace hy;

namespace {

// --- a session as the supervisor and the adapter see it -----------------------------------------------------------------

struct FakeSession {
    void start() { started = true; }
    void stop() { stop_flag = true; }
    [[nodiscard]] bool stopped() const { return stop_flag; }
    [[nodiscard]] bool is_connected() const { return connected; }
    [[nodiscard]] std::int64_t last_activity_steady_ms() const { return last_activity; }
    [[nodiscard]] std::uint64_t ring_drops() const { return drops; }
    [[nodiscard]] std::uint64_t lost_events() const { return lost; }

    bool started{false};
    bool stop_flag{false};
    bool connected{false};
    std::int64_t last_activity{0};
    std::uint64_t drops{0};
    std::uint64_t lost{0};
};

static_assert(SupervisedFeedSession<FakeSession>);
static_assert(ObservableFeedSession<FakeSession>);

// Supervisable, but without the three accessors the barrier needs: not observable.
struct PlainSession {
    void start();
    void stop();
    [[nodiscard]] bool stopped() const;
    [[nodiscard]] bool is_connected() const;
};
struct NoActivitySession : PlainSession {
    [[nodiscard]] std::uint64_t ring_drops() const;
    [[nodiscard]] std::uint64_t lost_events() const;
};
struct NoDropsSession : PlainSession {
    [[nodiscard]] std::int64_t last_activity_steady_ms() const;
    [[nodiscard]] std::uint64_t lost_events() const;
};
struct NoLostSession : PlainSession {
    [[nodiscard]] std::int64_t last_activity_steady_ms() const;
    [[nodiscard]] std::uint64_t ring_drops() const;
};

static_assert(SupervisedFeedSession<PlainSession>);
static_assert(!ObservableFeedSession<PlainSession>);
static_assert(!ObservableFeedSession<NoActivitySession>);
static_assert(!ObservableFeedSession<NoDropsSession>);
static_assert(!ObservableFeedSession<NoLostSession>);

constexpr std::int64_t kT = 10'000;  // an arbitrary steady-clock reading, ms

}  // namespace

// --- the adapter: PublicFeedSupervisor + session -> FeedObservation -------------------------------------------------------

namespace {

class FeedObservationTest : public ::testing::Test {
protected:
    std::vector<std::shared_ptr<FakeSession>> sessions_;
    PublicFeedSupervisor<FakeSession> supervisor_{[this](std::uint64_t) {
        auto s = std::make_shared<FakeSession>();
        sessions_.push_back(s);
        return s;
    }};

    FakeSession& session() { return *sessions_.back(); }

    // Idle -> Connecting (the session exists and was started) -> Connected (the supervisor has polled it connected).
    void bring_up(std::int64_t now) {
        supervisor_.poll(now);
        ASSERT_FALSE(sessions_.empty());
        session().connected = true;
        session().last_activity = now;
        supervisor_.poll(now + 1);
        ASSERT_TRUE(supervisor_.healthy());
    }

    void expect_not_connected(const FeedObservation& o, std::uint64_t generation, std::int64_t sampled_at) {
        EXPECT_EQ(o.generation, generation);
        EXPECT_FALSE(o.connected);
        EXPECT_EQ(o.last_activity_ms, 0) << "nothing is read off a session that does not count as connected";
        EXPECT_EQ(o.ring_drops, 0U);
        EXPECT_EQ(o.lost_events, 0U);
        EXPECT_EQ(o.sampled_at_ms, sampled_at);
    }
};

}  // namespace

TEST_F(FeedObservationTest, NothingStartedYetIsNotConnected) {
    expect_not_connected(make_feed_observation(supervisor_, kT), 0U, kT);
}

TEST_F(FeedObservationTest, AFreshSessionIsNotConnectedUntilTheSupervisorHasSeenItConnected) {
    supervisor_.poll(kT);  // generation 1 exists and was started
    ASSERT_EQ(supervisor_.generation(), 1U);
    expect_not_connected(make_feed_observation(supervisor_, kT), 1U, kT);

    session().connected = true;  // the session says so, the supervisor has not polled since: fail closed by one tick
    session().last_activity = kT;
    expect_not_connected(make_feed_observation(supervisor_, kT), 1U, kT);

    supervisor_.poll(kT + 1);
    const FeedObservation o = make_feed_observation(supervisor_, kT + 1);
    EXPECT_TRUE(o.connected);
    EXPECT_EQ(o.generation, 1U);
}

TEST_F(FeedObservationTest, AConnectedSessionCarriesItsActivityAndItsCounters) {
    bring_up(kT);
    session().last_activity = 12'345;
    session().drops = 5;
    session().lost = 2;
    const FeedObservation o = make_feed_observation(supervisor_, kT + 500);
    EXPECT_TRUE(o.connected);
    EXPECT_EQ(o.generation, 1U);
    EXPECT_EQ(o.last_activity_ms, 12'345);
    EXPECT_EQ(o.ring_drops, 5U);
    EXPECT_EQ(o.lost_events, 2U);
    EXPECT_EQ(o.sampled_at_ms, kT + 500) << "the caller's clock, not the session's";
}

TEST_F(FeedObservationTest, AStoppedSessionIsNotConnectedBeforeTheSupervisorHasNoticed) {
    bring_up(kT);
    session().stop();  // dies right now; the supervisor learns of it only at its next poll()
    ASSERT_EQ(supervisor_.state(), FeedState::Connected);
    expect_not_connected(make_feed_observation(supervisor_, kT + 2), 1U, kT + 2);
}

TEST_F(FeedObservationTest, ASessionWhoseSubscriptionIsGoneIsNotConnected) {
    bring_up(kT);
    session().connected = false;  // e.g. the subscription ended while the socket is still open
    ASSERT_EQ(supervisor_.state(), FeedState::Connected);
    expect_not_connected(make_feed_observation(supervisor_, kT + 2), 1U, kT + 2);
}

TEST_F(FeedObservationTest, BetweenGenerationsThereIsNoSessionToObserve) {
    bring_up(kT);
    session().stop();
    supervisor_.poll(kT + 2);  // the generation ends: no session until the backoff has passed
    ASSERT_TRUE(supervisor_.current() == nullptr);
    ASSERT_EQ(supervisor_.state(), FeedState::Backoff);
    expect_not_connected(make_feed_observation(supervisor_, kT + 3), 1U, kT + 3);
}

TEST_F(FeedObservationTest, TheNextGenerationIsObservedFromItsOwnSession) {
    bring_up(kT);
    session().drops = 7;
    session().lost = 7;
    session().stop();
    supervisor_.poll(kT + 2);
    supervisor_.poll(kT + 5'000);  // past any jittered backoff: generation 2 starts
    ASSERT_EQ(supervisor_.generation(), 2U);
    ASSERT_EQ(sessions_.size(), 2U);
    expect_not_connected(make_feed_observation(supervisor_, kT + 5'000), 2U, kT + 5'000);

    session().connected = true;
    session().last_activity = kT + 5'001;
    supervisor_.poll(kT + 5'001);
    const FeedObservation o = make_feed_observation(supervisor_, kT + 5'002);
    EXPECT_TRUE(o.connected);
    EXPECT_EQ(o.generation, 2U);
    EXPECT_EQ(o.last_activity_ms, kT + 5'001);
    EXPECT_EQ(o.ring_drops, 0U) << "generation 2's counters are its own, not generation 1's";
    EXPECT_EQ(o.lost_events, 0U);
}

TEST_F(FeedObservationTest, AShutDownSupervisorIsNotConnected) {
    bring_up(kT);
    supervisor_.shutdown();
    expect_not_connected(make_feed_observation(supervisor_, kT + 2), 1U, kT + 2);
}

// --- the readiness: each condition, its reason, and the order the reasons come in ----------------------------------------------

namespace {

struct Script {
    FeedObservation feed{};
    RecoveryObservation recovery{};
    bool clock_ready{true};
    int feed_reads{0};
    int recovery_reads{0};
    int clock_reads{0};
};

FeedObservation read_feed(void* p) noexcept {
    auto* s = static_cast<Script*>(p);
    ++s->feed_reads;
    return s->feed;
}
RecoveryObservation read_recovery(void* p) noexcept {
    auto* s = static_cast<Script*>(p);
    ++s->recovery_reads;
    return s->recovery;
}
bool read_clock(void* p) noexcept {
    auto* s = static_cast<Script*>(p);
    ++s->clock_reads;
    return s->clock_ready;
}

RecoveryObservation clean_recovery() {
    RecoveryObservation o{};
    o.clock_synced = true;
    o.exchange_info_loaded = true;
    o.sink_open = true;
    o.sink_fenced = false;
    o.scan_status = RecoveryScanStatus::Clean;
    o.recovery_applied = true;
    o.account_fresh = true;
    o.user_data_connected = true;
    return o;
}

FeedObservation live_feed(std::uint64_t generation, std::int64_t now) {
    FeedObservation o{};
    o.generation = generation;
    o.connected = true;
    o.last_activity_ms = now;
    o.sampled_at_ms = now;
    return o;
}

void expect_refused(const ReadinessResult& r, NotReadyReason why, const char* note = "") {
    EXPECT_FALSE(r.ready()) << note;
    EXPECT_FALSE(r.token.has_value()) << note;
    EXPECT_EQ(r.reason, why) << "got " << not_ready_reason_name(r.reason) << ", expected " << not_ready_reason_name(why) << "  " << note;
}

class SubmitReadinessTest : public ::testing::Test {
protected:
    static constexpr std::int64_t kNow = 5'000'000;

    KillSwitch kill_switch_;
    StartupRecovery recovery_{60'000};
    Script script_;
    SubmitReadiness readiness_{recovery_, kill_switch_, ReadinessProbe{&read_feed, &read_recovery, &read_clock, &script_}};

    void SetUp() override {
        script_.recovery = clean_recovery();
        script_.feed = live_feed(1, kNow);
        ASSERT_EQ(recovery_.update(script_.recovery, kNow), RunState::Ready);
    }

    ReadinessProbe probe() { return ReadinessProbe{&read_feed, &read_recovery, &read_clock, &script_}; }

    // The barrier to Reconciled(generation) through its public protocol, as the loop and the sweep will do it.
    void reconcile(std::uint64_t generation = 1) {
        script_.feed = live_feed(generation, kNow);
        readiness_.observe();
        const std::optional<std::uint64_t> epoch = readiness_.barrier().take_sweep_request(kNow);
        ASSERT_TRUE(epoch.has_value());
        readiness_.barrier().on_sweep_report(SweepReport{*epoch, 0U, true}, kNow);
        ASSERT_EQ(readiness_.barrier().state(), BarrierState::Reconciled);
    }

    // After reconcile(): sweep the feed's current generation again (used after a drop / a new generation).
    void sweep_clean() {
        const std::optional<std::uint64_t> epoch = readiness_.barrier().take_sweep_request(kNow);
        ASSERT_TRUE(epoch.has_value());
        readiness_.barrier().on_sweep_report(SweepReport{*epoch, 0U, true}, kNow);
        ASSERT_EQ(readiness_.barrier().state(), BarrierState::Reconciled);
    }
};

}  // namespace

TEST_F(SubmitReadinessTest, ATokenIsIssuedWhenEverythingHolds) {
    reconcile();
    const ReadinessResult r = readiness_.authorize(kNow + 7);
    ASSERT_TRUE(r.ready());
    EXPECT_EQ(r.reason, NotReadyReason::None);
    ASSERT_TRUE(r.token.has_value());
    EXPECT_EQ(r.token->generation(), 1U);
    EXPECT_EQ(r.token->issued_at_ms(), kNow + 7);
    EXPECT_EQ(readiness_.stats().authorized, 1U);
    for (const std::uint64_t n : readiness_.stats().refused) EXPECT_EQ(n, 0U);
}

TEST_F(SubmitReadinessTest, AnUnsweptSubscriptionIsNotReadyEvenIfEverythingElseIs) {
    // no reconcile(): the feed is up, nothing swept
    expect_refused(readiness_.authorize(kNow), NotReadyReason::BarrierSweeping);
    ASSERT_TRUE(readiness_.barrier().take_sweep_request(kNow).has_value()) << "authorize() brought the barrier up; a sweep is due";
}

// --- no probe ------------------------------------------------------------------------------------------------------------------

TEST_F(SubmitReadinessTest, AProbeWithAMissingFunctionRefusesEverythingAndReadsNothing) {
    for (int missing = 0; missing < 4; ++missing) {
        SCOPED_TRACE(missing);
        ReadinessProbe p = probe();
        if (missing == 0) p.feed = nullptr;
        if (missing == 1) p.recovery = nullptr;
        if (missing == 2) p.clock_ready = nullptr;
        if (missing == 3) p = ReadinessProbe{};
        SubmitReadiness r(recovery_, kill_switch_, p);
        r.observe();  // a no-op, not a crash
        expect_refused(r.authorize(kNow), NotReadyReason::NoProbe);
        EXPECT_EQ(r.stats().refused[static_cast<std::size_t>(NotReadyReason::NoProbe)], 1U);
        EXPECT_EQ(r.stats().authorized, 0U);
        EXPECT_EQ(r.barrier().state(), BarrierState::Down);
    }
    EXPECT_EQ(script_.feed_reads, 0) << "an incomplete probe is not called at all";
    EXPECT_EQ(script_.recovery_reads, 0);
    EXPECT_EQ(script_.clock_reads, 0);
}

// --- each condition alone ------------------------------------------------------------------------------------------------------

TEST_F(SubmitReadinessTest, AKillSwitchThatIsNotNormalRefuses) {
    reconcile();
    ASSERT_TRUE(readiness_.authorize(kNow).ready());
    kill_switch_.arm();
    expect_refused(readiness_.authorize(kNow), NotReadyReason::KillSwitchNotNormal);
    kill_switch_.trigger();
    expect_refused(readiness_.authorize(kNow), NotReadyReason::KillSwitchNotNormal);
    kill_switch_.latch();
    expect_refused(readiness_.authorize(kNow), NotReadyReason::KillSwitchNotNormal);
    kill_switch_.operator_reset();
    EXPECT_TRUE(readiness_.authorize(kNow).ready()) << "the kill switch is not part of the barrier: nothing to sweep again";
}

TEST_F(SubmitReadinessTest, ARecoveryThatNeverReachedReadyRefuses) {
    StartupRecovery fresh{60'000};  // Bootstrapping
    SubmitReadiness other(fresh, kill_switch_, probe());
    expect_refused(other.authorize(kNow), NotReadyReason::StartupNotReady);
}

TEST_F(SubmitReadinessTest, ADegradedRecoveryRefusesForGood) {
    reconcile();
    RecoveryObservation bad = script_.recovery;
    bad.sink_open = false;
    ASSERT_EQ(recovery_.update(bad, kNow), RunState::Degraded);
    expect_refused(readiness_.authorize(kNow), NotReadyReason::StartupNotReady);
    ASSERT_EQ(recovery_.update(script_.recovery, kNow), RunState::Degraded) << "sticky: only a restart clears it";
    expect_refused(readiness_.authorize(kNow), NotReadyReason::StartupNotReady);
}

TEST_F(SubmitReadinessTest, EachLivePrerequisiteOfStartupRecoveryRefusesWhileItDoesNotHold) {
    reconcile();
    struct Case {
        const char* name;
        void (*break_it)(RecoveryObservation&);
    };
    const Case cases[] = {
        {"the user-data stream is not connected", [](RecoveryObservation& o) { o.user_data_connected = false; }},
        {"the account snapshot is stale", [](RecoveryObservation& o) { o.account_fresh = false; }},
        {"the clock is not synced", [](RecoveryObservation& o) { o.clock_synced = false; }},
        {"the durable log is not open", [](RecoveryObservation& o) { o.sink_open = false; }},
        {"the durable log is fenced", [](RecoveryObservation& o) { o.sink_fenced = true; }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        script_.recovery = clean_recovery();
        c.break_it(script_.recovery);
        expect_refused(readiness_.authorize(kNow), NotReadyReason::StartupNotReady);
        script_.recovery = clean_recovery();
        EXPECT_TRUE(readiness_.authorize(kNow).ready()) << "StartupRecovery itself stayed Ready: the prerequisite is read afresh each time";
    }
}

TEST_F(SubmitReadinessTest, AFeedThatIsNotConnectedRefusesAsDown) {
    reconcile();
    script_.feed.connected = false;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::FeedDown);
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Down);
}

TEST_F(SubmitReadinessTest, AFeedWithNoGenerationRefusesAsDown) {
    script_.feed = FeedObservation{};
    expect_refused(readiness_.authorize(kNow), NotReadyReason::FeedDown);
}

TEST_F(SubmitReadinessTest, AFeedThatDroppedAndCameBackNeedsANewSweep) {
    reconcile();
    script_.feed.connected = false;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::FeedDown);
    script_.feed = live_feed(1, kNow);  // the same generation number, connected again
    expect_refused(readiness_.authorize(kNow), NotReadyReason::BarrierSweeping);
    sweep_clean();
    EXPECT_TRUE(readiness_.authorize(kNow).ready());
}

TEST_F(SubmitReadinessTest, AClockThatCannotSignRefusesAndNeedsNoNewSweepWhenItIsBack) {
    reconcile();
    script_.clock_ready = false;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::ClockNotReady);
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Reconciled) << "the clock is not the barrier's business";
    script_.clock_ready = true;
    EXPECT_TRUE(readiness_.authorize(kNow).ready());
}

// --- priority: a refusal names the first condition that fails, in one fixed order --------------------------------------------------

TEST_F(SubmitReadinessTest, TheKillSwitchIsReportedBeforeEverythingElse) {
    kill_switch_.trigger();
    script_.recovery.user_data_connected = false;
    script_.feed.connected = false;
    script_.clock_ready = false;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::KillSwitchNotNormal);
}

TEST_F(SubmitReadinessTest, StartupIsReportedBeforeTheFeedAndTheClock) {
    script_.recovery.account_fresh = false;
    script_.feed.connected = false;
    script_.clock_ready = false;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::StartupNotReady);
}

TEST_F(SubmitReadinessTest, TheFeedIsReportedBeforeTheClock) {
    script_.feed.connected = false;
    script_.clock_ready = false;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::FeedDown);
    script_.feed = live_feed(1, kNow);  // up again, not swept
    expect_refused(readiness_.authorize(kNow), NotReadyReason::BarrierSweeping);
}

TEST_F(SubmitReadinessTest, TheClockIsReportedOnlyWhenEverythingElseHolds) {
    reconcile();
    script_.clock_ready = false;
    script_.clock_reads = 0;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::ClockNotReady);
    EXPECT_EQ(script_.clock_reads, 1);
    // ... and it is not even asked while an earlier condition already refuses
    script_.clock_reads = 0;
    script_.recovery.account_fresh = false;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::StartupNotReady);
    EXPECT_EQ(script_.clock_reads, 0);
}

// --- the facts are read at the moment of the decision ---------------------------------------------------------------------------------

TEST_F(SubmitReadinessTest, AuthorizeBringsTheBarrierUpToDateItself) {
    reconcile();
    // the feed drops and the loop has not run since: no observe() call
    script_.feed.connected = false;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::FeedDown);
}

TEST_F(SubmitReadinessTest, ASilentConnectionSeenOnlyByAuthorizeIsRefusedAndTheLoopIsToldToStopIt) {
    reconcile();
    script_.feed = live_feed(1, kNow);
    script_.feed.last_activity_ms = kNow - 100'000;  // no frame, no ping, for 100 s
    expect_refused(readiness_.authorize(kNow), NotReadyReason::FeedDown);
    EXPECT_TRUE(readiness_.barrier().take_stop_request());
}

TEST_F(SubmitReadinessTest, ADroppedEventSeenOnlyByAuthorizeSendsTheSubscriptionBackToSweeping) {
    reconcile();
    script_.feed.ring_drops = 1;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::BarrierSweeping);
    EXPECT_EQ(readiness_.barrier().stats().soft_barriers, 1U);
    sweep_clean();
    EXPECT_TRUE(readiness_.authorize(kNow).ready());
}

TEST_F(SubmitReadinessTest, AnUnreadableExecutionReportDoesTheSame) {
    reconcile();
    script_.feed.lost_events = 1;
    expect_refused(readiness_.authorize(kNow), NotReadyReason::BarrierSweeping);
}

TEST_F(SubmitReadinessTest, ANewGenerationIsNotAuthorizedUntilItIsSwept) {
    reconcile(1);
    ASSERT_TRUE(readiness_.authorize(kNow).ready());
    script_.feed = live_feed(2, kNow);
    expect_refused(readiness_.authorize(kNow), NotReadyReason::BarrierSweeping);
    EXPECT_EQ(readiness_.barrier().generation(), 2U);
    sweep_clean();
    const ReadinessResult r = readiness_.authorize(kNow);
    ASSERT_TRUE(r.ready());
    EXPECT_EQ(r.token->generation(), 2U) << "the token rests on the generation that was swept";
}

TEST_F(SubmitReadinessTest, AGenerationThatEndedIsNotAuthorizedAgainWhileTheFeedStillLooksConnected) {
    reconcile(1);
    readiness_.barrier().on_generation_ended(1);  // the consumer drained its serverShutdown
    expect_refused(readiness_.authorize(kNow), NotReadyReason::FeedDown);
    expect_refused(readiness_.authorize(kNow), NotReadyReason::FeedDown);
    script_.feed = live_feed(2, kNow);
    expect_refused(readiness_.authorize(kNow), NotReadyReason::BarrierSweeping);
    sweep_clean();
    EXPECT_TRUE(readiness_.authorize(kNow).ready());
}

TEST_F(SubmitReadinessTest, EveryDecisionReadsEachFactOnceAndOnlyWhatItNeeds) {
    reconcile();
    script_.feed_reads = script_.recovery_reads = script_.clock_reads = 0;
    ASSERT_TRUE(readiness_.authorize(kNow).ready());
    EXPECT_EQ(script_.feed_reads, 1);
    EXPECT_EQ(script_.recovery_reads, 1);
    EXPECT_EQ(script_.clock_reads, 1);
    script_.feed_reads = script_.recovery_reads = script_.clock_reads = 0;
    kill_switch_.trigger();
    expect_refused(readiness_.authorize(kNow), NotReadyReason::KillSwitchNotNormal);
    EXPECT_EQ(script_.feed_reads, 1) << "the barrier is kept current even for a refused decision";
    EXPECT_EQ(script_.recovery_reads, 0);
    EXPECT_EQ(script_.clock_reads, 0);
}

TEST_F(SubmitReadinessTest, RefusalsAreCountedByReason) {
    reconcile();
    ASSERT_TRUE(readiness_.authorize(kNow).ready());
    kill_switch_.arm();
    (void)readiness_.authorize(kNow);
    (void)readiness_.authorize(kNow);
    kill_switch_.operator_reset();
    script_.recovery.account_fresh = false;
    (void)readiness_.authorize(kNow);
    script_.recovery = clean_recovery();
    script_.clock_ready = false;
    (void)readiness_.authorize(kNow);
    (void)readiness_.authorize(kNow);
    (void)readiness_.authorize(kNow);
    script_.clock_ready = true;
    ASSERT_TRUE(readiness_.authorize(kNow).ready());

    const ReadinessStats& st = readiness_.stats();
    EXPECT_EQ(st.authorized, 2U);
    EXPECT_EQ(st.refused[static_cast<std::size_t>(NotReadyReason::KillSwitchNotNormal)], 2U);
    EXPECT_EQ(st.refused[static_cast<std::size_t>(NotReadyReason::StartupNotReady)], 1U);
    EXPECT_EQ(st.refused[static_cast<std::size_t>(NotReadyReason::ClockNotReady)], 3U);
    EXPECT_EQ(st.refused[static_cast<std::size_t>(NotReadyReason::None)], 0U);
    EXPECT_EQ(st.refused[static_cast<std::size_t>(NotReadyReason::FeedDown)], 0U);
}

// --- end to end: a real supervisor, a real barrier, scripted sessions ----------------------------------------------------------------

namespace {

class SupervisedReadinessTest : public ::testing::Test {
protected:
    struct Wiring {
        PublicFeedSupervisor<FakeSession>* supervisor{nullptr};
        const std::int64_t* clock_ms{nullptr};
        RecoveryObservation recovery{};
        bool clock_ready{true};
    };

    static FeedObservation wired_feed(void* p) noexcept {
        auto* w = static_cast<Wiring*>(p);
        return make_feed_observation(*w->supervisor, *w->clock_ms);
    }
    static RecoveryObservation wired_recovery(void* p) noexcept { return static_cast<Wiring*>(p)->recovery; }
    static bool wired_clock(void* p) noexcept { return static_cast<Wiring*>(p)->clock_ready; }

    std::int64_t now_{kT};
    std::vector<std::shared_ptr<FakeSession>> sessions_;
    PublicFeedSupervisor<FakeSession> supervisor_{[this](std::uint64_t) {
        auto s = std::make_shared<FakeSession>();
        s->last_activity = now_;
        sessions_.push_back(s);
        return s;
    }};
    KillSwitch kill_switch_;
    StartupRecovery recovery_{60'000};
    Wiring wiring_{};
    std::optional<SubmitReadiness> readiness_;

    void SetUp() override {
        wiring_.supervisor = &supervisor_;
        wiring_.clock_ms = &now_;
        wiring_.recovery = clean_recovery();
        ASSERT_EQ(recovery_.update(wiring_.recovery, now_), RunState::Ready);
        readiness_.emplace(recovery_, kill_switch_, ReadinessProbe{&wired_feed, &wired_recovery, &wired_clock, &wiring_});
    }

    FakeSession& session() { return *sessions_.back(); }

    // One turn of the hot loop as M5 will run it: the supervisor, then the barrier, then the stop the barrier may ask for.
    void loop_turn(std::int64_t now) {
        now_ = now;
        supervisor_.poll(now);
        readiness_->observe();
        if (readiness_->barrier().take_stop_request()) {
            if (const std::shared_ptr<FakeSession> s = supervisor_.current()) s->stop();
        }
    }

    // A frame arrived (a data frame or the server's ping): the session's last activity is `now`.
    void frame_at(std::int64_t now) { session().last_activity = now; }

    void sweep_clean() {
        const std::optional<std::uint64_t> epoch = readiness_->barrier().take_sweep_request(now_);
        ASSERT_TRUE(epoch.has_value());
        readiness_->barrier().on_sweep_report(SweepReport{*epoch, 0U, true}, now_);
    }

    // generation 1 connected, observed, swept, authorized.
    void bring_up_and_sweep() {
        loop_turn(now_);  // the supervisor starts generation 1
        ASSERT_FALSE(sessions_.empty());
        session().connected = true;
        frame_at(now_ + 1);
        loop_turn(now_ + 1);  // observed connected
        expect_refused(readiness_->authorize(now_), NotReadyReason::BarrierSweeping);
        sweep_clean();
        const ReadinessResult r = readiness_->authorize(now_);
        ASSERT_TRUE(r.ready());
        ASSERT_EQ(r.token->generation(), 1U);
    }
};

}  // namespace

TEST_F(SupervisedReadinessTest, TheFirstGenerationIsReadyOnlyAfterItIsConnectedAndSwept) {
    expect_refused(readiness_->authorize(now_), NotReadyReason::FeedDown, "nothing started");
    bring_up_and_sweep();
}

TEST_F(SupervisedReadinessTest, ARotationBlocksFromTheMomentTheOldConnectionIsGoneUntilTheNewOneIsSwept) {  // FI-043
    bring_up_and_sweep();
    session().stop();  // the old connection dies; the supervisor has not polled yet
    expect_refused(readiness_->authorize(now_ + 2), NotReadyReason::FeedDown);
    loop_turn(now_ + 2);
    expect_refused(readiness_->authorize(now_), NotReadyReason::FeedDown, "in backoff: no session at all");
    loop_turn(kT + 5'000);  // generation 2 starts
    ASSERT_EQ(supervisor_.generation(), 2U);
    expect_refused(readiness_->authorize(now_), NotReadyReason::FeedDown, "started, not connected");
    session().connected = true;
    frame_at(now_ + 1);
    loop_turn(now_ + 1);
    expect_refused(readiness_->authorize(now_), NotReadyReason::BarrierSweeping, "connected, not swept");
    EXPECT_EQ(readiness_->barrier().generation(), 2U);
    sweep_clean();
    const ReadinessResult r = readiness_->authorize(now_);
    ASSERT_TRUE(r.ready());
    EXPECT_EQ(r.token->generation(), 2U);
}

TEST_F(SupervisedReadinessTest, ATwentyFourHourRolloverIsTreatedLikeAnyOtherNewGeneration) {  // FI-043, planned
    // The supervisor replaces a healthy connection at the next boundary and starts the next generation in the same tick.
    FeedSupervisorPolicy policy;
    policy.max_connection_age_ms = 1'000;
    PublicFeedSupervisor<FakeSession> rolling(
        [this](std::uint64_t) {
            auto s = std::make_shared<FakeSession>();
            s->last_activity = now_;
            sessions_.push_back(s);
            return s;
        },
        policy);
    wiring_.supervisor = &rolling;
    rolling.poll(now_);
    session().connected = true;
    frame_at(now_ + 1);
    rolling.poll(now_ + 1);
    readiness_->observe();
    sweep_clean();
    ASSERT_TRUE(readiness_->authorize(now_ + 1).ready());

    now_ += 2'000;
    rolling.poll(now_);  // past max_connection_age: the old session is stopped
    expect_refused(readiness_->authorize(now_), NotReadyReason::FeedDown);
    rolling.poll(now_ + 1);  // ... ended; the next generation starts at once
    ASSERT_EQ(rolling.generation(), 2U);
    session().connected = true;
    frame_at(now_ + 2);
    rolling.poll(now_ + 2);
    expect_refused(readiness_->authorize(now_ + 2), NotReadyReason::BarrierSweeping);
    sweep_clean();
    const ReadinessResult r = readiness_->authorize(now_ + 2);
    ASSERT_TRUE(r.ready());
    EXPECT_EQ(r.token->generation(), 2U);
}

TEST_F(SupervisedReadinessTest, ASilentConnectionIsRefusedStoppedAndReplacedByAnUnsweptGeneration) {  // FI-020
    bring_up_and_sweep();
    // the TCP connection stays open, the server's pings stop: no frame for a minute and a bit
    const std::int64_t silent_until = now_ + 61'000;
    loop_turn(silent_until);
    expect_refused(readiness_->authorize(silent_until), NotReadyReason::FeedDown);
    EXPECT_TRUE(session().stopped()) << "the loop stopped the session the barrier declared dead";
    loop_turn(silent_until + 1);  // the supervisor sees the stop: backoff
    loop_turn(silent_until + 6'000);  // generation 2
    ASSERT_EQ(supervisor_.generation(), 2U);
    session().connected = true;
    frame_at(silent_until + 6'001);
    loop_turn(silent_until + 6'001);
    expect_refused(readiness_->authorize(silent_until + 6'001), NotReadyReason::BarrierSweeping);
    sweep_clean();
    EXPECT_TRUE(readiness_->authorize(silent_until + 6'001).ready());
}

TEST_F(SupervisedReadinessTest, AQuietAccountWhoseServerKeepsPingingStaysReady) {  // FI-020, the other half
    bring_up_and_sweep();
    const std::int64_t base = now_;
    for (int ping = 1; ping <= 10; ++ping) {
        const std::int64_t t = base + ping * 20'000;  // a ping every 20 s, no data frame at all
        frame_at(t);
        loop_turn(t);
        EXPECT_TRUE(readiness_->authorize(t).ready()) << "ping " << ping;
    }
    EXPECT_EQ(readiness_->barrier().stats().stale_heartbeats, 0U);
}

TEST_F(SupervisedReadinessTest, ALostEventIsASoftBarrierWithoutAReconnect) {  // F5
    bring_up_and_sweep();
    session().drops = 1;
    loop_turn(now_ + 10);
    expect_refused(readiness_->authorize(now_ + 10), NotReadyReason::BarrierSweeping);
    EXPECT_EQ(supervisor_.generation(), 1U) << "no reconnect";
    sweep_clean();
    EXPECT_TRUE(readiness_->authorize(now_ + 10).ready());
}

// --- clock_ready_now() -------------------------------------------------------------------------------------------------------------------

namespace {

// A snapshot fetched `steady_age_ms` ago by the steady clock, with the wall clock `wall_skew_ms` off the steady one since.
ClockOffsetSnapshot snapshot_aged(std::int64_t steady_age_ms, std::int64_t wall_skew_ms = 0) {
    const ClockPairSample now = fetch_clock_pair();
    ClockOffsetSnapshot s{};
    s.offset_ms = 0;
    s.error_bound_ms = 100;
    s.steady_at_fetch_ms = now.steady_ms() - steady_age_ms;
    s.system_at_fetch_ms = now.system_ms() - steady_age_ms - wall_skew_ms;
    return s;
}

}  // namespace

TEST(ClockReadyNow, AClockThatWasNeverPublishedCannotSign) {
    ClockOffsetPublisher publisher;
    EXPECT_FALSE(clock_ready_now(publisher));
}

TEST(ClockReadyNow, AFreshSnapshotCanSign) {
    ClockOffsetPublisher publisher;
    ASSERT_TRUE(publisher.publish(snapshot_aged(10)));
    EXPECT_TRUE(clock_ready_now(publisher));
}

TEST(ClockReadyNow, ASnapshotPastItsTtlCannotSign) {
    ClockOffsetPublisher publisher;
    ASSERT_TRUE(publisher.publish(snapshot_aged(kOffsetTtlMs + 5'000)));
    EXPECT_FALSE(clock_ready_now(publisher));
}

TEST(ClockReadyNow, AWallClockThatJumpedCannotSign) {
    ClockOffsetPublisher publisher;
    ASSERT_TRUE(publisher.publish(snapshot_aged(10, kMaxClockDriftMs + 5'000)));
    EXPECT_FALSE(clock_ready_now(publisher));
}

TEST(ClockReadyNow, IsTheSamePredicateAsTheTokenIssuer) {
    ClockOffsetPublisher publisher;
    ASSERT_TRUE(publisher.publish(snapshot_aged(10)));
    EXPECT_EQ(clock_ready_now(publisher), issue_clock_ready_token(publisher, fetch_clock_pair()).has_value());
    ClockOffsetPublisher stale;
    ASSERT_TRUE(stale.publish(snapshot_aged(kOffsetTtlMs + 5'000)));
    EXPECT_EQ(clock_ready_now(stale), issue_clock_ready_token(stale, fetch_clock_pair()).has_value());
}

// --- the synthetic readiness --------------------------------------------------------------------------------------------------------------

TEST(SyntheticReadiness, AuthorizesBecauseTheRealComponentsWereDrivenToReady) {
    KillSwitch kill_switch;
    SyntheticReadiness synthetic(kill_switch);
    EXPECT_EQ(synthetic.recovery().state(), RunState::Ready);
    EXPECT_EQ(synthetic.readiness().barrier().state(), BarrierState::Reconciled);
    const ReadinessResult r = synthetic.readiness().authorize(42);
    ASSERT_TRUE(r.ready());
    EXPECT_EQ(r.token->generation(), 1U);
    EXPECT_EQ(r.token->issued_at_ms(), 42);
}

TEST(SyntheticReadiness, TheGenerationIsConfigurableAndZeroMeansOne) {
    KillSwitch kill_switch;
    SyntheticReadiness seven(kill_switch, 7);
    EXPECT_EQ(seven.readiness().authorize(0).token->generation(), 7U);
    SyntheticReadiness zero(kill_switch, 0);
    EXPECT_EQ(zero.readiness().authorize(0).token->generation(), 1U) << "generation 0 means 'no session' to the barrier";
}

TEST(SyntheticReadiness, IsNotABypassTheKillSwitchStillRefuses) {
    KillSwitch kill_switch;
    SyntheticReadiness synthetic(kill_switch);
    kill_switch.trigger();
    expect_refused(synthetic.readiness().authorize(0), NotReadyReason::KillSwitchNotNormal);
    kill_switch.operator_reset();
    EXPECT_TRUE(synthetic.readiness().authorize(0).ready());
}

TEST(SyntheticReadiness, IsNotABypassTheBarrierStillCloses) {
    KillSwitch kill_switch;
    SyntheticReadiness synthetic(kill_switch);
    synthetic.readiness().barrier().on_generation_ended(1);  // a drained serverShutdown, as in a drill
    expect_refused(synthetic.readiness().authorize(0), NotReadyReason::FeedDown);
}

TEST(SyntheticReadiness, ASweepIsStillRequiredAfterATroubleEvent) {
    KillSwitch kill_switch;
    SyntheticReadiness synthetic(kill_switch);
    const std::int64_t steady_now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    FeedObservation trouble = live_feed(1, steady_now);
    trouble.ring_drops = 3;  // the synthetic feed says 0: any difference is a barrier-visible trouble
    synthetic.readiness().barrier().observe(trouble);
    ASSERT_EQ(synthetic.readiness().barrier().state(), BarrierState::Sweeping);
    expect_refused(synthetic.readiness().authorize(0), NotReadyReason::BarrierSweeping);
}

// --- the types ------------------------------------------------------------------------------------------------------------------------------

TEST(SubmitReadyToken, IsAMoveOnlyValueWithOneIssuer) {
    static_assert(!std::is_copy_constructible_v<SubmitReadyToken>);
    static_assert(!std::is_copy_assignable_v<SubmitReadyToken>);
    static_assert(std::is_nothrow_move_constructible_v<SubmitReadyToken>);
    static_assert(std::is_nothrow_move_assignable_v<SubmitReadyToken>);
    static_assert(!std::is_default_constructible_v<SubmitReadyToken>);
    static_assert(!std::is_constructible_v<SubmitReadyToken, std::uint64_t, std::int64_t>, "only SubmitReadiness::authorize() can mint one");
    static_assert(!std::is_copy_constructible_v<SubmitReadiness>);
    static_assert(!std::is_copy_assignable_v<SubmitReadiness>);
    SUCCEED();
}

TEST(SubmitReadyToken, ADefaultResultIsAnUnwiredRefusal) {
    const ReadinessResult r{};
    EXPECT_FALSE(r.ready());
    EXPECT_FALSE(r.token.has_value());
    EXPECT_EQ(r.reason, NotReadyReason::Unwired);
}

TEST(SubmitReadyToken, ATokenCanBeMovedIntoAnOptional) {
    KillSwitch kill_switch;
    SyntheticReadiness synthetic(kill_switch, 3);
    ReadinessResult r = synthetic.readiness().authorize(9);
    ASSERT_TRUE(r.ready());
    std::optional<SubmitReadyToken> held = std::move(r.token);
    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(held->generation(), 3U);
    EXPECT_EQ(held->issued_at_ms(), 9);
}

TEST(NotReadyReasonNames, EveryReasonHasADistinctNameAndTheCountMatches) {
    static_assert(kNotReadyReasonCount == static_cast<std::size_t>(NotReadyReason::ClockNotReady) + 1U,
                  "kNotReadyReasonCount must follow the last enumerator");
    std::vector<std::string_view> seen;
    for (std::size_t i = 0; i < kNotReadyReasonCount; ++i) {
        const std::string_view name = not_ready_reason_name(static_cast<NotReadyReason>(i));
        EXPECT_NE(name, "?") << i;
        EXPECT_FALSE(name.empty()) << i;
        for (const std::string_view other : seen) EXPECT_NE(name, other) << i;
        seen.push_back(name);
    }
    EXPECT_EQ(std::string_view(not_ready_reason_name(static_cast<NotReadyReason>(kNotReadyReasonCount))), "?");
    EXPECT_EQ(std::string_view(not_ready_reason_name(NotReadyReason::None)), "None");
    EXPECT_EQ(std::string_view(not_ready_reason_name(NotReadyReason::Unwired)), "Unwired");
}
