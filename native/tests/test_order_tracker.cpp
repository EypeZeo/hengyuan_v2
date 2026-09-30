// Layer for order_tracker.hpp: the reconcile/poll loop that drives Ambiguous
// orders toward a discovered state. Single-threaded here (poll_once() and
// drain_reconcile_events() are plain functions, exactly as testable as
// orchestrate_submit() itself) -- the cross-thread claim gets its own real
// two-thread TSan test (test_spsc_concurrency.cpp-adjacent, see plan).
#include <gtest/gtest.h>
#include <hengyuan/order_tracker.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>

using namespace hy;

// --- Mock QueryPort (mirrors test_live_submit_orchestrator.cpp's mock_submit pattern) ---

static QueryResult g_mock_query_result{};
static int g_mock_query_call_count = 0;

static QueryResult mock_query(const OrderExpectation& /*expected*/, void* /*ud*/) {
    ++g_mock_query_call_count;
    return g_mock_query_result;
}

namespace {

OrderRecord make_ambiguous_record(const char* coid, std::int64_t intended_qty_ticks = 100) {
    OrderRecord rec{};
    // See make_live_record() below for why snprintf rather than strncpy.
    std::snprintf(rec.client_order_id.id, sizeof(rec.client_order_id.id), "%s", coid);
    rec.symbol_id = 1;
    rec.intended_price_ticks = 5000;
    rec.intended_qty_ticks = intended_qty_ticks;
    rec.state = OrderState::Submitting;
    rec.transition_to(OrderState::Ambiguous);
    return rec;
}

// A copy of what the tracker currently holds for `coid` (nullopt if untracked) -- the R-10 tests
// read the stamped anchors and the sent-query count back through the tracker's public iterator.
std::optional<OrderRecord> tracked_record(OrderTracker& tracker, const char* coid) {
    std::optional<OrderRecord> out;
    tracker.for_each_active([&](InFlightHandle, OrderRecord& record, std::int64_t) {
        if (record.client_order_id.view() == coid) out = record;
    });
    return out;
}

class OrderTrackerTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_mock_query_result = QueryResult{};
        g_mock_query_call_count = 0;
        query_port_.fn = mock_query;
        query_port_.user_data = nullptr;
    }

    OrderTracker tracker_;
    ToReconcileRing inbound_{};
    ReconcileEventRing outbound_{};
    QueryPort query_port_{};
    ReconcilePollPolicy policy_{};
};

}  // namespace

// --- OrderTracker basic behavior ---

TEST_F(OrderTrackerTest, TrackAndUntrack) {
    auto rec = make_ambiguous_record("HY-A");
    EXPECT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));
    EXPECT_EQ(tracker_.count(), 1u);
    tracker_.untrack("HY-A");
    EXPECT_EQ(tracker_.count(), 0u);
}

TEST_F(OrderTrackerTest, DuplicateCoidRejected) {
    auto rec = make_ambiguous_record("HY-A");
    EXPECT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));
    EXPECT_FALSE(tracker_.track(InFlightHandle{1, 1}, rec, 1000));
    EXPECT_EQ(tracker_.count(), 1u);
}

TEST_F(OrderTrackerTest, CapacityExhaustedFailsClosed) {
    char coid[32];
    for (std::size_t i = 0; i < OrderTracker::capacity(); ++i) {
        std::snprintf(coid, sizeof(coid), "HY-%zu", i);
        auto rec = make_ambiguous_record(coid);
        ASSERT_TRUE(tracker_.track(InFlightHandle{i, 1}, rec, 1000));
    }
    auto overflow = make_ambiguous_record("HY-OVERFLOW");
    EXPECT_FALSE(tracker_.track(InFlightHandle{99, 1}, overflow, 1000));
}

// --- poll_once: Found path, one case per legal target state ---

// SpscRing/OrderTracker are deliberately non-copy/move-assignable (spsc_ring.hpp);
// each case gets fresh local instances rather than reassigning the fixture's
// members, so this stays a single parameterized test instead of four near-
// identical ones without needing a workaround for that deletion.
TEST_F(OrderTrackerTest, FoundResolvesToEachExchangeFinalState) {
    const OrderState targets[] = {OrderState::Filled, OrderState::Cancelled,
                                   OrderState::Rejected, OrderState::Expired};
    for (OrderState target : targets) {
        SCOPED_TRACE(order_state_name(target));
        OrderTracker tracker;
        ToReconcileRing inbound;
        ReconcileEventRing outbound;

        auto rec = make_ambiguous_record("HY-A", 100);
        ASSERT_TRUE(tracker.track(InFlightHandle{0, 7}, rec, 1000));

        g_mock_query_result = QueryResult{QueryOutcome::Found, target, /*exchange_order_id=*/555,
                                           /*filled_qty=*/target == OrderState::Filled ? 100 : 0,
                                           /*avg_price=*/target == OrderState::Filled ? 5000 : 0};

        poll_once(tracker, inbound, outbound, query_port_, policy_, /*now_ms=*/1000);

        ReconcileEvent ev{};
        ASSERT_TRUE(outbound.try_pop(ev));
        EXPECT_EQ(ev.resulting_state, target);
        EXPECT_EQ(ev.exchange_order_id, 555);
        EXPECT_EQ(ev.handle.slot_index, 0u);
        EXPECT_EQ(ev.handle.generation, 7u);
        EXPECT_EQ(tracker.count(), 0u) << "resolved order must leave the tracker";
    }
}

