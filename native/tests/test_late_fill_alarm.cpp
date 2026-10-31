// D5 slice R4: the late-fill alarm (late_fill_alarm.hpp; docs/OPERATOR_RESOLUTION_DESIGN.md 9 "R-late", blueprint L-47).
//
// An order an operator wrote off ("the exchange has no such order and never will") is final and untracked. If the proof was wrong, the exchange's
// execution reports for it used to be thrown away by drain_user_data_events() without a trace. The alarm watches exactly the ids that recovery lists
// as operator-resolved, trips the breaker on any report for one of them, and writes its own audit records. Three layers here:
//   1. the alarm by itself: the table, the trip, the records, what a reset does, what a missing or full audit sink does, an incomplete table;
//   2. the alarm inside the production drain: which events reach it, which do not, that it sees an older generation's report before the barrier's
//      stale filter can drop it, and that it moves no position;
//   3. the recovery round trip: a crafted log with operator frames, read by the production sink, its list handed to the alarm, a report fired through
//      the drain. (test_live_submit_orchestrator.cpp has the fourth layer: the very call that drains the report is refused.)
//
// The logs of layer 3 are crafted exactly like test_operator_resolution_reader.cpp's (the production encoder, chained and anchored the way
// DurableAuditSink does it, every log read by the production sink), and every scenario that reopens a store uses a fresh file name -- deleting a log
// and creating one of the same name a moment later races a real-time antivirus scanner on a Windows host.

#include <gtest/gtest.h>
#include <hengyuan/binance_user_data_event.hpp>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/late_fill_alarm.hpp>
#include <hengyuan/operator_resolution_frame.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

using namespace hy;

namespace {

OperatorResolvedOrder resolved_entry(const char* coid, std::uint32_t symbol_id = 7, OrderSide side = OrderSide::Sell, std::int64_t qty = 100) {
    OperatorResolvedOrder o{};
    std::snprintf(o.client_order_id.id, sizeof(o.client_order_id.id), "%s", coid);
    o.symbol_id = symbol_id;
    o.side = side;
    o.qty_ticks = qty;
    o.frame_sequence = 5;
    return o;
}

LateFillReport report_of(ExecutionType type = ExecutionType::New, std::int64_t exchange_order_id = 4242) {
    LateFillReport r{};
    r.exchange_order_id = exchange_order_id;
    r.exec_type = static_cast<std::uint8_t>(type);
    return r;
}

constexpr std::int64_t kNow = 5000;

}  // namespace

// === 1. the alarm by itself ===========================================================================================================

class LateFillAlarmTest : public ::testing::Test {
protected:
    KillSwitch kill_;
    AuditRingSink audit_;
    std::vector<OperatorResolvedOrder> list_;
    std::optional<LateFillAlarm> alarm_;

    // The alarm over list_ as it is now; `total` defaults to "the scan found exactly what it listed".
    LateFillAlarm& arm(std::optional<std::size_t> total = std::nullopt) {
        alarm_.emplace(std::span<const OperatorResolvedOrder>(list_), total.value_or(list_.size()), kill_);
        return *alarm_;
    }

    const AuditRecord& record(std::size_t i) const {
        const AuditRecord* r = audit_.at(i);
        EXPECT_NE(r, nullptr) << "no audit record " << i;
        static const AuditRecord kNone{};
        return r != nullptr ? *r : kNone;
    }
};

TEST_F(LateFillAlarmTest, AnEmptyTableWatchesNothingAndTouchesNothing) {
    LateFillAlarm& alarm = arm();

    EXPECT_TRUE(alarm.complete());
    EXPECT_EQ(alarm.watched(), 0u);
    EXPECT_FALSE(alarm.watches("HY-A"));
    EXPECT_FALSE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow));
    EXPECT_EQ(kill_.state(), KillState::Normal);
    EXPECT_EQ(audit_.count(), 0u);
    EXPECT_EQ(alarm.stats().reports, 0u);
}

TEST_F(LateFillAlarmTest, AReportForAWatchedOrderTripsTheBreaker) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();
    ASSERT_EQ(kill_.state(), KillState::Normal) << "a complete table does not touch the breaker by itself";

    EXPECT_TRUE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow));

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(kill_.trigger_count(), 1u);
    EXPECT_EQ(alarm.stats().reports, 1u);
    EXPECT_EQ(alarm.stats().alarmed_orders, 1u);
    EXPECT_EQ(alarm.stats().trips, 1u);
    EXPECT_EQ(alarm.stats().audit_failed, 0u);
}

TEST_F(LateFillAlarmTest, AnotherOrderIsNotTheAlarmsBusiness) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();

    EXPECT_FALSE(alarm.watches("HY-B"));
    EXPECT_FALSE(alarm.on_execution_report("HY-B", report_of(), &audit_, kNow));

    EXPECT_EQ(kill_.state(), KillState::Normal);
    EXPECT_EQ(audit_.count(), 0u);
    EXPECT_EQ(alarm.stats().reports, 0u) << "a report for an order the alarm does not watch is not counted either";
}

