// Private-stream migration M3b: the reconcile thread's half of the barrier sweep -- OrderTracker's per-epoch bookkeeping, poll_once() with a SweepJob,
// and drain_reconcile_events() delivering the SweepDone marker (order_tracker.hpp, barrier_sweep.hpp). Single-threaded and deterministic; the same
// code under two real threads is test_sweep_concurrency.cpp.
//
// The contract under test, in the order the tests below take it:
//   * a sweep for epoch E asks EVERY tracked order that poll_once() can ask about, at once -- inside its own backoff, and even when its last query
//     was a moment ago -- and an order counts as answered only by a DEFINITE answer from after the sweep began: state identified (changed or not),
//     and a changed state handed to the hot thread; inconclusive, never-sent, invalid, illegal and refused-by-a-full-ring are not answers;
//   * the SweepDone marker is pushed when no tracked order is waiting, AFTER every resolution of the sweep (one FIFO ring), and a full ring delays it;
//   * an order escalated to the operator is unknown for good: the marker's `unresolved` counts it, in this sweep and in every later one;
//   * a newer epoch restarts the sweep (answers belong to one epoch), a repeated or older one changes nothing, and an idle job does nothing;
//   * drain_reconcile_events() hands the marker to the sink only after every resolution in front of it was durably acknowledged and published, and the
//     marker itself touches nothing: no durable frame, no audit record, no PositionTruth, no slot.
#include <gtest/gtest.h>

#include <hengyuan/order_tracker.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace hy;

namespace {

OrderRecord make_record(const char* coid, OrderState target, std::int64_t qty = 100) {
    OrderRecord rec{};
    std::snprintf(rec.client_order_id.id, sizeof(rec.client_order_id.id), "%s", coid);
    rec.symbol_id = 1;
    rec.intended_price_ticks = 5000;
    rec.intended_qty_ticks = qty;
    rec.exchange_order_id = 555;
    rec.state = OrderState::Submitting;
    switch (target) {
        case OrderState::Ambiguous:
            rec.transition_to(OrderState::Ambiguous);
            break;
        case OrderState::Accepted:
            rec.transition_to(OrderState::Accepted);
            break;
        case OrderState::PartialFill:
            rec.transition_to(OrderState::Accepted);
            rec.transition_to(OrderState::PartialFill);
            break;
        case OrderState::EscalatedToOperator:
            rec.transition_to(OrderState::Ambiguous);
            rec.transition_to(OrderState::EscalatedToOperator);
            break;
        default:
            break;
    }
    return rec;
}

// The scripted exchange: what a query for a client order id returns. Absent = inconclusive.
struct Exchange {
    std::map<std::string, QueryResult> answers;
    std::vector<std::string> asked;

    [[nodiscard]] std::size_t times_asked(const char* coid) const {
        return static_cast<std::size_t>(std::count(asked.begin(), asked.end(), std::string(coid)));
    }
};

QueryResult ask(const OrderExpectation& expected, void* user_data) {
    auto* x = static_cast<Exchange*>(user_data);
    const std::string coid(expected.client_order_id.view());
    x->asked.push_back(coid);
    const auto it = x->answers.find(coid);
    return it == x->answers.end() ? QueryResult{} : it->second;
}

QueryResult found(OrderState s, std::int64_t filled = 0, std::int64_t avg = 0) {
    return QueryResult{QueryOutcome::Found, s, 555, filled, avg};
}
QueryResult filled_all() { return found(OrderState::Filled, 100, 5000); }

std::vector<ReconcileEvent> pop_all(ReconcileEventRing& ring) {
    std::vector<ReconcileEvent> out;
    ReconcileEvent ev{};
    while (ring.try_pop(ev)) out.push_back(ev);
    return out;
}

bool is_marker(const ReconcileEvent& e) { return e.kind == ReconcileEventKind::SweepDone; }

std::size_t marker_count(const std::vector<ReconcileEvent>& events) {
    return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), is_marker));
}

class SweepTrackerTest : public ::testing::Test {
protected:
    OrderTracker tracker_;
    ToReconcileRing inbound_;
    ReconcileEventRing outbound_;
    SweepRequestRing requests_;
    SweepJob job_{requests_};
    Exchange exchange_;
    QueryPort port_{&ask, &exchange_};
    ReconcilePollPolicy policy_{};
    std::size_t next_slot_{0};