TEST_F(OrderTrackerTest, FoundResolvesToStillLiveStates) {
    const OrderState targets[] = {OrderState::Accepted, OrderState::PartialFill};
    for (OrderState target : targets) {
        SCOPED_TRACE(order_state_name(target));
        OrderTracker tracker;
        ToReconcileRing inbound;
        ReconcileEventRing outbound;

        auto rec = make_ambiguous_record("HY-A", 100);
        ASSERT_TRUE(tracker.track(InFlightHandle{0, 1}, rec, 1000));
        g_mock_query_result = QueryResult{QueryOutcome::Found, target, 555, 40, 5000};

        poll_once(tracker, inbound, outbound, query_port_, policy_, 1000);

        ReconcileEvent ev{};
        ASSERT_TRUE(outbound.try_pop(ev));
        EXPECT_EQ(ev.resulting_state, target);
        // AUDIT EXEC-INFLIGHT-003: this used to assert count()==0 -- reconciliation
        // discovered the order was still live and then dropped it. Since
        // drain_reconcile_events() correctly refuses to release a non-exchange-final
        // in-flight slot, that combination stranded the slot forever. A still-live
        // order must STAY tracked so it still has a route to a terminal state.
        EXPECT_EQ(tracker.count(), 1u)
            << "a still-live order must remain tracked until it reaches a terminal state";
    }
}

// --- Live-order polling: the terminal path for Accepted/PartialFill ---

namespace {
OrderRecord make_live_record(const char* coid, OrderState state,
                             std::int64_t intended_qty_ticks = 100) {
    OrderRecord rec{};
    // snprintf, not strncpy: strncpy(dst, src, kClientOrderIdLen) writes no
    // terminator when src is exactly kClientOrderIdLen chars, which GCC flags under
    // -Werror=stringop-truncation once the source is a runtime buffer it cannot
    // bound. Value-initialization happens to leave id[kClientOrderIdLen] zero, but
    // relying on that is exactly the kind of implicit invariant this repo avoids.
    std::snprintf(rec.client_order_id.id, sizeof(rec.client_order_id.id), "%s", coid);
    rec.symbol_id = 1;
    rec.intended_price_ticks = 5000;
    rec.intended_qty_ticks = intended_qty_ticks;
    rec.exchange_order_id = 555;
    rec.state = OrderState::Submitting;
    rec.transition_to(OrderState::Accepted);
    if (state == OrderState::PartialFill) rec.transition_to(OrderState::PartialFill);
    return rec;
}
}  // namespace

TEST_F(OrderTrackerTest, AcceptedOrderReachesFilledAndLeavesTracker) {
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{3, 9}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Filled, 555, 100, 5000};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::Filled);
    EXPECT_EQ(ev.filled_qty_ticks, 100);
    EXPECT_EQ(ev.handle.slot_index, 3u);
    EXPECT_EQ(tracker_.count(), 0u);
}

TEST_F(OrderTrackerTest, AcceptedOrderReachesOperatorCancelledAndLeavesTracker) {
    // The docs/NATIVE_EXIT_SAFETY_RUNBOOK.md case: the operator cancels from the
    // Binance app, so the order goes Accepted -> Cancelled with no local
    // CancelRequested. validate_transition() used to reject that edge outright
    // (audit STATE-TRANS-011).
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Cancelled, 555, 0, 0};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::Cancelled);
    EXPECT_EQ(tracker_.count(), 0u);
}

TEST_F(OrderTrackerTest, AcceptedOrderDiscoversCancelRequestedViaLivePoll) {
    // L4 §6.6: a resting order's ordinary flat-cadence live poll (NOT an Ambiguous
    // reconciliation) can discover PENDING_CANCEL -- exercises
    // detail::is_legal_query_target()'s Accepted->CancelRequested edge specifically, which is
    // a separate legality table from detail::is_legal_ambiguous_target() and was the bug this
    // batch's external review caught: it gates poll_once() for the live-poll path (line ~450)
    // before validate_transition() is ever consulted.
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::CancelRequested, 555, 0, 0};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev)) << "state changed (Accepted -> CancelRequested), must emit";
    EXPECT_EQ(ev.resulting_state, OrderState::CancelRequested);
    // CancelRequested is not exchange-final -- the cancel is still in flight on Binance's side,
    // so the order must stay tracked, unlike the *Cancelled tests above.
    EXPECT_EQ(tracker_.count(), 1u);
}

TEST_F(OrderTrackerTest, PartialFillOrderReachesCancelled) {
    auto rec = make_live_record("HY-LIVE", OrderState::PartialFill);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Cancelled, 555, 40, 5000};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::Cancelled);
    EXPECT_EQ(ev.filled_qty_ticks, 40);
    EXPECT_EQ(tracker_.count(), 0u);
}

TEST_F(OrderTrackerTest, StillRestingLiveOrderEmitsNoEventAndStaysTracked) {
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Accepted, 555, 0, 0};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev)) << "no state change means no event";
    EXPECT_EQ(tracker_.count(), 1u);
    EXPECT_EQ(g_mock_query_call_count, 1);
}

TEST_F(OrderTrackerTest, LiveOrderIsNeverEscalated) {
    // A resting order polled many times must NOT trip the Ambiguous quarantine
    // criterion (R-10): a live order is a normal steady state, not an anomaly.
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Accepted, 555, 0, 0};

    std::int64_t now = 1000;
    for (int i = 0; i < 20; ++i) {
        poll_once(tracker_, inbound_, outbound_, query_port_, policy_, now);
        now += policy_.live_poll_interval_ms;
    }

    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev)) << "a resting order must never escalate itself";
    EXPECT_EQ(tracker_.count(), 1u);
    EXPECT_GE(g_mock_query_call_count, 20);
}

TEST_F(OrderTrackerTest, LiveOrderRespectsFlatPollInterval) {
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Accepted, 555, 0, 0};

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);
    ASSERT_EQ(g_mock_query_call_count, 1);

    // Too soon: no second query.
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_,
              1000 + policy_.live_poll_interval_ms - 1);
    EXPECT_EQ(g_mock_query_call_count, 1);

    // Interval elapsed: query again.
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_,
              1000 + policy_.live_poll_interval_ms);
    EXPECT_EQ(g_mock_query_call_count, 2);
}

TEST_F(OrderTrackerTest, LivePartialFillGrowthUpdatesRecordWithoutEvent) {
    auto rec = make_live_record("HY-LIVE", OrderState::PartialFill);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::PartialFill, 555, 60, 5000};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
    EXPECT_EQ(tracker_.count(), 1u);

    // ...and it still reaches a terminal state afterwards.
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Filled, 555, 100, 5000};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_,
              1000 + policy_.live_poll_interval_ms);
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::Filled);
    EXPECT_EQ(tracker_.count(), 0u);
}