TEST_F(LateFillAlarmTest, AnIdMustMatchExactlyNotByPrefixOrCase) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();

    for (const char* other : {"HY-AB", "HY-", "HY", "H", "hy-a", " HY-A", "HY-A ", "HY-A\n"}) {
        EXPECT_FALSE(alarm.watches(other)) << "`" << other << "`";
    }
    EXPECT_TRUE(alarm.watches("HY-A"));
}

TEST_F(LateFillAlarmTest, AnIdOfTheFullLengthIsMatchedAndALongerOneIsNot) {
    const std::string full(kClientOrderIdLen, 'A');  // the longest id a ClientOrderId holds
    list_ = {resolved_entry(full.c_str())};
    LateFillAlarm& alarm = arm();

    EXPECT_TRUE(alarm.watches(full));
    EXPECT_FALSE(alarm.watches(full + "A")) << "one character more is another id";
    EXPECT_FALSE(alarm.watches(full.substr(1)));
    ASSERT_TRUE(alarm.on_execution_report(full, report_of(), &audit_, kNow));
    EXPECT_EQ(std::strlen(record(0).client_order_id), kClientOrderIdLen) << "the record keeps the whole id";
}

TEST_F(LateFillAlarmTest, TheAlarmIsNotCopyableSoThereIsOneStateAndOneBreakerReference) {
    static_assert(!std::is_copy_constructible_v<LateFillAlarm>);
    static_assert(!std::is_copy_assignable_v<LateFillAlarm>);
    static_assert(!std::is_move_constructible_v<LateFillAlarm>);
    static_assert(!std::is_move_assignable_v<LateFillAlarm>);
    SUCCEED();
}

TEST_F(LateFillAlarmTest, AnEmptyIdMatchesNothingEvenAgainstAnEmptyEntry) {
    list_ = {resolved_entry("")};  // cannot come out of recovery (an operator frame always carries its order id); the guard is for the day it does
    LateFillAlarm& alarm = arm();

    EXPECT_FALSE(alarm.watches(""));
    EXPECT_FALSE(alarm.on_execution_report("", report_of(), &audit_, kNow));
    EXPECT_EQ(kill_.state(), KillState::Normal);
}

TEST_F(LateFillAlarmTest, EveryPositionInTheTableIsFoundAndUsesItsOwnEntry) {
    list_ = {resolved_entry("HY-0", 10, OrderSide::Buy, 11), resolved_entry("HY-1", 11, OrderSide::Sell, 22),
             resolved_entry("HY-2", 12, OrderSide::Buy, 33), resolved_entry("HY-3", 13, OrderSide::Sell, 44),
             resolved_entry("HY-4", 14, OrderSide::Buy, 55)};
    LateFillAlarm& alarm = arm();
    for (const char* coid : {"HY-0", "HY-1", "HY-2", "HY-3", "HY-4"}) EXPECT_TRUE(alarm.watches(coid)) << coid;

    ASSERT_TRUE(alarm.on_execution_report("HY-4", report_of(), &audit_, kNow));  // the last one
    ASSERT_TRUE(alarm.on_execution_report("HY-0", report_of(), &audit_, kNow));  // the first one
    ASSERT_TRUE(alarm.on_execution_report("HY-2", report_of(), &audit_, kNow));  // one in the middle

    // observed + trip for HY-4, observed for HY-0, observed for HY-2 (the breaker was tripped already)
    ASSERT_EQ(audit_.count(), 4u);
    EXPECT_STREQ(record(0).client_order_id, "HY-4");
    EXPECT_EQ(record(0).symbol_id, 14u);
    EXPECT_EQ(record(0).side, OrderSide::Buy);
    EXPECT_EQ(record(0).qty_ticks, 55);
    EXPECT_STREQ(record(2).client_order_id, "HY-0");
    EXPECT_EQ(record(2).symbol_id, 10u);
    EXPECT_EQ(record(2).qty_ticks, 11);
    EXPECT_STREQ(record(3).client_order_id, "HY-2");
    EXPECT_EQ(record(3).symbol_id, 12u);
    EXPECT_EQ(record(3).qty_ticks, 33);
    EXPECT_EQ(alarm.stats().alarmed_orders, 3u);
}

TEST_F(LateFillAlarmTest, AnArmedBreakerIsEscalatedToTriggered) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();
    kill_.arm();
    ASSERT_EQ(kill_.state(), KillState::Armed);

    ASSERT_TRUE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow));

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(alarm.stats().trips, 1u);
}

TEST_F(LateFillAlarmTest, ABreakerThatIsAlreadyTriggeredIsLeftAloneAndNotCountedAgain) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();
    kill_.trigger();
    ASSERT_EQ(kill_.trigger_count(), 1u);

    ASSERT_TRUE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow));

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(kill_.trigger_count(), 1u) << "trigger() counts every call; the alarm calls it only when the state moves";
    EXPECT_EQ(alarm.stats().trips, 0u);
    ASSERT_EQ(audit_.count(), 1u) << "the observation is recorded, the trip (which did not happen) is not";
    EXPECT_EQ(record(0).event_type, AuditEventType::UserDataStreamEventObserved);
}

TEST_F(LateFillAlarmTest, ALatchedBreakerStaysLatched) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();
    kill_.latch();

    ASSERT_TRUE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow));

    EXPECT_EQ(kill_.state(), KillState::Latched) << "the terminal lockout is never downgraded";
    EXPECT_EQ(kill_.trigger_count(), 0u);
    EXPECT_EQ(alarm.stats().trips, 0u);
}