    void track(const OrderRecord& rec, std::int64_t now_ms = 0) {
        ASSERT_TRUE(tracker_.track(InFlightHandle{next_slot_++, 1}, rec, now_ms));
    }
    void post(std::uint64_t epoch) { ASSERT_TRUE(requests_.try_push(SweepRequest{epoch})); }
    void tick(std::int64_t now_ms) { poll_once(tracker_, inbound_, outbound_, port_, policy_, now_ms, OrderRecord::kClockUnset, &job_); }
    void tick_without_sweep(std::int64_t now_ms) { poll_once(tracker_, inbound_, outbound_, port_, policy_, now_ms); }
};

}  // namespace

// --- OrderTracker's per-epoch bookkeeping --------------------------------------------------------------------------------------

TEST(OrderTrackerSweepBookkeeping, AnAnswerBelongsToOneEpochAndAFreshSlotStartsUnanswered) {
    OrderTracker tracker;
    ASSERT_TRUE(tracker.track(InFlightHandle{0, 1}, make_record("HY-A", OrderState::Accepted), 0));
    EXPECT_EQ(tracker.answered_epoch("HY-A"), 0U);
    EXPECT_EQ(tracker.count_unanswered(5), 1U);
    tracker.note_answered("HY-A", 5);
    EXPECT_EQ(tracker.answered_epoch("HY-A"), 5U);
    EXPECT_EQ(tracker.count_unanswered(5), 0U);
    EXPECT_EQ(tracker.count_unanswered(6), 1U) << "a newer epoch starts with every order unanswered again";

    tracker.untrack("HY-A");
    ASSERT_TRUE(tracker.track(InFlightHandle{0, 2}, make_record("HY-B", OrderState::Accepted), 0));
    EXPECT_EQ(tracker.answered_epoch("HY-B"), 0U) << "the reused slot must not inherit the previous order's answer";
    EXPECT_EQ(tracker.answered_epoch("HY-GONE"), 0U);
    tracker.note_answered("HY-GONE", 5);  // not tracked: ignored, no crash
}

TEST(OrderTrackerSweepBookkeeping, OnlyOrdersThatCanBePolledAreWaitedFor) {
    OrderTracker tracker;
    ASSERT_TRUE(tracker.track(InFlightHandle{0, 1}, make_record("HY-AMB", OrderState::Ambiguous), 0));
    ASSERT_TRUE(tracker.track(InFlightHandle{1, 1}, make_record("HY-LIVE", OrderState::Accepted), 0));
    ASSERT_TRUE(tracker.track(InFlightHandle{2, 1}, make_record("HY-PART", OrderState::PartialFill), 0));
    ASSERT_TRUE(tracker.track(InFlightHandle{3, 1}, make_record("HY-ESC", OrderState::EscalatedToOperator), 0));
    EXPECT_EQ(tracker.count_unanswered(1), 3U) << "the escalated one is never asked about, so the sweep does not wait for it";
    EXPECT_EQ(tracker.count_unknown_fate(), 1U) << "...but its fate is unknown, and that is reported";
}

TEST(OrderTrackerSweepBookkeeping, AnEscalationIsRememberedAfterTheOrderLeftTheTable) {
    OrderTracker tracker;
    EXPECT_EQ(tracker.count_unknown_fate(), 0U);
    tracker.note_escalated();
    tracker.note_escalated();
    EXPECT_EQ(tracker.count_unknown_fate(), 2U);
    ASSERT_TRUE(tracker.track(InFlightHandle{0, 1}, make_record("HY-ESC", OrderState::EscalatedToOperator), 0));
    EXPECT_EQ(tracker.count_unknown_fate(), 3U);
    tracker.untrack("HY-ESC");
    EXPECT_EQ(tracker.count_unknown_fate(), 2U) << "an order that was tracked and then untracked leaves only the escalation counter";
}

// --- poll_once with a sweep: when and whom it asks ------------------------------------------------------------------------------

TEST_F(SweepTrackerTest, WithoutASweepJobARequestOnTheRingIsLeftAlone) {
    post(1);
    track(make_record("HY-A", OrderState::Accepted));
    exchange_.answers["HY-A"] = found(OrderState::Accepted);
    tick_without_sweep(1000);
    SweepRequest r{};
    EXPECT_TRUE(requests_.try_pop(r)) << "poll_once() without a job does not consume the ring";
    EXPECT_EQ(r.epoch, 1U);
    EXPECT_EQ(marker_count(pop_all(outbound_)), 0U);
}