// --- ReconcileEvent symbol_id/side/fill_delta_qty_ticks (TODO 1A.3 follow-up) ---

TEST_F(OrderTrackerTest, ReconcileEventCarriesSymbolIdSideAndFillDelta) {
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    rec.symbol_id = 7;
    rec.side = OrderSide::Sell;
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Filled, 555, 100, 5000};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.symbol_id, 7u);
    EXPECT_EQ(ev.side, OrderSide::Sell);
    // Baseline was 0 (never polled before track()) -> full amount is the delta.
    EXPECT_EQ(ev.fill_delta_qty_ticks, 100);
}

TEST_F(OrderTrackerTest, ReconcileEventFillDeltaIsIncrementalNotCumulative) {
    // PartialFill(40) already resting when tracked (as if a prior poll had
    // already observed 40 filled), THEN discovered PartialFill(60) growing to
    // Filled(100) on this poll -- the delta must be 100-40=60, not the full
    // 100, or PositionTruth would double-count the first 40.
    auto rec = make_live_record("HY-LIVE", OrderState::PartialFill);
    rec.filled_qty_ticks = 40;
    rec.avg_fill_price_ticks = 5000;
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Filled, 555, 100, 5000};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.fill_delta_qty_ticks, 60);
}

TEST_F(OrderTrackerTest, FilledWithShortQuantityIsRejected) {
    // An exchange (or a buggy adapter) claiming FILLED while filled_qty is short of
    // intended must not release the in-flight slot on an order that still has
    // quantity resting.
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Filled, 555, 99, 5000};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
    EXPECT_EQ(tracker_.count(), 1u) << "must stay tracked, not silently resolved";
}

TEST_F(OrderTrackerTest, LiveOrderCannotRegressToAmbiguous) {
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Ambiguous, 555, 0, 0};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
    EXPECT_EQ(tracker_.count(), 1u);
}

// The full audit chain: Ambiguous -> reconciled to still-live -> polled ->
// terminal -> in-flight slot released. This is the loop that did not close.
TEST_F(OrderTrackerTest, AmbiguousToLiveToTerminalReleasesInFlightSlot) {
    InFlightRegistry in_flight;
    AuditRingSink audit;

    auto rec = make_ambiguous_record("HY-CHAIN", 100);
    auto handle = in_flight.register_submit_handle(rec.client_order_id.view());
    ASSERT_TRUE(handle.valid());
    ASSERT_TRUE(inbound_.try_push(ReconcileIngress{handle, rec}));

    // 1) reconcile discovers the order is actually still live
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Accepted, 555, 0, 0};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);
    drain_reconcile_events(in_flight, &audit, outbound_, 1000);
    EXPECT_TRUE(in_flight.is_in_flight("HY-CHAIN")) << "still live: slot correctly retained";
    EXPECT_EQ(tracker_.count(), 1u) << "still live: still tracked";

    // 2) later it fills
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Filled, 555, 100, 5000};
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_,
              1000 + policy_.live_poll_interval_ms);
    drain_reconcile_events(in_flight, &audit, outbound_, 2000);

    EXPECT_FALSE(in_flight.is_in_flight("HY-CHAIN")) << "terminal: slot must be released";
    EXPECT_EQ(in_flight.count(), 0u);
    EXPECT_EQ(tracker_.count(), 0u);
}

// The exhaustion scenario the audit reproduced, now driven all the way through.
TEST_F(OrderTrackerTest, SustainedAcceptedOrdersDoNotExhaustInFlightRegistry) {
    InFlightRegistry in_flight;
    AuditRingSink audit;

    for (std::size_t i = 0; i < kMaxInFlight * 2; ++i) {
        char coid[kClientOrderIdLen + 1];
        std::snprintf(coid, sizeof(coid), "HY-%zu", i);

        auto h = in_flight.register_submit_handle(coid);
        ASSERT_TRUE(h.valid()) << "submit #" << i << " was refused a slot";

        auto rec = make_live_record(coid, OrderState::Accepted);
        ASSERT_TRUE(inbound_.try_push(ReconcileIngress{h, rec}));

        const std::int64_t now = 1000 + static_cast<std::int64_t>(i) * policy_.live_poll_interval_ms;
        g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Filled, 555, 100, 5000};
        poll_once(tracker_, inbound_, outbound_, query_port_, policy_, now);
        drain_reconcile_events(in_flight, &audit, outbound_, now);
    }

    EXPECT_EQ(in_flight.count(), 0u) << "every filled order must have released its slot";
    EXPECT_EQ(tracker_.count(), 0u);
}

// --- poll_once: Inconclusive path ---

TEST_F(OrderTrackerTest, InconclusiveStaysTrackedAndAmbiguous) {
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));
    g_mock_query_result = QueryResult{};  // Inconclusive by default

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    EXPECT_EQ(tracker_.count(), 1u);
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev)) << "no event should be published for Inconclusive";
}

TEST_F(OrderTrackerTest, NullQueryPortIsNotSentAndNeverCounted) {
    QueryPort empty_port{};  // fn == nullptr
    EXPECT_FALSE(empty_port.is_valid());
    EXPECT_EQ(empty_port.call(OrderExpectation{}).outcome, QueryOutcome::NotSent)
        << "nothing can be sent through a port that is not wired";
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));

    poll_once(tracker_, inbound_, outbound_, empty_port, policy_, 1000);

    EXPECT_EQ(tracker_.count(), 1u) << "null port must not crash/hang and must not resolve or escalate anything";
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->query_attempts, 0) << "an unwired port sends nothing, so counts nothing";
}

// --- poll_once: invalid/out-of-range Query results are rejected, never applied ---

TEST_F(OrderTrackerTest, IllegalConfirmedStateRejected) {
    auto rec = make_ambiguous_record("HY-A", 100);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));
    // Intent is not a legal Ambiguous target under any circumstance.
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Intent, 1, 0, 0};

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    EXPECT_EQ(tracker_.count(), 1u) << "must stay tracked, not silently transitioned";
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
}

