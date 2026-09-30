// 批次 6 6b-0d: startup_recovery.hpp tests.
//
// The recovery tests use a REAL DurableAuditSink on a real temp file, and the real reconcile path
// (apply_recovery -> poll_once -> drain_reconcile_events) against a fake exchange -- the point of
// this batch is that recovery actually happens, so nothing between the log and PositionTruth is
// mocked except the exchange itself. A "restart" is a destroyed sink re-opened over the same path
// (recovery runs in the constructor), exactly as test_durable_audit_sink.cpp does it.

#include <gtest/gtest.h>
#include <hengyuan/startup_recovery.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace hy;

namespace {

// --- durable-log record builders (same shapes test_durable_audit_sink.cpp uses) -----------------

AuditRecord make_intent(const char* coid, std::int64_t price, std::int64_t qty, std::uint32_t symbol_id,
                        OrderSide side = OrderSide::Buy) {
    AuditRecord rec{};
    rec.timestamp_ms = 1000;
    rec.event_type = AuditEventType::OrderIntentCreated;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.price_ticks = price;
    rec.qty_ticks = qty;
    rec.resulting_state = OrderState::Intent;
    rec.side = side;
    return rec;
}

AuditRecord make_submitted(const char* coid, std::int64_t price, std::int64_t qty, std::uint32_t symbol_id) {
    AuditRecord rec{};
    rec.timestamp_ms = 1001;
    rec.event_type = AuditEventType::OrderSubmitted;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.price_ticks = price;
    rec.qty_ticks = qty;
    rec.resulting_state = OrderState::Submitting;
    return rec;
}

AuditRecord make_accepted(const char* coid, std::uint32_t symbol_id, std::int64_t exch_id) {
    AuditRecord rec{};
    rec.timestamp_ms = 1002;
    rec.event_type = AuditEventType::OrderAccepted;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.exchange_order_id = exch_id;
    rec.resulting_state = OrderState::Accepted;
    return rec;
}

AuditRecord make_partial_fill(const char* coid, std::uint32_t symbol_id, std::int64_t exch_id,
                              std::int64_t filled_qty, std::int64_t avg_price) {
    AuditRecord rec{};
    rec.timestamp_ms = 1003;
    rec.event_type = AuditEventType::OrderPartialFill;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.exchange_order_id = exch_id;
    rec.resulting_state = OrderState::PartialFill;
    rec.filled_qty_ticks = filled_qty;
    rec.avg_fill_price_ticks = avg_price;
    return rec;
}

std::vector<std::byte> test_key() {
    static const char kKey[] = "startup-recovery-test-key";
    std::vector<std::byte> k(sizeof(kKey) - 1);
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::byte>(kKey[i]);
    return k;
}

// --- fake exchange behind the real QueryPort ---------------------------------------------------

struct FakeExchange {
    QueryOutcome outcome{QueryOutcome::Inconclusive};
    OrderState state{OrderState::Filled};
    std::int64_t filled_qty{0};
    std::int64_t exchange_order_id{555};
    int calls{0};
};

QueryResult fake_query(const OrderExpectation&, void* user_data) {
    auto* fx = static_cast<FakeExchange*>(user_data);
    ++fx->calls;
    QueryResult r{};
    r.outcome = fx->outcome;
    r.confirmed_state = fx->state;
    r.exchange_order_id = fx->exchange_order_id;
    r.filled_qty_ticks = fx->filled_qty;
    r.avg_fill_price_ticks = fx->filled_qty > 0 ? 100 : 0;
    return r;
}

// Symbol 1 is known (scales 2/2/4); anything else is "unknown symbol" (rules_version == 0).
SymbolRules rules_for_test(std::uint32_t symbol_id) {
    SymbolRules r{};
    if (symbol_id == 1) {
        r.rules_version = 1;
        r.price_scale = 2;
        r.qty_scale = 2;
        r.quote_scale = 4;
    }
    return r;
}

RecoveryObservation healthy_env_obs() {
    RecoveryObservation o;
    o.clock_synced = true;
    o.exchange_info_loaded = true;
    o.sink_open = true;
    o.sink_fenced = false;
    o.scan_status = RecoveryScanStatus::Clean;
    o.account_fresh = true;
    o.user_data_connected = true;
    return o;
}

}  // namespace