TEST_F(SweepTrackerTest, AnEmptyTrackerAnswersAtOnce) {
    post(1);
    tick(1000);
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(is_marker(events[0]));
    EXPECT_EQ(events[0].sweep_epoch, 1U);
    EXPECT_EQ(events[0].sweep_unresolved, 0U);
    EXPECT_FALSE(job_.running());
    EXPECT_EQ(job_.stats().sweeps_completed, 1U);
}

TEST_F(SweepTrackerTest, EveryTrackedOrderIsAskedAtOnceEvenInsideItsOwnBackoff) {
    track(make_record("HY-AMB", OrderState::Ambiguous));
    track(make_record("HY-LIVE", OrderState::Accepted));
    tick_without_sweep(1000);  // both asked (never polled), both inconclusive: last query at 1000
    ASSERT_EQ(exchange_.asked.size(), 2U);
    tick_without_sweep(1100);
    EXPECT_EQ(exchange_.asked.size(), 2U) << "inside the backoff (200 ms) and the live cadence (2 s) nothing is asked: the ordinary schedule";

    exchange_.answers["HY-AMB"] = filled_all();
    exchange_.answers["HY-LIVE"] = found(OrderState::Accepted);
    post(1);
    tick(1100);
    EXPECT_EQ(exchange_.times_asked("HY-AMB"), 2U);
    EXPECT_EQ(exchange_.times_asked("HY-LIVE"), 2U) << "the sweep does not wait for a schedule: both are asked again now";

    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].kind, ReconcileEventKind::Resolution);
    EXPECT_EQ(events[0].resulting_state, OrderState::Filled);
    ASSERT_TRUE(is_marker(events[1]));
    EXPECT_EQ(events[1].sweep_epoch, 1U);
    EXPECT_EQ(events[1].sweep_unresolved, 0U);
    EXPECT_EQ(tracker_.answered_epoch("HY-LIVE"), 1U);
}

TEST_F(SweepTrackerTest, TheMarkerIsBehindEveryResolutionTheSweepProduced) {
    for (const char* coid : {"HY-1", "HY-2", "HY-3"}) {
        track(make_record(coid, OrderState::Ambiguous));
        exchange_.answers[coid] = filled_all();
    }
    post(4);
    tick(1000);
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 4U);
    for (std::size_t i = 0; i < 3; ++i) EXPECT_EQ(events[i].kind, ReconcileEventKind::Resolution) << i;
    EXPECT_TRUE(is_marker(events[3])) << "FIFO: when the hot thread pops the marker, the three resolutions are already behind it";
    EXPECT_EQ(tracker_.count(), 0U) << "exchange-final orders left the table";
}

TEST_F(SweepTrackerTest, AnOrderIngressedInTheSameTickIsSweptToo) {
    exchange_.answers["HY-NEW"] = filled_all();
    ASSERT_TRUE(inbound_.try_push(ReconcileIngress{InFlightHandle{0, 1}, make_record("HY-NEW", OrderState::Ambiguous)}));
    post(1);
    tick(1000);
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].resulting_state, OrderState::Filled);
    EXPECT_TRUE(is_marker(events[1]));
}

TEST_F(SweepTrackerTest, ASameStateAnswerIsDefinite_AndTheNextEpochAsksAgain) {
    track(make_record("HY-LIVE", OrderState::Accepted));
    exchange_.answers["HY-LIVE"] = found(OrderState::Accepted);
    post(1);
    tick(1000);
    std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 1U) << "no resolution (the state did not change), just the marker";
    EXPECT_TRUE(is_marker(events[0]));
    EXPECT_EQ(events[0].sweep_unresolved, 0U);
    EXPECT_EQ(tracker_.count(), 1U);

    post(2);
    tick(1001);  // inside the live cadence, but the answer belongs to epoch 1
    EXPECT_EQ(exchange_.times_asked("HY-LIVE"), 2U);
    events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].sweep_epoch, 2U);
}