TEST_F(LateFillAlarmTest, TheFirstReportWritesAnObservedRecordAndATripRecord) {
    list_ = {resolved_entry("HY-A", /*symbol_id=*/9, OrderSide::Buy, /*qty=*/250)};
    LateFillAlarm& alarm = arm();
    LateFillReport report = report_of(ExecutionType::Trade, /*exchange_order_id=*/9001);
    report.order_status = OrderState::PartialFill;
    report.order_status_known = true;

    ASSERT_TRUE(alarm.on_execution_report("HY-A", report, &audit_, kNow));

    ASSERT_EQ(audit_.count(), 2u);
    const AuditRecord& observed = record(0);
    EXPECT_EQ(observed.event_type, AuditEventType::UserDataStreamEventObserved);
    EXPECT_STREQ(observed.detail_msg, LateFillAlarm::kObservedMsg);
    EXPECT_STREQ(LateFillAlarm::kObservedMsg, "late report: operator-resolved order") << "the text consumers of the audit trail look for";
    const AuditRecord& trip = record(1);
    EXPECT_EQ(trip.event_type, AuditEventType::KillSwitchTriggered);
    EXPECT_STREQ(trip.detail_msg, LateFillAlarm::kTripMsg);
    EXPECT_STREQ(LateFillAlarm::kTripMsg, "breaker: report on operator-resolved order");
    for (const AuditRecord* r : {&observed, &trip}) {
        EXPECT_EQ(r->timestamp_ms, kNow);
        EXPECT_EQ(r->mode, ExecutionMode::Live);
        EXPECT_STREQ(r->client_order_id, "HY-A");
        EXPECT_EQ(r->symbol_id, 9u);
        EXPECT_EQ(r->side, OrderSide::Buy);
        EXPECT_EQ(r->qty_ticks, 250) << "the intended quantity of the written-off order";
        EXPECT_EQ(r->filled_qty_ticks, 0) << "what the report says was filled is deliberately not parsed";
        EXPECT_EQ(r->exchange_order_id, 9001);
        EXPECT_EQ(r->detail_code, static_cast<std::int64_t>(ExecutionType::Trade));
        EXPECT_EQ(r->resulting_state, OrderState::PartialFill);
    }
    // The two messages fit the record's field whole (set_detail truncates silently).
    EXPECT_LT(std::strlen(LateFillAlarm::kObservedMsg), sizeof(AuditRecord::detail_msg));
    EXPECT_LT(std::strlen(LateFillAlarm::kTripMsg), sizeof(AuditRecord::detail_msg));
}

TEST_F(LateFillAlarmTest, AReportWithoutAKnownStatusLeavesTheRecordsStateAlone) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();
    LateFillReport report = report_of();
    report.order_status = OrderState::Filled;  // ignored: order_status_known stays false
    ASSERT_TRUE(alarm.on_execution_report("HY-A", report, &audit_, kNow));

    ASSERT_EQ(audit_.count(), 2u);
    EXPECT_EQ(record(0).resulting_state, OrderState::Intent);
    EXPECT_EQ(record(1).resulting_state, OrderState::Intent);
    // (this entry is the default one: a Sell -- the other tests above check a Buy, so a side that is not copied from the entry shows here)
    EXPECT_EQ(record(0).side, OrderSide::Sell);
    EXPECT_EQ(record(1).side, OrderSide::Sell);
}

TEST_F(LateFillAlarmTest, RepeatedReportsAreCountedButAuditedOnce) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();

    for (int i = 0; i < 4; ++i) ASSERT_TRUE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow + i));

    EXPECT_EQ(alarm.stats().reports, 4u);
    EXPECT_EQ(alarm.stats().alarmed_orders, 1u);
    EXPECT_EQ(alarm.stats().trips, 1u);
    EXPECT_EQ(audit_.count(), 2u) << "one observed + one trip, however often the exchange repeats itself";
}

TEST_F(LateFillAlarmTest, ASecondOrderIsAuditedEvenWhenTheBreakerIsAlreadyTripped) {
    list_ = {resolved_entry("HY-A", 1), resolved_entry("HY-B", 2)};
    LateFillAlarm& alarm = arm();

    ASSERT_TRUE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow));
    ASSERT_TRUE(alarm.on_execution_report("HY-B", report_of(), &audit_, kNow));

    ASSERT_EQ(audit_.count(), 3u);
    EXPECT_STREQ(record(2).client_order_id, "HY-B");
    EXPECT_EQ(record(2).event_type, AuditEventType::UserDataStreamEventObserved) << "no second trip: the breaker was down already";
    EXPECT_EQ(alarm.stats().alarmed_orders, 2u);
    EXPECT_EQ(alarm.stats().trips, 1u);
}

