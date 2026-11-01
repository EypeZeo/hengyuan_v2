// SPDX-License-Identifier: proprietary
// test_user_data_durable.cpp -- private-stream migration M4: UserDataDurablePublisher and drain_user_data_events() in durable mode (binance_user_data_event.hpp,
// docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 6). A fake durable port with failure injection, no files: what reaches memory and when, what the log is asked to
// write, what is refused, and what the shadow looks like after each of those. The real-log round trips (restart, both writers, a random walk) are in
// test_order_durable_path.cpp, which has the fixture for real sinks.
#include <gtest/gtest.h>
#include <hengyuan/binance_user_data_event.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace hy;

namespace {

constexpr std::uint32_t kSymbol = 3;

// The durable port under test: acknowledges unless told otherwise, keeps the acknowledged frames, and records what memory looked like from INSIDE each call --
// the whole point of durable-first is that it still looked untouched.
struct Probe {
    std::vector<int> fail_calls;  // 1-based numbers of the append calls that answer Failed
    bool fail_all{false};
    int calls{0};
    std::vector<AuditRecord> frames;  // the acknowledged frames, in order
    struct Seen {
        std::int64_t net_qty;
        std::size_t audit_count;
    };
    std::vector<Seen> seen;
    const PositionTruth* truth{nullptr};
    const AuditRingSink* audit{nullptr};
};

AuditAppendResult probe_append(void* user_data, const AuditRecord& rec, std::int64_t) noexcept {
    auto* p = static_cast<Probe*>(user_data);
    ++p->calls;
    p->seen.push_back(Probe::Seen{p->truth->net_qty_ticks(rec.symbol_id), p->audit->count()});
    AuditAppendResult r{};  // Failed by default
    if (p->fail_all || std::find(p->fail_calls.begin(), p->fail_calls.end(), p->calls) != p->fail_calls.end()) return r;
    p->frames.push_back(rec);
    r.status = AuditAppendResult::Status::Acked;
    r.sequence = static_cast<std::uint64_t>(p->frames.size());
    return r;
}

// An execution report: status "X" and cumulative filled quantity "z" (the quote quantity "Z" feeds the derived average price).
UserDataWsEvent report(const char* coid, OrderState status, const char* z, const char* quote = "0", std::int64_t exchange_order_id = 555) {
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::snprintf(ev.coid.id, sizeof(ev.coid.id), "%s", coid);
    ev.exchange_order_id = exchange_order_id;
    ev.order_status = status;
    ev.order_status_known = true;
    ev.side = OrderSide::Buy;
    ev.side_known = true;
    std::snprintf(ev.cumulative_filled_qty_raw, sizeof(ev.cumulative_filled_qty_raw), "%s", z);
    std::snprintf(ev.cumulative_quote_qty_raw, sizeof(ev.cumulative_quote_qty_raw), "%s", quote);
    return ev;
}

void make_reconciled(ReconnectBarrier& b) {
    FeedObservation o{};
    o.generation = 1;
    o.connected = true;
    o.last_activity_ms = 1000;
    o.sampled_at_ms = 1000;
    b.observe(o);
    const std::optional<std::uint64_t> epoch = b.take_sweep_request(1000);
    ASSERT_TRUE(epoch.has_value());
    b.on_sweep_report(SweepReport{*epoch, 0U, true}, 1000);
    ASSERT_EQ(b.state(), BarrierState::Reconciled);
}

}  // namespace

class UserDataDurableTest : public ::testing::Test {
protected:
    InFlightRegistry in_flight_;
    AuditRingSink audit_;
    UserDataWsEventRing ring_;
    PositionTruth truth_;
    OrderFillContext fills_;
    ReconnectBarrier barrier_;
    Probe probe_;
    UserDataDurablePublisher publisher_;

    void SetUp() override {
        probe_.truth = &truth_;
        probe_.audit = &audit_;
        ASSERT_TRUE(publisher_.set_port(DurableOrderAuditPort{&probe_append, nullptr, &probe_}));
        make_reconciled(barrier_);
    }

