// TODO 1A.4: binance_user_data_event.hpp unit tests -- drain_user_data_events(), mirroring
// test_order_tracker.cpp's DrainEventsTest coverage for drain_reconcile_events().
//
// TODO 1A.4 batch 2: extended with PositionTruth/OrderFillContext fold-in coverage -- the
// dedup-safety numeric scenario, side-mismatch skip, untracked-in-fill-context (late terminal
// message) skip, and avg_fill_price_ticks computation including the z=0 divide-by-zero-safety
// case (checked_scaled_mul_div()'s own divisor<=0 guard, exercised on the real call path here,
// not just in isolation as account_truth.hpp's own unit tests already do).
#include <gtest/gtest.h>
#include <hengyuan/binance_user_data_event.hpp>

#include <cstdint>
#include <cstring>
#include <optional>

using hy::AuditEventType;
using hy::AuditRingSink;
using hy::BarrierState;
using hy::drain_user_data_events;
using hy::FeedObservation;
using hy::InFlightRegistry;
using hy::kClientOrderIdLen;
using hy::OrderFillContext;
using hy::OrderSide;
using hy::PositionTruth;
using hy::ReconnectBarrier;
using hy::SweepReport;
using hy::UserDataEventKind;
using hy::UserDataWsEvent;
using hy::UserDataWsEventRing;

namespace {
void set_raw(char (&buf)[24], const char* value) {
    std::strncpy(buf, value, sizeof(buf) - 1);
}
}  // namespace

namespace {

class DrainUserDataEventsTest : public ::testing::Test {
protected:
    InFlightRegistry in_flight_;
    AuditRingSink audit_;
    UserDataWsEventRing events_{};
};

}  // namespace

TEST_F(DrainUserDataEventsTest, TrackedCoidExecutionReportWritesAuditRecord) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());

    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.exchange_order_id = 777;
    ASSERT_TRUE(events_.try_push(ev));

    drain_user_data_events(in_flight_, &audit_, events_, 2000);

    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->event_type, AuditEventType::UserDataStreamEventObserved);
    EXPECT_EQ(audit_.last()->exchange_order_id, 777);
    EXPECT_STREQ(audit_.last()->client_order_id, "HY-A");
    // This function only observes -- it must never release the in-flight slot or otherwise
    // mutate tracking state (see its own header comment on why: full fold-in is a future slice).
    EXPECT_TRUE(in_flight_.is_in_flight("HY-A"));
}

TEST_F(DrainUserDataEventsTest, UntrackedCoidIsSilentlyIgnoredNotCrashed) {
    // No register_submit_handle() call for "HY-UNKNOWN" -- an event for an order this process
    // isn't tracking (already resolved, or belongs to a different session) is expected, not
    // exceptional.
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::strncpy(ev.coid.id, "HY-UNKNOWN", kClientOrderIdLen);
    ASSERT_TRUE(events_.try_push(ev));

    drain_user_data_events(in_flight_, &audit_, events_, 2000);

    EXPECT_EQ(audit_.count(), 0u);
}

TEST_F(DrainUserDataEventsTest, NonExecutionReportKindsIgnored) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());

    for (auto kind : {UserDataEventKind::OutboundAccountPosition,
                       UserDataEventKind::ListenKeyExpired, UserDataEventKind::Unknown}) {
        UserDataWsEvent ev{};
        ev.kind = kind;
        std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
        ASSERT_TRUE(events_.try_push(ev));
    }

    drain_user_data_events(in_flight_, &audit_, events_, 2000);

    EXPECT_EQ(audit_.count(), 0u);
}

TEST_F(DrainUserDataEventsTest, NullAuditSinkIsToleratedNotCrashed) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());

    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ASSERT_TRUE(events_.try_push(ev));

    drain_user_data_events(in_flight_, /*audit=*/nullptr, events_, 2000);

    // No crash is the assertion; nothing else to check.
    SUCCEED();
}

TEST_F(DrainUserDataEventsTest, DrainsMultipleEventsInOneCall) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-B").valid());

    for (const char* coid : {"HY-A", "HY-B"}) {
        UserDataWsEvent ev{};
        ev.kind = UserDataEventKind::ExecutionReport;
        std::strncpy(ev.coid.id, coid, kClientOrderIdLen);
        ASSERT_TRUE(events_.try_push(ev));
    }

    drain_user_data_events(in_flight_, &audit_, events_, 2000);

    EXPECT_EQ(audit_.count(), 2u);
}