TEST_F(OrderTrackerTest, FilledQtyExceedingIntendedQtyRejected) {
    auto rec = make_ambiguous_record("HY-A", /*intended_qty_ticks=*/100);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Filled, 1,
                                       /*filled_qty=*/101, /*avg_price=*/5000};

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    EXPECT_EQ(tracker_.count(), 1u);
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
}

TEST_F(OrderTrackerTest, NegativeFilledQtyRejected) {
    auto rec = make_ambiguous_record("HY-A", 100);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::PartialFill, 1, -1, 0};

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    EXPECT_EQ(tracker_.count(), 1u);
}

TEST_F(OrderTrackerTest, NonzeroPriceWithZeroFilledQtyRejected) {
    auto rec = make_ambiguous_record("HY-A", 100);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));
    // Inconsistent: claims a fill price but zero quantity filled.
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Accepted, 1, 0, 5000};

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    EXPECT_EQ(tracker_.count(), 1u);
}

// --- poll_once: escalation (R-10, the UNKNOWN quarantine criterion) ---
//
// Owner decision 2026-09-29: an Ambiguous order goes to the operator when (1) at least 5 queries
// were actually SENT and at least 5000 ms have passed since it first became Ambiguous, or (2) at
// least 15000 ms have passed since then, whatever the count. Replaces "the third inconclusive
// query", which quarantined about a second in. Time is read on the monotonic clock poll_once() is
// driven by and, when supplied, the wall clock -- the larger elapsed time counts.

TEST_F(OrderTrackerTest, EscalatesOnceEnoughSentQueriesAndEnoughTimeHavePassed) {
    auto rec = make_ambiguous_record("HY-A");
    rec.query_attempts = 5;
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 3}, rec, 1000));  // first became Ambiguous at 1000

    // 5 queries already sent, but only 4999 ms in: keep asking (the query is sent, the count moves on).
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000 + 4999);
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
    EXPECT_EQ(g_mock_query_call_count, 1);
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->query_attempts, 6);

    // 5000 ms in: quarantine, without firing another query.
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000 + 5000);
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
    EXPECT_EQ(ev.handle.generation, 3u);
    EXPECT_EQ(tracker_.count(), 0u);
    EXPECT_EQ(g_mock_query_call_count, 1) << "escalation must not fire another query";
}

TEST_F(OrderTrackerTest, TheThirdInconclusiveQueryNoLongerEscalates) {
    // The pre-R-10 cap: three inconclusive queries in about a second -> EscalatedToOperator.
    auto rec = make_ambiguous_record("HY-A");
    rec.query_attempts = 3;
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000 + 1000);

    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
    EXPECT_EQ(tracker_.count(), 1u);
    EXPECT_EQ(g_mock_query_call_count, 1) << "still Ambiguous: the reconcile query goes out";
}

TEST_F(OrderTrackerTest, HardCapEscalatesEvenIfNoQueryWasEverSent) {
    // Every query dies locally (the rate limiter refuses it, the clock is not fresh...): none is
    // "actually sent", so none counts -- but the time it burns still runs into the 15 s cap.
    g_mock_query_result = QueryResult::not_sent();
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    for (std::int64_t t = 0; t < 15'000; t += 250) {
        poll_once(tracker_, inbound_, outbound_, query_port_, policy_, t);
    }
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev)) << "not before the cap";
    ASSERT_TRUE(tracked_record(tracker_, "HY-A").has_value());
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->query_attempts, 0) << "nothing was sent, nothing counted";
    EXPECT_GT(g_mock_query_call_count, 10) << "the loop kept trying to send meanwhile";

    const int calls_before = g_mock_query_call_count;
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 15'000);
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
    EXPECT_EQ(tracker_.count(), 0u);
    EXPECT_EQ(g_mock_query_call_count, calls_before) << "escalation must not fire another query";
}

TEST_F(OrderTrackerTest, OnlySentQueriesCountTowardTheQuarantineCriterion) {
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, make_ambiguous_record("HY-A"), 0));

    g_mock_query_result = QueryResult::not_sent();
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 0);
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->query_attempts, 0) << "NotSent is not a sent query";

    g_mock_query_result = QueryResult{};  // Inconclusive: the request WAS sent
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->query_attempts, 1);
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000 + policy_.base_interval_ms);
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->query_attempts, 2);
}

TEST_F(OrderTrackerTest, LocallyRefusedPollsKeepTheBaseBackoffNotTheMaximum) {
    // Regression for the backoff index: with nothing counted yet, query_attempts is 0 and a naive
    // "query_attempts - 1" wraps a uint8 to 255 -- a saturated 5 s delay after a poll that never
    // left the process. The base interval must apply instead.
    g_mock_query_result = QueryResult::not_sent();
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 0);
    ASSERT_EQ(g_mock_query_call_count, 1);
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, policy_.base_interval_ms - 1);
    EXPECT_EQ(g_mock_query_call_count, 1) << "before the base interval: no new attempt";
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, policy_.base_interval_ms);
    EXPECT_EQ(g_mock_query_call_count, 2) << "at the base interval, not five seconds later";
}

TEST_F(OrderTrackerTest, DefaultCadenceQuarantinesAfterFiveSentQueriesAboutNineSecondsIn) {
    // The whole criterion driven by a realistic loop: an always-inconclusive exchange, the default
    // backoff (200 ms, x4, cap 5000 ms) and a 100 ms tick. Queries go out at 0, 0.2, 1.0, 4.2 and
    // 9.2 s; the fifth is the first that lands past the 5 s floor, so the order is quarantined at
    // the next tick -- 9.3 s in, with exactly five sent queries. The old rule did it at about 1 s.
    g_mock_query_result = QueryResult{};  // Inconclusive, every time
    constexpr std::int64_t kStart = 10'000;
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, kStart));

    std::int64_t escalated_at = -1;
    for (std::int64_t t = kStart; t <= kStart + 20'000 && escalated_at < 0; t += 100) {
        poll_once(tracker_, inbound_, outbound_, query_port_, policy_, t);
        ReconcileEvent ev{};
        if (outbound_.try_pop(ev)) {
            EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
            escalated_at = t;
        }
    }
    ASSERT_GE(escalated_at, 0) << "an order that never resolves must reach the operator";
    EXPECT_EQ(g_mock_query_call_count, 5);
    EXPECT_EQ(escalated_at - kStart, 9300);
    EXPECT_GE(escalated_at - kStart, policy_.quarantine.min_elapsed_ms);
    EXPECT_LT(escalated_at - kStart, policy_.quarantine.hard_cap_ms);
}