TEST_F(SweepTrackerTest, AnAmbiguousOrderFoundStillLiveIsAnsweredAndStaysTracked) {
    track(make_record("HY-A", OrderState::Ambiguous));
    exchange_.answers["HY-A"] = found(OrderState::Accepted);
    post(1);
    tick(1000);
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].resulting_state, OrderState::Accepted);
    EXPECT_TRUE(is_marker(events[1]));
    EXPECT_EQ(tracker_.count(), 1U) << "still live: it keeps a route to a terminal state";
    EXPECT_EQ(tracker_.answered_epoch("HY-A"), 1U);
}

TEST_F(SweepTrackerTest, InconclusiveAndNeverSentAreNotAnswers) {
    track(make_record("HY-INC", OrderState::Accepted));
    track(make_record("HY-NS", OrderState::Accepted));
    exchange_.answers["HY-NS"] = QueryResult::not_sent();
    post(1);
    tick(1000);
    EXPECT_EQ(marker_count(pop_all(outbound_)), 0U);
    EXPECT_TRUE(job_.running());
    EXPECT_EQ(tracker_.count_unanswered(1), 2U);

    exchange_.answers["HY-INC"] = found(OrderState::Accepted);
    exchange_.answers["HY-NS"] = found(OrderState::Accepted);
    tick(1600);  // past sweep_retry_interval_ms (500)
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(is_marker(events[0]));
    EXPECT_FALSE(job_.running());
}

TEST_F(SweepTrackerTest, AnInvalidOrIllegalAnswerIsNotAnAnswer) {
    track(make_record("HY-OVER", OrderState::Accepted));    // reports more filled than ordered
    track(make_record("HY-BACK", OrderState::Accepted));    // a live order cannot become Ambiguous
    exchange_.answers["HY-OVER"] = found(OrderState::PartialFill, 500, 5000);
    exchange_.answers["HY-BACK"] = found(OrderState::Ambiguous);
    post(1);
    tick(1000);
    EXPECT_EQ(marker_count(pop_all(outbound_)), 0U);
    EXPECT_EQ(tracker_.count_unanswered(1), 2U);
}

TEST_F(SweepTrackerTest, AnOrderIsAskedAgainAfterAtMostTheSweepRetryIntervalAndNeverSlowerThanItsOwnCadence) {
    track(make_record("HY-AMB", OrderState::Ambiguous));
    track(make_record("HY-LIVE", OrderState::Accepted));
    post(1);
    tick(1000);  // both asked (never polled), both inconclusive
    ASSERT_EQ(exchange_.asked.size(), 2U);
    tick(1100);
    EXPECT_EQ(exchange_.asked.size(), 2U) << "100 ms: inside both waits";
    tick(1200);
    EXPECT_EQ(exchange_.times_asked("HY-AMB"), 2U) << "the ambiguous order's own backoff is 200 ms, shorter than the sweep's 500";
    EXPECT_EQ(exchange_.times_asked("HY-LIVE"), 1U);
    tick(1500);
    EXPECT_EQ(exchange_.times_asked("HY-LIVE"), 2U) << "the live cadence is 2 s, but a sweep waits at most 500 ms";
}

TEST_F(SweepTrackerTest, AnOrderTheSweepAlreadyAnsweredGoesBackToItsOwnCadenceWhileAnotherKeepsTheSweepRunning) {
    track(make_record("HY-DONE", OrderState::Accepted));
    track(make_record("HY-WAIT", OrderState::Accepted));
    exchange_.answers["HY-DONE"] = found(OrderState::Accepted);  // HY-WAIT stays inconclusive
    post(1);
    tick(1000);  // both asked; HY-DONE answered, HY-WAIT not
    ASSERT_EQ(tracker_.answered_epoch("HY-DONE"), 1U);
    ASSERT_TRUE(job_.running());

    tick(1600);  // past the sweep's 500 ms retry, inside the live orders' own 2 s cadence
    EXPECT_EQ(exchange_.times_asked("HY-WAIT"), 2U) << "the order with no answer yet is asked again at the sweep's pace";
    EXPECT_EQ(exchange_.times_asked("HY-DONE"), 1U) << "the answered one is back on its own cadence: a sweep must not make a quiet order chatty";

    tick(3100);  // 2.1 s after its last query: its own cadence is due again
    EXPECT_EQ(exchange_.times_asked("HY-DONE"), 2U);
}