// ================================================================================================
// StartupRecovery state machine -- pure, no sink
// ================================================================================================

TEST(StartupRecoveryStateMachine, DefaultObservationFailsClosedToDegraded) {
    StartupRecovery sm(10'000);
    EXPECT_EQ(sm.update(RecoveryObservation{}, 0), RunState::Degraded);
    EXPECT_EQ(sm.degraded_reason(), DegradedReason::SinkNotOpen);
}

TEST(StartupRecoveryStateMachine, BootstrapsUntilClockAndExchangeInfoAreReady) {
    StartupRecovery sm(10'000);
    auto o = healthy_env_obs();
    o.clock_synced = false;
    EXPECT_EQ(sm.update(o, 0), RunState::Bootstrapping);
    o.clock_synced = true;
    o.exchange_info_loaded = false;
    EXPECT_EQ(sm.update(o, 0), RunState::Bootstrapping);
    o.exchange_info_loaded = true;
    EXPECT_EQ(sm.update(o, 0), RunState::Recovering);  // waiting for apply_recovery()
}

TEST(StartupRecoveryStateMachine, EachUnhealthyDurableLogConditionDegradesWithItsOwnReason) {
    {
        StartupRecovery sm(10'000);
        auto o = healthy_env_obs();
        o.sink_fenced = true;
        EXPECT_EQ(sm.update(o, 0), RunState::Degraded);
        EXPECT_EQ(sm.degraded_reason(), DegradedReason::SinkFenced);
    }
    for (const auto status : {RecoveryScanStatus::Corrupt, RecoveryScanStatus::CapacityExceeded,
                              RecoveryScanStatus::IoError, RecoveryScanStatus::ExternalAnchorUnavailable}) {
        StartupRecovery sm(10'000);
        auto o = healthy_env_obs();
        o.scan_status = status;
        EXPECT_EQ(sm.update(o, 0), RunState::Degraded);
        EXPECT_EQ(sm.degraded_reason(), DegradedReason::RecoveryScanFailed);
    }
}

TEST(StartupRecoveryStateMachine, LogHealthIsCheckedInEveryStateNotJustAtStart) {
    StartupRecovery sm(10'000);
    auto o = healthy_env_obs();
    o.recovery_applied = true;
    ASSERT_EQ(sm.update(o, 0), RunState::Ready);
    o.sink_fenced = true;  // e.g. an append failed later and fenced the writer
    EXPECT_EQ(sm.update(o, 1), RunState::Degraded);
    EXPECT_EQ(sm.degraded_reason(), DegradedReason::SinkFenced);
}

TEST(StartupRecoveryStateMachine, IncompleteApplicationDegrades) {
    StartupRecovery sm(10'000);
    auto o = healthy_env_obs();
    o.recovery_applied = true;
    o.recovered_orders = 3;
    o.applied_orders = 2;
    EXPECT_EQ(sm.update(o, 0), RunState::Degraded);
    EXPECT_EQ(sm.degraded_reason(), DegradedReason::RepopulateIncomplete);
}

TEST(StartupRecoveryStateMachine, ReconcilingWaitsForEveryOrderToBeKnown) {
    StartupRecovery sm(10'000);
    auto o = healthy_env_obs();
    o.recovery_applied = true;
    o.recovered_orders = o.applied_orders = 1;
    o.unconfirmed_orders = 1;
    EXPECT_EQ(sm.update(o, 0), RunState::Reconciling);
    EXPECT_FALSE(sm.submit_enabled(o));
    o.unconfirmed_orders = 0;
    EXPECT_EQ(sm.update(o, 100), RunState::Ready);
    EXPECT_TRUE(sm.submit_enabled(o));
}

TEST(StartupRecoveryStateMachine, ReadyAlsoNeedsAFreshAccountAndAConnectedUserDataStream) {
    StartupRecovery sm(10'000);
    auto o = healthy_env_obs();
    o.recovery_applied = true;
    o.account_fresh = false;
    EXPECT_EQ(sm.update(o, 0), RunState::Reconciling);
    o.account_fresh = true;
    o.user_data_connected = false;
    EXPECT_EQ(sm.update(o, 1), RunState::Reconciling);
    o.user_data_connected = true;
    EXPECT_EQ(sm.update(o, 2), RunState::Ready);
}

// Ready is necessary, not sufficient: losing a live prerequisite switches submission off again
// without any state transition.
TEST(StartupRecoveryStateMachine, SubmitEnabledDropsWhenALivePrerequisiteIsLostAfterReady) {
    StartupRecovery sm(10'000);
    auto o = healthy_env_obs();
    o.recovery_applied = true;
    ASSERT_EQ(sm.update(o, 0), RunState::Ready);
    ASSERT_TRUE(sm.submit_enabled(o));

    auto lost_user_data = o;
    lost_user_data.user_data_connected = false;
    EXPECT_FALSE(sm.submit_enabled(lost_user_data));
    auto stale_account = o;
    stale_account.account_fresh = false;
    EXPECT_FALSE(sm.submit_enabled(stale_account));
    auto lost_clock = o;
    lost_clock.clock_synced = false;
    EXPECT_FALSE(sm.submit_enabled(lost_clock));
    EXPECT_EQ(sm.state(), RunState::Ready);  // state itself unchanged
}

TEST(StartupRecoveryStateMachine, ReconcileDeadlineIsStrictAndDegradesStickily) {
    StartupRecovery sm(1'000);
    auto o = healthy_env_obs();
    o.recovery_applied = true;
    o.recovered_orders = o.applied_orders = 1;
    o.unconfirmed_orders = 1;
    ASSERT_EQ(sm.update(o, 5'000), RunState::Reconciling);  // deadline counts from entering it
    EXPECT_EQ(sm.update(o, 6'000), RunState::Reconciling);  // exactly at the deadline: not yet
    EXPECT_EQ(sm.update(o, 6'001), RunState::Degraded);
    EXPECT_EQ(sm.degraded_reason(), DegradedReason::ReconcileTimeout);

    // Sticky: a perfect observation afterwards changes nothing, and submission stays off.
    auto perfect = healthy_env_obs();
    perfect.recovery_applied = true;
    EXPECT_EQ(sm.update(perfect, 7'000), RunState::Degraded);
    EXPECT_FALSE(sm.submit_enabled(perfect));
}

TEST(RunStateNames, AreDistinct) {
    const RunState all[] = {RunState::Bootstrapping, RunState::Recovering, RunState::Reconciling,
                            RunState::Ready, RunState::Degraded};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        EXPECT_STRNE(run_state_name(all[i]), "?");
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            EXPECT_STRNE(run_state_name(all[i]), run_state_name(all[j]));
        }
    }
    EXPECT_STRNE(degraded_reason_name(DegradedReason::ReconcileTimeout), "?");
}

// ================================================================================================
// Recovery against a REAL durable log
// ================================================================================================

class StartupRecoveryLogTest : public ::testing::Test {
protected:
    std::string path_;
    std::unique_ptr<KeyRing> key_ring_;

    // Everything apply_recovery()/poll_once()/drain_reconcile_events() touch.
    InFlightRegistry in_flight_;
    OrderTracker tracker_;
    ToReconcileRing to_reconcile_;
    ReconcileEventRing reconcile_events_;
    PositionTruth truth_;
    OrderFillContext fill_context_;
    FakeExchange exchange_;
    ReconcilePollPolicy policy_{};

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        path_ = std::string(tmp) + "hy_startup_recovery_" + std::to_string(GetCurrentProcessId()) + "_" +
                std::to_string(counter) + ".log";
#else
        path_ = "/tmp/hy_startup_recovery_" + std::to_string(getpid()) + "_" + std::to_string(counter) + ".log";
#endif
        remove_all();
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x30 + i);
        key_ring_ = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        ASSERT_EQ(key_ring_->add_key(1, test_key(), rec), KeyRingAddStatus::Ok);
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock",
                                   ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid",
                                   ".storeid.lock", ".storeid.tip", ".storeid.tip.tmp"}) {
            std::remove((path_ + suffix).c_str());
        }
    }

    void corrupt_log_byte(std::size_t offset) {
        std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        f.seekg(static_cast<std::streamoff>(offset));
        char b = 0;
        f.read(&b, 1);
        b = static_cast<char>(b ^ 0x01);
        f.seekp(static_cast<std::streamoff>(offset));
        f.write(&b, 1);
    }

    QueryPort port() { return QueryPort{&fake_query, &exchange_}; }

    // One reconcile-loop tick, exactly as a main loop runs it. `wall_ms` is the wall clock (R-10),
    // or OrderRecord::kClockUnset for a loop that has none.
    void tick(std::int64_t now_ms, std::int64_t wall_ms = OrderRecord::kClockUnset) {
        poll_once(tracker_, to_reconcile_, reconcile_events_, port(), policy_, now_ms, wall_ms);
        drain_reconcile_events(in_flight_, nullptr, reconcile_events_, now_ms, &truth_, &fill_context_);
    }

    RecoveryApplyResult apply(const DurableAuditSink& sink, std::int64_t now_wall_ms = OrderRecord::kClockUnset) {
        return apply_recovery(sink.recovered_checkpoints(), in_flight_, to_reconcile_, truth_, fill_context_,
                              &rules_for_test, now_wall_ms);
    }

    RecoveryObservation observe(const DurableAuditSink& sink, const RecoveryApplyResult& applied) {
        auto o = healthy_env_obs();
        o.sink_open = sink.is_open();
        o.sink_fenced = sink.fenced();
        o.scan_status = sink.recovery_status();
        o.recovery_applied = true;
        o.recovered_orders = applied.recovered;
        o.applied_orders = applied.applied;
        o.unconfirmed_orders = tally_recovered_orders(in_flight_, tracker_, sink.recovered_checkpoints()).unconfirmed;
        return o;
    }
};

// A crash while Submitting recovers as Ambiguous -- the case where this process genuinely does
// not know whether the order exists. Nothing may be submitted until the exchange has answered.
TEST_F(StartupRecoveryLogTest, AmbiguousRecoveredOrderBlocksReadyUntilTheExchangeAnswers) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }  // "crash"

    DurableAuditSink restarted(path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    ASSERT_EQ(restarted.recovered_checkpoints().size(), 1u);
    ASSERT_EQ(restarted.recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);

    const RecoveryApplyResult applied = apply(restarted);
    ASSERT_TRUE(applied.ok());
    EXPECT_TRUE(in_flight_.is_in_flight("HY-A"));

    StartupRecovery sm(60'000);
    exchange_.outcome = QueryOutcome::Inconclusive;  // exchange unreachable / no answer yet
    tick(0);
    EXPECT_EQ(sm.update(observe(restarted, applied), 0), RunState::Reconciling);
    EXPECT_FALSE(sm.submit_enabled(observe(restarted, applied)));
    EXPECT_GE(exchange_.calls, 1);  // it really did go and ask, by clientOrderId

    // The exchange finally answers: the order was filled while we were down.
    exchange_.outcome = QueryOutcome::Found;
    exchange_.state = OrderState::Filled;
    exchange_.filled_qty = 10;
    tick(1'000);
    EXPECT_FALSE(in_flight_.is_in_flight("HY-A"));  // released once final
    EXPECT_EQ(truth_.net_qty_ticks(1), 10);          // and the fill is now in PositionTruth
    EXPECT_EQ(sm.update(observe(restarted, applied), 1'000), RunState::Ready);
    EXPECT_TRUE(sm.submit_enabled(observe(restarted, applied)));
}

// --- R-10 / L-30: the unresolved time survives a restart ---
//
// With frames stamped by a real wall time, the durable "first became uncertain" time seeds the order's
// wall anchor: the unresolved time is measured from the log, not from this process's own first sight
// of the order. Lenient rule (Owner's default): the time the process was DOWN does not count until the
// order has been sent one query in this life -- every recovered order gets one authoritative look.

namespace {
constexpr std::int64_t kT0 = 1'790'000'000'000;  // a plausible wall time (2026-09)
}

// A planned restart, an hour of downtime, an exchange that answers: the order must get its first
// query BEFORE any quarantine. Held against it unseen, that hour would put every restart that leaves
// an order unresolved into Degraded for want of a single GET.
TEST_F(StartupRecoveryLogTest, AnOrderLeftUnresolvedByAPlannedRestartGetsItsFirstQueryBeforeAnyQuarantine) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), kT0).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), kT0 + 100).acked());
    }
    DurableAuditSink restarted(path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovered_checkpoints().size(), 1u);
    constexpr std::int64_t kRestartWall = kT0 + 3'600'000;  // an hour of downtime
    ASSERT_TRUE(apply(restarted, kRestartWall).ok());

    exchange_.outcome = QueryOutcome::Found;
    exchange_.state = OrderState::Filled;
    exchange_.filled_qty = 10;
    tick(0, kRestartWall);

    EXPECT_EQ(exchange_.calls, 1) << "the order was asked about first";
    EXPECT_FALSE(in_flight_.is_in_flight("HY-A")) << "and it resolved: it was not quarantined unseen";
    EXPECT_EQ(truth_.net_qty_ticks(1), 10) << "the fill reached PositionTruth, which an escalation would not do";
}

TEST_F(StartupRecoveryLogTest, AnOrderStillUnansweredAfterItsFirstQueryIsQuarantinedAtOnce) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), kT0).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), kT0 + 100).acked());
    }
    DurableAuditSink restarted(path_, *key_ring_, 1);
    constexpr std::int64_t kRestartWall = kT0 + 3'600'000;
    ASSERT_TRUE(apply(restarted, kRestartWall).ok());
    exchange_.outcome = QueryOutcome::Inconclusive;  // the exchange is not answering

    tick(0, kRestartWall);
    EXPECT_EQ(exchange_.calls, 1);
    EXPECT_EQ(tracker_.count(), 1u) << "the first look comes before the quarantine";

    tick(100, kRestartWall + 100);
    EXPECT_EQ(tracker_.count(), 0u) << "the hour of unresolved time now counts: quarantined";
    EXPECT_EQ(exchange_.calls, 1) << "escalation fires no further query";
    EXPECT_TRUE(in_flight_.is_in_flight("HY-A")) << "a quarantined order keeps its slot";
}

// L-30's point: a process that keeps crashing and restarting does not keep starting the clock over.
TEST_F(StartupRecoveryLogTest, ACrashLoopDoesNotResetTheUnresolvedTime) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), kT0).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), kT0 + 100).acked());
    }  // the first process dies with the order Submitting
    exchange_.outcome = QueryOutcome::Inconclusive;  // the exchange never answers

    // One "life" of the process: a fresh registry, tracker and rings (a restart forgets all of memory),
    // recovery from the SAME durable log, and a reconcile loop ticking every 100 ms of its own monotonic
    // clock (which starts at 0) against a wall clock that read `wall_start` at that moment. Returns the
    // wall time of the tick at which the order was quarantined, or -1.
    auto live = [&](std::int64_t wall_start, std::int64_t lifetime_ms) -> std::int64_t {
        DurableAuditSink sink(path_, *key_ring_, 1);
        InFlightRegistry in_flight;
        OrderTracker tracker;
        ToReconcileRing to_reconcile;
        ReconcileEventRing events;
        PositionTruth truth;
        OrderFillContext fill_context;
        const RecoveryApplyResult applied = apply_recovery(sink.recovered_checkpoints(), in_flight, to_reconcile,
                                                            truth, fill_context, &rules_for_test, wall_start);
        EXPECT_TRUE(applied.ok());
        for (std::int64_t mono = 0; mono <= lifetime_ms; mono += 100) {
            poll_once(tracker, to_reconcile, events, port(), policy_, mono, wall_start + mono);
            drain_reconcile_events(in_flight, nullptr, events, mono, &truth, &fill_context);
            if (tracker.count() == 0) return wall_start + mono;  // only an escalation empties it here
        }
        return -1;
    };

    EXPECT_EQ(live(kT0 + 3'000, 4'000), -1) << "life 1 ends 7 s after the order became uncertain";
    EXPECT_EQ(live(kT0 + 9'000, 3'000), -1) << "life 2 ends 12 s after it";
    // Life 3 lives 2 s. Counted from its own start it could never reach 15 s; counted from the durable
    // log it is quarantined the moment 15 s have passed since the order became uncertain (the
    // Submitting frame, kT0 + 100), one query into its life.
    EXPECT_EQ(live(kT0 + 14'500, 2'000), kT0 + 15'100);
}