TEST_F(OrderTrackerTest, WallClockAloneCanQuarantineWhenTheMonotonicClockStoodStill) {
    // A suspended host: the monotonic clock did not move, the wall clock did.
    constexpr std::int64_t kWall = 1'700'000'000'000;
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000, kWall));

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000, kWall + 14'999);
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000, kWall + 15'000);
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
}

TEST_F(OrderTrackerTest, WallClockSteppingBackwardsNeverPostponesQuarantine) {
    constexpr std::int64_t kWall = 1'700'000'000'000;
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000, kWall));

    // The wall clock is now an hour in the past; the monotonic clock says 15 s have gone by.
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000 + 15'000, kWall - 3'600'000);
    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
}

TEST_F(OrderTrackerTest, CallersWithoutAWallClockDecideOnTheMonotonicClockAlone) {
    // Every call site that predates R-10 passes no wall clock and must keep working.
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000 + 14'999);
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000 + 15'000);
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
}

TEST_F(OrderTrackerTest, QuarantinePolicyIsConfigurable) {
    policy_.quarantine.min_sent_queries = 1;
    policy_.quarantine.min_elapsed_ms = 500;
    policy_.quarantine.hard_cap_ms = 2000;
    auto rec = make_ambiguous_record("HY-A");
    rec.query_attempts = 1;
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 499);  // one short of the floor
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 500);  // the configured floor
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
}

TEST_F(OrderTrackerTest, SentQueryCountSaturatesInsteadOfWrapping) {
    // A wrapped uint8 would read 0 and quietly stop counting toward the criterion.
    policy_.quarantine.hard_cap_ms = std::numeric_limits<std::int64_t>::max();
    policy_.quarantine.min_elapsed_ms = std::numeric_limits<std::int64_t>::max();
    auto rec = make_ambiguous_record("HY-A");
    rec.query_attempts = 254;
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0));

    std::int64_t t = 0;
    for (int i = 0; i < 4; ++i, t += 5000) poll_once(tracker_, inbound_, outbound_, query_port_, policy_, t);
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->query_attempts, 255);
}

TEST_F(OrderTrackerTest, TrackStampsFirstSightOnBothClocksAndKeepsAnAnchorAlreadySet) {
    auto fresh = make_ambiguous_record("HY-FRESH");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, fresh, 1000, 5000));
    EXPECT_EQ(tracked_record(tracker_, "HY-FRESH")->unknown_since_mono_ms, 1000);
    EXPECT_EQ(tracked_record(tracker_, "HY-FRESH")->unknown_since_wall_ms, 5000);

    auto stamped = make_ambiguous_record("HY-STAMPED");
    stamped.unknown_since_mono_ms = 42;
    stamped.unknown_since_wall_ms = 4200;
    ASSERT_TRUE(tracker_.track(InFlightHandle{1, 1}, stamped, 1000, 5000));
    EXPECT_EQ(tracked_record(tracker_, "HY-STAMPED")->unknown_since_mono_ms, 42) << "an anchor already set is kept";
    EXPECT_EQ(tracked_record(tracker_, "HY-STAMPED")->unknown_since_wall_ms, 4200);

    auto live = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{2, 1}, live, 1000, 5000));
    EXPECT_EQ(tracked_record(tracker_, "HY-LIVE")->unknown_since_mono_ms, OrderRecord::kClockUnset)
        << "a live order was never UNKNOWN in this loop: nothing is stamped";
}

TEST_F(OrderTrackerTest, IngressIsStampedOnTheReconcileLoopsOwnClocks) {
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(inbound_.try_push(ReconcileIngress{InFlightHandle{2, 9}, rec}));
    g_mock_query_result = QueryResult{};

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 7000, 9000);

    ASSERT_TRUE(tracked_record(tracker_, "HY-A").has_value());
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->unknown_since_mono_ms, 7000);
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->unknown_since_wall_ms, 9000);
}

TEST_F(OrderTrackerTest, PollOnceStampsARecordThatArrivedWithoutAnAnchor) {
    // Defense in depth: track() with no monotonic reading leaves the anchor unset; the first
    // poll_once() must close that door (an unset anchor reads as "no time has passed" for ever).
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, OrderRecord::kClockUnset));
    ASSERT_EQ(tracked_record(tracker_, "HY-A")->unknown_since_mono_ms, OrderRecord::kClockUnset);

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 5000);
    EXPECT_EQ(tracked_record(tracker_, "HY-A")->unknown_since_mono_ms, 5000);

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 5000 + 15'000);
    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
}

TEST_F(OrderTrackerTest, LiveOrderIsNeverQuarantinedHoweverLongItRests) {
    auto rec = make_live_record("HY-LIVE", OrderState::Accepted);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 0, 1'700'000'000'000));
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Accepted, 555, 0, 0};

    // A day of ticks on both clocks: far past every quarantine threshold.
    for (std::int64_t t = 0; t <= 86'400'000; t += 60'000) {
        poll_once(tracker_, inbound_, outbound_, query_port_, policy_, t, 1'700'000'000'000 + t);
    }
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
    EXPECT_EQ(tracker_.count(), 1u);
}