TEST_F(LateFillAlarmTest, AnOperatorResetFollowedByAnotherReportTripsTheBreakerAgainAndSaysSo) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();
    ASSERT_TRUE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow));
    ASSERT_EQ(kill_.state(), KillState::Triggered);
    ASSERT_EQ(audit_.count(), 2u);

    kill_.operator_reset();  // the restart equivalent
    ASSERT_EQ(kill_.state(), KillState::Normal);
    ASSERT_TRUE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow + 1));

    EXPECT_EQ(kill_.state(), KillState::Triggered) << "the per-order de-duplication is for the audit ring, never for the breaker";
    EXPECT_EQ(alarm.stats().trips, 2u);
    ASSERT_EQ(audit_.count(), 3u);
    EXPECT_EQ(record(2).event_type, AuditEventType::KillSwitchTriggered) << "a trip that is not the order's first report is still recorded";
    EXPECT_EQ(record(2).timestamp_ms, kNow + 1);
}

TEST_F(LateFillAlarmTest, ANullAuditSinkNeverBlocksTheBreaker) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();

    EXPECT_TRUE(alarm.on_execution_report("HY-A", report_of(), /*audit=*/nullptr, kNow));

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(alarm.stats().audit_failed, 2u) << "the observed record and the trip record, both lost and both counted";
}

TEST_F(LateFillAlarmTest, AFullAuditRingNeverBlocksTheBreaker) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm();
    while (!audit_.is_full()) ASSERT_TRUE(audit_.append(AuditRecord{}));
    const std::size_t before = audit_.count();

    EXPECT_TRUE(alarm.on_execution_report("HY-A", report_of(), &audit_, kNow));

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(audit_.count(), before) << "a full ring takes nothing more";
    EXPECT_EQ(alarm.stats().audit_failed, 2u);
}

TEST_F(LateFillAlarmTest, ATableHandedOverWithHolesTripsTheBreakerAtOnce) {
    list_ = {resolved_entry("HY-A")};
    LateFillAlarm& alarm = arm(/*total=*/2);  // the scan found two, the list holds one: an OperatorOpen scan that overflowed

    EXPECT_FALSE(alarm.complete());
    EXPECT_EQ(kill_.state(), KillState::Triggered) << "a watch list with holes is no watch list: no order may go out on the strength of it";
    EXPECT_EQ(kill_.trigger_count(), 1u);
    EXPECT_TRUE(alarm.watches("HY-A")) << "it still watches what it has";
    EXPECT_EQ(alarm.stats().trips, 0u) << "the stats count reports, not the constructor";
}

TEST_F(LateFillAlarmTest, ATotalSmallerThanTheListIsInconsistentAndIncompleteToo) {
    list_ = {resolved_entry("HY-A")};
    arm(/*total=*/0);  // fewer found than listed cannot happen: inconsistent input is not to be trusted either
    EXPECT_FALSE(alarm_->complete());
    EXPECT_EQ(kill_.state(), KillState::Triggered);
}

TEST_F(LateFillAlarmTest, ATableLongerThanTheAlarmCanHoldWatchesTheFirstOnesAndIsIncomplete) {
    list_.resize(kMaxOperatorResolved + 1);
    for (std::size_t i = 0; i < list_.size(); ++i) {
        list_[i] = resolved_entry(("HY-BIG-" + std::to_string(i)).c_str());
    }
    LateFillAlarm& alarm = arm();  // total == size, but more than the alarm can hold

    EXPECT_FALSE(alarm.complete());
    EXPECT_EQ(alarm.watched(), kMaxOperatorResolved);
    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_TRUE(alarm.watches(("HY-BIG-" + std::to_string(kMaxOperatorResolved - 1)).c_str()));
    EXPECT_FALSE(alarm.watches(("HY-BIG-" + std::to_string(kMaxOperatorResolved)).c_str())) << "the one that did not fit";
}

TEST_F(LateFillAlarmTest, AFullTableOfExactlyTheCapacityIsComplete) {
    list_.resize(kMaxOperatorResolved);
    for (std::size_t i = 0; i < list_.size(); ++i) {
        list_[i] = resolved_entry(("HY-BIG-" + std::to_string(i)).c_str());
    }
    LateFillAlarm& alarm = arm();

    EXPECT_TRUE(alarm.complete());
    EXPECT_EQ(alarm.watched(), kMaxOperatorResolved);
    EXPECT_EQ(kill_.state(), KillState::Normal);
    EXPECT_TRUE(alarm.on_execution_report(("HY-BIG-" + std::to_string(kMaxOperatorResolved - 1)).c_str(), report_of(), &audit_, kNow))
        << "the last slot of the table works";
    EXPECT_EQ(kill_.state(), KillState::Triggered);
}

// === 2. the alarm inside the production drain =========================================================================================

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

}  // namespace

class LateFillDrainTest : public LateFillAlarmTest {
protected:
    InFlightRegistry in_flight_;
    UserDataWsEventRing events_{};
    PositionTruth position_truth_;
    OrderFillContext fill_context_;

    void push(const char* coid, UserDataEventKind kind = UserDataEventKind::ExecutionReport, ExecutionType type = ExecutionType::New,
              const char* cumulative_filled = "0.00000000", std::uint64_t generation = 0) {
        UserDataWsEvent ev{};
        ev.kind = kind;
        std::snprintf(ev.coid.id, sizeof(ev.coid.id), "%s", coid);
        ev.exchange_order_id = 777;
        ev.exec_type = type;
        std::snprintf(ev.cumulative_filled_qty_raw, sizeof(ev.cumulative_filled_qty_raw), "%s", cumulative_filled);
        ev.generation = generation;
        ASSERT_TRUE(events_.try_push(ev));
    }