// --- Fold-in coverage (PositionTruth/OrderFillContext) ---

namespace {

class DrainUserDataEventsFoldInTest : public ::testing::Test {
protected:
    InFlightRegistry in_flight_;
    AuditRingSink audit_;
    UserDataWsEventRing events_{};
    PositionTruth position_truth_;
    OrderFillContext fill_context_;

    // scale=0 for price/qty/quote throughout -- keeps the raw decimal strings plain integers so
    // the expected avg-price arithmetic is checkable by hand (exponent == 0, no rescaling).
    void track_order(const char* coid, std::uint32_t symbol_id, OrderSide side) {
        ASSERT_TRUE(in_flight_.register_submit_handle(coid).valid());
        ASSERT_TRUE(fill_context_.track(coid, symbol_id, side, /*price_scale=*/0, /*qty_scale=*/0,
                                         /*quote_scale=*/0));
    }

    void push_execution_report(const char* coid, const char* cumulative_filled_qty,
                                const char* cumulative_quote_qty, bool side_known = false,
                                OrderSide side = OrderSide::Buy) {
        UserDataWsEvent ev{};
        ev.kind = UserDataEventKind::ExecutionReport;
        std::strncpy(ev.coid.id, coid, kClientOrderIdLen);
        set_raw(ev.cumulative_filled_qty_raw, cumulative_filled_qty);
        set_raw(ev.cumulative_quote_qty_raw, cumulative_quote_qty);
        ev.side_known = side_known;
        ev.side = side;
        ASSERT_TRUE(events_.try_push(ev));
    }

    void drain() {
        drain_user_data_events(in_flight_, &audit_, events_, 2000, &position_truth_,
                                &fill_context_);
    }
};

}  // namespace

TEST_F(DrainUserDataEventsFoldInTest, FoldsFirstFillIntoPositionTruthAndComputesAvgPrice) {
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    push_execution_report("HY-A", "30", "3000");  // avg price = 3000/30 = 100

    drain();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 30);
    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->filled_qty_ticks, 30);
    EXPECT_EQ(audit_.last()->avg_fill_price_ticks, 100);
    EXPECT_EQ(audit_.last()->symbol_id, 7u);
    EXPECT_EQ(audit_.last()->side, OrderSide::Buy);
}

TEST_F(DrainUserDataEventsFoldInTest, DuplicateFillEventDoesNotDoubleApply) {
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    push_execution_report("HY-A", "30", "3000");
    drain();
    ASSERT_EQ(position_truth_.net_qty_ticks(7), 30);

    // Same cumulative z redelivered (e.g. WS reconnect replay) -- delta must be 0, not another
    // +30, matching order_fill_context.hpp's own DedupSafetyWorkedNumericScenario.
    push_execution_report("HY-A", "30", "3000");
    drain();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 30);
    EXPECT_EQ(audit_.last()->filled_qty_ticks, 0);
}

TEST_F(DrainUserDataEventsFoldInTest, SideMismatchSkipsFoldInButStillAudits) {
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    // Exchange reports Sell for an order this process locally recorded as Buy at submit time --
    // suspicious, must not be trusted as the operational side.
    push_execution_report("HY-A", "30", "3000", /*side_known=*/true, OrderSide::Sell);

    drain();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 0);
    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->filled_qty_ticks, 0);
    EXPECT_EQ(audit_.last()->avg_fill_price_ticks, 0);
    // The coid is still legitimately in-flight, so the audit record is still written.
    EXPECT_STREQ(audit_.last()->client_order_id, "HY-A");
}

TEST_F(DrainUserDataEventsFoldInTest, UntrackedInFillContextIsSilentlySkippedNotCrashed) {
    // In-flight per InFlightRegistry (so the audit gate still passes) but never tracked in
    // OrderFillContext -- the "late/reordered WS message for an already-resolved order"
    // scenario this file's own header comment documents (fill_context->remove() already ran at
    // the same call site that resolved the order).
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());
    push_execution_report("HY-A", "30", "3000");

    drain();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 0);
    EXPECT_EQ(fill_context_.count(), 0u);
    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->filled_qty_ticks, 0);
    EXPECT_EQ(audit_.last()->symbol_id, 0u);  // no OrderFillContextEntry to source it from
}

