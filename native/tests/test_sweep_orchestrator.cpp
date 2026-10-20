// Private-stream migration M3b: the sweep wired into orchestrate_submit() (OrchestratorContext::sweep, live_submit_orchestrator.hpp), single-threaded and
// deterministic. The reconcile thread's half is driven by hand (poll_once() with a SweepJob, between the hot thread's calls), so the order of events is
// exactly the one a real run produces and nothing depends on scheduling.
//
// What is pinned:
//   * with the pump wired, a refused call (Gate 2b: the barrier is Sweeping) is also the call that POSTS the sweep request, and the next call -- after
//     the reconcile thread answered -- delivers the marker at the top, finds the barrier Reconciled and sends the order: the sweep needs no loop of its own;
//   * without the pump nothing is posted and the barrier stays where the scripted facts leave it (the pump is opt-in, like the other nullable fields);
//   * the marker reaches the barrier even when the call is refused at an early gate: servicing and draining come before every gate;
//   * without a durable publisher the reconcile ring is not drained at all (fail-closed, as before), so the marker waits and the barrier stays Sweeping.
#include <gtest/gtest.h>

#include "test_helpers/account_snapshot_fixtures.hpp"
#include <hengyuan/live_submit_orchestrator.hpp>
#include <hengyuan/startup_recovery.hpp>
#include <hengyuan/submit_readiness.hpp>
#include <hengyuan/sweep_pump.hpp>

#include <cstdint>
#include <cstring>
#include <optional>

using namespace hy;