    void drain(ReconnectBarrier* barrier = nullptr) {
        drain_user_data_events(in_flight_, &audit_, events_, kNow, &position_truth_, &fill_context_, barrier, alarm_ ? &*alarm_ : nullptr);
    }
};

TEST_F(LateFillDrainTest, AReportForAWrittenOffOrderTripsTheBreakerThroughTheDrain) {
    list_ = {resolved_entry("HY-A", 7, OrderSide::Sell, 100)};
    arm();
    push("HY-A", UserDataEventKind::ExecutionReport, ExecutionType::New, "0.00000000");

    drain();

    EXPECT_EQ(kill_.state(), KillState::Triggered) << "even a report that carries no fill contradicts the operator's proof";
    ASSERT_EQ(audit_.count(), 2u);
    EXPECT_EQ(record(0).event_type, AuditEventType::UserDataStreamEventObserved);
    EXPECT_STREQ(record(0).detail_msg, LateFillAlarm::kObservedMsg);
    EXPECT_EQ(record(0).exchange_order_id, 777);
    EXPECT_EQ(record(1).event_type, AuditEventType::KillSwitchTriggered);
    EXPECT_EQ(record(0).detail_code, static_cast<std::int64_t>(ExecutionType::New));
    EXPECT_EQ(record(0).timestamp_ms, kNow) << "the drain's now_ms stamps the records";
    EXPECT_EQ(record(1).timestamp_ms, kNow);
    EXPECT_EQ(record(0).symbol_id, 7u);
    EXPECT_EQ(record(0).qty_ticks, 100);
}

TEST_F(LateFillDrainTest, AFillForAWrittenOffOrderTripsItAndTheStatusIsCarried) {
    list_ = {resolved_entry("HY-A")};
    arm();
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::snprintf(ev.coid.id, sizeof(ev.coid.id), "%s", "HY-A");
    ev.exec_type = ExecutionType::Trade;
    ev.order_status = OrderState::PartialFill;
    ev.order_status_known = true;
    std::snprintf(ev.cumulative_filled_qty_raw, sizeof(ev.cumulative_filled_qty_raw), "%s", "0.50000000");
    ASSERT_TRUE(events_.try_push(ev));

    drain();

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    ASSERT_EQ(audit_.count(), 2u);
    EXPECT_EQ(record(0).resulting_state, OrderState::PartialFill);
    EXPECT_EQ(record(0).detail_code, static_cast<std::int64_t>(ExecutionType::Trade));
}

TEST_F(LateFillDrainTest, ACancelOrAnExpiryWithNoFillTripsItToo) {
    list_ = {resolved_entry("HY-A"), resolved_entry("HY-B")};
    arm();
    push("HY-A", UserDataEventKind::ExecutionReport, ExecutionType::Canceled);
    push("HY-B", UserDataEventKind::ExecutionReport, ExecutionType::Expired);

    drain();

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(alarm_->stats().alarmed_orders, 2u);
}

TEST_F(LateFillDrainTest, OnlyExecutionReportsReachTheAlarm) {
    list_ = {resolved_entry("HY-A")};
    arm();
    for (UserDataEventKind kind :
         {UserDataEventKind::Unknown, UserDataEventKind::OutboundAccountPosition, UserDataEventKind::ListenKeyExpired,
          UserDataEventKind::BalanceUpdate, UserDataEventKind::ExternalLockUpdate, UserDataEventKind::ListStatus,
          UserDataEventKind::EventStreamTerminated, UserDataEventKind::ServerShutdown}) {
        push("HY-A", kind);
    }

    drain();

    EXPECT_EQ(kill_.state(), KillState::Normal) << "an id in the coid field of another kind of event means nothing";
    EXPECT_EQ(alarm_->stats().reports, 0u);
    EXPECT_EQ(audit_.count(), 0u);
}

TEST_F(LateFillDrainTest, ATrackedOrderIsNotAlarmedEvenIfItsIdIsOnTheList) {
    list_ = {resolved_entry("HY-A")};
    arm();
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());  // only a re-used id can be both: the registry's order is the tracked one
    push("HY-A", UserDataEventKind::ExecutionReport, ExecutionType::New);

    drain();

    EXPECT_EQ(kill_.state(), KillState::Normal);
    EXPECT_EQ(alarm_->stats().reports, 0u);
    ASSERT_EQ(audit_.count(), 1u);
    EXPECT_EQ(record(0).event_type, AuditEventType::UserDataStreamEventObserved);
    EXPECT_STRNE(record(0).detail_msg, LateFillAlarm::kObservedMsg) << "the ordinary record of a tracked order, not the alarm's";
}

TEST_F(LateFillDrainTest, WithoutAnAlarmTheReportIsIgnoredAsBefore) {
    push("HY-A", UserDataEventKind::ExecutionReport, ExecutionType::Trade, "1.00000000");

    drain();  // alarm_ is not armed: the drain gets nullptr

    EXPECT_EQ(kill_.state(), KillState::Normal);
    EXPECT_EQ(audit_.count(), 0u);
}