    // An order in flight and tracked with the quantity it was sent with, whose log holds (`logged`, `filled`); `maybe` = a recovered Ambiguous order.
    void admit(const char* coid, OrderState logged, std::int64_t filled = 0, std::int64_t intended = 10, bool maybe = false) {
        ASSERT_TRUE(in_flight_.register_submit_handle(coid).valid());
        ASSERT_TRUE(fills_.track(coid, kSymbol, OrderSide::Buy, 0, 0, 0, intended));
        ASSERT_TRUE(fills_.seed_shadow(coid, DurableShadow{logged, maybe, filled}));
    }

    void push(const UserDataWsEvent& ev) { ASSERT_TRUE(ring_.try_push(ev)); }

    void drain(std::int64_t now_ms = 5000) {
        drain_user_data_events(in_flight_, &audit_, ring_, now_ms, &truth_, &fills_, &barrier_, nullptr, &publisher_);
    }

    DurableShadow shadow_of(const char* coid) const {
        const OrderFillContextEntry* e = fills_.find(coid);
        return e != nullptr ? e->shadow : DurableShadow{};
    }
};

// --- the order of things ------------------------------------------------------------------------------------------------------------------------------

TEST_F(UserDataDurableTest, TheAckComesBeforeEveryMemoryEffect) {
    admit("HY-A", OrderState::Accepted);
    push(report("HY-A", OrderState::PartialFill, "4", "400"));

    drain();

    ASSERT_EQ(probe_.seen.size(), 1u);
    EXPECT_EQ(probe_.seen[0].net_qty, 0) << "PositionTruth must not have moved when the sink is asked";
    EXPECT_EQ(probe_.seen[0].audit_count, 0u) << "the audit ring must not know the report yet";
    // once the ACK is in, all of it happens
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 4);
    EXPECT_EQ(audit_.count(), 1u);
    EXPECT_EQ(shadow_of("HY-A").state, OrderState::PartialFill);
    EXPECT_EQ(shadow_of("HY-A").filled_qty_ticks, 4);
    EXPECT_FALSE(publisher_.blocked());
    EXPECT_EQ(publisher_.stats().acked_frames, 1u);
    EXPECT_EQ(publisher_.stats().written, 1u);
}

TEST_F(UserDataDurableTest, TheFrameCarriesTheCumulativeFillAndTheRingRecordIsTheSameRecord) {
    admit("HY-A", OrderState::Accepted, 0, /*intended=*/10);
    UserDataWsEvent ev = report("HY-A", OrderState::PartialFill, "4", "400", /*exchange_order_id=*/777);
    push(ev);

    drain(/*now_ms=*/4242);

    ASSERT_EQ(probe_.frames.size(), 1u);
    ASSERT_EQ(audit_.count(), 1u);
    const AuditRecord& frame = probe_.frames[0];
    const AuditRecord& ring = *audit_.at(0);
    EXPECT_EQ(frame.event_type, AuditEventType::UserDataStreamEventObserved);
    EXPECT_EQ(frame.mode, ExecutionMode::Live);
    EXPECT_EQ(frame.resulting_state, OrderState::PartialFill);
    EXPECT_EQ(frame.filled_qty_ticks, 4) << "cumulative, not the delta: the recovery scan holds it monotone";
    EXPECT_EQ(frame.avg_fill_price_ticks, 100) << "400 quote over 4 filled";
    EXPECT_EQ(frame.exchange_order_id, 777);
    EXPECT_EQ(frame.symbol_id, kSymbol);
    EXPECT_EQ(frame.side, OrderSide::Buy);
    EXPECT_EQ(frame.qty_ticks, 10);
    EXPECT_EQ(frame.timestamp_ms, 4242);
    EXPECT_STREQ(frame.client_order_id, "HY-A");
    EXPECT_EQ(ring.event_type, frame.event_type);
    EXPECT_EQ(ring.resulting_state, frame.resulting_state);
    EXPECT_EQ(ring.filled_qty_ticks, frame.filled_qty_ticks);
    EXPECT_EQ(ring.avg_fill_price_ticks, frame.avg_fill_price_ticks);
    EXPECT_EQ(ring.exchange_order_id, frame.exchange_order_id);
    EXPECT_EQ(ring.symbol_id, frame.symbol_id);
    EXPECT_EQ(ring.side, frame.side);
    EXPECT_EQ(ring.timestamp_ms, frame.timestamp_ms);
    EXPECT_STREQ(ring.client_order_id, frame.client_order_id);
}