TEST_F(SweepTrackerTest, TheTickBudgetStillAppliesSoALargeSweepTakesSeveralTicks) {
    policy_.max_queries_per_tick = 4;
    for (int i = 0; i < 10; ++i) {
        char coid[16];
        std::snprintf(coid, sizeof(coid), "HY-%02d", i);
        track(make_record(coid, OrderState::Ambiguous));
        exchange_.answers[coid] = filled_all();
    }
    post(1);
    tick(1000);
    EXPECT_EQ(exchange_.asked.size(), 4U);
    EXPECT_EQ(marker_count(pop_all(outbound_)), 0U) << "six orders have not been asked yet";
    tick(1001);
    EXPECT_EQ(exchange_.asked.size(), 8U);
    EXPECT_EQ(marker_count(pop_all(outbound_)), 0U);
    tick(1002);
    EXPECT_EQ(exchange_.asked.size(), 10U);
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    EXPECT_EQ(marker_count(events), 1U);
    EXPECT_TRUE(is_marker(events.back())) << "the marker comes last";
    EXPECT_EQ(tracker_.count(), 0U);
}

// --- poll_once with a sweep: a ring that is full ---------------------------------------------------------------------------------

TEST_F(SweepTrackerTest, AResolutionTheFullRingRefusedIsNotAnAnswerAndHoldsTheMarkerBack) {
    for (std::size_t i = 0; i < ReconcileEventRing::capacity(); ++i) ASSERT_TRUE(outbound_.try_push(ReconcileEvent{}));
    track(make_record("HY-A", OrderState::Ambiguous));
    exchange_.answers["HY-A"] = filled_all();
    post(1);
    tick(1000);
    EXPECT_EQ(tracker_.count(), 1U) << "the resolution could not be pushed: the order keeps its prior state";
    EXPECT_EQ(tracker_.answered_epoch("HY-A"), 0U);
    EXPECT_TRUE(job_.running());

    (void)pop_all(outbound_);  // the hot thread caught up
    tick(1200);                // past the order's own 200 ms backoff
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].resulting_state, OrderState::Filled);
    EXPECT_TRUE(is_marker(events[1]));
    EXPECT_FALSE(job_.running());
}

TEST_F(SweepTrackerTest, AMarkerTheFullRingRefusedIsRetriedNextTickAndNeverLost) {
    track(make_record("HY-LIVE", OrderState::Accepted));
    exchange_.answers["HY-LIVE"] = found(OrderState::Accepted);
    for (std::size_t i = 0; i < ReconcileEventRing::capacity(); ++i) ASSERT_TRUE(outbound_.try_push(ReconcileEvent{}));
    post(1);
    tick(1000);
    EXPECT_TRUE(job_.running()) << "every order is answered, but the marker could not be pushed";
    EXPECT_EQ(job_.stats().marker_push_failures, 1U);
    EXPECT_EQ(job_.stats().sweeps_completed, 0U);

    ReconcileEvent dummy{};
    ASSERT_TRUE(outbound_.try_pop(dummy));  // one slot
    tick(1001);
    EXPECT_FALSE(job_.running());
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    EXPECT_TRUE(is_marker(events.back()));
    EXPECT_EQ(marker_count(events), 1U);
    EXPECT_EQ(exchange_.times_asked("HY-LIVE"), 1U) << "the answered order is not asked again just because the marker was late";
}

// --- poll_once with a sweep: orders the loop will never ask about ---------------------------------------------------------------

TEST_F(SweepTrackerTest, AnEscalationDuringTheSweepIsCountedAndNeverForgottenByLaterSweeps) {
    track(make_record("HY-STUCK", OrderState::Ambiguous), /*now_ms=*/0);
    post(1);
    tick(policy_.quarantine.hard_cap_ms + 1000);  // long past the absolute cap: the order goes to the operator
    std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].resulting_state, OrderState::EscalatedToOperator);
    ASSERT_TRUE(is_marker(events[1]));
    EXPECT_EQ(events[1].sweep_unresolved, 1U) << "an order whose fate nobody knows is not a clean sweep";
    EXPECT_EQ(tracker_.count(), 0U);

    post(2);
    tick(policy_.quarantine.hard_cap_ms + 2000);
    events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].sweep_epoch, 2U);
    EXPECT_EQ(events[0].sweep_unresolved, 1U) << "the next sweep finds an empty table and still reports it";
}