TEST_F(LateFillDrainTest, ADrainWithoutAnAuditSinkStillTripsTheBreaker) {
    list_ = {resolved_entry("HY-A")};
    arm();
    push("HY-A", UserDataEventKind::ExecutionReport, ExecutionType::Trade, "0.50000000");

    drain_user_data_events(in_flight_, /*audit=*/nullptr, events_, kNow, &position_truth_, &fill_context_, /*barrier=*/nullptr, &*alarm_);

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(alarm_->stats().audit_failed, 2u);
}

TEST_F(LateFillDrainTest, AnUnwatchedUntrackedReportIsStillIgnoredSilently) {
    list_ = {resolved_entry("HY-A")};
    arm();
    push("HY-OTHER", UserDataEventKind::ExecutionReport, ExecutionType::Trade, "1.00000000");

    drain();

    EXPECT_EQ(kill_.state(), KillState::Normal) << "the late duplicate of an ordinary final order is not what this alarm is for";
    EXPECT_EQ(audit_.count(), 0u);
    EXPECT_EQ(alarm_->stats().reports, 0u);
}

TEST_F(LateFillDrainTest, AnOlderGenerationsReportStillRaisesTheAlarmBeforeTheBarrierCanDropIt) {
    list_ = {resolved_entry("HY-A")};
    arm();
    ReconnectBarrier barrier = reconciled_barrier(2);
    push("HY-A", UserDataEventKind::ExecutionReport, ExecutionType::Trade, "0.50000000", /*generation=*/1);

    drain(&barrier);

    EXPECT_EQ(barrier.stats().late_events_dropped, 1u) << "the barrier still drops it, as before: nothing else about the drain changed";
    EXPECT_EQ(kill_.state(), KillState::Triggered)
        << "the sweep that made generation 2 Reconciled reflects the orders it tracks; nothing sweeps a written-off one";
    EXPECT_EQ(alarm_->stats().reports, 1u);
}

TEST_F(LateFillDrainTest, TheAlarmDoesNotGetInTheWayOfTheBarriersShutdownHandling) {
    list_ = {resolved_entry("HY-A")};
    arm();
    ReconnectBarrier barrier = reconciled_barrier(1);
    ASSERT_EQ(barrier.state(), BarrierState::Reconciled);
    push("HY-A", UserDataEventKind::ExecutionReport, ExecutionType::Trade, "0.50000000", /*generation=*/1);
    push("HY-A", UserDataEventKind::ServerShutdown, ExecutionType::Unknown, "0", /*generation=*/1);

    drain(&barrier);

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(barrier.state(), BarrierState::Down) << "the serverShutdown after the alarmed report ended its generation as usual";
    EXPECT_EQ(barrier.stats().generation_ends_seen, 1u);
}

TEST_F(LateFillDrainTest, TheAlarmMovesNoPositionAndLeavesNoFillContext) {
    list_ = {resolved_entry("HY-A", /*symbol_id=*/7, OrderSide::Buy, 100)};
    arm();
    push("HY-A", UserDataEventKind::ExecutionReport, ExecutionType::Trade, "100");

    drain();

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(position_truth_.net_qty_ticks(7), 0)
        << "folding the fill in is K3 (resolution with a fill): the symbol and side are known here, the quantity scale is not";
    EXPECT_EQ(fill_context_.find("HY-A"), nullptr);
}

TEST_F(LateFillDrainTest, AMixedBatchRaisesTheAlarmOnlyForTheWrittenOffOrder) {
    list_ = {resolved_entry("HY-A")};
    arm();
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-T").valid());
    push("HY-T");                                              // tracked: the ordinary observed record
    push("HY-X", UserDataEventKind::ExecutionReport, ExecutionType::Trade, "1");  // nobody's: ignored
    push("HY-A");                                              // written off: the alarm

    drain();

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    ASSERT_EQ(audit_.count(), 3u);
    EXPECT_STREQ(record(0).client_order_id, "HY-T");
    EXPECT_STRNE(record(0).detail_msg, LateFillAlarm::kObservedMsg);
    EXPECT_STREQ(record(1).client_order_id, "HY-A");
    EXPECT_STREQ(record(1).detail_msg, LateFillAlarm::kObservedMsg);
    EXPECT_EQ(record(2).event_type, AuditEventType::KillSwitchTriggered);
    EXPECT_EQ(alarm_->stats().reports, 1u);
}

// === 3. the recovery round trip =======================================================================================================