// A frame time ahead of the wall clock this process reads is not trusted: it would sit in the anchor
// (nothing moves a set anchor) and silence the wall clock for that order. apply_recovery() must hand
// the wall clock down so the check can see it.
TEST_F(StartupRecoveryLogTest, AFrameTimeAheadOfTheWallClockIsNotTrustedAndSeedsNoAnchor) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), kT0 + 3'600'000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), kT0 + 3'600'100).acked());
    }
    DurableAuditSink restarted(path_, *key_ring_, 1);
    ASSERT_TRUE(apply(restarted, /*now_wall_ms=*/kT0).ok());
    exchange_.outcome = QueryOutcome::Inconclusive;
    tick(0, kT0);

    bool found = false;
    tracker_.for_each_active([&](InFlightHandle, OrderRecord& record, std::int64_t) {
        found = true;
        EXPECT_FALSE(record.wall_anchor_recovered);
        EXPECT_EQ(record.unknown_since_wall_ms, kT0) << "stamped by this process at first sight instead";
    });
    EXPECT_TRUE(found);
}

// THE double-count regression. The order was already 4/10 filled when we crashed, so seeding puts
// +4 in PositionTruth. When reconcile then reports the final cumulative 10, only the +6 that
// happened AFTER the crash may be added -- not +10.
TEST_F(StartupRecoveryLogTest, RecoveredPartialFillIsNotCountedTwiceWhenReconcileReportsTheFinalFill) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_partial_fill("HY-A", 1, 555, 4, 100), 1002).acked());
    }
    DurableAuditSink restarted(path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovered_checkpoints().size(), 1u);
    ASSERT_EQ(restarted.recovered_checkpoints()[0].resulting_state, OrderState::PartialFill);

    const RecoveryApplyResult applied = apply(restarted);
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(truth_.net_qty_ticks(1), 4);  // seeded from the log

    exchange_.outcome = QueryOutcome::Found;
    exchange_.state = OrderState::Filled;
    exchange_.filled_qty = 10;  // cumulative
    tick(0);

    EXPECT_EQ(truth_.net_qty_ticks(1), 10);  // 4 seeded + 6 new. NOT 14.
    EXPECT_FALSE(in_flight_.is_in_flight("HY-A"));
}