TEST_F(UserDataDurableTest, AReportThatSaysNothingNewWritesNoFrameAndCreditsNothing) {
    admit("HY-A", OrderState::Accepted);  // the submit response already said Accepted
    push(report("HY-A", OrderState::Accepted, "0"));  // the NEW event of the same order

    drain();

    EXPECT_EQ(probe_.calls, 0) << "nothing to record: no fsync";
    EXPECT_EQ(publisher_.stats().stale, 1u);
    EXPECT_EQ(publisher_.stats().written, 0u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 0);
    EXPECT_EQ(shadow_of("HY-A").state, OrderState::Accepted);
    EXPECT_EQ(audit_.count(), 1u) << "still observed";
    EXPECT_EQ(barrier_.state(), BarrierState::Reconciled) << "a duplicate is not trouble";
}

TEST_F(UserDataDurableTest, AStaleReportStillBringsMemoryUpToWhatTheLogHolds) {
    // the log holds PartialFill(4) but this process's baseline is behind it (it never folded that frame): the report says the same thing again
    admit("HY-A", OrderState::PartialFill, /*filled=*/4);
    push(report("HY-A", OrderState::PartialFill, "4"));

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().stale, 1u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 4) << "the log already holds it, so memory may: durable-first is about never getting AHEAD of the log";
}

// --- coalescing -----------------------------------------------------------------------------------------------------------------------------------

TEST_F(UserDataDurableTest, ABurstOfReportsForOneOrderIsOneFrameWithTheLatestFill) {
    admit("HY-A", OrderState::Accepted);
    push(report("HY-A", OrderState::Accepted, "0"));
    push(report("HY-A", OrderState::PartialFill, "3"));
    push(report("HY-A", OrderState::PartialFill, "7"));
    push(report("HY-A", OrderState::Filled, "10"));

    drain();

    ASSERT_EQ(probe_.frames.size(), 1u);
    EXPECT_EQ(probe_.frames[0].resulting_state, OrderState::Filled);
    EXPECT_EQ(probe_.frames[0].filled_qty_ticks, 10);
    EXPECT_EQ(publisher_.stats().coalesced, 3u);
    EXPECT_EQ(publisher_.stats().written, 1u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 10) << "the whole fill, once";
    EXPECT_EQ(audit_.count(), 1u);
}

TEST_F(UserDataDurableTest, TwoOrdersInOneDrainAreTwoFramesInTheOrderTheyWereFirstHeld) {
    admit("HY-A", OrderState::Accepted);
    admit("HY-B", OrderState::Accepted);
    push(report("HY-A", OrderState::PartialFill, "2"));
    push(report("HY-B", OrderState::PartialFill, "5"));
    push(report("HY-A", OrderState::PartialFill, "6"));  // merges into HY-A's, which stays first

    drain();

    ASSERT_EQ(probe_.frames.size(), 2u);
    EXPECT_STREQ(probe_.frames[0].client_order_id, "HY-A");
    EXPECT_EQ(probe_.frames[0].filled_qty_ticks, 6);
    EXPECT_STREQ(probe_.frames[1].client_order_id, "HY-B");
    EXPECT_EQ(probe_.frames[1].filled_qty_ticks, 5);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 11);
}

TEST_F(UserDataDurableTest, AnOlderReportAfterANewerOneInTheSameDrainIsDropped) {
    admit("HY-A", OrderState::Accepted);
    push(report("HY-A", OrderState::Filled, "10"));
    push(report("HY-A", OrderState::PartialFill, "4"));  // delivered late

    drain();

    ASSERT_EQ(probe_.frames.size(), 1u);
    EXPECT_EQ(probe_.frames[0].resulting_state, OrderState::Filled);
    EXPECT_EQ(publisher_.stats().coalesced, 1u);
    EXPECT_EQ(publisher_.stats().refused_contradiction, 0u) << "older is not contradictory";
}

TEST_F(UserDataDurableTest, TwoReportsThatCannotBothBeTrueRefuseTheSecondAndTellTheBarrier) {
    admit("HY-A", OrderState::Accepted);
    push(report("HY-A", OrderState::Filled, "10"));
    push(report("HY-A", OrderState::Cancelled, "10"));  // both final, and different

    drain();

    ASSERT_EQ(probe_.frames.size(), 1u) << "the first stands";
    EXPECT_EQ(probe_.frames[0].resulting_state, OrderState::Filled);
    EXPECT_EQ(publisher_.stats().refused_contradiction, 1u);
    EXPECT_EQ(barrier_.state(), BarrierState::Sweeping) << "what the stream says about this order is not to be trusted: a sweep reads REST";
    EXPECT_EQ(barrier_.stats().events_refused, 1u);
    EXPECT_EQ(audit_.count(), 2u) << "the refusal leaves a trace next to the written frame's record";
    EXPECT_EQ(audit_.at(0)->filled_qty_ticks, 0) << "the trace credits nothing";
}