namespace {

constexpr std::uint32_t kKeyId = 1;

std::vector<std::byte> plaintext_key() {
    static const char kKey[] = "late-fill-alarm-test-key";
    std::vector<std::byte> k(sizeof(kKey) - 1);
    std::memcpy(k.data(), kKey, k.size());
    return k;
}

struct OrderSpec {
    std::string coid;
    std::uint32_t symbol_id{7};
    OrderSide side{OrderSide::Sell};
    std::int64_t price{500'000};
    std::int64_t qty{100};
};

AuditRecord make_record(const OrderSpec& o, AuditEventType ev, OrderState st) {
    AuditRecord r{};
    r.timestamp_ms = 1000;
    r.event_type = ev;
    r.mode = ExecutionMode::Live;
    r.symbol_id = o.symbol_id;
    r.set_client_order_id(o.coid);
    r.price_ticks = o.price;
    r.qty_ticks = o.qty;
    r.resulting_state = st;
    r.side = o.side;
    return r;
}

AuditRecord intent(const OrderSpec& o) { return make_record(o, AuditEventType::OrderIntentCreated, OrderState::Intent); }
AuditRecord prepared(const OrderSpec& o) { return make_record(o, AuditEventType::OrderSubmitPrepared, OrderState::Submitting); }
AuditRecord ambiguous(const OrderSpec& o) { return make_record(o, AuditEventType::OrderAmbiguous, OrderState::Ambiguous); }

AuditRecord operator_resolved(const OrderSpec& o) {
    AuditRecord r = make_record(o, AuditEventType::OrderOperatorResolved, OrderState::Rejected);
    r.detail_code = static_cast<std::int64_t>(operator_resolution::kKindAbsent | operator_resolution::kAbsentRequiredBits |
                                              (std::uint64_t{270} << operator_resolution::kWaitedSecondsShift) |
                                              (std::uint64_t{1} << operator_resolution::kToolVersionShift));
    r.set_detail("op=test;ev=00000000000000000000000000000000");
    return r;
}

struct CraftedFrame {
    std::uint8_t version;
    AuditRecord rec;
};

CraftedFrame v5f(const AuditRecord& r) { return {kFrameFormatVersion, r}; }
CraftedFrame v6f(const AuditRecord& r) { return {kFrameFormatVersionV6, r}; }

struct Crafted {
    std::vector<std::byte> log;
    std::array<std::byte, kTipAnchorSize> anchor{};
    bool ok{false};
};

Crafted craft(const KeyRing& ring, const std::vector<CraftedFrame>& frames) {
    Crafted c;
    std::array<std::byte, kKeyBlockSize> key_block{};
    if (!ring.active_key(kKeyId, key_block)) return c;
    const std::span<const std::byte> key(key_block.data(), key_block.size());
    std::array<std::byte, kMacLen> prev{};
    std::uint64_t seq = 0;
    c.log.reserve(frames.size() * kOrderEventFrameSize);
    for (const CraftedFrame& f : frames) {
        std::array<std::byte, kOrderEventFrameSize> buf{};
        const auto n = encode_order_event_frame(buf, kKeyId, seq, FrameTimeKind::ServerCorrectedUtc, static_cast<std::int64_t>(1000 + seq), f.rec,
                                                prev, key, f.version);
        if (n != kOrderEventFrameSize) return c;
        std::memcpy(prev.data(), buf.data() + kOrderEventFrameSize - kMacLen, kMacLen);
        c.log.insert(c.log.end(), buf.begin(), buf.end());
        ++seq;
    }
    if (!frames.empty()) detail::encode_tip_anchor(c.anchor, seq - 1, prev, kKeyId, key);
    c.ok = true;
    return c;
}

}  // namespace

class LateFillRecoveryTest : public LateFillDrainTest {
protected:
    std::string stem_;
    std::string base_path_;
    unsigned used_{0};
    std::unique_ptr<KeyRing> ring_;
    std::unique_ptr<DurableAuditSink> sink_;

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        stem_ = std::string(tmp) + "hy_latefill_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_latefill_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x30 + i);
        ring_ = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        ASSERT_EQ(ring_->add_key(kKeyId, plaintext_key(), rec), KeyRingAddStatus::Ok);
    }

    void TearDown() override {
        alarm_.reset();  // the alarm views the sink's list: it goes first
        sink_.reset();
        remove_all();
    }

    void remove_all() {
        if (base_path_.empty()) return;
        for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock", ".keyrotations.tip",
                                   ".keyrotations.tip.tmp", ".storeid", ".storeid.lock", ".storeid.tip", ".storeid.tip.tmp"}) {
            std::remove((base_path_ + suffix).c_str());
        }
    }

    // Lays the frames down as a log + tip anchor on a fresh path, the way a writer that died right after an append would have left it.
    void write_frames(const std::vector<CraftedFrame>& frames) {
        const Crafted c = craft(*ring_, frames);
        ASSERT_TRUE(c.ok) << "the crafted log could not be built";
        sink_.reset();
        remove_all();
        base_path_ = stem_ + "_" + std::to_string(++used_) + ".log";
        DurableLogStore store(base_path_, base_path_ + ".lock", base_path_ + ".tip");
        ASSERT_TRUE(store.acquire_lock());
        ASSERT_TRUE(store.open_log());
        ASSERT_TRUE(store.append_and_fsync(std::span<const std::byte>(c.log.data(), c.log.size())));
        ASSERT_TRUE(store.write_tip_anchor(std::span<const std::byte>(c.anchor.data(), c.anchor.size())));
        store.close_log();
        store.release_lock();
    }

    void open(AuditSinkOpenMode mode = AuditSinkOpenMode::Trading) {
        sink_ = std::make_unique<DurableAuditSink>(base_path_, *ring_, kKeyId, kMaxIntentOrphans, mode);
    }

    // The alarm exactly as a trading process would build it from the store it recovered.
    LateFillAlarm& arm_from_sink() {
        alarm_.emplace(sink_->recovered_operator_resolved(), sink_->recovered_operator_resolved_total(), kill_);
        return *alarm_;
    }
};

