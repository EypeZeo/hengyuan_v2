// Layer for order_tracker.hpp: the reconcile/poll loop that drives Ambiguous
// orders toward a discovered state. Single-threaded here (poll_once() and
// drain_reconcile_events() are plain functions, exactly as testable as
// orchestrate_submit() itself) -- the cross-thread claim gets its own real
// two-thread TSan test (test_spsc_concurrency.cpp-adjacent, see plan).
#include <gtest/gtest.h>
#include <hengyuan/order_tracker.hpp>

#include <cstring>

using namespace hy;

// --- Mock QueryPort (mirrors test_live_submit_orchestrator.cpp's mock_submit pattern) ---

static QueryResult g_mock_query_result{};
static int g_mock_query_call_count = 0;

static QueryResult mock_query(const char* /*coid*/, void* /*ud*/) {
    ++g_mock_query_call_count;
    return g_mock_query_result;
}

namespace {

OrderRecord make_ambiguous_record(const char* coid, std::int64_t intended_qty_ticks = 100) {
    OrderRecord rec{};
    rec.client_order_id.id[0] = '\0';
    std::strncpy(rec.client_order_id.id, coid, kClientOrderIdLen);
    rec.symbol_id = 1;
    rec.intended_price_ticks = 5000;
    rec.intended_qty_ticks = intended_qty_ticks;
    rec.state = OrderState::Submitting;
    rec.transition_to(OrderState::Ambiguous);
    return rec;
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
        // Still-live: leaves the tracker (poll_once's job here is done -- open-
        // order polling until fill/cancel is a separate, not-yet-built mechanism,
        // see order_tracker.hpp's file header and docs/SPEC_INVARIANTS.md).
        EXPECT_EQ(tracker.count(), 0u);
    }
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

TEST_F(OrderTrackerTest, NullQueryPortFoldsIntoInconclusive) {
    QueryPort empty_port{};  // fn == nullptr
    EXPECT_FALSE(empty_port.is_valid());
    auto rec = make_ambiguous_record("HY-A");
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 1}, rec, 1000));

    poll_once(tracker_, inbound_, outbound_, empty_port, policy_, 1000);

    EXPECT_EQ(tracker_.count(), 1u) << "null port must behave exactly like Inconclusive, not crash/hang";
    ReconcileEvent ev{};
    EXPECT_FALSE(outbound_.try_pop(ev));
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

// --- poll_once: escalation ---

TEST_F(OrderTrackerTest, EscalatesAfterMaxQueryAttempts) {
    auto rec = make_ambiguous_record("HY-A");
    rec.query_attempts = OrderRecord::kMaxQueryAttempts;
    ASSERT_TRUE(tracker_.track(InFlightHandle{0, 3}, rec, 1000));

    poll_once(tracker_, inbound_, outbound_, query_port_, policy_, 1000);

    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));
    EXPECT_EQ(ev.resulting_state, OrderState::EscalatedToOperator);
    EXPECT_EQ(ev.handle.generation, 3u);
    EXPECT_EQ(tracker_.count(), 0u);
    EXPECT_EQ(g_mock_query_call_count, 0) << "escalation must not fire another query";
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