// --- a failed append -------------------------------------------------------------------------------------------------------------------------------

TEST_F(UserDataDurableTest, AFailedAppendPublishesNothingAndNothingIsPoppedBehindIt) {
    admit("HY-A", OrderState::Accepted);
    admit("HY-B", OrderState::Accepted);
    push(report("HY-A", OrderState::Filled, "10"));
    probe_.fail_all = true;

    drain();

    EXPECT_TRUE(publisher_.blocked());
    EXPECT_EQ(publisher_.held_count(), 1u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 0) << "no frame, no fold";
    EXPECT_EQ(audit_.count(), 0u);
    EXPECT_EQ(shadow_of("HY-A").state, OrderState::Accepted) << "a shadow ahead of the log makes the next planned frame illegal";
    EXPECT_EQ(shadow_of("HY-A").filled_qty_ticks, 0);
    EXPECT_EQ(publisher_.stats().failed_appends, 1u);
    EXPECT_EQ(publisher_.stats().acked_frames, 0u);

    push(report("HY-B", OrderState::Filled, "10"));  // arrives while HY-A's report is unacknowledged
    drain();                                          // the sink still refuses
    EXPECT_EQ(publisher_.held_count(), 1u);
    UserDataWsEvent waiting{};
    EXPECT_TRUE(ring_.try_pop(waiting)) << "HY-B's report is still in the ring: the head of the line blocks it (the ring fills, the session drops, the "
                                           "barrier blocks new orders)";
    EXPECT_STREQ(waiting.coid.id, "HY-B");
    push(waiting);

    probe_.fail_all = false;
    drain();  // the sink answers

    EXPECT_FALSE(publisher_.blocked());
    ASSERT_EQ(probe_.frames.size(), 2u) << "HY-A first, then HY-B, each exactly once";
    EXPECT_STREQ(probe_.frames[0].client_order_id, "HY-A");
    EXPECT_STREQ(probe_.frames[1].client_order_id, "HY-B");
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 20) << "applied exactly once each: the failed attempts consumed nothing";
}

TEST_F(UserDataDurableTest, ARetryAfterTheAmbiguousFrameLandedWritesOnlyTheObservation) {
    admit("HY-A", OrderState::Ambiguous, 0, 10, /*maybe=*/true);  // recovered: the log holds Submitting or Ambiguous
    push(report("HY-A", OrderState::Cancelled, "0"));
    probe_.fail_calls = {2};  // the Ambiguous frame lands, the observation does not

    drain();

    EXPECT_TRUE(publisher_.blocked());
    ASSERT_EQ(probe_.frames.size(), 1u);
    EXPECT_EQ(probe_.frames[0].resulting_state, OrderState::Ambiguous);
    EXPECT_EQ(probe_.frames[0].event_type, AuditEventType::OrderAmbiguous);
    EXPECT_EQ(shadow_of("HY-A").state, OrderState::Ambiguous) << "advanced for the frame that WAS acknowledged";
    EXPECT_FALSE(shadow_of("HY-A").maybe_submitting) << "and exact from then on";

    drain();

    ASSERT_EQ(probe_.frames.size(), 2u) << "the retry plans from what the log now holds: no second Ambiguous frame";
    EXPECT_EQ(probe_.frames[1].resulting_state, OrderState::Cancelled);
    EXPECT_FALSE(publisher_.blocked());
    EXPECT_EQ(shadow_of("HY-A").state, OrderState::Cancelled);
}

TEST_F(UserDataDurableTest, AnObservationWhoseOrderFinishedBeforeTheRetryIsDropped) {
    admit("HY-A", OrderState::Accepted);
    push(report("HY-A", OrderState::Filled, "10"));
    probe_.fail_all = true;
    drain();
    ASSERT_TRUE(publisher_.blocked());

    fills_.remove("HY-A");  // the reconcile path resolved the order in the meantime
    probe_.fail_all = false;
    drain();

    EXPECT_FALSE(publisher_.blocked());
    EXPECT_EQ(publisher_.stats().dropped_gone, 1u);
    EXPECT_EQ(probe_.frames.size(), 0u);
}