namespace {

SubmitResponse mock_submit(const char*, std::uint32_t, OrderSide, OrderType, std::int64_t, std::int64_t, const SymbolRules&, void*) {
    return SubmitResponse{SubmitOutcome::Accepted, 42, 0};
}
std::uint32_t mock_rules_version(void*) { return 1; }

// the facts SubmitReadiness reads, scripted (hot thread only)
struct Script {
    FeedObservation feed{};
    RecoveryObservation recovery{};
    bool clock_ready{true};
};
FeedObservation read_feed(void* p) noexcept { return static_cast<Script*>(p)->feed; }
RecoveryObservation read_recovery(void* p) noexcept { return static_cast<Script*>(p)->recovery; }
bool read_clock(void* p) noexcept { return static_cast<Script*>(p)->clock_ready; }

struct Account {
    std::int64_t refreshed_at_ms{0};
};
bool account_fresh(void* p, std::int64_t since) noexcept { return static_cast<Account*>(p)->refreshed_at_ms >= since; }

struct Durable {
    std::uint64_t frames{0};
};
AuditAppendResult durable_append(void* p, const AuditRecord&, std::int64_t) noexcept {
    auto* d = static_cast<Durable*>(p);
    AuditAppendResult r{};
    r.status = AuditAppendResult::Status::Acked;
    r.sequence = ++d->frames;
    return r;
}
bool durable_fenced(void*) noexcept { return false; }

QueryResult never_asked(const OrderExpectation&, void*) { return QueryResult{}; }

constexpr std::int64_t kNow = 5'000'000;

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

class SweepOrchestratorTest : public ::testing::Test {
protected:
    AuditRingSink audit_;
    KillSwitch kill_switch_;
    DryRunEvidenceChain evidence_;
    SpotRateLimitTracker rate_limiter_;
    InFlightRegistry in_flight_;
    SymbolRules rules_{};
    AccountSnapshot account_snapshot_{};
    PositionTruth truth_;
    OrderFillContext fill_context_;
    ToReconcileRing to_reconcile_;
    ReconcileEventRing reconcile_events_;
    SweepRequestRing requests_;
    SweepJob job_{requests_};
    OrderTracker tracker_;
    QueryPort query_port_{&never_asked, nullptr};
    ReconcilePollPolicy poll_policy_{};
    Durable durable_;
    DurableOrderAuditPort durable_port_{&durable_append, &durable_fenced, &durable_};
    ReconcileDurablePublisher publisher_{durable_port_};
    StartupRecovery recovery_{60'000};
    Script script_;
    Account account_;
    SubmitReadiness readiness_{recovery_, kill_switch_, ReadinessProbe{&read_feed, &read_recovery, &read_clock, &script_}};
    SweepPump pump_{readiness_, requests_, SweepAccountProbe{&account_fresh, &account_}};
    int sequence_{0};

    void SetUp() override {
        audit_.set_available(true);
        kill_switch_.operator_reset();
        evidence_.record(EvidencePath::SubmitSuccess, 1, 0xABCD, 1);
        evidence_.record(EvidencePath::SubmitReject, 2, 0xABCD, 1);
        evidence_.record(EvidencePath::SubmitAmbiguous, 3, 0xABCD, 1);
        evidence_.record(EvidencePath::KillSwitch, 4, 0xABCD, 1);
        rate_limiter_.configure(100000, 500, 100000, 500, 100000, 500);
        std::strncpy(rules_.symbol, "BTCUSDT", sizeof(rules_.symbol) - 1);
        rules_.is_trading = true;
        rules_.min_price_ticks = 100;
        rules_.max_price_ticks = 10000000;
        rules_.tick_size_ticks = 100;
        rules_.min_qty_ticks = 10;
        rules_.max_qty_ticks = 1000000;
        rules_.step_size_ticks = 10;
        rules_.min_notional_ticks = 1000;
        rules_.price_scale = 0;
        rules_.qty_scale = 8;
        rules_.rules_version = 1;
        account_snapshot_ = hy::test_helpers::make_account(kNow - 100, /*can_trade=*/true, {{"USDT", 999999999, 0}});  // fresh at ctx.now_ms

        script_.recovery = clean_recovery();
        script_.feed.generation = 1;
        script_.feed.connected = true;
        script_.feed.last_activity_ms = kNow;
        script_.feed.sampled_at_ms = kNow;
        ASSERT_EQ(recovery_.update(script_.recovery, kNow), RunState::Ready);
        account_.refreshed_at_ms = kNow + 1'000'000;  // refreshed after any request this test makes
    }

    OrchestratorContext context(bool with_pump, bool with_publisher) {
        OrchestratorContext ctx{};
        ctx.audit = &audit_;
        ctx.kill_switch = &kill_switch_;
        ctx.readiness = &readiness_;
        ctx.sweep = with_pump ? &pump_ : nullptr;
        ctx.evidence = &evidence_;
        ctx.signer_ready = true;
        ctx.depth_synced = true;
        ctx.rate_limiter = &rate_limiter_;
        ctx.in_flight = &in_flight_;
        ctx.symbol_rules = &rules_;
        ctx.account = &account_snapshot_;
        ctx.position_truth = &truth_;
        ctx.fill_context = &fill_context_;
        ctx.side = OrderSide::Buy;
        ctx.order_type = OrderType::Limit;
        ctx.base_asset = "BTC";
        ctx.quote_asset = "USDT";
        ctx.now_ms = kNow;
        ctx.exposure_limits.freshness_max_age_ms = 3'000'000;
        ctx.exposure_limits.single_order_notional_cap = 500000;
        ctx.exposure_limits.total_exposure_notional_cap = 100'000'000;
        ctx.price_ticks = 1000;
        ctx.qty_ticks = 10;
        ctx.symbol_id = 1;
        ctx.sequence = static_cast<std::uint32_t>(sequence_++);
        ctx.mode = ExecutionMode::Live;
        ctx.order_weight = 1;
        ctx.to_reconcile = &to_reconcile_;
        ctx.reconcile_events = &reconcile_events_;
        ctx.reconcile_durable = with_publisher ? &publisher_ : nullptr;
        ctx.durable_audit = durable_port_;
        ctx.confirmation.confirmed = true;
        ctx.confirmation.symbol_id = ctx.symbol_id;
        ctx.confirmation.side = ctx.side;
        ctx.confirmation.type = ctx.order_type;
        ctx.confirmation.price_ticks = ctx.price_ticks;
        ctx.confirmation.qty_ticks = ctx.qty_ticks;
        ctx.confirmation.max_notional = 1000000;
        ctx.confirmation.valid_until_ms = ctx.now_ms + 60000;
        ctx.submit_port = {mock_submit, nullptr, mock_rules_version};
        return ctx;
    }

    // The reconcile thread's tick, by hand: takes the sweep request, sweeps the (empty) table, pushes the marker.
    void reconcile_tick() {
        poll_once(tracker_, to_reconcile_, reconcile_events_, query_port_, poll_policy_, kNow, OrderRecord::kClockUnset, &job_);
    }
};

}  // namespace

TEST_F(SweepOrchestratorTest, ARefusedCallPostsTheSweepRequestAndTheNextCallAfterTheAnswerSendsTheOrder) {
    OrchestratorContext first = context(/*with_pump=*/true, /*with_publisher=*/true);
    const OrchestratorResult r1 = orchestrate_submit(first);
    EXPECT_EQ(r1.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(r1.not_ready_reason, NotReadyReason::BarrierSweeping);
    SweepRequest posted{};
    ASSERT_TRUE(requests_.try_pop(posted)) << "the call that was refused is the call that asked for the sweep";
    EXPECT_NE(posted.epoch, 0U);
    ASSERT_TRUE(requests_.try_push(posted));  // hand it to the reconcile thread's tick

    reconcile_tick();
    EXPECT_EQ(job_.stats().sweeps_completed, 1U);

    OrchestratorContext second = context(true, true);
    const OrchestratorResult r2 = orchestrate_submit(second);
    EXPECT_EQ(r2.gate, OrchestratorGate::SubmitAccepted) << "the marker was delivered at the top of this call: Reconciled, token issued, order sent";
    EXPECT_EQ(r2.ready_generation, 1U);
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Reconciled);
    EXPECT_EQ(pump_.stats().markers_received, 1U);
    EXPECT_EQ(pump_.stats().requests_sent, 1U);
}

TEST_F(SweepOrchestratorTest, WithoutThePumpNothingIsPostedAndTheBarrierStaysSweeping) {
    for (int i = 0; i < 3; ++i) {
        OrchestratorContext ctx = context(/*with_pump=*/false, true);
        const OrchestratorResult r = orchestrate_submit(ctx);
        EXPECT_EQ(r.gate, OrchestratorGate::NotReady);
        EXPECT_EQ(r.not_ready_reason, NotReadyReason::BarrierSweeping);
    }
    SweepRequest r{};
    EXPECT_FALSE(requests_.try_pop(r)) << "an unwired pump posts nothing";
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Sweeping);
}