TEST_F(OrderTrackerTest, TheInFlightSlotIsHeldThroughTheWholeWindowAndAfterQuarantine) {
    // Blueprint R-10 invariant: while the window runs, and after the order is quarantined, its
    // InFlightRegistry slot is NOT released -- a longer window costs occupancy, never an
    // unguarded exposure. Driven end to end: ring -> poll_once -> drain_reconcile_events.
    InFlightRegistry in_flight;
    AuditRingSink audit;
    auto rec = make_ambiguous_record("HY-Q");
    auto handle = in_flight.register_submit_handle(rec.client_order_id.view());
    ASSERT_TRUE(handle.valid());
    ASSERT_TRUE(inbound_.try_push(ReconcileIngress{handle, rec}));
    g_mock_query_result = QueryResult{};  // Inconclusive, every time

    bool quarantined = false;
    for (std::int64_t t = 0; t <= 20'000 && !quarantined; t += 100) {
        poll_once(tracker_, inbound_, outbound_, query_port_, policy_, t);
        drain_reconcile_events(in_flight, &audit, outbound_, t);
        EXPECT_TRUE(in_flight.is_in_flight("HY-Q")) << "the slot must be held at t=" << t;
        quarantined = (tracker_.count() == 0);  // poll_once untracks an escalated order
    }
    ASSERT_TRUE(quarantined) << "an order that never resolves must reach the operator";
    ASSERT_NE(audit.count(), 0u);
    EXPECT_EQ(audit.last()->event_type, AuditEventType::OrderEscalated);
    EXPECT_TRUE(in_flight.is_in_flight("HY-Q"))
        << "a quarantined order keeps its slot until an operator resolves it";
    EXPECT_EQ(in_flight.count(), 1u);
}

// AUDIT ORDER-RECONCILED-SYMBOL-014's sibling: the escalation branch of poll_once() built its
// ReconcileEvent from the handle, the coid and the resulting state only, so every OrderEscalated
// audit record -- and the durable recovery checkpoint built from it -- carried symbol 0 / Buy,
// for exactly the orders a human has to resolve by hand.
TEST_F(OrderTrackerTest, EscalationEventCarriesTheOrdersIdentityButNoFillFields) {
    auto rec = make_ambiguous_record("HY-ESC");
    rec.symbol_id = 7;
    rec.side = OrderSide::Sell;
    rec.exchange_order_id = 4242;
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 5}, rec, 0));

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, policy_.quarantine.hard_cap_ms);

    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
    EXPECT_EQ(ev.symbol_id, 7u);
    EXPECT_EQ(ev.side, OrderSide::Sell);
    EXPECT_EQ(ev.exchange_order_id, 4242);
    // Nothing new was observed by an escalation: no fill may reach OrderFillContext/PositionTruth.
    EXPECT_EQ(ev.filled_qty_ticks, 0);
    EXPECT_EQ(ev.avg_fill_price_ticks, 0);
    EXPECT_EQ(ev.fill_delta_qty_ticks, 0);
}

TEST_F(OrderTrackerTest, EscalatedAuditRecordCarriesSymbolSideAndExchangeOrderId) {
    InFlightRegistry in_flight;
    AuditRingSink audit;
    auto rec = make_ambiguous_record("HY-ESC");
    rec.symbol_id = 7;
    rec.side = OrderSide::Sell;
    rec.exchange_order_id = 4242;
    auto handle = in_flight.register_submit_handle(rec.client_order_id.view());
    ASSERT_TRUE(handle.valid());
    ASSERT_TRUE(tracker_.track(handle, rec, 0));

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, policy_.quarantine.hard_cap_ms);
    drain_reconcile_events(in_flight, &audit, outbound_, policy_.quarantine.hard_cap_ms);

    ASSERT_NE(audit.count(), 0u);
    EXPECT_EQ(audit.last()->event_type, AuditEventType::OrderEscalated);
    EXPECT_EQ(audit.last()->symbol_id, 7u);
    EXPECT_EQ(audit.last()->side, OrderSide::Sell);
    EXPECT_EQ(audit.last()->exchange_order_id, 4242);
    EXPECT_TRUE(in_flight.is_in_flight("HY-ESC")) << "an escalated order keeps its slot";
}

// --- poll_once: backoff and throttling ---

TEST_F(OrderTrackerTest, BackoffDelaysNextQueryForSameOrder) {
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, /*now_ms=*/1000));
    g_mock_query_result = QueryResult{};  // stays Inconclusive every time

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);
    EXPECT_EQ(g_mock_query_call_count, 1);

    // base_interval_ms defaults to 200; well before that has elapsed, must not
    // query again.
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1050);
    EXPECT_EQ(g_mock_query_call_count, 1) << "too soon, must skip";

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1250);
    EXPECT_EQ(g_mock_query_call_count, 2) << "backoff elapsed, must query again";
}

TEST_F(OrderTrackerTest, BackoffGrowsWithAttemptCount) {
    EXPECT_EQ(reconcile_backoff_delay_ms(policy_, 0), 200);
    EXPECT_EQ(reconcile_backoff_delay_ms(policy_, 1), 800);
    EXPECT_EQ(reconcile_backoff_delay_ms(policy_, 2), 3200);
    EXPECT_EQ(reconcile_backoff_delay_ms(policy_, 3), 5000) << "saturates at max_interval_ms";
    EXPECT_EQ(reconcile_backoff_delay_ms(policy_, 255), 5000) << "never overflows for a large attempt count";
}

TEST_F(OrderTrackerTest, DegenerateBackoffConfigFailsClosedNotImmediate) {
    ReconcilePollPolicy degenerate{};
    degenerate.base_interval_ms = 0;
    degenerate.max_interval_ms = 3000;
    EXPECT_EQ(reconcile_backoff_delay_ms(degenerate, 0), 3000)
        << "an unrepresentable backoff must delay, never mean immediate retry";
}

TEST_F(OrderTrackerTest, MaxQueriesPerTickThrottles) {
    policy_.max_queries_per_tick = 2;
    g_mock_query_result = QueryResult{};  // Inconclusive, so nothing gets untracked mid-scan
    char coid[32];
    for (int i = 0; i < 5; ++i) {
        std::snprintf(coid, sizeof(coid), "HY-%d", i);
        auto rec = make_ambiguous_record(coid);
        ASSERT_TRUE(tracker_.track(InFlightHandle{static_cast<std::size_t>(i), 1}, rec, 0));
    }

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    EXPECT_EQ(g_mock_query_call_count, 2) << "must stop at the per-tick budget, not query all 5";
    EXPECT_EQ(tracker_.count(), 5u) << "none resolved (Inconclusive), all remain tracked";
}