TEST_F(DrainUserDataEventsFoldInTest, AvgFillPriceLeftAtZeroWhenCumulativeFilledIsZero) {
    // A NEW (or CANCELED/REJECTED) executionReport reports "z"="0" -- checked_scaled_mul_div()'s
    // own divisor<=0 guard must fail closed here, not divide-by-zero crash, and
    // avg_fill_price_ticks must stay at its default 0, exercised on the real call path (not just
    // account_truth.hpp's own isolated unit tests for the formula).
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    push_execution_report("HY-A", "0", "0");

    drain();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 0);
    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->filled_qty_ticks, 0);
    EXPECT_EQ(audit_.last()->avg_fill_price_ticks, 0);
}

TEST_F(DrainUserDataEventsFoldInTest, NullPositionTruthAndFillContextIsUnchangedFromNarrowerSlice) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.exchange_order_id = 42;
    ASSERT_TRUE(events_.try_push(ev));

    // Trailing params omitted entirely -- default nullptr, byte-identical to the original
    // narrower slice's own behavior.
    drain_user_data_events(in_flight_, &audit_, events_, 2000);

    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->exchange_order_id, 42);
    EXPECT_EQ(audit_.last()->symbol_id, 0u);
    EXPECT_EQ(audit_.last()->filled_qty_ticks, 0);
}

// --- Private-stream migration M3a: the ReconnectBarrier filter of drain_user_data_events() ---------------------------------
//
//   FI-009  an event stamped with an OLDER generation than the one the barrier has Reconciled is dropped (and counted); until then it is merged
//   FI-011  a serverShutdown / eventStreamTerminated event ends its generation at once, whatever the supervisor still says
//   FI-043  ... and the generation after it starts from "not swept"
// The barrier's own state machine is pinned in test_reconnect_barrier.cpp; this is the wiring: what the drain feeds it, and what it lets through.

namespace {

FeedObservation barrier_feed(std::uint64_t generation) {
    FeedObservation o{};
    o.generation = generation;
    o.connected = true;
    o.last_activity_ms = 1000;
    o.sampled_at_ms = 1000;
    return o;
}

// Subscribed to `generation` and swept: Reconciled(generation).
ReconnectBarrier reconciled_barrier(std::uint64_t generation) {
    ReconnectBarrier b;
    b.observe(barrier_feed(generation));
    if (const std::optional<std::uint64_t> epoch = b.take_sweep_request(1000)) {
        b.on_sweep_report(SweepReport{*epoch, 0U, true}, 1000);
    }
    return b;
}

class DrainUserDataEventsBarrierTest : public DrainUserDataEventsFoldInTest {
protected:
    ReconnectBarrier barrier_;

    void push_report(const char* coid, const char* z, const char* quote_z, std::uint64_t generation) {
        UserDataWsEvent ev{};
        ev.kind = UserDataEventKind::ExecutionReport;
        std::strncpy(ev.coid.id, coid, kClientOrderIdLen);
        set_raw(ev.cumulative_filled_qty_raw, z);
        set_raw(ev.cumulative_quote_qty_raw, quote_z);
        ev.generation = generation;
        ASSERT_TRUE(events_.try_push(ev));
    }

    void push_kind(UserDataEventKind kind, std::uint64_t generation) {
        UserDataWsEvent ev{};
        ev.kind = kind;
        ev.generation = generation;
        ASSERT_TRUE(events_.try_push(ev));
    }

    void drain_with_barrier() {
        drain_user_data_events(in_flight_, &audit_, events_, 2000, &position_truth_, &fill_context_, &barrier_);
    }
};

}  // namespace

TEST_F(DrainUserDataEventsBarrierTest, AnOlderGenerationsFillIsDroppedOnceTheNewGenerationIsReconciled) {  // FI-009
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    barrier_ = reconciled_barrier(2);
    push_report("HY-A", "30", "3000", /*generation=*/1);

    drain_with_barrier();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 0) << "the sweep that made generation 2 Reconciled already reflects whatever generation 1 said";
    EXPECT_EQ(audit_.count(), 0u);
    EXPECT_EQ(barrier_.stats().late_events_dropped, 1u);

    push_report("HY-A", "30", "3000", /*generation=*/2);
    drain_with_barrier();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 30);
    EXPECT_NE(audit_.count(), 0u);
    EXPECT_EQ(barrier_.stats().late_events_dropped, 1u) << "the current generation's own event is not late";
}