TEST_F(SweepTrackerTest, ATrackedOrderThePollLoopSkipsIsReportedAsUnresolvedNotWaitedFor) {
    track(make_record("HY-ESC", OrderState::EscalatedToOperator));
    post(1);
    tick(1000);
    EXPECT_TRUE(exchange_.asked.empty());
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].sweep_unresolved, 1U);
}

// --- poll_once with a sweep: requests ------------------------------------------------------------------------------------------

TEST_F(SweepTrackerTest, AnOrderThatArrivesMidSweepHoldsTheMarkerBackUntilItIsAnswered) {
    track(make_record("HY-A", OrderState::Accepted));
    post(1);
    tick(1000);  // HY-A inconclusive
    EXPECT_EQ(marker_count(pop_all(outbound_)), 0U);

    exchange_.answers["HY-B"] = filled_all();
    ASSERT_TRUE(inbound_.try_push(ReconcileIngress{InFlightHandle{7, 1}, make_record("HY-B", OrderState::Ambiguous)}));
    tick(1001);
    std::vector<ReconcileEvent> events = pop_all(outbound_);
    EXPECT_EQ(events.size(), 1U) << "HY-B resolved; HY-A is still unanswered";
    EXPECT_EQ(marker_count(events), 0U);

    exchange_.answers["HY-A"] = found(OrderState::Accepted);
    tick(1500);
    events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(is_marker(events[0]));
}

TEST_F(SweepTrackerTest, ANewerEpochRestartsTheSweepAndAnOlderEpochIsNeverReported) {
    track(make_record("HY-A", OrderState::Accepted));
    track(make_record("HY-B", OrderState::Accepted));
    exchange_.answers["HY-A"] = found(OrderState::Accepted);  // HY-B stays inconclusive
    post(1);
    tick(1000);
    EXPECT_EQ(tracker_.answered_epoch("HY-A"), 1U);
    EXPECT_EQ(marker_count(pop_all(outbound_)), 0U);

    post(2);
    tick(1100);
    EXPECT_EQ(job_.epoch(), 2U);
    EXPECT_EQ(exchange_.times_asked("HY-A"), 2U) << "an answer from before epoch 2 began does not count for it";
    EXPECT_EQ(tracker_.answered_epoch("HY-A"), 2U);

    exchange_.answers["HY-B"] = found(OrderState::Accepted);
    tick(1700);
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(is_marker(events[0]));
    EXPECT_EQ(events[0].sweep_epoch, 2U) << "epoch 1 was superseded before it completed: it is never reported";
}

TEST_F(SweepTrackerTest, ARepeatedOrOlderRequestAfterTheMarkerChangesNothing) {
    post(2);
    tick(1000);
    ASSERT_EQ(marker_count(pop_all(outbound_)), 1U);
    post(2);
    post(1);
    post(0);
    tick(1001);
    tick(1002);
    EXPECT_EQ(marker_count(pop_all(outbound_)), 0U);
    EXPECT_FALSE(job_.running());
    EXPECT_EQ(job_.stats().requests_stale, 3U);
}

TEST_F(SweepTrackerTest, AnIdleJobAddsNothingToOrdinaryPolling) {
    track(make_record("HY-A", OrderState::Ambiguous));
    exchange_.answers["HY-A"] = filled_all();
    tick(1000);
    const std::vector<ReconcileEvent> events = pop_all(outbound_);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, ReconcileEventKind::Resolution);
    EXPECT_EQ(job_.stats().requests_taken, 0U);
}

// --- drain_reconcile_events: the marker's side ----------------------------------------------------------------------------------