TEST_F(StartupRecoveryLogTest, RecoveredSellOrderFoldsWithTheRightSign) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-S", 100, 10, 1, OrderSide::Sell), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-S", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_partial_fill("HY-S", 1, 556, 3, 100), 1002).acked());
    }
    DurableAuditSink restarted(path_, *key_ring_, 1);
    const RecoveryApplyResult applied = apply(restarted);
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(truth_.net_qty_ticks(1), -3);

    exchange_.outcome = QueryOutcome::Found;
    exchange_.state = OrderState::Filled;
    exchange_.filled_qty = 10;
    tick(0);
    EXPECT_EQ(truth_.net_qty_ticks(1), -10);  // -3 seeded, -7 more; never -13
}

// A resting order we knew about at the crash is a KNOWN state, so it does not block Ready -- but it
// stays in flight, which is what keeps the planner from stacking a second order on top of it.
TEST_F(StartupRecoveryLogTest, KnownLiveRecoveredOrderReachesReadyButStaysInFlight) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
    }
    DurableAuditSink restarted(path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovered_checkpoints()[0].resulting_state, OrderState::Accepted);
    const RecoveryApplyResult applied = apply(restarted);
    ASSERT_TRUE(applied.ok());

    StartupRecovery sm(60'000);
    exchange_.outcome = QueryOutcome::Found;
    exchange_.state = OrderState::Accepted;  // still resting, unchanged
    exchange_.filled_qty = 0;
    tick(0);

    const auto tally = tally_recovered_orders(in_flight_, tracker_, restarted.recovered_checkpoints());
    EXPECT_EQ(tally.known_live, 1u);
    EXPECT_EQ(tally.unconfirmed, 0u);
    EXPECT_EQ(sm.update(observe(restarted, applied), 0), RunState::Ready);
    EXPECT_EQ(in_flight_.count(), 1u);  // still in flight -> the planner's one-order rule holds
}