TEST_F(DrainUserDataEventsBarrierTest, ANewerGenerationsFillIsNotLate) {
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    barrier_ = reconciled_barrier(2);
    push_report("HY-A", "30", "3000", /*generation=*/3);

    drain_with_barrier();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 30);
    EXPECT_EQ(barrier_.stats().late_events_dropped, 0u);
}

TEST_F(DrainUserDataEventsBarrierTest, AnOlderGenerationsFillIsStillMergedWhileTheNewGenerationIsBeingSwept) {  // FI-009
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    barrier_.observe(barrier_feed(2));  // generation 2 is subscribed; its sweep has not been answered
    ASSERT_EQ(barrier_.state(), BarrierState::Sweeping);
    push_report("HY-A", "30", "3000", /*generation=*/1);

    drain_with_barrier();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 30) << "before Reconciled nothing says the sweep has seen this fill: merge it (positive deltas only)";
    EXPECT_EQ(barrier_.stats().late_events_dropped, 0u);
}

TEST_F(DrainUserDataEventsBarrierTest, AnOlderGenerationsFillIsStillMergedWhileTheBarrierIsDown) {
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    push_report("HY-A", "30", "3000", /*generation=*/1);  // the barrier has seen nothing at all

    drain_with_barrier();

    EXPECT_EQ(barrier_.state(), BarrierState::Down);
    EXPECT_EQ(position_truth_.net_qty_ticks(7), 30);
}

TEST_F(DrainUserDataEventsBarrierTest, AnUnstampedEventIsAlwaysMerged) {
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    barrier_ = reconciled_barrier(5);
    push_report("HY-A", "30", "3000", /*generation=*/0);  // the retired listenKey session never stamps a generation

    drain_with_barrier();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 30);
    EXPECT_EQ(barrier_.stats().late_events_dropped, 0u);
}

TEST_F(DrainUserDataEventsBarrierTest, EveryKindOfOlderEventIsCountedNotOnlyFills) {
    barrier_ = reconciled_barrier(3);
    push_kind(UserDataEventKind::BalanceUpdate, 1);
    push_kind(UserDataEventKind::OutboundAccountPosition, 2);
    push_kind(UserDataEventKind::ListStatus, 2);

    drain_with_barrier();

    EXPECT_EQ(barrier_.stats().late_events_dropped, 3u);
    EXPECT_EQ(barrier_.state(), BarrierState::Reconciled);
}

TEST_F(DrainUserDataEventsBarrierTest, ServerShutdownEndsItsGenerationAtOnce) {  // FI-011
    barrier_ = reconciled_barrier(1);
    push_kind(UserDataEventKind::ServerShutdown, /*generation=*/1);

    drain_with_barrier();

    EXPECT_EQ(barrier_.state(), BarrierState::Down);
    EXPECT_EQ(barrier_.reconciled_generation(), 0u);
    EXPECT_EQ(barrier_.stats().generation_ends_seen, 1u);
    // The session still looks connected for a moment (the supervisor has not polled): that must not bring the barrier back.
    barrier_.observe(barrier_feed(1));
    EXPECT_EQ(barrier_.state(), BarrierState::Down);
}

TEST_F(DrainUserDataEventsBarrierTest, EventStreamTerminatedEndsItsGenerationToo) {  // FI-011
    barrier_ = reconciled_barrier(1);
    push_kind(UserDataEventKind::EventStreamTerminated, /*generation=*/1);

    drain_with_barrier();

    EXPECT_EQ(barrier_.state(), BarrierState::Down);
    barrier_.observe(barrier_feed(1));
    EXPECT_EQ(barrier_.state(), BarrierState::Down);
}

TEST_F(DrainUserDataEventsBarrierTest, OtherEventsDoNotEndAGeneration) {
    barrier_ = reconciled_barrier(1);
    push_kind(UserDataEventKind::BalanceUpdate, 1);
    push_kind(UserDataEventKind::OutboundAccountPosition, 1);
    push_kind(UserDataEventKind::ExternalLockUpdate, 1);
    push_kind(UserDataEventKind::ListStatus, 1);
    push_kind(UserDataEventKind::ListenKeyExpired, 1);
    push_kind(UserDataEventKind::Unknown, 1);

    drain_with_barrier();

    EXPECT_EQ(barrier_.state(), BarrierState::Reconciled);
    EXPECT_EQ(barrier_.stats().generation_ends_seen, 0u);
}