// --- ToReconcileRing ingestion ---

TEST_F(OrderTrackerTest, IngressIsDrainedIntoTracker) {
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(inbound_.try_push(ReconcileIngress{InFlightHandle{2, 9}, rec}));

    g_mock_query_result = QueryResult{};  // Inconclusive; just verify it got tracked at all
    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    EXPECT_EQ(tracker_.count(), 1u);
    EXPECT_EQ(g_mock_query_call_count, 1) << "the ingested record must actually be polled";
}

// --- drain_reconcile_events: audit + conditional release, thread-boundary contract ---

namespace {

class DrainEventsTest : public ::testing::Test {
protected:
    InFlightRegistry in_flight_;
    AuditRingSink audit_;
    ReconcileEventRing events_{};
};

}  // namespace

TEST_F(DrainEventsTest, ExchangeFinalReleasesSlotAndAudits) {
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());
    ASSERT_TRUE(in_flight_.is_in_flight("HY-A"));

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Filled;
    ev.exchange_order_id = 777;
    ASSERT_TRUE(events_.try_push(ev));

    drain_reconcile_events(in_flight_, &audit_, events_, 2000);

    EXPECT_FALSE(in_flight_.is_in_flight("HY-A")) << "exchange-final must release the slot";
    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->event_type, AuditEventType::OrderReconciled);
    EXPECT_EQ(audit_.last()->exchange_order_id, 777);
}

TEST_F(DrainEventsTest, EscalatedAuditsButDoesNotRelease) {
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::EscalatedToOperator;
    ASSERT_TRUE(events_.try_push(ev));

    drain_reconcile_events(in_flight_, &audit_, events_, 2000);

    EXPECT_TRUE(in_flight_.is_in_flight("HY-A")) << "escalated orders may still be live; must not release";
    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->event_type, AuditEventType::OrderEscalated);
}

TEST_F(DrainEventsTest, StillLiveDiscoveryAuditsButDoesNotRelease) {
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Accepted;
    ASSERT_TRUE(events_.try_push(ev));

    drain_reconcile_events(in_flight_, &audit_, events_, 2000);

    EXPECT_TRUE(in_flight_.is_in_flight("HY-A"))
        << "Accepted is not exchange-final -- the order may still be resting";
    EXPECT_EQ(audit_.last()->event_type, AuditEventType::OrderReconciled);
}

TEST_F(DrainEventsTest, StaleHandleAfterSlotReuseDoesNotReleaseWrongOrder) {
    auto stale = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(stale.valid());
    in_flight_.mark_resolved("HY-A");
    auto fresh = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(fresh.valid());

    ReconcileEvent ev{};
    ev.handle = stale;  // deliberately the OLD handle
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Filled;
    ASSERT_TRUE(events_.try_push(ev));

    drain_reconcile_events(in_flight_, &audit_, events_, 2000);

    EXPECT_TRUE(in_flight_.is_in_flight("HY-A"))
        << "a stale reconcile event must not release the CURRENT occupant's slot";
}

TEST_F(DrainEventsTest, NullAuditSinkIsToleratedNotCrashed) {
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());
    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Rejected;
    ASSERT_TRUE(events_.try_push(ev));

    drain_reconcile_events(in_flight_, /*audit=*/nullptr, events_, 2000);

    EXPECT_FALSE(in_flight_.is_in_flight("HY-A"));
}

// TODO 1A.3 follow-up: regression test for a real, pre-existing bug found
// while adding these fields -- drain_reconcile_events() is the ONLY place
// OrderReconciled/OrderEscalated AuditRecords are ever emitted, and it never
// assigned ar.symbol_id (or, before this batch, ar.side) at all. Every such
// AuditRecord's symbol_id had been the struct's default (0) since this
// function landed, unlike orchestrate_submit()'s own AuditRecord
// constructions, which do assign ctx.symbol_id.
TEST_F(DrainEventsTest, SymbolIdAndSideCarriedIntoAuditRecord) {
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Filled;
    ev.symbol_id = 9;
    ev.side = OrderSide::Sell;
    ASSERT_TRUE(events_.try_push(ev));

    drain_reconcile_events(in_flight_, &audit_, events_, 2000);

    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->symbol_id, 9u);
    EXPECT_EQ(audit_.last()->side, OrderSide::Sell);
}

// --- PositionTruth fold-in ---

TEST_F(DrainEventsTest, PositionTruthFoldsPositiveFillDelta) {
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Filled;
    ev.symbol_id = 3;
    ev.side = OrderSide::Buy;
    ev.fill_delta_qty_ticks = 25;
    ASSERT_TRUE(events_.try_push(ev));

    PositionTruth truth;
    drain_reconcile_events(in_flight_, &audit_, events_, 2000, &truth);

    EXPECT_EQ(truth.net_qty_ticks(3), 25);
}

TEST_F(DrainEventsTest, PositionTruthIgnoresNonPositiveFillDelta) {
    // A state transition with no fill growth (e.g. Accepted -> Cancelled)
    // must not touch PositionTruth at all.
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Cancelled;
    ev.symbol_id = 3;
    ev.fill_delta_qty_ticks = 0;
    ASSERT_TRUE(events_.try_push(ev));

    PositionTruth truth;
    drain_reconcile_events(in_flight_, &audit_, events_, 2000, &truth);

    EXPECT_EQ(truth.net_qty_ticks(3), 0);
    EXPECT_EQ(truth.tracked_symbol_count(), 0u);
}

TEST_F(DrainEventsTest, NullPositionTruthIsToleratedNotCrashed) {
    // Trailing-default-parameter backward compatibility -- every existing
    // call site (this file's own earlier tests, live_submit_orchestrator.hpp
    // callers that predate this batch) omits the argument entirely and must
    // keep behaving exactly as before.
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Filled;
    ev.fill_delta_qty_ticks = 10;
    ASSERT_TRUE(events_.try_push(ev));

    drain_reconcile_events(in_flight_, &audit_, events_, 2000);  // no position_truth argument

    EXPECT_FALSE(in_flight_.is_in_flight("HY-A"));
}