// An order the exchange never answers about escalates to the operator after a few attempts. That
// is exactly the case that must NOT become Ready, and the failure must be permanent.
TEST_F(StartupRecoveryLogTest, AnOrderThatStaysUnknownTimesOutIntoStickyDegraded) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    DurableAuditSink restarted(path_, *key_ring_, 1);
    const RecoveryApplyResult applied = apply(restarted);
    ASSERT_TRUE(applied.ok());

    StartupRecovery sm(10'000);
    exchange_.outcome = QueryOutcome::Inconclusive;  // never answers
    RunState state = RunState::Bootstrapping;
    for (std::int64_t t = 0; t <= 12'000; t += 250) {
        tick(t);
        state = sm.update(observe(restarted, applied), t);
    }
    EXPECT_EQ(state, RunState::Degraded);
    EXPECT_EQ(sm.degraded_reason(), DegradedReason::ReconcileTimeout);

    // The exchange finally starts answering -- too late: Degraded has no way out but a restart.
    exchange_.outcome = QueryOutcome::Found;
    exchange_.state = OrderState::Filled;
    exchange_.filled_qty = 10;
    for (std::int64_t t = 13'000; t <= 16'000; t += 250) {
        tick(t);
        EXPECT_EQ(sm.update(observe(restarted, applied), t), RunState::Degraded);
    }
    EXPECT_FALSE(sm.submit_enabled(observe(restarted, applied)));
}