namespace {

struct DrainProbe {
    std::vector<int> fail_calls;
    int calls{0};
    std::vector<AuditRecord> frames;
};

AuditAppendResult drain_append(void* user_data, const AuditRecord& rec, std::int64_t) noexcept {
    auto* p = static_cast<DrainProbe*>(user_data);
    ++p->calls;
    AuditAppendResult r{};  // Failed by default
    if (std::find(p->fail_calls.begin(), p->fail_calls.end(), p->calls) != p->fail_calls.end()) return r;
    p->frames.push_back(rec);
    r.status = AuditAppendResult::Status::Acked;
    r.sequence = static_cast<std::uint64_t>(p->frames.size());
    return r;
}

// What memory looked like at the moment the sink was called.
struct SinkSeen {
    const InFlightRegistry* in_flight{nullptr};
    const AuditRingSink* audit{nullptr};
    const PositionTruth* truth{nullptr};
    const DrainProbe* durable{nullptr};
    struct Call {
        SweepDone done;
        bool slot_held;
        std::size_t audit_count;
        std::int64_t net_qty;
        std::size_t durable_frames;
    };
    std::vector<Call> calls;
};

void sink_fn(void* user_data, const SweepDone& done) noexcept {
    auto* s = static_cast<SinkSeen*>(user_data);
    s->calls.push_back(SinkSeen::Call{done, s->in_flight->is_in_flight("HY-A"), s->audit->count(), s->truth->net_qty_ticks(3),
                                      s->durable->frames.size()});
}

class SweepDrainTest : public ::testing::Test {
protected:
    InFlightRegistry in_flight_;
    AuditRingSink audit_;
    ReconcileEventRing events_;
    PositionTruth truth_;
    OrderFillContext fill_context_;
    DrainProbe durable_;
    ReconcileDurablePublisher publisher_;
    SinkSeen seen_;
    SweepSink sink_{&sink_fn, &seen_};

    void SetUp() override {
        seen_.in_flight = &in_flight_;
        seen_.audit = &audit_;
        seen_.truth = &truth_;
        seen_.durable = &durable_;
        EXPECT_TRUE(publisher_.set_port(DurableOrderAuditPort{&drain_append, nullptr, &durable_}));
    }

    InFlightHandle admit(const char* coid) {
        const InFlightHandle h = in_flight_.register_submit_handle(coid);
        EXPECT_TRUE(h.valid());
        EXPECT_TRUE(fill_context_.track(coid, 3, OrderSide::Buy, 0, 0, 0));
        return h;
    }

    void push_resolution(const char* coid, InFlightHandle h) {
        ReconcileEvent ev{};
        ev.handle = h;
        std::snprintf(ev.coid.id, sizeof(ev.coid.id), "%s", coid);
        ev.resulting_state = OrderState::Filled;
        ev.prior_state = OrderState::Accepted;
        ev.symbol_id = 3;
        ev.side = OrderSide::Buy;
        ev.exchange_order_id = 555;
        ev.filled_qty_ticks = 100;
        ev.fill_delta_qty_ticks = 100;
        ev.avg_fill_price_ticks = 5000;
        ASSERT_TRUE(events_.try_push(ev));
    }

    void push_marker(std::uint64_t epoch, std::uint32_t unresolved) {
        ReconcileEvent m{};
        m.kind = ReconcileEventKind::SweepDone;
        m.sweep_epoch = epoch;
        m.sweep_unresolved = unresolved;
        ASSERT_TRUE(events_.try_push(m));
    }

    void drain(const SweepSink* sink, ReconcileDurablePublisher* durable = nullptr) {
        drain_reconcile_events(in_flight_, &audit_, events_, 2000, &truth_, &fill_context_, durable, sink);
    }
};

}  // namespace

TEST_F(SweepDrainTest, TheMarkerReachesTheSinkOnlyAfterTheResolutionBehindItWasMadeDurableAndPublished) {
    const InFlightHandle h = admit("HY-A");
    push_resolution("HY-A", h);
    push_marker(7, 2);

    drain(&sink_, &publisher_);

    ASSERT_EQ(seen_.calls.size(), 1U);
    const SinkSeen::Call& c = seen_.calls[0];
    EXPECT_EQ(c.done.epoch, 7U);
    EXPECT_EQ(c.done.unresolved, 2U);
    EXPECT_FALSE(c.slot_held) << "the slot of the resolved order was released before the barrier hears the sweep is done";
    EXPECT_EQ(c.audit_count, 1U);
    EXPECT_EQ(c.net_qty, 100) << "PositionTruth already holds the fill the sweep found";
    EXPECT_EQ(c.durable_frames, 1U) << "...and it is durable";
}

TEST_F(SweepDrainTest, TheMarkerItselfMakesNothingDurableAndTouchesNothing) {
    push_marker(1, 0);
    drain(&sink_, &publisher_);
    EXPECT_EQ(seen_.calls.size(), 1U);
    EXPECT_EQ(durable_.calls, 0) << "no durable append for a control entry";
    EXPECT_EQ(audit_.count(), 0U);
    EXPECT_EQ(publisher_.acked_frames(), 0U);
    EXPECT_FALSE(publisher_.blocked());
    EXPECT_EQ(truth_.net_qty_ticks(3), 0);
}