TEST_F(LateFillRecoveryTest, TheListRecoveryHandsOutIsTheListTheAlarmWatches) {
    const OrderSpec written_off{"HY-LF-A", 7, OrderSide::Sell, 500'000, 100};
    const OrderSpec stuck{"HY-LF-B", 8, OrderSide::Buy, 400'000, 50};
    const OrderSpec second_written_off{"HY-LF-C", 9, OrderSide::Buy, 300'000, 25};
    write_frames({v5f(intent(written_off)), v5f(prepared(written_off)), v5f(intent(stuck)), v5f(prepared(stuck)), v5f(ambiguous(stuck)),
                  v5f(intent(second_written_off)), v5f(prepared(second_written_off)), v6f(operator_resolved(written_off)),
                  v6f(operator_resolved(second_written_off))});
    open();
    ASSERT_EQ(sink_->recovery_status(), RecoveryScanStatus::Recovered);
    ASSERT_EQ(sink_->recovered_operator_resolved().size(), 2u);
    ASSERT_EQ(sink_->recovered_checkpoints().size(), 1u) << "the stuck order is still a checkpoint -- reconciliation's business, not the alarm's";

    LateFillAlarm& alarm = arm_from_sink();
    EXPECT_TRUE(alarm.complete());
    EXPECT_EQ(alarm.watched(), 2u);
    EXPECT_EQ(kill_.state(), KillState::Normal);
    EXPECT_TRUE(alarm.watches("HY-LF-A"));
    EXPECT_TRUE(alarm.watches("HY-LF-C"));
    EXPECT_FALSE(alarm.watches("HY-LF-B")) << "an order still being reconciled is not a written-off one";

    push("HY-LF-B", UserDataEventKind::ExecutionReport, ExecutionType::Trade, "0.10000000");  // the stuck order reports: not the alarm's
    drain();
    EXPECT_EQ(kill_.state(), KillState::Normal);
    EXPECT_EQ(audit_.count(), 0u);

    push("HY-LF-C", UserDataEventKind::ExecutionReport, ExecutionType::Trade, "0.05000000");  // the second written-off order fills
    drain();

    EXPECT_EQ(kill_.state(), KillState::Triggered);
    ASSERT_EQ(audit_.count(), 2u);
    EXPECT_STREQ(record(0).client_order_id, "HY-LF-C");
    EXPECT_EQ(record(0).symbol_id, 9u) << "straight from the recovered entry";
    EXPECT_EQ(record(0).side, OrderSide::Buy);
    EXPECT_EQ(record(0).qty_ticks, 25);
}

TEST_F(LateFillRecoveryTest, AnOperatorOpenScanThatOverflowedGivesAnAlarmThatCannotBeTrusted) {
    // kMaxOperatorResolved + 1 written-off orders: a trading open refuses such a store (CapacityExceeded); the tool's OperatorOpen mode keeps the
    // first kMaxOperatorResolved and counts the rest. An alarm built from that list says so through the total, and stops the process at once.
    // (A store's frame versions never go back, so every v5 frame comes before the first v6 one.)
    std::vector<OrderSpec> orders;
    orders.reserve(kMaxOperatorResolved + 1);
    for (std::size_t i = 0; i < kMaxOperatorResolved + 1; ++i) {
        orders.push_back(OrderSpec{"HY-LF-N" + std::to_string(i), 7, OrderSide::Sell, 500'000, 100});
    }
    std::vector<CraftedFrame> frames;
    frames.reserve(orders.size() * 3);
    for (const OrderSpec& o : orders) {
        frames.push_back(v5f(intent(o)));
        frames.push_back(v5f(prepared(o)));
    }
    for (const OrderSpec& o : orders) frames.push_back(v6f(operator_resolved(o)));
    write_frames(frames);
    open(AuditSinkOpenMode::OperatorOpen);
    ASSERT_FALSE(sink_->fenced()) << "the crafted log must be a valid one";
    ASSERT_EQ(sink_->recovered_operator_resolved().size(), kMaxOperatorResolved);
    ASSERT_EQ(sink_->recovered_operator_resolved_total(), kMaxOperatorResolved + 1);

    LateFillAlarm& alarm = arm_from_sink();

    EXPECT_FALSE(alarm.complete());
    EXPECT_EQ(kill_.state(), KillState::Triggered);
    EXPECT_EQ(alarm.watched(), kMaxOperatorResolved);
}

TEST_F(LateFillRecoveryTest, AStoreWithNoWrittenOffOrderGivesAnAlarmThatWatchesNothing) {
    const OrderSpec stuck{"HY-LF-B", 8, OrderSide::Buy, 400'000, 50};
    write_frames({v5f(intent(stuck)), v5f(prepared(stuck)), v5f(ambiguous(stuck))});
    open();

    LateFillAlarm& alarm = arm_from_sink();

    EXPECT_TRUE(alarm.complete());
    EXPECT_EQ(alarm.watched(), 0u);
    EXPECT_EQ(kill_.state(), KillState::Normal);
}