TEST_F(StartupRecoveryLogTest, FreshLogWithNothingToRecoverReachesReadyImmediately) {
    DurableAuditSink sink(path_, *key_ring_, 1);
    ASSERT_TRUE(sink.is_open());
    ASSERT_EQ(sink.recovery_status(), RecoveryScanStatus::Clean);
    const RecoveryApplyResult applied = apply(sink);
    EXPECT_EQ(applied.recovered, 0u);
    EXPECT_TRUE(applied.ok());

    StartupRecovery sm(10'000);
    EXPECT_EQ(sm.update(observe(sink, applied), 0), RunState::Ready);
    EXPECT_TRUE(sm.submit_enabled(observe(sink, applied)));
}

TEST_F(StartupRecoveryLogTest, ACorruptedLogFencesTheSinkAndDegradesImmediately) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    corrupt_log_byte(20);  // inside the first frame

    DurableAuditSink restarted(path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.fenced());  // recovery could not account for the log

    const RecoveryApplyResult applied = apply(restarted);
    StartupRecovery sm(10'000);
    EXPECT_EQ(sm.update(observe(restarted, applied), 0), RunState::Degraded);
    EXPECT_FALSE(sm.submit_enabled(observe(restarted, applied)));
}

TEST_F(StartupRecoveryLogTest, ApplyingRecoveryFailsClosedOnAnUnknownSymbol) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        // symbol 99 is not in rules_for_test(): it cannot be scaled, so it cannot be reconciled.
        ASSERT_TRUE(sink.append_durable(make_intent("HY-X", 100, 10, 99), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-X", 100, 10, 99), 1001).acked());
    }
    DurableAuditSink restarted(path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovered_checkpoints().size(), 1u);

    const RecoveryApplyResult applied = apply(restarted);
    EXPECT_FALSE(applied.ok());
    EXPECT_EQ(applied.applied, 0u);
    EXPECT_EQ(in_flight_.count(), 0u);  // nothing half-registered

    StartupRecovery sm(10'000);
    EXPECT_EQ(sm.update(observe(restarted, applied), 0), RunState::Degraded);
    EXPECT_EQ(sm.degraded_reason(), DegradedReason::RepopulateIncomplete);
}

TEST_F(StartupRecoveryLogTest, TallyClassifiesResolvedKnownLiveAndUnconfirmed) {
    {
        DurableAuditSink sink(path_, *key_ring_, 1);
        // A: crashed Submitting -> Ambiguous.  B: known Accepted.  Both symbol 1.
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-B", 100, 10, 1), 1002).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-B", 100, 10, 1), 1003).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-B", 1, 556), 1004).acked());
    }
    DurableAuditSink restarted(path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovered_checkpoints().size(), 2u);
    ASSERT_TRUE(apply(restarted).ok());

    // Before any tick nothing is tracked yet: both are unconfirmed.
    auto before = tally_recovered_orders(in_flight_, tracker_, restarted.recovered_checkpoints());
    EXPECT_EQ(before.total, 2u);
    EXPECT_EQ(before.unconfirmed, 2u);

    // The exchange says: everything is Accepted (A becomes known-live too).
    exchange_.outcome = QueryOutcome::Found;
    exchange_.state = OrderState::Accepted;
    exchange_.filled_qty = 0;
    tick(0);
    auto after = tally_recovered_orders(in_flight_, tracker_, restarted.recovered_checkpoints());
    EXPECT_EQ(after.known_live, 2u);
    EXPECT_EQ(after.unconfirmed, 0u);
    EXPECT_EQ(after.resolved, 0u);
}