TEST_F(DrainUserDataEventsBarrierTest, TheEndOfAnOlderGenerationLeavesTheCurrentOneAlone) {
    barrier_ = reconciled_barrier(2);
    push_kind(UserDataEventKind::ServerShutdown, /*generation=*/1);

    drain_with_barrier();

    EXPECT_EQ(barrier_.state(), BarrierState::Reconciled);
    EXPECT_EQ(barrier_.stats().generation_ends_seen, 0u);
}

TEST_F(DrainUserDataEventsBarrierTest, AnUnstampedShutdownSaysNothingAboutThisStream) {
    barrier_ = reconciled_barrier(2);
    push_kind(UserDataEventKind::ServerShutdown, /*generation=*/0);

    drain_with_barrier();

    EXPECT_EQ(barrier_.state(), BarrierState::Reconciled);
}

TEST_F(DrainUserDataEventsBarrierTest, AFillBehindItsGenerationsShutdownInTheSameBatchIsStillApplied) {  // FI-011 + FI-009
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    barrier_ = reconciled_barrier(1);
    push_report("HY-A", "10", "1000", 1);
    push_kind(UserDataEventKind::ServerShutdown, 1);
    push_report("HY-A", "20", "2000", 1);  // drained after the shutdown of its own generation: NOT late, the generation is over, not superseded

    drain_with_barrier();

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 20);
    EXPECT_EQ(barrier_.state(), BarrierState::Down);
    EXPECT_EQ(barrier_.stats().late_events_dropped, 0u);
}

TEST_F(DrainUserDataEventsBarrierTest, ARotationEndToEnd) {  // FI-043 + FI-011 + FI-009 through the drain
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    barrier_ = reconciled_barrier(1);

    // generation 1 ends with a fill behind its shutdown
    push_kind(UserDataEventKind::ServerShutdown, 1);
    push_report("HY-A", "10", "1000", 1);
    drain_with_barrier();
    ASSERT_EQ(barrier_.state(), BarrierState::Down);
    EXPECT_EQ(position_truth_.net_qty_ticks(7), 10);

    // generation 2 connects: Sweeping until its sweep is answered
    barrier_.observe(barrier_feed(2));
    ASSERT_EQ(barrier_.state(), BarrierState::Sweeping);
    push_report("HY-A", "20", "2000", 1);  // a straggler of generation 1 while the sweep is pending: merged
    drain_with_barrier();
    EXPECT_EQ(position_truth_.net_qty_ticks(7), 20);

    const std::optional<std::uint64_t> epoch = barrier_.take_sweep_request(1000);
    ASSERT_TRUE(epoch.has_value());
    barrier_.on_sweep_report(SweepReport{*epoch, 0U, true}, 1000);
    ASSERT_EQ(barrier_.state(), BarrierState::Reconciled);

    // ... and once swept, a straggler of generation 1 is dropped, a fill of generation 2 is applied
    push_report("HY-A", "30", "3000", 1);
    drain_with_barrier();
    EXPECT_EQ(position_truth_.net_qty_ticks(7), 20);
    EXPECT_EQ(barrier_.stats().late_events_dropped, 1u);
    push_report("HY-A", "30", "3000", 2);
    drain_with_barrier();
    EXPECT_EQ(position_truth_.net_qty_ticks(7), 30);
}

TEST_F(DrainUserDataEventsBarrierTest, ANullBarrierLeavesTheDrainExactlyAsItWas) {
    track_order("HY-A", /*symbol_id=*/7, OrderSide::Buy);
    push_kind(UserDataEventKind::ServerShutdown, 1);
    push_report("HY-A", "30", "3000", /*generation=*/1);

    drain_user_data_events(in_flight_, &audit_, events_, 2000, &position_truth_, &fill_context_, /*barrier=*/nullptr);

    EXPECT_EQ(position_truth_.net_qty_ticks(7), 30);
    EXPECT_EQ(barrier_.state(), BarrierState::Down) << "an unrelated barrier is not touched";
    EXPECT_EQ(barrier_.stats().generation_ends_seen, 0u);
}