// --- the Ambiguous frame ----------------------------------------------------------------------------------------------------------------------------

TEST_F(UserDataDurableTest, ARecoveredOrderGetsTheAmbiguousFrameOnlyWhereTheTargetNeedsIt) {
    struct Case {
        const char* coid;
        OrderState reported;
        std::size_t frames;  // a target legal from both Submitting and Ambiguous needs none in front of it
    };
    const Case cases[] = {
        {"HY-ACC", OrderState::Accepted, 1},
        {"HY-FILLED", OrderState::Filled, 1},
        {"HY-REJ", OrderState::Rejected, 1},
        {"HY-CANCEL", OrderState::Cancelled, 2},
        {"HY-EXP", OrderState::Expired, 2},
        {"HY-PEND", OrderState::CancelRequested, 2},
    };
    std::size_t expected = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.coid);
        admit(c.coid, OrderState::Ambiguous, 0, 10, /*maybe=*/true);
        push(report(c.coid, c.reported, c.reported == OrderState::Filled ? "10" : "0"));
        drain();
        expected += c.frames;
        ASSERT_EQ(probe_.frames.size(), expected);
        EXPECT_EQ(probe_.frames.back().resulting_state, c.reported);
        if (c.frames == 2) EXPECT_EQ(probe_.frames[probe_.frames.size() - 2].resulting_state, OrderState::Ambiguous);
    }
}

TEST_F(UserDataDurableTest, AnOrderTheExchangeAcknowledgedIsNeverGivenAnAmbiguousFrame) {
    admit("HY-A", OrderState::Accepted);
    push(report("HY-A", OrderState::Cancelled, "0"));

    drain();

    ASSERT_EQ(probe_.frames.size(), 1u) << "Accepted -> Ambiguous is not an edge, and neither is it true";
    EXPECT_EQ(probe_.frames[0].resulting_state, OrderState::Cancelled);
}

// --- refusals ---------------------------------------------------------------------------------------------------------------------------------------

TEST_F(UserDataDurableTest, AReportTheLogCannotTakeIsRefusedWithoutAFrameAndTheBarrierIsToldOnce) {
    admit("HY-A", OrderState::Rejected);  // the submit was rejected ...
    push(report("HY-A", OrderState::Accepted, "0"));  // ... and a NEW arrives for it

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().refused_contradiction, 1u);
    EXPECT_EQ(barrier_.state(), BarrierState::Sweeping);
    EXPECT_EQ(barrier_.stats().events_refused, 1u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 0);
    EXPECT_EQ(shadow_of("HY-A").state, OrderState::Rejected) << "the shadow is the log's, and nothing was written";
    ASSERT_EQ(audit_.count(), 1u);
    EXPECT_EQ(audit_.at(0)->filled_qty_ticks, 0);
}

TEST_F(UserDataDurableTest, AStatusNoMappingNamesIsRefused) {
    admit("HY-A", OrderState::Accepted);
    UserDataWsEvent ev = report("HY-A", OrderState::Filled, "10");
    ev.order_status_known = false;
    push(ev);

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().refused_unreadable, 1u);
    EXPECT_EQ(barrier_.stats().events_refused, 1u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 0) << "unlike the memory-first drain, an unmapped status credits no fill";
}

TEST_F(UserDataDurableTest, AFillThatDoesNotParseIsRefused) {
    admit("HY-A", OrderState::Accepted);
    push(report("HY-A", OrderState::PartialFill, "not-a-number"));

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().refused_unreadable, 1u);
    EXPECT_EQ(barrier_.stats().events_refused, 1u);
}

TEST_F(UserDataDurableTest, AnInFlightOrderTheFillContextDoesNotHoldIsRefusedAsUntracked) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());  // registered, never tracked: no shadow to plan against
    push(report("HY-A", OrderState::Filled, "10"));

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().refused_untracked, 1u);
    EXPECT_EQ(barrier_.stats().events_refused, 1u);
}