// --- OrderFillContext fold-in: the cross-mechanism double-count fix (TODO 1A.4 batch 2) ---

// THE key regression test for this batch's own core structural claim: without a shared
// baseline, an REST-observed cumulative fill and a previously-WS-observed partial fill of the
// SAME order would double-count. Pre-seeds OrderFillContext's baseline to 30 (simulating "WS
// already credited 30 via its own consume_delta() call, elsewhere"), then pushes a
// ReconcileEvent computed by poll_once()'s own WS-unaware private baseline as
// {filled_qty_ticks=100, fill_delta_qty_ticks=100} (poll_once() has no way to know WS already
// saw 30 of this). Asserts PositionTruth receives only +70 (100 shared-baseline-relative, minus
// the 30 already applied), not +100 -- proving the fix actually blocks the double-count, not
// just "reads correctly in isolation" the way order_fill_context.hpp's own unit tests already
// do for the primitive alone.
TEST_F(DrainEventsTest, FillContextPreventsCrossMechanismDoubleCount) {
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    OrderFillContext fill_context;
    ASSERT_TRUE(fill_context.track("HY-A", /*symbol_id=*/3, OrderSide::Buy, 0, 0, 0));
    ASSERT_EQ(fill_context.consume_delta("HY-A", 30), 30);  // simulates a prior WS credit

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Filled;
    ev.symbol_id = 3;
    ev.side = OrderSide::Buy;
    ev.filled_qty_ticks = 100;       // OrderTracker's own view: cumulative fill is now 100
    ev.fill_delta_qty_ticks = 100;   // OrderTracker's own (WS-unaware) private baseline was 0
    ASSERT_TRUE(events_.try_push(ev));

    PositionTruth truth;
    drain_reconcile_events(in_flight_, &audit_, events_, 2000, &truth, &fill_context);

    EXPECT_EQ(truth.net_qty_ticks(3), 70) << "must apply only the shared-baseline-relative "
                                              "delta (100-30=70), not the raw 100";
}

TEST_F(DrainEventsTest, FillContextRemovedOnExchangeFinalSymmetricWithInFlightRegistry) {
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    OrderFillContext fill_context;
    ASSERT_TRUE(fill_context.track("HY-A", 3, OrderSide::Buy, 0, 0, 0));
    ASSERT_EQ(fill_context.count(), 1u);

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Filled;  // exchange-final
    ev.filled_qty_ticks = 50;
    ASSERT_TRUE(events_.try_push(ev));

    drain_reconcile_events(in_flight_, &audit_, events_, 2000, /*position_truth=*/nullptr,
                            &fill_context);

    // The two tables' lifecycles must stay symmetric -- both released together, matching
    // InFlightRegistry's own resolve call in the same branch.
    EXPECT_FALSE(in_flight_.is_in_flight("HY-A"));
    EXPECT_EQ(fill_context.count(), in_flight_.count());
    EXPECT_EQ(fill_context.find("HY-A"), nullptr);
}

TEST_F(DrainEventsTest, FillContextNotRemovedWhenNotExchangeFinal) {
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    OrderFillContext fill_context;
    ASSERT_TRUE(fill_context.track("HY-A", 3, OrderSide::Buy, 0, 0, 0));

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::PartialFill;  // still live, not exchange-final
    ev.symbol_id = 3;
    ev.side = OrderSide::Buy;
    ev.filled_qty_ticks = 40;
    ASSERT_TRUE(events_.try_push(ev));

    PositionTruth truth;
    drain_reconcile_events(in_flight_, &audit_, events_, 2000, &truth, &fill_context);

    EXPECT_TRUE(in_flight_.is_in_flight("HY-A"));
    EXPECT_NE(fill_context.find("HY-A"), nullptr) << "still-live orders keep their fill baseline";
    EXPECT_EQ(truth.net_qty_ticks(3), 40);
}

TEST_F(DrainEventsTest, NullFillContextIsUnchangedFromNarrowerSlice) {
    // Trailing-default-parameter backward compatibility, same discipline as
    // NullPositionTruthIsToleratedNotCrashed above -- every call site that predates this batch
    // (including this file's own earlier PositionTruth-only tests) omits the argument and must
    // keep using ev.fill_delta_qty_ticks directly, byte-identical to before.
    auto handle = in_flight_.register_submit_handle("HY-A");
    ASSERT_TRUE(handle.valid());

    ReconcileEvent ev{};
    ev.handle = handle;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.resulting_state = OrderState::Filled;
    ev.symbol_id = 3;
    ev.side = OrderSide::Buy;
    ev.filled_qty_ticks = 100;
    ev.fill_delta_qty_ticks = 25;  // deliberately NOT equal to filled_qty_ticks
    ASSERT_TRUE(events_.try_push(ev));

    PositionTruth truth;
    drain_reconcile_events(in_flight_, &audit_, events_, 2000, &truth);  // no fill_context

    EXPECT_EQ(truth.net_qty_ticks(3), 25) << "must use fill_delta_qty_ticks directly, unchanged";
}

// --- Backpressure: a full outbound ring must not lose a resolution ---

TEST_F(OrderTrackerTest, FullOutboundRingKeepsOrderTrackedForRetry) {
    // Fill the ring completely with unrelated events so the real push must fail.
    for (std::size_t i = 0; i < outbound_.capacity(); ++i) {
        ReconcileEvent filler{};
        ASSERT_TRUE(outbound_.try_push(filler));
    }

    auto rec = make_ambiguous_record("HY-A", 100);
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));
    g_mock_query_result = QueryResult{QueryOutcome::Found, OrderState::Filled, 1, 100, 5000};

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    EXPECT_EQ(tracker_.count(), 1u)
        << "publish failed (ring full) -- the resolution must not be lost, order stays tracked";
}
