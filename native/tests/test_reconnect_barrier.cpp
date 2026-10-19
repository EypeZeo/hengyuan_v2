// Private-stream migration M3a: ReconnectBarrier (reconnect_barrier.hpp) -- the pure state machine behind "may a new order leave while the private
// stream is anything less than a connected, verified, swept subscription?". No stream, no clock, no I/O: observations in, a state out.
//
// What is pinned here, by the design's fault cases:
//   FI-043  a generation change (rotation, reconnect): blocked from the moment the old one is gone until the NEW one is swept
//   FI-011  a serverShutdown / eventStreamTerminated event ends its generation before the supervisor has noticed anything
//   FI-020  a silent connection (no frame, ping included) is dead whatever TCP says, and the loop is told to stop it
//   FI-009  a late event of an older generation: merged until the new generation is Reconciled, dropped after
//   F5      a dropped or unreadable event is a soft barrier: back to Sweeping without a reconnect
// plus the sweep protocol (epochs), an exhaustive state x event table and a random walk that checks the invariants after every step.
#include <gtest/gtest.h>

#include <hengyuan/reconnect_barrier.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace hy;

namespace {

constexpr std::int64_t kNow = 1'000'000;  // an arbitrary steady-clock reading, ms

FeedObservation up(std::uint64_t generation, std::int64_t now = kNow, std::uint64_t drops = 0, std::uint64_t lost = 0) {
    FeedObservation o{};
    o.generation = generation;
    o.connected = true;
    o.last_activity_ms = now;
    o.ring_drops = drops;
    o.lost_events = lost;
    o.sampled_at_ms = now;
    return o;
}

FeedObservation down(std::uint64_t generation, std::int64_t now = kNow) {
    FeedObservation o{};
    o.generation = generation;
    o.connected = false;
    o.sampled_at_ms = now;
    return o;
}

// Sweeping for `generation`, request outstanding.
ReconnectBarrier sweeping_outstanding(std::uint64_t generation = 1, BarrierPolicy policy = {}) {
    ReconnectBarrier b(policy);
    b.observe(up(generation));
    EXPECT_TRUE(b.take_sweep_request(kNow).has_value());
    return b;
}

// Swept clean: Reconciled(generation).
ReconnectBarrier reconciled(std::uint64_t generation = 1, BarrierPolicy policy = {}) {
    ReconnectBarrier b = sweeping_outstanding(generation, policy);
    b.on_sweep_report(SweepReport{b.epoch(), 0U, true}, kNow);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    return b;
}

SweepReport clean_report(const ReconnectBarrier& b) { return SweepReport{b.epoch(), 0U, true}; }

}  // namespace

// --- the sweep protocol -------------------------------------------------------------------------------------------------

// --- the defaults are the design's numbers ---------------------------------------------------------------------------------