TEST_F(SweepOrchestratorTest, TheMarkerReachesTheBarrierEvenWhenTheCallIsRefusedAtAnEarlyGate) {
    OrchestratorContext first = context(true, true);
    ASSERT_EQ(orchestrate_submit(first).gate, OrchestratorGate::NotReady);
    SweepRequest posted{};
    ASSERT_TRUE(requests_.try_pop(posted));
    ASSERT_TRUE(requests_.try_push(posted));
    reconcile_tick();

    audit_.set_available(false);  // Gate 1 will refuse this call
    OrchestratorContext second = context(true, true);
    EXPECT_EQ(orchestrate_submit(second).gate, OrchestratorGate::AuditUnavailable);
    EXPECT_EQ(readiness_.barrier().state(), BarrierState::Reconciled)
        << "the drain and the pump run before every gate, so a refused call still moves the sweep";
}

TEST_F(SweepOrchestratorTest, WithoutADurablePublisherTheReconcileRingIsNotDrainedSoTheMarkerWaits) {
    OrchestratorContext first = context(true, /*with_publisher=*/false);
    ASSERT_EQ(orchestrate_submit(first).gate, OrchestratorGate::NotReady);
    SweepRequest posted{};
    ASSERT_TRUE(requests_.try_pop(posted));
    ASSERT_TRUE(requests_.try_push(posted));
    reconcile_tick();

    OrchestratorContext second = context(true, /*with_publisher=*/false);
    const OrchestratorResult r = orchestrate_submit(second);
    EXPECT_EQ(r.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(r.not_ready_reason, NotReadyReason::BarrierSweeping);
    EXPECT_EQ(pump_.stats().markers_received, 0U);
    ReconcileEvent marker{};
    ASSERT_TRUE(reconcile_events_.try_pop(marker)) << "fail-closed: nothing is published memory-first, and that includes the marker";
    EXPECT_EQ(marker.kind, ReconcileEventKind::SweepDone);
}

TEST_F(SweepOrchestratorTest, ARefusedAccountKeepsTheOrderBackUntilTheSnapshotIsRefreshedAfterTheRequest) {
    account_.refreshed_at_ms = 0;  // older than any request
    OrchestratorContext first = context(true, true);
    ASSERT_EQ(orchestrate_submit(first).gate, OrchestratorGate::NotReady);
    SweepRequest posted{};
    ASSERT_TRUE(requests_.try_pop(posted));
    ASSERT_TRUE(requests_.try_push(posted));
    reconcile_tick();

    OrchestratorContext second = context(true, true);
    const OrchestratorResult r = orchestrate_submit(second);
    EXPECT_EQ(r.gate, OrchestratorGate::NotReady) << "a clean sweep with an account older than the request is not enough";
    EXPECT_EQ(r.not_ready_reason, NotReadyReason::BarrierSweeping);
    EXPECT_EQ(pump_.stats().reports_account_stale, 1U);
}