TEST_F(UserDataDurableTest, ASideThatDisagreesWithTheOneRecordedAtSubmitTimeIsRefused) {
    admit("HY-A", OrderState::Accepted);
    UserDataWsEvent ev = report("HY-A", OrderState::Filled, "10");
    ev.side = OrderSide::Sell;
    push(ev);

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().refused_contradiction, 1u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 0) << "never apply a fill with a side we did not record";
    EXPECT_EQ(barrier_.stats().events_refused, 1u);
}

TEST_F(UserDataDurableTest, AShadowStillAtIntentIsAWriterThatForgotItsAckAndEveryReportIsRefused) {
    admit("HY-A", OrderState::Intent);  // no frame of the order was ever acknowledged: nothing was sent
    push(report("HY-A", OrderState::Accepted, "0"));

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().refused_contradiction, 1u);
    EXPECT_EQ(barrier_.stats().events_refused, 1u) << "visibly: this is how a missed ACK point shows up";
}

TEST_F(UserDataDurableTest, AnUnknownQuantityRefusesEveryFillButNotAReportThatCarriesNone) {
    admit("HY-A", OrderState::Accepted, 0, /*intended=*/0);
    push(report("HY-A", OrderState::Cancelled, "0"));
    drain();
    EXPECT_EQ(probe_.frames.size(), 1u) << "no fill, no need for a bound";

    admit("HY-B", OrderState::Accepted, 0, /*intended=*/0);
    push(report("HY-B", OrderState::PartialFill, "4"));
    drain();
    EXPECT_EQ(probe_.frames.size(), 1u);
    EXPECT_EQ(publisher_.stats().refused_contradiction, 1u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 0);
}

TEST_F(UserDataDurableTest, AFillPastTheQuantityTheOrderWasSentWithIsRefused) {
    admit("HY-A", OrderState::Accepted, 0, /*intended=*/10);
    push(report("HY-A", OrderState::Filled, "11"));

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().refused_contradiction, 1u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 0);
}

TEST_F(UserDataDurableTest, WithoutAFillContextEveryReportAboutAnInFlightOrderIsRefusedAsUntracked) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());
    push(report("HY-A", OrderState::Filled, "10"));

    drain_user_data_events(in_flight_, &audit_, ring_, 5000, &truth_, /*fill_context=*/nullptr, &barrier_, nullptr, &publisher_);

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().refused_untracked, 1u);
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 0);
}

// --- what the mode leaves alone -----------------------------------------------------------------------------------------------------------------------

TEST_F(UserDataDurableTest, ControlEventsReachTheBarrierWhateverTheModeAndWithoutAFillContext) {
    UserDataWsEvent shutdown{};
    shutdown.kind = UserDataEventKind::ServerShutdown;
    shutdown.generation = 1;
    push(shutdown);

    drain_user_data_events(in_flight_, &audit_, ring_, 5000, &truth_, /*fill_context=*/nullptr, &barrier_, nullptr, &publisher_);

    EXPECT_EQ(barrier_.state(), BarrierState::Down) << "a serverShutdown ends its generation before the supervisor notices";
    EXPECT_EQ(barrier_.stats().generation_ends_seen, 1u);
}

TEST_F(UserDataDurableTest, AReportForAnOrderThatIsNotInFlightIsIgnoredAsBefore) {
    push(report("HY-GONE", OrderState::Filled, "10"));

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(publisher_.stats().refused_untracked, 0u) << "a late delivery for a finished order is expected, not trouble";
    EXPECT_EQ(barrier_.stats().events_refused, 0u);
    EXPECT_EQ(audit_.count(), 0u);
}

TEST_F(UserDataDurableTest, ANonExecutionEventIsLeftAlone) {
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::BalanceUpdate;
    push(ev);

    drain();

    EXPECT_EQ(probe_.calls, 0);
    EXPECT_EQ(audit_.count(), 0u);
    EXPECT_EQ(barrier_.state(), BarrierState::Reconciled);
}

TEST_F(UserDataDurableTest, AnEmptyRingAsksTheSinkNothing) {
    drain();
    drain();
    EXPECT_EQ(probe_.calls, 0);
    EXPECT_FALSE(publisher_.blocked());
}

// --- the publisher itself -------------------------------------------------------------------------------------------------------------------------