TEST(ReconnectBarrierPolicy, TheDefaultsAreWhatTheDesignSays) {
    const BarrierPolicy p;
    EXPECT_EQ(p.stale_after_ms, 60'000) << "three server ping intervals (20 s each), blueprint 5.7 P-30";
    EXPECT_EQ(p.resweep_after_ms, 1'000);
    EXPECT_EQ(p.sweep_timeout_ms, 30'000);
    const ReconnectBarrier b;
    EXPECT_EQ(b.policy().stale_after_ms, p.stale_after_ms);
    EXPECT_EQ(b.policy().resweep_after_ms, p.resweep_after_ms);
    EXPECT_EQ(b.policy().sweep_timeout_ms, p.sweep_timeout_ms);
}

TEST(ReconnectBarrier, StartsDownAndHasNothingToAsk) {
    ReconnectBarrier b;
    EXPECT_EQ(b.state(), BarrierState::Down);
    EXPECT_EQ(b.generation(), 0U);
    EXPECT_EQ(b.reconciled_generation(), 0U);
    EXPECT_FALSE(b.sweep_outstanding());
    EXPECT_FALSE(b.take_sweep_request(kNow).has_value());
    EXPECT_FALSE(b.take_stop_request());
}

TEST(ReconnectBarrier, AConnectedGenerationBeginsASweepAndIsNotReconciledByConnecting) {
    ReconnectBarrier b;
    b.observe(up(1));
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_EQ(b.generation(), 1U);
    EXPECT_EQ(b.reconciled_generation(), 0U);
    EXPECT_EQ(b.stats().generations_seen, 1U);
    EXPECT_EQ(b.stats().sweeps_begun, 1U);
}

TEST(ReconnectBarrier, TheSweepIsRequestedOnceUntilItIsAnswered) {
    ReconnectBarrier b;
    b.observe(up(1));
    const std::optional<std::uint64_t> first = b.take_sweep_request(kNow);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, b.epoch());
    EXPECT_TRUE(b.sweep_outstanding());
    EXPECT_FALSE(b.take_sweep_request(kNow).has_value());
    EXPECT_FALSE(b.take_sweep_request(kNow + 5).has_value());
    EXPECT_EQ(b.stats().sweeps_requested, 1U);
}

TEST(ReconnectBarrier, AGoodReportReconcilesTheGenerationThatWasSwept) {
    ReconnectBarrier b = sweeping_outstanding(3);
    b.on_sweep_report(clean_report(b), kNow);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    EXPECT_EQ(b.reconciled_generation(), 3U);
    EXPECT_FALSE(b.sweep_outstanding());
    EXPECT_EQ(b.stats().sweeps_completed, 1U);
    EXPECT_FALSE(b.take_sweep_request(kNow).has_value()) << "nothing more to ask once reconciled";
}

TEST(ReconnectBarrier, AReportWithUnresolvedOrdersKeepsItSweepingAndAsksAgainAfterAPause) {
    BarrierPolicy p;
    p.resweep_after_ms = 1000;
    ReconnectBarrier b = sweeping_outstanding(1, p);
    const std::uint64_t first_epoch = b.epoch();
    b.on_sweep_report(SweepReport{first_epoch, 2U, true}, kNow);
    EXPECT_EQ(b.state(), BarrierState::Sweeping) << "two orders still without an answer: not reconciled";
    EXPECT_EQ(b.stats().sweeps_unresolved, 1U);
    EXPECT_FALSE(b.sweep_outstanding());
    EXPECT_FALSE(b.take_sweep_request(kNow).has_value());
    EXPECT_FALSE(b.take_sweep_request(kNow + 999).has_value());
    const std::optional<std::uint64_t> second = b.take_sweep_request(kNow + 1000);
    ASSERT_TRUE(second.has_value());
    EXPECT_GT(*second, first_epoch) << "a new request is a new epoch, so a late duplicate of the old answer cannot count";
    b.on_sweep_report(SweepReport{first_epoch, 0U, true}, kNow + 1000);
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    b.on_sweep_report(SweepReport{*second, 0U, true}, kNow + 1000);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
}

TEST(ReconnectBarrier, AReportWithoutAFreshAccountDoesNotReconcile) {
    ReconnectBarrier b = sweeping_outstanding();
    b.on_sweep_report(SweepReport{b.epoch(), 0U, false}, kNow);
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_EQ(b.stats().sweeps_unresolved, 1U);
}

TEST(ReconnectBarrier, OnlyAReportForTheCurrentEpochCounts) {
    ReconnectBarrier b = sweeping_outstanding();
    const std::uint64_t epoch = b.epoch();
    b.on_sweep_report(SweepReport{epoch + 1, 0U, true}, kNow);
    b.on_sweep_report(SweepReport{epoch - 1, 0U, true}, kNow);
    b.on_sweep_report(SweepReport{0U, 0U, true}, kNow);
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_TRUE(b.sweep_outstanding()) << "wrong answers do not consume the request";
    EXPECT_EQ(b.stats().reports_ignored, 3U);
    b.on_sweep_report(SweepReport{epoch, 0U, true}, kNow);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    // ... and once reconciled, a repeat of the very same answer is an answer to nothing
    b.on_sweep_report(SweepReport{epoch, 7U, false}, kNow);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    EXPECT_EQ(b.stats().reports_ignored, 4U);
}

TEST(ReconnectBarrier, AnUnsolicitedReportChangesNothing) {
    ReconnectBarrier b;
    b.observe(up(1));  // Sweeping, but nobody asked yet
    b.on_sweep_report(SweepReport{b.epoch(), 0U, true}, kNow);
    EXPECT_EQ(b.state(), BarrierState::Sweeping) << "an answer to a question that was never asked";
    EXPECT_EQ(b.stats().reports_ignored, 1U);
    ReconnectBarrier d;
    d.on_sweep_report(SweepReport{d.epoch(), 0U, true}, kNow);
    EXPECT_EQ(d.state(), BarrierState::Down);
}

TEST(ReconnectBarrier, AReportForAGenerationThatIsGoneChangesNothing) {
    ReconnectBarrier b = sweeping_outstanding(1);
    const SweepReport old_answer = clean_report(b);
    b.observe(up(2));  // the supervisor moved on
    ASSERT_EQ(b.state(), BarrierState::Sweeping);
    b.on_sweep_report(old_answer, kNow);
    EXPECT_EQ(b.state(), BarrierState::Sweeping) << "the answer was about generation 1";
    EXPECT_EQ(b.reconciled_generation(), 0U);

    ReconnectBarrier c = sweeping_outstanding(1);
    const SweepReport old_answer_2 = clean_report(c);
    c.observe(down(2));
    c.on_sweep_report(old_answer_2, kNow);
    EXPECT_EQ(c.state(), BarrierState::Down);
}

TEST(ReconnectBarrier, ARequestNobodyAnsweredIsAskedAgainUnderANewEpoch) {
    BarrierPolicy p;
    p.sweep_timeout_ms = 5000;
    ReconnectBarrier b(p);
    b.observe(up(1));
    const std::optional<std::uint64_t> first = b.take_sweep_request(kNow);
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(b.take_sweep_request(kNow + 5000).has_value()) << "exactly at the timeout is still waiting";
    const std::optional<std::uint64_t> second = b.take_sweep_request(kNow + 5001);
    ASSERT_TRUE(second.has_value());
    EXPECT_GT(*second, *first);
    EXPECT_EQ(b.stats().sweep_timeouts, 1U);
    b.on_sweep_report(SweepReport{*first, 0U, true}, kNow + 5002);
    EXPECT_EQ(b.state(), BarrierState::Sweeping) << "the late answer to the first request";
    b.on_sweep_report(SweepReport{*second, 0U, true}, kNow + 5002);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
}

// --- FI-043: generation changes ----------------------------------------------------------------------------------------

TEST(ReconnectBarrierGenerations, LeavingConnectedBlocksAtOnce) {
    ReconnectBarrier b = reconciled(1);
    b.observe(down(1));
    EXPECT_EQ(b.state(), BarrierState::Down);
    EXPECT_EQ(b.reconciled_generation(), 0U);
    EXPECT_EQ(b.stats().downs, 1U);
}

TEST(ReconnectBarrierGenerations, ReconnectingTheSameGenerationNumberStillNeedsANewSweep) {
    ReconnectBarrier b = reconciled(1);
    b.observe(down(1));
    b.observe(up(1));
    EXPECT_EQ(b.state(), BarrierState::Sweeping) << "what happened while it was down is what the sweep is for";
}

TEST(ReconnectBarrierGenerations, TheNextGenerationIsNotReconciledJustBecauseTheLastOneWas) {
    ReconnectBarrier b = reconciled(1);
    b.observe(up(2));
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_EQ(b.generation(), 2U);
    EXPECT_EQ(b.reconciled_generation(), 0U);
    EXPECT_TRUE(b.admit_event(1)) << "not reconciled for 2 yet: the old generation's events are still merged";
}

TEST(ReconnectBarrierGenerations, ARotationRunsDownThenSweepingThenReconciledInThatOrder) {
    ReconnectBarrier b = reconciled(1);
    std::vector<BarrierState> seen;
    const auto note = [&] { seen.push_back(b.state()); };
    b.observe(down(1)); note();                       // the old connection is gone
    b.observe(down(2)); note();                       // the supervisor started 2, not connected yet
    b.observe(up(2)); note();                         // connected and verified
    ASSERT_TRUE(b.take_sweep_request(kNow).has_value());
    b.on_sweep_report(clean_report(b), kNow); note();  // swept
    EXPECT_EQ(seen, (std::vector<BarrierState>{BarrierState::Down, BarrierState::Down, BarrierState::Sweeping, BarrierState::Reconciled}));
    EXPECT_EQ(b.reconciled_generation(), 2U);
}

// --- FI-011: serverShutdown / eventStreamTerminated --------------------------------------------------------------------

TEST(ReconnectBarrierGenerations, ServerShutdownEndsTheGenerationBeforeTheSupervisorNoticesAnything) {
    ReconnectBarrier b = reconciled(1);
    b.on_generation_ended(1);
    EXPECT_EQ(b.state(), BarrierState::Down);
    EXPECT_EQ(b.reconciled_generation(), 0U);
    // The session still looks connected for a moment: that must not bring the barrier back.
    b.observe(up(1));
    EXPECT_EQ(b.state(), BarrierState::Down);
    b.observe(up(1));
    EXPECT_EQ(b.state(), BarrierState::Down);
    b.observe(down(1));
    EXPECT_EQ(b.state(), BarrierState::Down);
    // The next generation starts the normal way.
    b.observe(up(2));
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_EQ(b.stats().generation_ends_seen, 1U);
}

TEST(ReconnectBarrierGenerations, AnEndOfAnOlderGenerationArrivingLateIsIgnored) {
    ReconnectBarrier b = sweeping_outstanding(2);
    b.on_generation_ended(1);
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_TRUE(b.sweep_outstanding());
    EXPECT_EQ(b.stats().generation_ends_seen, 0U);
}

TEST(ReconnectBarrierGenerations, TheEndOfAGenerationNotYetSeenKeepsItDownWhenItAppears) {
    ReconnectBarrier b = reconciled(2);
    b.on_generation_ended(3);  // drained before the loop ever observed generation 3
    EXPECT_EQ(b.state(), BarrierState::Down);
    b.observe(up(3));
    EXPECT_EQ(b.state(), BarrierState::Down) << "generation 3 is over";
    b.observe(up(4));
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
}

TEST(ReconnectBarrierGenerations, AnUnstampedEndSaysNothingAboutThisStream) {
    ReconnectBarrier b = reconciled(1);
    b.on_generation_ended(0);  // the retired listenKey session never stamps a generation
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    EXPECT_EQ(b.stats().generation_ends_seen, 0U);

    ReconnectBarrier fresh;  // tracking no generation at all: 0 is not "older than the tracked one" here, so only the stamp itself says "ignore"
    fresh.on_generation_ended(0);
    EXPECT_EQ(fresh.stats().generation_ends_seen, 0U);
    fresh.observe(up(1));
    EXPECT_EQ(fresh.state(), BarrierState::Sweeping) << "an unstamped end did not poison generation 1";
}

TEST(ReconnectBarrierGenerations, AnEndWhileDownStillRemembersTheGeneration) {
    ReconnectBarrier b;
    b.on_generation_ended(1);
    b.observe(up(1));
    EXPECT_EQ(b.state(), BarrierState::Down);
}

// --- FI-020: the heartbeat -------------------------------------------------------------------------------------------------

TEST(ReconnectBarrierHeartbeat, ASilentConnectionIsDeadWhateverTcpSaysAndTheLoopIsToldToStopIt) {
    BarrierPolicy p;
    p.stale_after_ms = 60'000;
    ReconnectBarrier b = reconciled(1, p);
    FeedObservation silent = up(1, kNow);      // last activity: kNow
    silent.sampled_at_ms = kNow + 60'001;      // ... and a minute has passed with no frame, no ping
    b.observe(silent);
    EXPECT_EQ(b.state(), BarrierState::Down);
    EXPECT_EQ(b.reconciled_generation(), 0U);
    EXPECT_EQ(b.stats().stale_heartbeats, 1U);
    EXPECT_TRUE(b.take_stop_request());
    EXPECT_FALSE(b.take_stop_request()) << "once";
}

TEST(ReconnectBarrierHeartbeat, TheBoundaryIsExact) {
    BarrierPolicy p;
    p.stale_after_ms = 60'000;
    ReconnectBarrier b(p);
    FeedObservation edge = up(1, kNow);
    edge.sampled_at_ms = kNow + 60'000;
    b.observe(edge);
    EXPECT_EQ(b.state(), BarrierState::Sweeping) << "60 000 ms of silence is not yet more than 60 000 ms";
    EXPECT_FALSE(b.take_stop_request());
    FeedObservation over = edge;
    over.sampled_at_ms = kNow + 60'001;
    b.observe(over);
    EXPECT_EQ(b.state(), BarrierState::Down);
    EXPECT_TRUE(b.take_stop_request());
}

TEST(ReconnectBarrierHeartbeat, NoActivityAtAllIsSilence) {
    ReconnectBarrier b;
    FeedObservation o = up(1, kNow);
    o.last_activity_ms = 0;
    b.observe(o);
    EXPECT_EQ(b.state(), BarrierState::Down);
    EXPECT_TRUE(b.take_stop_request());
}

TEST(ReconnectBarrierHeartbeat, NoActivityAtAllIsSilenceEvenWhenTheClockIsYoung) {
    // A process in its first minute: the steady clock reads, say, 5 s. "No frame yet" must not look like 5 s of quiet that is still fine.
    ReconnectBarrier b;
    FeedObservation o = up(1, 5'000);
    o.last_activity_ms = 0;
    o.sampled_at_ms = 5'000;
    b.observe(o);
    EXPECT_EQ(b.state(), BarrierState::Down);
    EXPECT_TRUE(b.take_stop_request());
}

TEST(ReconnectBarrierHeartbeat, APingKeepsAQuietConnectionAlive) {
    // A quiet account has no events at all; the server's ping is the only sign of life (blueprint 5.7 P-30). The session stamps every control
    // frame into last_activity_ms, so a connection whose newest frame is a ping 20 s ago is healthy.
    ReconnectBarrier b = reconciled(1);
    FeedObservation quiet = up(1, kNow);
    quiet.sampled_at_ms = kNow + 20'000;
    quiet.last_activity_ms = kNow;
    b.observe(quiet);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    quiet.sampled_at_ms = kNow + 40'000;
    quiet.last_activity_ms = kNow + 20'000;  // the next ping arrived
    b.observe(quiet);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    EXPECT_FALSE(b.take_stop_request());
}

TEST(ReconnectBarrierHeartbeat, ASampleTakenJustBeforeTheLatestFrameIsNotSilence) {
    ReconnectBarrier b;
    FeedObservation o = up(1, kNow);
    o.last_activity_ms = kNow + 5;  // the frame landed between the probe's clock read and its field reads
    b.observe(o);
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
}

TEST(ReconnectBarrierHeartbeat, AGenerationDeclaredDeadDoesNotComeBackWhileTheSessionStillLooksConnected) {
    ReconnectBarrier b = reconciled(1);
    FeedObservation silent = up(1, kNow);
    silent.sampled_at_ms = kNow + 120'000;
    b.observe(silent);
    ASSERT_EQ(b.state(), BarrierState::Down);
    // the loop has not stopped the session yet, and a frame arrives right now:
    b.observe(up(1, kNow + 121'000));
    EXPECT_EQ(b.state(), BarrierState::Down);
    EXPECT_TRUE(b.take_stop_request()) << "still owed";
    // the supervisor moves on
    b.observe(up(2, kNow + 130'000));
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
}

TEST(ReconnectBarrierHeartbeat, AStopRequestForAGenerationThatAlreadyChangedIsMoot) {
    ReconnectBarrier b = reconciled(1);
    FeedObservation silent = up(1, kNow);
    silent.sampled_at_ms = kNow + 120'000;
    b.observe(silent);
    b.observe(down(2, kNow + 121'000));
    EXPECT_FALSE(b.take_stop_request()) << "generation 1 is gone already; stopping 'the current session' now would stop generation 2";
}

TEST(ReconnectBarrierHeartbeat, ASilentSessionIsStoppedEvenWhileTheBarrierWasAlreadyDown) {
    ReconnectBarrier b;
    b.observe(down(1));
    FeedObservation o = up(1, kNow);
    o.sampled_at_ms = kNow + 90'000;
    b.observe(o);
    EXPECT_TRUE(b.take_stop_request());
}

// --- F5: a lost event is a soft barrier ---------------------------------------------------------------------------------

TEST(ReconnectBarrierSoftBarrier, ADroppedEventSendsAReconciledGenerationBackToSweepingWithoutAReconnect) {
    ReconnectBarrier b = reconciled(1);
    const std::uint64_t downs = b.stats().downs;
    const std::uint64_t epoch = b.epoch();
    b.observe(up(1, kNow, /*drops=*/1, /*lost=*/0));
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_EQ(b.reconciled_generation(), 0U);
    EXPECT_EQ(b.generation(), 1U) << "same generation: nothing was reconnected";
    EXPECT_EQ(b.stats().downs, downs);
    EXPECT_EQ(b.stats().soft_barriers, 1U);
    EXPECT_GT(b.epoch(), epoch);
    ASSERT_TRUE(b.take_sweep_request(kNow).has_value());
    b.on_sweep_report(clean_report(b), kNow);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
}

TEST(ReconnectBarrierSoftBarrier, AnExecutionReportTheCodecCouldNotReadDoesTheSame) {
    ReconnectBarrier b = reconciled(1);
    b.observe(up(1, kNow, /*drops=*/0, /*lost=*/1));
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_EQ(b.stats().soft_barriers, 1U);
}

TEST(ReconnectBarrierSoftBarrier, ADropDuringASweepRestartsItUnderANewEpochAndTheOldAnswerIsIgnored) {
    ReconnectBarrier b = sweeping_outstanding(1);
    const SweepReport stale_answer = clean_report(b);
    b.observe(up(1, kNow, /*drops=*/2, /*lost=*/0));  // an event was lost AFTER that sweep began: what it found may be out of date
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_FALSE(b.sweep_outstanding());
    b.on_sweep_report(stale_answer, kNow);
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    const std::optional<std::uint64_t> again = b.take_sweep_request(kNow);
    ASSERT_TRUE(again.has_value());
    EXPECT_NE(*again, stale_answer.epoch);
    b.on_sweep_report(SweepReport{*again, 0U, true}, kNow);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
}

TEST(ReconnectBarrierSoftBarrier, CountersAlreadyNonZeroAtTheFirstSightAreABaselineNotATrouble) {
    ReconnectBarrier b;
    b.observe(up(2, kNow, /*drops=*/5, /*lost=*/3));
    ASSERT_TRUE(b.take_sweep_request(kNow).has_value());
    b.on_sweep_report(clean_report(b), kNow);
    ASSERT_EQ(b.state(), BarrierState::Reconciled);
    b.observe(up(2, kNow, 5, 3));
    EXPECT_EQ(b.state(), BarrierState::Reconciled) << "the same readings again are no news";
    EXPECT_EQ(b.stats().soft_barriers, 0U);
    b.observe(up(2, kNow, 6, 3));
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    EXPECT_EQ(b.stats().soft_barriers, 1U);
}

TEST(ReconnectBarrierSoftBarrier, ANewGenerationBringsItsOwnCounters) {
    ReconnectBarrier b = reconciled(1);
    b.observe(up(1, kNow, 9, 9));  // soft barrier in generation 1
    b.observe(up(2, kNow, 0, 0));  // generation 2's session starts counting from zero: a baseline, not a decrease to punish
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
    ASSERT_TRUE(b.take_sweep_request(kNow).has_value());
    b.on_sweep_report(clean_report(b), kNow);
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    EXPECT_EQ(b.stats().soft_barriers, 1U);
}

TEST(ReconnectBarrierSoftBarrier, ADroppedEventIsAProblemOnlyTheFirstTimeItIsSeen) {
    ReconnectBarrier b = reconciled(1);
    b.observe(up(1, kNow, /*drops=*/2, /*lost=*/0));
    ASSERT_EQ(b.state(), BarrierState::Sweeping);
    ASSERT_TRUE(b.take_sweep_request(kNow).has_value());
    b.on_sweep_report(clean_report(b), kNow);
    ASSERT_EQ(b.state(), BarrierState::Reconciled);
    b.observe(up(1, kNow, /*drops=*/2, /*lost=*/0));  // the same reading again is no news
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    EXPECT_EQ(b.stats().soft_barriers, 1U);
}

TEST(ReconnectBarrierSoftBarrier, AnUnreadableReportIsAProblemOnlyTheFirstTimeItIsSeen) {
    ReconnectBarrier b = reconciled(1);
    b.observe(up(1, kNow, /*drops=*/0, /*lost=*/1));
    ASSERT_EQ(b.state(), BarrierState::Sweeping);
    ASSERT_TRUE(b.take_sweep_request(kNow).has_value());
    b.on_sweep_report(clean_report(b), kNow);
    ASSERT_EQ(b.state(), BarrierState::Reconciled);
    b.observe(up(1, kNow, /*drops=*/0, /*lost=*/1));  // the same reading again is no news
    EXPECT_EQ(b.state(), BarrierState::Reconciled);
    EXPECT_EQ(b.stats().soft_barriers, 1U);
}

TEST(ReconnectBarrierSoftBarrier, ACounterThatWentDownIsAlsoNotTheSameReading) {
    ReconnectBarrier b = reconciled(1);
    b.observe(up(1, kNow, 4, 0));
    ASSERT_EQ(b.state(), BarrierState::Sweeping);
    ASSERT_TRUE(b.take_sweep_request(kNow).has_value());
    b.on_sweep_report(clean_report(b), kNow);
    ASSERT_EQ(b.state(), BarrierState::Reconciled);
    b.observe(up(1, kNow, 3, 0));  // monotone counters do not go down: whatever this is, it is not what the barrier thought it was
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
}

// --- FI-009: late events ---------------------------------------------------------------------------------------------------

TEST(ReconnectBarrierEvents, AnEventOfAnOlderGenerationIsDroppedAndCountedOnceReconciled) {
    ReconnectBarrier b = reconciled(2);
    EXPECT_FALSE(b.admit_event(1));
    EXPECT_EQ(b.stats().late_events_dropped, 1U);
    EXPECT_TRUE(b.admit_event(2));
    EXPECT_TRUE(b.admit_event(3)) << "a newer generation's event is not late";
    EXPECT_EQ(b.stats().late_events_dropped, 1U);
}

TEST(ReconnectBarrierEvents, BeforeReconciledOldEventsAreMerged) {
    ReconnectBarrier b;
    b.observe(up(2));
    EXPECT_TRUE(b.admit_event(1)) << "Sweeping";
    b.observe(down(2));
    EXPECT_TRUE(b.admit_event(1)) << "Down";
    EXPECT_EQ(b.stats().late_events_dropped, 0U);
}

TEST(ReconnectBarrierEvents, TheFloorIsGoneAsSoonAsTheBarrierLeavesReconciled) {
    ReconnectBarrier b = reconciled(2);
    ASSERT_FALSE(b.admit_event(1));
    b.on_generation_ended(2);
    EXPECT_TRUE(b.admit_event(1)) << "Down again: the events of the generation that just ended are the ones to merge";
}

TEST(ReconnectBarrierEvents, AnUnstampedEventIsAlwaysMerged) {
    ReconnectBarrier b = reconciled(5);
    EXPECT_TRUE(b.admit_event(0)) << "generation 0 = not stamped (the retired listenKey session)";
    EXPECT_EQ(b.stats().late_events_dropped, 0U);
}

TEST(ReconnectBarrierEvents, TheFirstGenerationHasNothingOlder) {
    ReconnectBarrier b = reconciled(1);
    EXPECT_TRUE(b.admit_event(1));
    EXPECT_TRUE(b.admit_event(0));
}

// --- corners ---------------------------------------------------------------------------------------------------------------

TEST(ReconnectBarrierGenerations, StayingDownIsNotEnteringDownAgain) {
    ReconnectBarrier b = reconciled(1);
    b.observe(down(1));
    const std::uint64_t downs = b.stats().downs;
    const std::uint64_t epoch = b.epoch();
    for (int i = 0; i < 5; ++i) b.observe(down(1));
    b.observe(FeedObservation{});
    b.on_generation_ended(1);
    EXPECT_EQ(b.stats().downs, downs) << "Down counts entries from another state, not observations";
    EXPECT_EQ(b.epoch(), epoch) << "nothing was invalidated: there was nothing outstanding";
    EXPECT_EQ(b.stats().generations_seen, 1U) << "'no generation yet' is not a generation";
}

TEST(ReconnectBarrierGenerations, TheEndOfAFutureGenerationIsNotForgottenByTheEndOfTheCurrentOne) {
    ReconnectBarrier b = reconciled(2);
    b.on_generation_ended(3);  // drained before the loop ever observed generation 3
    b.on_generation_ended(2);  // ... and now the current generation's own end arrives
    b.observe(up(3));
    EXPECT_EQ(b.state(), BarrierState::Down) << "generation 3 is over: the later, lower end must not lift it";
    b.observe(up(4));
    EXPECT_EQ(b.state(), BarrierState::Sweeping);
}

TEST(ReconnectBarrierSoftBarrier, ARestartedSweepIsNotHeldBackByThePauseOfTheOneItReplaced) {
    BarrierPolicy p;
    p.resweep_after_ms = 10'000;
    ReconnectBarrier b = sweeping_outstanding(1, p);
    b.on_sweep_report(SweepReport{b.epoch(), 3U, true}, kNow);  // unresolved: the next request waits 10 s
    ASSERT_FALSE(b.take_sweep_request(kNow + 1).has_value());
    b.observe(up(1, kNow, /*drops=*/1, /*lost=*/0));            // an event was lost: the sweep starts over
    EXPECT_TRUE(b.take_sweep_request(kNow + 2).has_value()) << "a restart is a fresh sweep, due at once";
}

TEST(ReconnectBarrierSoftBarrier, ANewGenerationsSweepIsNotHeldBackByThePauseOfTheLastOne) {
    BarrierPolicy p;
    p.resweep_after_ms = 10'000;
    ReconnectBarrier b = sweeping_outstanding(1, p);
    b.on_sweep_report(SweepReport{b.epoch(), 3U, true}, kNow);
    b.observe(up(2));
    EXPECT_TRUE(b.take_sweep_request(kNow + 2).has_value());
}

TEST(ReconnectBarrierArithmetic, ElapsedTimeNeverGoesNegativeAndNeverOverflows) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    EXPECT_EQ(detail::barrier_elapsed_ms(10, 4), 6);
    EXPECT_EQ(detail::barrier_elapsed_ms(4, 4), 0);
    EXPECT_EQ(detail::barrier_elapsed_ms(4, 10), 0) << "a clock that ran backwards: nothing has elapsed";
    EXPECT_EQ(detail::barrier_elapsed_ms(kMax, 0), kMax);
    EXPECT_EQ(detail::barrier_elapsed_ms(kMax, -1), kMax) << "kMax + 1 would overflow: saturate";
    EXPECT_EQ(detail::barrier_elapsed_ms(0, kMin), kMax);
    EXPECT_EQ(detail::barrier_elapsed_ms(kMax, kMin), kMax);
    EXPECT_EQ(detail::barrier_elapsed_ms(-5, -9), 4);
    EXPECT_EQ(detail::barrier_elapsed_ms(0, -1), 1);
}

TEST(ReconnectBarrierArithmetic, TimePlusDelaySaturates) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    EXPECT_EQ(detail::barrier_sat_add_ms(5, 7), 12);
    EXPECT_EQ(detail::barrier_sat_add_ms(kMax, 1), kMax);
    EXPECT_EQ(detail::barrier_sat_add_ms(kMax - 1, 1), kMax);
    EXPECT_EQ(detail::barrier_sat_add_ms(kMax - 1, 2), kMax);
    EXPECT_EQ(detail::barrier_sat_add_ms(kMin, -1), kMin);
    EXPECT_EQ(detail::barrier_sat_add_ms(kMin + 1, -1), kMin);
    EXPECT_EQ(detail::barrier_sat_add_ms(kMin + 1, -2), kMin);
    EXPECT_EQ(detail::barrier_sat_add_ms(-3, 3), 0);
}

TEST(ReconnectBarrierArithmetic, ExtremeClockReadingsNeitherOverflowNorFlipTheVerdict) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    ReconnectBarrier b;
    FeedObservation o = up(1);
    o.last_activity_ms = 1;
    o.sampled_at_ms = kMax;  // a very late sample: that is silence, not an overflow into a small number
    b.observe(o);
    EXPECT_EQ(b.state(), BarrierState::Down);
    EXPECT_EQ(b.stats().stale_heartbeats, 1U);

    // A request made at a hugely negative steady time: the elapsed time to kMax does not fit an int64 and must read as "a long time".
    ReconnectBarrier c;
    c.observe(up(1));
    ASSERT_TRUE(c.take_sweep_request(kMin + 5).has_value());
    EXPECT_TRUE(c.take_sweep_request(kMax).has_value()) << "kMax - (kMin + 5) overflows: it must saturate, not wrap into a small number";
    EXPECT_EQ(c.stats().sweep_timeouts, 1U);

    // A pause added to a time at the top of the range saturates instead of wrapping into the past.
    ReconnectBarrier d = sweeping_outstanding(1);
    d.on_sweep_report(SweepReport{d.epoch(), 1U, true}, kMax);
    EXPECT_FALSE(d.take_sweep_request(kMax - 1).has_value());
    EXPECT_TRUE(d.take_sweep_request(kMax).has_value()) << "saturated at the top: due at kMax, not wrapped into the past";
}

// --- every state x every event ----------------------------------------------------------------------------------------------

namespace {

enum Start { kDown = 0, kSweepingAsked = 1, kSweepingIdle = 2, kReconciled = 3 };
constexpr const char* kStartName[4] = {"Down(gen 1)", "Sweeping(gen 1, asked)", "Sweeping(gen 1, idle)", "Reconciled(gen 1)"};

ReconnectBarrier make(Start s) {
    switch (s) {
        case kDown: {
            ReconnectBarrier b = reconciled(1);
            b.observe(down(1));
            return b;
        }
        case kSweepingAsked: return sweeping_outstanding(1);
        case kSweepingIdle: {
            ReconnectBarrier b;
            b.observe(up(1));
            return b;
        }
        case kReconciled: return reconciled(1);
    }
    return ReconnectBarrier{};
}

struct Expect {
    BarrierState state;
    std::uint64_t reconciled_generation;
};

struct Event {
    const char* name;
    void (*apply)(ReconnectBarrier&);
    Expect expect[4];  // indexed by Start
};

FeedObservation stale_obs() {
    FeedObservation o = up(1, kNow);
    o.sampled_at_ms = kNow + 10'000'000;
    return o;
}

}  // namespace

TEST(ReconnectBarrierTransitions, EveryStateAndEveryEvent) {
    constexpr Expect Dn{BarrierState::Down, 0};
    constexpr Expect Sw{BarrierState::Sweeping, 0};
    constexpr Expect Rc{BarrierState::Reconciled, 1};
    // columns: Down(gen 1) | Sweeping(gen 1, asked) | Sweeping(gen 1, idle) | Reconciled(gen 1)
    const Event events[] = {
        {"the feed again, healthy, same generation", [](ReconnectBarrier& b) { b.observe(up(1)); }, {Sw, Sw, Sw, Rc}},
        {"the feed again with a dropped event", [](ReconnectBarrier& b) { b.observe(up(1, kNow, 1, 0)); }, {Sw, Sw, Sw, Sw}},
        {"the feed again with an unreadable report", [](ReconnectBarrier& b) { b.observe(up(1, kNow, 0, 1)); }, {Sw, Sw, Sw, Sw}},
        {"the session is not connected", [](ReconnectBarrier& b) { b.observe(down(1)); }, {Dn, Dn, Dn, Dn}},
        {"a new generation, connected", [](ReconnectBarrier& b) { b.observe(up(2)); }, {Sw, Sw, Sw, Sw}},
        {"a new generation, not connected", [](ReconnectBarrier& b) { b.observe(down(2)); }, {Dn, Dn, Dn, Dn}},
        {"no generation at all", [](ReconnectBarrier& b) { b.observe(FeedObservation{}); }, {Dn, Dn, Dn, Dn}},
        {"the heartbeat is stale", [](ReconnectBarrier& b) { b.observe(stale_obs()); }, {Dn, Dn, Dn, Dn}},
        {"the generation ended (serverShutdown)", [](ReconnectBarrier& b) { b.on_generation_ended(1); }, {Dn, Dn, Dn, Dn}},
        {"an unstamped generation ended", [](ReconnectBarrier& b) { b.on_generation_ended(0); }, {Dn, Sw, Sw, Rc}},
        {"a clean report for the current epoch", [](ReconnectBarrier& b) { b.on_sweep_report(SweepReport{b.epoch(), 0U, true}, kNow); },
         {Dn, Rc, Sw, Rc}},
        {"a report that leaves orders unresolved", [](ReconnectBarrier& b) { b.on_sweep_report(SweepReport{b.epoch(), 4U, true}, kNow); },
         {Dn, Sw, Sw, Rc}},
        {"a clean report for another epoch", [](ReconnectBarrier& b) { b.on_sweep_report(SweepReport{b.epoch() + 1, 0U, true}, kNow); },
         {Dn, Sw, Sw, Rc}},
        {"a sweep request", [](ReconnectBarrier& b) { (void)b.take_sweep_request(kNow); }, {Dn, Sw, Sw, Rc}},
        {"an event of generation 0", [](ReconnectBarrier& b) { (void)b.admit_event(0); }, {Dn, Sw, Sw, Rc}},
    };
    for (const Event& e : events) {
        for (int s = 0; s < 4; ++s) {
            SCOPED_TRACE(std::string(e.name) + "  from  " + kStartName[s]);
            ReconnectBarrier b = make(static_cast<Start>(s));
            e.apply(b);
            EXPECT_EQ(b.state(), e.expect[s].state) << barrier_state_name(b.state());
            EXPECT_EQ(b.reconciled_generation(), e.expect[s].reconciled_generation);
        }
    }
}

TEST(ReconnectBarrierTransitions, ASweepRequestIsDueExactlyWhenSweepingAndNothingIsOutstanding) {
    EXPECT_FALSE(make(kDown).take_sweep_request(kNow).has_value());
    EXPECT_FALSE(make(kSweepingAsked).take_sweep_request(kNow).has_value());
    EXPECT_TRUE(make(kSweepingIdle).take_sweep_request(kNow).has_value());
    EXPECT_FALSE(make(kReconciled).take_sweep_request(kNow).has_value());
}

// --- a random walk, invariants checked after every step ------------------------------------------------------------------

namespace {

struct Rng {
    std::uint64_t s{0x9E3779B97F4A7C15ULL};
    std::uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1DULL;
    }
    std::uint64_t below(std::uint64_t n) { return next() % n; }
};

}  // namespace

TEST(ReconnectBarrierProperties, ARandomWalkNeverBreaksTheInvariants) {
    Rng rng;
    ReconnectBarrier b;
    std::int64_t now = kNow;
    std::uint64_t drops = 0;
    std::uint64_t lost = 0;
    std::uint64_t supervisor_generation = 1;  // what a supervisor would say: it only moves forward, now and then
    std::uint64_t last_epoch = b.epoch();
    for (int step = 0; step < 300'000; ++step) {
        const BarrierState before = b.state();
        const std::uint64_t epoch_before = b.epoch();
        const bool outstanding_before = b.sweep_outstanding();
        const std::uint64_t gen_before = b.generation();
        enum class Op { Observe, Report, TakeSweep, Ended, Admit, TakeStop } op = Op::Observe;
        std::uint64_t pick = rng.below(100);
        bool report_accepted_by_model = false;
        bool observed_not_connected = false;
        bool observed_stale = false;
        bool ended_applies = false;

        now += static_cast<std::int64_t>(rng.below(3'000));
        if (rng.below(100) == 0) now += 35'000;  // now and then a long pause: a request nobody answered in time is asked again
        if (pick < 40) {
            op = Op::Observe;
            FeedObservation o{};
            if (rng.below(60) == 0) ++supervisor_generation;
            const std::uint64_t which_generation = rng.below(40);
            // mostly the supervisor's generation; sometimes "none yet" (0) or a stale reading of the previous one
            o.generation = which_generation == 0 ? 0 : (which_generation == 1 && supervisor_generation > 1 ? supervisor_generation - 1 : supervisor_generation);
            o.connected = rng.below(20) != 0;
            const std::uint64_t kind = rng.below(40);
            o.last_activity_ms = kind == 0 ? 0 : (kind == 1 ? now - 200'000 : now - static_cast<std::int64_t>(rng.below(1'000)));
            if (rng.below(40) == 0) drops += 1 + rng.below(2);
            if (rng.below(60) == 0) lost += 1 + rng.below(2);
            if (rng.below(100) == 0) drops = 0;  // a restarted counter must be survivable too
            o.ring_drops = drops;
            o.lost_events = lost;
            o.sampled_at_ms = now;
            observed_not_connected = !o.connected || o.generation == 0;
            observed_stale = !observed_not_connected && (o.last_activity_ms <= 0 || now - o.last_activity_ms > b.policy().stale_after_ms);
            b.observe(o);
        } else if (pick < 65) {
            op = Op::Report;
            SweepReport r{};
            const std::uint64_t which = rng.below(10);  // mostly the epoch that is current; sometimes one that is not
            r.epoch = which < 7 ? epoch_before : (which == 6 ? epoch_before + 1 : (which == 7 ? (epoch_before > 0 ? epoch_before - 1 : 0) : 0));
            r.unresolved = rng.below(3) == 0 ? 1U : 0U;
            r.account_fresh = rng.below(5) != 0;
            report_accepted_by_model = before == BarrierState::Sweeping && outstanding_before && r.epoch == epoch_before;
            b.on_sweep_report(r, now);
        } else if (pick < 85) {
            op = Op::TakeSweep;
            (void)b.take_sweep_request(now);
        } else if (pick < 88) {
            op = Op::Ended;
            const std::uint64_t which_end = rng.below(10);
            // mostly the generation that is running (a serverShutdown drained from the ring); sometimes a late end of an older one, or an unstamped one
            const std::uint64_t g = which_end < 6 ? supervisor_generation
                                    : (which_end < 8 ? (supervisor_generation > 1 ? supervisor_generation - 1 : 0) : 0);
            ended_applies = g != 0 && g >= gen_before;
            b.on_generation_ended(g);
        } else if (pick < 96) {
            op = Op::Admit;
            const std::uint64_t g = rng.below(supervisor_generation + 2);
            const bool admitted = b.admit_event(g);
            if (!admitted) {
                ASSERT_EQ(b.state(), BarrierState::Reconciled);
                ASSERT_GT(g, 0U);
                ASSERT_LT(g, b.reconciled_generation());
            }
        } else {
            op = Op::TakeStop;
            (void)b.take_stop_request();
        }

        // --- the invariants ---
        ASSERT_EQ(b.state() == BarrierState::Reconciled, b.reconciled_generation() != 0) << "step " << step;
        if (b.state() == BarrierState::Reconciled) {
            ASSERT_EQ(b.reconciled_generation(), b.generation());
            ASSERT_NE(b.generation(), 0U);
        }
        if (b.sweep_outstanding()) {
            ASSERT_EQ(b.state(), BarrierState::Sweeping);
        }
        ASSERT_GE(b.epoch(), last_epoch) << "the epoch only moves forward";
        last_epoch = b.epoch();
        // Reconciled is entered by exactly one thing: an accepted clean report.
        if (before != BarrierState::Reconciled && b.state() == BarrierState::Reconciled) {
            ASSERT_TRUE(op == Op::Report && report_accepted_by_model) << "step " << step;
        }
        if (op == Op::Observe && (observed_not_connected || observed_stale)) {
            ASSERT_EQ(b.state(), BarrierState::Down) << "step " << step;
        }
        if (op == Op::Ended && ended_applies) {
            ASSERT_EQ(b.state(), BarrierState::Down) << "step " << step;
        }
        if (op == Op::TakeSweep || op == Op::Admit || op == Op::TakeStop) {
            ASSERT_EQ(b.state(), before) << "step " << step;
        }
        if (op == Op::Report && !report_accepted_by_model) {
            ASSERT_EQ(b.state(), before) << "step " << step;
        }
    }
    // The walk must actually have been somewhere: every kind of event happened, many times (a walk stuck in Down would pass the invariants above).
    const BarrierStats& st = b.stats();
    // Measured on this seed (it is deterministic): generations 10351, downs 3279, sweeps begun 3929, requested 3268, completed 1130, unresolved 969,
    // timeouts 49, ignored reports 72862, soft barriers 650, stale heartbeats 842, generation ends 5320, late events dropped 941. The floors are
    // about half of that: far from a walk that never leaves Down, close enough to notice one that stops reaching a kind of event.
    EXPECT_GT(st.generations_seen, 5000U);
    EXPECT_GT(st.downs, 1500U);
    EXPECT_GT(st.sweeps_begun, 2000U);
    EXPECT_GT(st.sweeps_requested, 1500U);
    EXPECT_GT(st.sweeps_completed, 500U);
    EXPECT_GT(st.sweeps_unresolved, 500U);
    EXPECT_GT(st.sweep_timeouts, 20U);
    EXPECT_GT(st.reports_ignored, 30000U);
    EXPECT_GT(st.soft_barriers, 300U);
    EXPECT_GT(st.stale_heartbeats, 400U);
    EXPECT_GT(st.generation_ends_seen, 2500U);
    EXPECT_GT(st.late_events_dropped, 400U);
}

// --- the same machine, at compile time ----------------------------------------------------------------------------------

namespace {

constexpr bool compile_time_rotation() {
    ReconnectBarrier b;
    FeedObservation o{};
    o.generation = 1;
    o.connected = true;
    o.last_activity_ms = 100;
    o.sampled_at_ms = 100;
    b.observe(o);
    const std::optional<std::uint64_t> epoch = b.take_sweep_request(100);
    if (!epoch) return false;
    b.on_sweep_report(SweepReport{*epoch, 0U, true}, 100);
    if (b.state() != BarrierState::Reconciled || b.reconciled_generation() != 1U) return false;
    if (b.admit_event(1) != true) return false;
    o.generation = 2;
    b.observe(o);  // a new generation: Sweeping, not Reconciled
    if (b.state() != BarrierState::Sweeping) return false;
    b.on_generation_ended(2);
    return b.state() == BarrierState::Down;
}

static_assert(compile_time_rotation(), "the barrier's rotation, evaluated by the compiler");
static_assert(ReconnectBarrier{}.state() == BarrierState::Down, "a barrier starts Down");

}  // namespace