TEST_F(SweepDrainTest, AMarkerWaitsBehindAResolutionTheSinkHasNotAcknowledged) {
    const InFlightHandle h = admit("HY-A");
    push_resolution("HY-A", h);
    push_marker(9, 0);
    durable_.fail_calls.push_back(1);  // not `= {1}`: GCC 14 -O2 + ASan reports a false -Warray-bounds on the one-element initializer-list assignment

    drain(&sink_, &publisher_);
    EXPECT_TRUE(publisher_.blocked());
    EXPECT_TRUE(seen_.calls.empty()) << "head-of-line blocking: the marker is not popped past a resolution that is not durable";
    EXPECT_TRUE(in_flight_.is_in_flight("HY-A"));

    drain(&sink_, &publisher_);  // the sink answers now
    ASSERT_EQ(seen_.calls.size(), 1U);
    EXPECT_FALSE(seen_.calls[0].slot_held);
    EXPECT_EQ(seen_.calls[0].done.epoch, 9U);
}

TEST_F(SweepDrainTest, WithoutASinkTheMarkerIsConsumedAndDropped) {
    push_marker(3, 0);
    drain(nullptr, &publisher_);
    ReconcileEvent left{};
    EXPECT_FALSE(events_.try_pop(left));
    EXPECT_EQ(audit_.count(), 0U);
    EXPECT_EQ(durable_.calls, 0);
}

TEST_F(SweepDrainTest, TheMemoryOnlyDrainPassesTheMarkerToo) {
    const InFlightHandle h = admit("HY-A");
    push_resolution("HY-A", h);
    push_marker(5, 0);
    drain(&sink_, /*durable=*/nullptr);
    ASSERT_EQ(seen_.calls.size(), 1U);
    EXPECT_EQ(seen_.calls[0].audit_count, 1U);
    EXPECT_FALSE(seen_.calls[0].slot_held);
    EXPECT_EQ(durable_.calls, 0);
}

TEST_F(SweepDrainTest, SeveralMarkersAreDeliveredInTheOrderTheyWerePushed) {
    push_marker(1, 0);
    push_marker(2, 4);
    push_marker(3, 0);
    drain(&sink_, &publisher_);
    ASSERT_EQ(seen_.calls.size(), 3U);
    EXPECT_EQ(seen_.calls[0].done.epoch, 1U);
    EXPECT_EQ(seen_.calls[1].done.epoch, 2U);
    EXPECT_EQ(seen_.calls[1].done.unresolved, 4U);
    EXPECT_EQ(seen_.calls[2].done.epoch, 3U);
}

TEST_F(SweepDrainTest, ARealSweepEndToEndThroughTheRingsLeavesMemoryConsistentAtTheMarker) {
    // The reconcile side produces [resolution, marker]; the hot side drains. No scripted events: the whole path.
    OrderTracker tracker;
    ToReconcileRing inbound;
    SweepRequestRing requests;
    SweepJob job{requests};
    Exchange exchange;
    exchange.answers["HY-A"] = filled_all();
    const QueryPort port{&ask, &exchange};
    const InFlightHandle h = admit("HY-A");
    OrderRecord rec = make_record("HY-A", OrderState::Ambiguous);
    rec.symbol_id = 3;
    ASSERT_TRUE(inbound.try_push(ReconcileIngress{h, rec}));
    ASSERT_TRUE(requests.try_push(SweepRequest{11}));

    poll_once(tracker, inbound, events_, port, ReconcilePollPolicy{}, 1000, OrderRecord::kClockUnset, &job);
    drain(&sink_, &publisher_);

    ASSERT_EQ(seen_.calls.size(), 1U);
    EXPECT_EQ(seen_.calls[0].done.epoch, 11U);
    EXPECT_EQ(seen_.calls[0].done.unresolved, 0U);
    EXPECT_FALSE(seen_.calls[0].slot_held);
    EXPECT_EQ(seen_.calls[0].net_qty, 100);
    EXPECT_EQ(seen_.calls[0].durable_frames, 1U);
}