TEST(UserDataDurablePublisherSurface, HeldObservationsHaveOneOwner) {
    EXPECT_FALSE(std::is_copy_constructible_v<UserDataDurablePublisher>);
    EXPECT_FALSE(std::is_copy_assignable_v<UserDataDurablePublisher>);
    EXPECT_FALSE(std::is_move_constructible_v<UserDataDurablePublisher>);
    EXPECT_FALSE(std::is_move_assignable_v<UserDataDurablePublisher>);
}

TEST(UserDataDurablePublisherStaging, ANewReportReplacesAnOlderOneAndAnOlderOneIsDropped) {
    UserDataDurablePublisher p;
    HeldUserDataObservation a{};
    a.ev = report("HY-A", OrderState::Accepted, "0");
    HeldUserDataObservation b = a;
    b.ev = report("HY-A", OrderState::PartialFill, "4");
    b.cumulative_filled_qty_ticks = 4;
    EXPECT_EQ(p.stage(a, 10), UserDataDurablePublisher::Stage::Held);
    EXPECT_EQ(p.stage(b, 10), UserDataDurablePublisher::Stage::Replaced);
    EXPECT_EQ(p.stage(a, 10), UserDataDurablePublisher::Stage::DroppedOlder);
    EXPECT_EQ(p.held_count(), 1u);
    HeldUserDataObservation c = b;
    c.ev = report("HY-A", OrderState::Cancelled, "4");
    c.cumulative_filled_qty_ticks = 4;
    HeldUserDataObservation d = b;
    d.ev = report("HY-A", OrderState::Accepted, "9");  // NEW with 9 filled for an order the held report already says is Cancelled
    d.cumulative_filled_qty_ticks = 9;
    EXPECT_EQ(p.stage(c, 10), UserDataDurablePublisher::Stage::Replaced);
    EXPECT_EQ(p.stage(d, 10), UserDataDurablePublisher::Stage::Contradiction);
    EXPECT_EQ(p.held_count(), 1u);
}

TEST(UserDataDurablePublisherStaging, OneSlotPerOrderAndTheSixtyFifthDistinctOrderIsRefused) {
    UserDataDurablePublisher p;
    for (std::size_t i = 0; i < UserDataDurablePublisher::kCapacity; ++i) {
        HeldUserDataObservation h{};
        h.ev = report(("HY-" + std::to_string(i)).c_str(), OrderState::Accepted, "0");
        ASSERT_EQ(p.stage(h, 10), UserDataDurablePublisher::Stage::Held) << i;
    }
    HeldUserDataObservation extra{};
    extra.ev = report("HY-EXTRA", OrderState::Accepted, "0");
    EXPECT_EQ(p.stage(extra, 10), UserDataDurablePublisher::Stage::Full);
    HeldUserDataObservation again{};
    again.ev = report("HY-0", OrderState::Accepted, "0");
    EXPECT_EQ(p.stage(again, 10), UserDataDurablePublisher::Stage::DroppedOlder) << "an order already held still merges when the table is full";
}

TEST_F(UserDataDurableTest, AnIdlePublisherCanBePointedAtAnotherPortButOneHoldingAnObservationCannot) {
    Probe other;
    other.truth = &truth_;
    other.audit = &audit_;
    const DurableOrderAuditPort other_port{&probe_append, nullptr, &other};
    EXPECT_TRUE(publisher_.set_port(other_port));

    admit("HY-A", OrderState::Accepted);
    push(report("HY-A", OrderState::Filled, "10"));
    other.fail_all = true;
    drain();
    ASSERT_TRUE(publisher_.blocked());
    EXPECT_FALSE(publisher_.set_port(DurableOrderAuditPort{&probe_append, nullptr, &probe_})) << "part of the observation may be in the log behind the old port";
}

TEST_F(UserDataDurableTest, AnUnwiredPortNeverAcknowledgesAndPublishesNothing) {
    UserDataDurablePublisher unwired;  // no port injected
    admit("HY-A", OrderState::Accepted);
    push(report("HY-A", OrderState::Filled, "10"));

    drain_user_data_events(in_flight_, &audit_, ring_, 5000, &truth_, &fills_, &barrier_, nullptr, &unwired);

    EXPECT_TRUE(unwired.blocked());
    EXPECT_EQ(truth_.net_qty_ticks(kSymbol), 0);
    EXPECT_EQ(audit_.count(), 0u);
    EXPECT_EQ(unwired.stats().failed_appends, 1u);
}
