// P2-EXEC-LIVE-01 D12-9: live submit orchestrator integration tests.
// All gates tested with mock submit port. No real network. No real money.
#include <gtest/gtest.h>

#include "test_helpers/account_snapshot_fixtures.hpp"
#include "test_helpers/alloc_probe.hpp"
#include <hengyuan/live_submit_orchestrator.hpp>
#include <hengyuan/synthetic_readiness.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
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

// --- Mock submit port ---

static SubmitResponse g_mock_response{};
// How many times the mock port was actually called -- i.e. how many orders "reached the exchange". The recovery rule
// for Intent-only orphans (SAFE-03 slice 2) is only sound if a refusal that leaves one never sent anything.
static int g_mock_submit_calls{0};

static SubmitResponse mock_submit(
    const char* /*coid*/, std::uint32_t /*sym*/,
    OrderSide /*side*/, OrderType /*type*/,
    std::int64_t /*price*/, std::int64_t /*qty*/,
    const SymbolRules& /*rules_snapshot*/, void* /*ud*/) {
    ++g_mock_submit_calls;
    return g_mock_response;
}

// Fixed non-zero version so the default-constructed SymbolRules in the test
// fixture (rules_version left at 0, its default) mismatches by default unless
// a test explicitly sets rules_.rules_version to match -- lets
// StaleRulesVersion-path tests opt in without disturbing every other test's
// default "both sides are 0" pass-through (see mock_submit's own comment
// history: existing tests never touch rules_version at all).
static std::uint32_t g_mock_current_rules_version{0};

static std::uint32_t mock_current_rules_version(void* /*ud*/) {
    return g_mock_current_rules_version;
}

// --- Fake durable-audit port (Phase 3) ---
//
// Real DurableAuditSink is real file I/O -- deliberately not used by most of
// this file's gate-logic tests (see live_submit_orchestrator.hpp's
// DurableOrderAuditPort doc comment for why it's injectable like SubmitPort).
// This fake defaults to "never fails", matching what every test below already
// assumed about ctx_.audit before this port existed; the three *_should_fail
// flags let individual tests precisely target the Intent/Prepared/Outcome
// durable-write gate without touching disk at all.
struct FakeDurableAudit {
    bool intent_should_fail{false};
    bool prepared_should_fail{false};
    bool outcome_should_fail{false};
    std::uint64_t next_sequence{0};
    bool is_fenced{false};
};

static AuditAppendResult fake_durable_append(void* user_data, const AuditRecord& rec, std::int64_t) noexcept {
    auto* f = static_cast<FakeDurableAudit*>(user_data);
    const bool is_intent = rec.event_type == AuditEventType::OrderIntentCreated;
    const bool is_prepared = rec.event_type == AuditEventType::OrderSubmitPrepared;
    const bool fail = (is_intent && f->intent_should_fail) ||
                       (is_prepared && f->prepared_should_fail) ||
                       (!is_intent && !is_prepared && f->outcome_should_fail);
    AuditAppendResult r{};
    if (fail) {
        f->is_fenced = true;
        r.status = AuditAppendResult::Status::Failed;
        return r;
    }
    r.status = AuditAppendResult::Status::Acked;
    r.sequence = f->next_sequence++;
    return r;
}

static bool fake_durable_fenced(void* user_data) noexcept {
    return static_cast<FakeDurableAudit*>(user_data)->is_fenced;
}

// --- Test fixture ---

class LiveSubmitTest : public ::testing::Test {
protected:
    AuditRingSink audit_;
    KillSwitch kill_switch_;
    // Gate 2b (private-stream migration M3a): the real barrier and startup recovery, driven to "ready" by scripted observations.
    SyntheticReadiness readiness_{kill_switch_};
    DryRunEvidenceChain evidence_;
    SpotRateLimitTracker rate_limiter_;
    InFlightRegistry in_flight_;
    SymbolRules rules_{};
    AccountSnapshot account_{};
    OrchestratorContext ctx_{};
    FakeDurableAudit fake_durable_audit_{};

    void SetUp() override {
        // Default: all gates pass
        audit_.set_available(true);
        kill_switch_.operator_reset();  // Normal
        fill_evidence();
        rate_limiter_.configure(6000, 500, 60000, 5000, 100, 10);

        std::strncpy(rules_.symbol, "BTCUSDT", sizeof(rules_.symbol) - 1);
        rules_.is_trading = true;
        rules_.min_price_ticks = 100;
        rules_.max_price_ticks = 10000000;
        rules_.tick_size_ticks = 100;
        rules_.min_qty_ticks = 10;
        rules_.max_qty_ticks = 1000000;
        rules_.step_size_ticks = 10;
        rules_.min_notional_ticks = 1000;
        // AUDIT L4-SYMBOLRULES-SCALE (§5.1.1): see test_account_truth.cpp's
        // make_btcusdt_rules() for why 0/8 specifically -- makes both of
        // validate_pre_trade()'s rescale_notional_ceil() calls (notional and, for the
        // Sell-side test below, the base-asset qty check) an identity transform, so none
        // of this fixture's existing numeric expectations needed to change.
        rules_.price_scale = 0;
        rules_.qty_scale = 8;

        account_ = hy::test_helpers::make_account(900, /*can_trade=*/true, {{"USDT", 999999, 0}});

        ctx_.audit = &audit_;
        ctx_.kill_switch = &kill_switch_;
        ctx_.readiness = &readiness_.readiness();
        ctx_.evidence = &evidence_;
        ctx_.signer_ready = true;
        ctx_.depth_synced = true;
        ctx_.rate_limiter = &rate_limiter_;
        ctx_.in_flight = &in_flight_;
        ctx_.symbol_rules = &rules_;
        ctx_.account = &account_;
        ctx_.side = OrderSide::Buy;
        ctx_.order_type = OrderType::Limit;
        ctx_.base_asset = "BTC";
        ctx_.quote_asset = "USDT";
        ctx_.now_ms = 1000;
        ctx_.exposure_limits.freshness_max_age_ms = 30000;
        ctx_.exposure_limits.single_order_notional_cap = 500000;
        ctx_.exposure_limits.total_exposure_notional_cap = 1000000;
        ctx_.price_ticks = 1000;
        ctx_.qty_ticks = 10;
        ctx_.symbol_id = 1;
        ctx_.sequence = 1;
        ctx_.mode = ExecutionMode::Live;
        ctx_.order_weight = 1;

        // Bind the operator confirmation to the exact order set above (F3).
        bind_confirmation();

        // Both sides matching and non-zero by default -- a stronger baseline
        // than relying on "both happen to be 0", which would mask a Gate 1
        // comparison that got accidentally skipped entirely.
        rules_.rules_version = 1;
        g_mock_current_rules_version = 1;

        g_mock_response = {SubmitOutcome::Accepted, 12345, 0};
        g_mock_submit_calls = 0;
        ctx_.submit_port = {mock_submit, nullptr, mock_current_rules_version};

        // Phase 3: fake durable-audit port, defaults to "never fails" -- matches
        // every test's existing assumption about ctx_.audit before this port
        // existed. Individual tests opt into a specific failure via
        // fake_durable_audit_.*_should_fail.
        ctx_.durable_audit = {&fake_durable_append, &fake_durable_fenced, &fake_durable_audit_};
    }

    // Build a confirmation that authorizes the current ctx_ order params.
    void bind_confirmation() {
        ctx_.confirmation.confirmed = true;
        ctx_.confirmation.symbol_id = ctx_.symbol_id;
        ctx_.confirmation.side = ctx_.side;
        ctx_.confirmation.type = ctx_.order_type;
        ctx_.confirmation.price_ticks = ctx_.price_ticks;
        ctx_.confirmation.qty_ticks = ctx_.qty_ticks;
        ctx_.confirmation.max_notional = 1000000;  // ceiling above notional (10000)
        ctx_.confirmation.valid_until_ms = ctx_.now_ms + 60000;
    }

    void fill_evidence() {
        evidence_.record(EvidencePath::SubmitSuccess, 1, 0xABCD, 1);
        evidence_.record(EvidencePath::SubmitReject, 2, 0xABCD, 1);
        evidence_.record(EvidencePath::SubmitAmbiguous, 3, 0xABCD, 1);
        evidence_.record(EvidencePath::KillSwitch, 4, 0xABCD, 1);
    }
};

// --- Happy path ---

TEST_F(LiveSubmitTest, HappyPathAccepted) {
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(r.order.state, OrderState::Accepted);
    EXPECT_EQ(r.order.exchange_order_id, 12345);
    EXPECT_FALSE(r.order.client_order_id.empty());
    EXPECT_GE(audit_.count(), 3u);  // intent + submitted + accepted
}

// TODO 1A.3: exchange_status now actually drives which OrderState is reached, not a blanket
// OrderState::Accepted for every SubmitOutcome::Accepted response.

TEST_F(LiveSubmitTest, ExchangeStatusPartialFillRoutesToPartialFillAndWritesBackFillFields) {
    g_mock_response = SubmitResponse{};
    g_mock_response.outcome = SubmitOutcome::Accepted;
    g_mock_response.exchange_order_id = 555;
    g_mock_response.exchange_status = OrderState::PartialFill;
    g_mock_response.filled_qty_ticks = 40;
    g_mock_response.avg_fill_price_ticks = 5000;

    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitPartialFill);
    EXPECT_EQ(r.order.state, OrderState::PartialFill);
    EXPECT_EQ(r.order.filled_qty_ticks, 40);
    EXPECT_EQ(r.order.avg_fill_price_ticks, 5000);
}

TEST_F(LiveSubmitTest, ExchangeStatusFilledRoutesToFilledReleasesInFlightAndSkipsReconcile) {
    g_mock_response = SubmitResponse{};
    g_mock_response.outcome = SubmitOutcome::Accepted;
    g_mock_response.exchange_order_id = 556;
    g_mock_response.exchange_status = OrderState::Filled;
    g_mock_response.filled_qty_ticks = 100;
    g_mock_response.avg_fill_price_ticks = 5000;

    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitFilled);
    EXPECT_EQ(r.order.state, OrderState::Filled);
    EXPECT_EQ(r.order.filled_qty_ticks, 100);
    EXPECT_EQ(r.order.avg_fill_price_ticks, 5000);
    // Filled is exchange-final -- the in-flight slot must be released immediately, same as
    // Rejected, not left occupied waiting on a reconcile push that would never resolve it
    // (is_exchange_final() is already true; nothing would ever consume that push).
    EXPECT_FALSE(in_flight_.is_in_flight(r.order.client_order_id.view()));
}

TEST_F(LiveSubmitTest, ExchangeStatusAmbiguousDefaultFallsBackToPlainAccepted) {
    // 3-arg positional init (matching every pre-1A.3 test in this file) leaves
    // exchange_status at its default OrderState::Ambiguous -- must fall back to the original
    // OrderState::Accepted behavior, not be treated as a genuinely ambiguous outcome.
    g_mock_response = {SubmitOutcome::Accepted, 557, 0};
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(r.order.state, OrderState::Accepted);
}

// --- PositionTruth direct-fill fold-in (TODO 1A.3 follow-up) ---

TEST_F(LiveSubmitTest, PositionTruthUnsetIsNoOp) {
    // Backward compatibility: every test above (and every pre-1A.3-follow-up
    // caller) leaves ctx_.position_truth at its default nullptr and must keep
    // behaving exactly as before -- no crash, no change in result.
    ASSERT_EQ(ctx_.position_truth, nullptr);
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
}

TEST_F(LiveSubmitTest, PlainAcceptedWithNoFillDoesNotTouchPositionTruth) {
    PositionTruth truth;
    ctx_.position_truth = &truth;
    g_mock_response = {SubmitOutcome::Accepted, 12345, 0};  // filled_qty_ticks defaults to 0

    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(truth.net_qty_ticks(ctx_.symbol_id), 0);
    EXPECT_EQ(truth.tracked_symbol_count(), 0u);
}

TEST_F(LiveSubmitTest, ExchangeStatusPartialFillFoldsIntoPositionTruth) {
    PositionTruth truth;
    ctx_.position_truth = &truth;
    ctx_.side = OrderSide::Buy;
    bind_confirmation();  // re-bind: side is part of the confirmation binding

    g_mock_response = SubmitResponse{};
    g_mock_response.outcome = SubmitOutcome::Accepted;
    g_mock_response.exchange_order_id = 555;
    g_mock_response.exchange_status = OrderState::PartialFill;
    g_mock_response.filled_qty_ticks = 40;
    g_mock_response.avg_fill_price_ticks = 5000;

    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitPartialFill);
    EXPECT_EQ(truth.net_qty_ticks(ctx_.symbol_id), 40);
}

TEST_F(LiveSubmitTest, ExchangeStatusFilledFoldsIntoPositionTruth) {
    PositionTruth truth;
    ctx_.position_truth = &truth;

    g_mock_response = SubmitResponse{};
    g_mock_response.outcome = SubmitOutcome::Accepted;
    g_mock_response.exchange_order_id = 556;
    g_mock_response.exchange_status = OrderState::Filled;
    g_mock_response.filled_qty_ticks = 100;
    g_mock_response.avg_fill_price_ticks = 5000;

    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitFilled);
    EXPECT_EQ(truth.net_qty_ticks(ctx_.symbol_id), 100);
}

TEST_F(LiveSubmitTest, SellSideFillFoldsAsNegativePosition) {
    PositionTruth truth;
    ctx_.position_truth = &truth;
    account_ = hy::test_helpers::make_account(900, /*can_trade=*/true,
                                              {{"USDT", 999999, 0}, {"BTC", 999999, 0}});  // enough base to deliver
    ctx_.side = OrderSide::Sell;
    bind_confirmation();  // re-bind: side is part of the confirmation binding

    g_mock_response = SubmitResponse{};
    g_mock_response.outcome = SubmitOutcome::Accepted;
    g_mock_response.exchange_order_id = 558;
    g_mock_response.exchange_status = OrderState::Filled;
    g_mock_response.filled_qty_ticks = 100;
    g_mock_response.avg_fill_price_ticks = 5000;

    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitFilled);
    EXPECT_EQ(truth.net_qty_ticks(ctx_.symbol_id), -100);
}

TEST_F(LiveSubmitTest, RejectedDoesNotTouchPositionTruth) {
    PositionTruth truth;
    ctx_.position_truth = &truth;
    g_mock_response = {SubmitOutcome::Rejected, 0, -1013};

    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitRejected);
    EXPECT_EQ(truth.tracked_symbol_count(), 0u);
}

// --- OrderFillContext cross-mechanism dedup (TODO 1A.4 batch 2) ---

// The regression test this batch's own plan calls for: submit -> Accepted(PartialFill 30) ->
// a duplicate WS executionReport for the SAME coid, reporting the SAME cumulative fill (30) ->
// drain -> PositionTruth must stay at 30, not double to 60. Uses PartialFill (not Filled) so
// the OrderFillContext entry is still tracked when the WS event arrives -- a Filled order would
// have already had its entry removed by the same call, which would test the (also correct, but
// different) "untracked coid" skip path instead of the dedup path this test targets.
TEST_F(LiveSubmitTest, DuplicateWsEventAfterDirectFillDoesNotDoubleApply) {
    PositionTruth truth;
    OrderFillContext fill_context;
    UserDataWsEventRing ws_events;
    ctx_.position_truth = &truth;
    ctx_.fill_context = &fill_context;
    ctx_.user_data_events = &ws_events;

    g_mock_response = SubmitResponse{};
    g_mock_response.outcome = SubmitOutcome::Accepted;
    g_mock_response.exchange_order_id = 555;
    g_mock_response.exchange_status = OrderState::PartialFill;
    g_mock_response.filled_qty_ticks = 30;
    g_mock_response.avg_fill_price_ticks = 5000;

    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitPartialFill);
    ASSERT_EQ(truth.net_qty_ticks(ctx_.symbol_id), 30);
    ASSERT_NE(fill_context.find(r.order.client_order_id.view()), nullptr)
        << "still-live PartialFill must keep its OrderFillContext entry";

    // Same coid this call generated (make_client_order_id is a pure function of
    // now_ms/sequence/symbol_id, all unchanged since the call above).
    const auto coid = make_client_order_id(ctx_.now_ms, ctx_.sequence, ctx_.symbol_id);
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    // snprintf, not strncpy: coid.view().data() is a runtime buffer GCC cannot bound at
    // exactly kClientOrderIdLen chars, which -Werror=stringop-truncation flags (same reasoning
    // this file's own make_live_record()-adjacent precedent in test_order_tracker.cpp gives).
    std::snprintf(ev.coid.id, sizeof(ev.coid.id), "%s", coid.view().data());
    // rules_.qty_scale == 8 (fixture default) -- "0.00000030" at scale 8 decodes to the same
    // 30 ticks resp.filled_qty_ticks already reported, simulating a genuine WS redelivery of
    // the SAME cumulative fill, not a different one.
    std::snprintf(ev.cumulative_filled_qty_raw, sizeof(ev.cumulative_filled_qty_raw), "%s",
                  "0.00000030");
    ASSERT_TRUE(ws_events.try_push(ev));

    // A second orchestrate_submit() call drains ws_events at its own top before attempting
    // another submission -- the second submission attempt itself will hit DuplicateInFlight
    // (same coid, still in-flight) and is irrelevant to this test; only the drain's side effect
    // on `truth` is being asserted.
    (void)orchestrate_submit(ctx_);

    EXPECT_EQ(truth.net_qty_ticks(ctx_.symbol_id), 30) << "duplicate WS z must not double-apply";
}

// --- Gate 1: Audit unavailable ---

TEST_F(LiveSubmitTest, AuditUnavailableBlocks) {
    audit_.set_available(false);
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::AuditUnavailable);
}

// --- Gate 2: Kill switch ---

TEST_F(LiveSubmitTest, KillSwitchTriggeredBlocks) {
    kill_switch_.trigger();
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::KillSwitchNotNormal);
    EXPECT_GE(audit_.count(), 1u);
}

// --- Gate 2b: SubmitReadyToken (private-stream migration M3a) ---
//
// The readiness is the REAL SubmitReadiness over the real barrier and startup recovery, driven to "ready" by scripted observations
// (synthetic_readiness.hpp); the tests below take it apart through the barrier's own public API, exactly as a stream event would.
// Which conditions refuse, and in which order, is pinned in test_submit_readiness.cpp; here it is the orchestrator's side of the contract:
// where the gate sits, what a refusal does and does not leave behind, and that an unwired readiness is a refusal.

TEST_F(LiveSubmitTest, UnwiredReadinessIsARefusalNotAPassThrough) {
    ctx_.readiness = nullptr;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(r.not_ready_reason, NotReadyReason::Unwired);
    EXPECT_EQ(r.ready_generation, 0u);
    EXPECT_EQ(g_mock_submit_calls, 0);
}

TEST_F(LiveSubmitTest, AReadyOrderCarriesTheGenerationItsTokenRestedOn) {
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(r.not_ready_reason, NotReadyReason::None);
    EXPECT_EQ(r.ready_generation, 1u);
}

TEST_F(LiveSubmitTest, ARefusalForANotReadyReasonWritesNothingDurableAndSendsNothing) {
    ctx_.readiness = nullptr;
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(fake_durable_audit_.next_sequence, 0u) << "no Intent frame, no Prepared frame: the refusal precedes every durable write";
    EXPECT_EQ(g_mock_submit_calls, 0);
    EXPECT_TRUE(r.order.client_order_id.empty());
}

TEST_F(LiveSubmitTest, ANotReadyRefusalIsAuditedWithItsReason) {
    ctx_.readiness = nullptr;
    const std::size_t before = audit_.count();
    (void)orchestrate_submit(ctx_);
    ASSERT_EQ(audit_.count(), before + 1);
    const AuditRecord* ar = audit_.last();
    ASSERT_NE(ar, nullptr);
    EXPECT_EQ(ar->event_type, AuditEventType::PreflightFailed);
    EXPECT_EQ(ar->detail_code, static_cast<std::int64_t>(NotReadyReason::Unwired));
    EXPECT_STREQ(ar->detail_msg, "Unwired");
    EXPECT_EQ(ar->mode, ExecutionMode::Live);
}

TEST_F(LiveSubmitTest, ADrainedServerShutdownRefusesTheNextOrderAsFeedDown) {  // FI-011
    // The consumer drained a serverShutdown of generation 1; the session still looks connected to the supervisor.
    readiness_.readiness().barrier().on_generation_ended(1);
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(r.not_ready_reason, NotReadyReason::FeedDown);
    EXPECT_EQ(g_mock_submit_calls, 0);
    EXPECT_STREQ(audit_.last()->detail_msg, "FeedDown");
}

TEST_F(LiveSubmitTest, ALostEventRefusesAsSweepingUntilTheSweepIsAnswered) {  // F5, FI-009
    const std::int64_t steady_now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    FeedObservation trouble{};
    trouble.generation = 1;
    trouble.connected = true;
    trouble.last_activity_ms = steady_now;
    trouble.sampled_at_ms = steady_now;
    trouble.ring_drops = 1;  // an event was dropped; the synthetic feed reports 0, any difference is a trouble reading
    ReconnectBarrier& barrier = readiness_.readiness().barrier();
    barrier.observe(trouble);
    ASSERT_EQ(barrier.state(), BarrierState::Sweeping);

    auto refused = orchestrate_submit(ctx_);
    EXPECT_EQ(refused.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(refused.not_ready_reason, NotReadyReason::BarrierSweeping);
    EXPECT_EQ(g_mock_submit_calls, 0);

    const std::optional<std::uint64_t> epoch = barrier.take_sweep_request(ctx_.now_ms);
    ASSERT_TRUE(epoch.has_value());
    barrier.on_sweep_report(SweepReport{*epoch, 0U, true}, ctx_.now_ms);

    auto accepted = orchestrate_submit(ctx_);
    EXPECT_EQ(accepted.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(g_mock_submit_calls, 1);
}

TEST_F(LiveSubmitTest, AGenerationChangeRefusesEveryOrderUntilItsSweepIsAnswered) {  // FI-043
    const std::int64_t steady_now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    FeedObservation next_generation{};
    next_generation.generation = 2;  // the barrier follows whatever generation the feed reports; the synthetic feed says 1, so it re-sweeps either way
    next_generation.connected = true;
    next_generation.last_activity_ms = steady_now;
    next_generation.sampled_at_ms = steady_now;
    ReconnectBarrier& barrier = readiness_.readiness().barrier();
    barrier.observe(next_generation);
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto r = orchestrate_submit(ctx_);
        EXPECT_EQ(r.gate, OrchestratorGate::NotReady) << attempt;
        EXPECT_EQ(r.not_ready_reason, NotReadyReason::BarrierSweeping) << attempt;
    }
    EXPECT_EQ(g_mock_submit_calls, 0);

    const std::optional<std::uint64_t> epoch = barrier.take_sweep_request(ctx_.now_ms);
    ASSERT_TRUE(epoch.has_value());
    barrier.on_sweep_report(SweepReport{*epoch, 0U, true}, ctx_.now_ms);
    auto accepted = orchestrate_submit(ctx_);
    EXPECT_EQ(accepted.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(accepted.ready_generation, 1u) << "the token rests on the generation the feed reports now";
}

TEST_F(LiveSubmitTest, AKillSwitchResetNeedsNoNewSweep) {
    kill_switch_.trigger();
    EXPECT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::KillSwitchNotNormal);
    kill_switch_.operator_reset();
    EXPECT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::SubmitAccepted);
}

// --- Gate 2b and the user-data events the same call drains (private-stream migration M3a) ---

namespace {

UserDataWsEvent stamped_event(UserDataEventKind kind, std::uint64_t generation) {
    UserDataWsEvent ev{};
    ev.kind = kind;
    ev.generation = generation;
    return ev;
}

}  // namespace

TEST_F(LiveSubmitTest, AnOlderGenerationsEventDrainedByTheCallIsCountedAsLate) {  // FI-009 through the production entry point
    SyntheticReadiness second_generation(kill_switch_, /*generation=*/2);
    ctx_.readiness = &second_generation.readiness();
    UserDataWsEventRing ring;
    ctx_.user_data_events = &ring;
    ASSERT_TRUE(ring.try_push(stamped_event(UserDataEventKind::BalanceUpdate, 1)));
    ASSERT_TRUE(ring.try_push(stamped_event(UserDataEventKind::BalanceUpdate, 2)));
    ASSERT_TRUE(ring.try_push(stamped_event(UserDataEventKind::BalanceUpdate, 0)));  // not stamped: always merged

    auto r = orchestrate_submit(ctx_);

    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(r.ready_generation, 2u);
    EXPECT_EQ(second_generation.readiness().barrier().stats().late_events_dropped, 1u)
        << "only the generation-1 event is older than the Reconciled generation 2";
}

TEST_F(LiveSubmitTest, ADrainedServerShutdownRefusesTheVeryCallThatDrainedIt) {  // FI-011 through the production entry point
    UserDataWsEventRing ring;
    ctx_.user_data_events = &ring;
    ASSERT_TRUE(ring.try_push(stamped_event(UserDataEventKind::ServerShutdown, /*generation=*/1)));

    auto r = orchestrate_submit(ctx_);  // drains the shutdown first, then asks Gate 2b

    EXPECT_EQ(r.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(r.not_ready_reason, NotReadyReason::FeedDown);
    EXPECT_EQ(g_mock_submit_calls, 0);
    EXPECT_EQ(readiness_.readiness().barrier().stats().generation_ends_seen, 1u);
}

TEST_F(LiveSubmitTest, AnEventStreamTerminatedRefusesTheVeryCallThatDrainedItToo) {  // FI-011
    UserDataWsEventRing ring;
    ctx_.user_data_events = &ring;
    ASSERT_TRUE(ring.try_push(stamped_event(UserDataEventKind::EventStreamTerminated, /*generation=*/1)));

    auto r = orchestrate_submit(ctx_);

    EXPECT_EQ(r.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(r.not_ready_reason, NotReadyReason::FeedDown);
}

TEST_F(LiveSubmitTest, AnUnwiredReadinessStillDrainsTheRingWithoutABarrier) {
    ctx_.readiness = nullptr;
    UserDataWsEventRing ring;
    ctx_.user_data_events = &ring;
    ASSERT_TRUE(ring.try_push(stamped_event(UserDataEventKind::ServerShutdown, 1)));

    auto r = orchestrate_submit(ctx_);

    EXPECT_EQ(r.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(r.not_ready_reason, NotReadyReason::Unwired);
    UserDataWsEvent leftover{};
    EXPECT_FALSE(ring.try_pop(leftover)) << "the event was drained, barrier or not";
}

// --- Gate ordering around 2b: audit and kill switch before it, evidence and everything else after it ---

TEST_F(LiveSubmitTest, AuditCheckedBeforeReadiness) {
    audit_.set_available(false);
    ctx_.readiness = nullptr;
    EXPECT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::AuditUnavailable);
}

TEST_F(LiveSubmitTest, KillSwitchCheckedBeforeReadiness) {
    kill_switch_.trigger();
    ctx_.readiness = nullptr;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::KillSwitchNotNormal) << "a tripped switch keeps reporting itself, whatever the readiness says";
}

TEST_F(LiveSubmitTest, ReadinessCheckedBeforeEvidence) {
    evidence_.reset();
    ctx_.readiness = nullptr;
    EXPECT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::NotReady);
}

TEST_F(LiveSubmitTest, ReadinessCheckedBeforeEveryLaterGate) {
    ctx_.readiness = nullptr;
    evidence_.reset();
    ctx_.signer_ready = false;
    ctx_.depth_synced = false;
    ctx_.confirmation.confirmed = false;
    ctx_.symbol_rules = nullptr;
    ctx_.account = nullptr;
    EXPECT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::NotReady);
}

TEST_F(LiveSubmitTest, NotReadyHasItsOwnGateValueAfterTheSpecsLastOne) {
    EXPECT_EQ(static_cast<int>(OrchestratorGate::NotReady), 23);
    EXPECT_EQ(static_cast<int>(OrchestratorGate::AuditWriteNotAcked), 22) << "the spec's last value is untouched";
}

// --- Gate 3: Dry-run evidence ---

TEST_F(LiveSubmitTest, EvidenceIncompleteBlocks) {
    evidence_.reset();
    evidence_.record(EvidencePath::SubmitSuccess, 1, 0xABCD, 1);
    // missing 3 paths
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::DryRunEvidenceIncomplete);
}

TEST_F(LiveSubmitTest, EvidenceInconsistentBuildBlocks) {
    evidence_.reset();
    evidence_.record(EvidencePath::SubmitSuccess, 1, 0xABCD, 1);
    evidence_.record(EvidencePath::SubmitReject, 2, 0xABCD, 1);
    evidence_.record(EvidencePath::SubmitAmbiguous, 3, 0x9999, 1);  // different build
    evidence_.record(EvidencePath::KillSwitch, 4, 0xABCD, 1);
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::DryRunEvidenceIncomplete);
}

// --- Gate 4: Signer ---

TEST_F(LiveSubmitTest, SignerNotReadyBlocks) {
    ctx_.signer_ready = false;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SignerNotReady);
}

// --- Gate 5: Depth ---

TEST_F(LiveSubmitTest, DepthNotSyncedBlocks) {
    ctx_.depth_synced = false;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::DepthNotSynced);
}

// --- Gate 6+7: Pre-trade ---

// --- Gate "1" (spec 2.1/2.2/3): symbol registry version fast-reject ---

TEST_F(LiveSubmitTest, MatchingRulesVersionPasses) {
    // SetUp already binds both sides to 1 -- this pins that as a deliberate,
    // asserted baseline rather than an incidental default.
    ASSERT_EQ(rules_.rules_version, 1u);
    ASSERT_EQ(g_mock_current_rules_version, 1u);
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
}

TEST_F(LiveSubmitTest, StaleRulesVersionBlocksBeforePreTrade) {
    g_mock_current_rules_version = 2;  // registry moved on; snapshot is now stale
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitStaleRulesVersion);
    // Fails closed before ANY state mutation: no Submitting transition, no
    // in-flight registration.
    EXPECT_EQ(r.order.state, OrderState::Intent);
    EXPECT_EQ(in_flight_.count(), 0u);
}

TEST_F(LiveSubmitTest, MissingCurrentRulesVersionFnFailsClosedAgainstNonZeroVersion) {
    // A real (non-zero) rules_version with no way to confirm it against the
    // registry must never be treated as "trust it" -- current_rules_version()
    // returns 0 when the function pointer is null, and 0 never matches a real
    // (>=1) version by construction.
    ctx_.submit_port = {mock_submit, nullptr, nullptr};
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitStaleRulesVersion);
}

TEST_F(LiveSubmitTest, SnapshotCapturedFromSymbolRulesAtCallTime) {
    // Deliberately does NOT perturb any pre-trade-validated field (price/qty/
    // notional bounds) -- this test is about capture fidelity, not pre-trade
    // logic. tick_size_ticks is read by validate_pre_trade for step alignment
    // but ctx_.price_ticks (1000) already aligns to both the fixture default
    // (100) and this new value (50), so changing it doesn't also change
    // whether the order passes.
    rules_.tick_size_ticks = 50;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(ctx_.pre_trade_rules_snapshot.tick_size_ticks, 50);
}

// --- Gate 6+7: Pre-trade validation ---

TEST_F(LiveSubmitTest, PreTradeInsufficientBalance) {
    account_ = hy::test_helpers::make_account(900, /*can_trade=*/true, {{"USDT", 1, 0}});
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::PreTradeFailed);
    EXPECT_EQ(r.pre_trade_detail, PreTradeCheck::InsufficientBalance);
}

TEST_F(LiveSubmitTest, PreTradeSymbolNotTrading) {
    rules_.is_trading = false;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::PreTradeFailed);
    EXPECT_EQ(r.pre_trade_detail, PreTradeCheck::SymbolNotTrading);
}

TEST_F(LiveSubmitTest, PreTradePriceStepViolation) {
    ctx_.price_ticks = 150;  // not aligned to tick_size=100
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::PreTradeFailed);
    EXPECT_EQ(r.pre_trade_detail, PreTradeCheck::PriceStepViolation);
}

// --- Gate 8: Rate limit ---

TEST_F(LiveSubmitTest, RateLimitExhausted) {
    // PartitionedRateBudget floors the 80/10/10 split on TOP of limit-safety: weight
    // available_total = 100-0 = 100, Strategy lane = floor(100*80/100) = 80.
    rate_limiter_.configure(/*weight*/ 100, 0, /*raw*/ 100000, 0, /*orders*/ 100000, 0);
    ASSERT_TRUE(rate_limiter_.try_reserve_order(RateLimitLane::Strategy, 80));  // exhausts the weight lane
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::RateLimitExhausted);
}

// TODO 1A.2: this scenario was structurally impossible to express before this batch —
// ORDERS wasn't tracked at all, only weight. Confirms try_reserve_order()'s third
// (ORDERS) stage genuinely gates Gate 12c, not just weight/raw.
TEST_F(LiveSubmitTest, RateLimitExhaustedOnOrdersLaneOnlyStillBlocksSubmission) {
    // orders available_total = 2-0 = 2, Strategy lane = floor(2*80/100) = 1.
    rate_limiter_.configure(/*weight*/ 100000, 0, /*raw*/ 100000, 0, /*orders*/ 2, 0);
    ASSERT_TRUE(rate_limiter_.try_reserve_order(RateLimitLane::Strategy, 1));  // exhausts only orders
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::RateLimitExhausted);
}

// --- Gate 11: Operator CONFIRM ---

TEST_F(LiveSubmitTest, OperatorNotConfirmedBlocks) {
    ctx_.confirmation.confirmed = false;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::OperatorNotConfirmed);
    // Intent should be audited before CONFIRM gate
    bool found_intent = false;
    for (std::size_t i = 0; i < audit_.count(); ++i) {
        auto* ar = audit_.at(i);
        if (ar && ar->event_type == AuditEventType::OrderIntentCreated) {
            found_intent = true;
        }
    }
    EXPECT_TRUE(found_intent);
}

// --- Gate 12: Submit port ---

TEST_F(LiveSubmitTest, SubmitPortInvalidBlocks) {
    // Keep current_rules_version_fn wired so Gate 1 (which runs earlier, at the
    // pre-trade block) passes and execution actually reaches this gate -- an
    // all-null submit_port would fail Gate 1 first (current_rules_version()
    // fails closed to 0 when the fn pointer is null) and never exercise the
    // thing this test is named for. is_valid() checks fn specifically, so only
    // fn needs to be null here.
    ctx_.submit_port = {nullptr, nullptr, mock_current_rules_version};
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitPortInvalid);
}

// --- Gate 13: Submit outcomes ---

TEST_F(LiveSubmitTest, SubmitRejectedRouting) {
    g_mock_response = {SubmitOutcome::Rejected, 0, -1013};
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitRejected);
    EXPECT_EQ(r.order.state, OrderState::Rejected);
}

TEST_F(LiveSubmitTest, SubmitTimeoutAmbiguous) {
    g_mock_response = {SubmitOutcome::Timeout, 0, 0};
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAmbiguous);
    EXPECT_EQ(r.order.state, OrderState::Ambiguous);
    // Verify ambiguous is audited
    auto* last = audit_.last();
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->event_type, AuditEventType::OrderAmbiguous);
}

TEST_F(LiveSubmitTest, SubmitNetworkErrorAmbiguous) {
    g_mock_response = {SubmitOutcome::NetworkError, 0, 0};
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitNetworkError);
    EXPECT_EQ(r.order.state, OrderState::Ambiguous);
}

// --- Idempotency ---

TEST_F(LiveSubmitTest, ClientOrderIdGenerated) {
    auto r = orchestrate_submit(ctx_);
    EXPECT_FALSE(r.order.client_order_id.empty());
    auto v = r.order.client_order_id.view();
    EXPECT_TRUE(v.starts_with("HY-"));
}

TEST_F(LiveSubmitTest, SameInputsSameClientOrderId) {
    // Two orchestrations with identical inputs should produce same clientOrderId
    auto coid1 = make_client_order_id(ctx_.now_ms, ctx_.sequence, ctx_.symbol_id);
    auto coid2 = make_client_order_id(ctx_.now_ms, ctx_.sequence, ctx_.symbol_id);
    EXPECT_EQ(coid1.view(), coid2.view());
}

// --- Audit trail completeness ---

TEST_F(LiveSubmitTest, HappyPathAuditSequence) {
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    // Should have: intent, submitted, accepted (at minimum)
    EXPECT_GE(audit_.count(), 3u);

    bool has_intent = false, has_submit = false, has_accept = false;
    for (std::size_t i = 0; i < audit_.count(); ++i) {
        auto* ar = audit_.at(i);
        if (!ar) continue;
        if (ar->event_type == AuditEventType::OrderIntentCreated) has_intent = true;
        if (ar->event_type == AuditEventType::OrderSubmitted) has_submit = true;
        if (ar->event_type == AuditEventType::OrderAccepted) has_accept = true;
        // All records should be Live mode
        EXPECT_EQ(ar->mode, ExecutionMode::Live);
    }
    EXPECT_TRUE(has_intent);
    EXPECT_TRUE(has_submit);
    EXPECT_TRUE(has_accept);
}

// --- Gate ordering: early gates checked first ---

TEST_F(LiveSubmitTest, AuditCheckedBeforeKillSwitch) {
    audit_.set_available(false);
    kill_switch_.trigger();
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::AuditUnavailable);
}

TEST_F(LiveSubmitTest, KillSwitchCheckedBeforeEvidence) {
    kill_switch_.trigger();
    evidence_.reset();
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::KillSwitchNotNormal);
}

// --- F3: confirmation bound to immutable summary (ADR-019 D3 M3) ---

TEST_F(LiveSubmitTest, ConfirmationPriceMismatchBlocks) {
    // Operator confirmed price 1000 (SetUp); order price changed to a still-valid
    // 2000. Pre-trade passes, but the confirmation no longer authorizes it.
    ctx_.price_ticks = 2000;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::ConfirmationMismatch);
}

TEST_F(LiveSubmitTest, ConfirmationExpiredBlocks) {
    ctx_.confirmation.valid_until_ms = ctx_.now_ms - 1;  // expired
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::ConfirmationMismatch);
}

TEST_F(LiveSubmitTest, ConfirmationNotionalCeilingBlocks) {
    ctx_.confirmation.max_notional = 5000;  // below notional (1000*10=10000)
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::ConfirmationMismatch);
}

TEST_F(LiveSubmitTest, ConfirmationZeroCeilingBlocks) {
    ctx_.confirmation.max_notional = 0;  // unset ceiling → fail-closed
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::ConfirmationMismatch);
}

// --- F1: sell-side end-to-end ---

TEST_F(LiveSubmitTest, SellSideHappyPath) {
    account_ = hy::test_helpers::make_account(900, /*can_trade=*/true,
                                              {{"USDT", 999999, 0}, {"BTC", 999999, 0}});  // enough base to deliver
    ctx_.side = OrderSide::Sell;
    bind_confirmation();  // re-bind confirmation to side=Sell
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
}

// --- F4: idempotency / no-blind-retry guard (ADR-019 D8 M6) ---

TEST_F(LiveSubmitTest, DuplicateInFlightBlocked) {
    // Pre-register the exact id the orchestrator will generate.
    auto coid = make_client_order_id(ctx_.now_ms, ctx_.sequence, ctx_.symbol_id);
    ASSERT_TRUE(in_flight_.register_submit(coid.view()));
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::DuplicateInFlight);
}

TEST_F(LiveSubmitTest, ADuplicateOfAnInFlightOrderIsRefusedBeforeAnyDurableFrameIsWritten) {
    // SAFE-03 slice 2: the Intent frame is the FIRST frame of an id's history. A duplicate of an order that is
    // already in flight used to write a second one -- an Intent after Submitting / Accepted is an illegal
    // transition for the recovery scan, and made the whole log Corrupt on the next start -- and was refused only
    // afterwards, at the registry (Gate 12b).
    auto coid = make_client_order_id(ctx_.now_ms, ctx_.sequence, ctx_.symbol_id);
    ASSERT_TRUE(in_flight_.register_submit(coid.view()));
    const std::uint64_t frames_before = fake_durable_audit_.next_sequence;
    const std::size_t ring_before = audit_.count();

    auto r = orchestrate_submit(ctx_);

    EXPECT_EQ(r.gate, OrchestratorGate::DuplicateInFlight);
    EXPECT_EQ(fake_durable_audit_.next_sequence, frames_before) << "a refused duplicate writes no durable frame";
    EXPECT_EQ(in_flight_.count(), 1u) << "the order that is in flight still holds its one slot";
    EXPECT_GT(audit_.count(), ring_before) << "the refusal is still on the memory audit ring";
}

TEST_F(LiveSubmitTest, InFlightRegistryUnavailableFailsClosed) {
    ctx_.in_flight = nullptr;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::InFlightRegistryUnavailable);
}

TEST_F(LiveSubmitTest, AcceptedStaysInFlight) {
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(in_flight_.count(), 1u);
    EXPECT_TRUE(in_flight_.is_in_flight(r.order.client_order_id.view()));
}

TEST_F(LiveSubmitTest, AmbiguousStaysInFlight) {
    g_mock_response = {SubmitOutcome::Timeout, 0, 0};
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitAmbiguous);
    // Stays registered → a naive resubmit with the same id is refused.
    EXPECT_EQ(in_flight_.count(), 1u);
}

TEST_F(LiveSubmitTest, RejectedReleasesInFlight) {
    g_mock_response = {SubmitOutcome::Rejected, 0, -1013};
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitRejected);
    EXPECT_EQ(in_flight_.count(), 0u);  // terminal → slot released
}

// --- Phase 3: durable-before-send gates (spec §3/§6.2/§6.4) ---

TEST_F(LiveSubmitTest, AuditWriteNotAckedAtIntentGate) {
    fake_durable_audit_.intent_should_fail = true;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(r.order.state, OrderState::Intent);
}

TEST_F(LiveSubmitTest, SubmittingNotSetIfPreparedAppendFails) {
    fake_durable_audit_.prepared_should_fail = true;
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::AuditWriteNotAcked);
    // Not Submitting -- the transition site never ran.
    EXPECT_EQ(r.order.state, OrderState::Intent);
}

TEST_F(LiveSubmitTest, NothingIsSentWhenThePreparedAckFails) {
    // The premise of the Intent-only orphan rule (SAFE-03 slice 2): OrderSubmitPrepared is durable BEFORE the send, so a
    // client order id whose last durable frame is Intent was never sent. Pin the premise itself.
    fake_durable_audit_.prepared_should_fail = true;
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(g_mock_submit_calls, 0) << "the port was called although the Prepared frame was not acknowledged";
}

TEST_F(LiveSubmitTest, NothingIsSentWhenTheIntentAckFailsEither) {
    fake_durable_audit_.intent_should_fail = true;
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(g_mock_submit_calls, 0);
}

TEST_F(LiveSubmitTest, ARefusalAfterTheIntentFrameSendsNothing) {
    // Every local refusal that leaves an Intent-only order id behind (Gate 11 to Gate 12c) stops before the port.
    ctx_.confirmation.confirmed = false;
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::OperatorNotConfirmed);
    bind_confirmation();

    ctx_.confirmation.qty_ticks += 10;
    ctx_.sequence = 2;
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::ConfirmationMismatch);
    bind_confirmation();

    const SubmitPort good_port = ctx_.submit_port;
    ctx_.submit_port = {nullptr, nullptr, mock_current_rules_version};
    ctx_.sequence = 3;
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::SubmitPortInvalid);
    ctx_.submit_port = good_port;

    for (std::size_t i = 0; i < kMaxInFlight; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "HY-FULL%02u", static_cast<unsigned>(i));
        ASSERT_TRUE(in_flight_.register_submit(id));
    }
    ctx_.sequence = 4;
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::DuplicateInFlight);  // no free slot

    EXPECT_EQ(g_mock_submit_calls, 0) << "none of the four refusals may have reached the exchange";
}

TEST_F(LiveSubmitTest, InFlightNotReleasedIfOutcomeAppendFailsRejected) {
    g_mock_response = {SubmitOutcome::Rejected, 0, -1013};
    fake_durable_audit_.outcome_should_fail = true;
    auto coid = make_client_order_id(ctx_.now_ms, ctx_.sequence, ctx_.symbol_id);
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::AuditWriteNotAcked);
    // Unlike an Acked reject, the slot must stay held -- release only happens
    // after the outcome Ack succeeds.
    EXPECT_TRUE(in_flight_.is_in_flight(coid.view()));
}

TEST_F(LiveSubmitTest, DurableAuditPortNotWiredFailsClosedAtIntentGate) {
    ctx_.durable_audit = {};  // default-constructed: is_valid() == false
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(r.order.state, OrderState::Intent);
}

// --- Reconcile/poll loop wiring (order_tracker.hpp) ---
//
// ctx_.to_reconcile/ctx_.reconcile_events are left null by SetUp() -- every
// test above this section already exercises that backward-compatible path
// (both fields default to nullptr, orchestrate_submit() behaves exactly as
// before this wiring existed). This section wires them explicitly.

class LiveSubmitReconcileTest : public LiveSubmitTest {
protected:
    ToReconcileRing to_reconcile_{};
    ReconcileEventRing reconcile_events_{};
    // SAFE-03 slice 1: the durable-first publication state for reconcile_events_ -- bound to the same (fake)
    // durable port the submit path uses, as a real wiring would.
    ReconcileDurablePublisher reconcile_durable_{};

    void SetUp() override {
        LiveSubmitTest::SetUp();
        ctx_.to_reconcile = &to_reconcile_;
        ctx_.reconcile_events = &reconcile_events_;
        EXPECT_TRUE(reconcile_durable_.set_port(ctx_.durable_audit));
        ctx_.reconcile_durable = &reconcile_durable_;
    }

    bool ring_has(AuditEventType type) const {
        for (std::size_t i = 0; i < audit_.count(); ++i) {
            const auto* ar = audit_.at(i);
            if (ar && ar->event_type == type) return true;
        }
        return false;
    }

    // One order that went Ambiguous, handed to the reconcile ring, plus the resolution the reconcile thread
    // would send back for it (queued, not yet drained).
    ClientOrderId submit_ambiguous_and_queue_resolution(OrderState resolution) {
        g_mock_response = {SubmitOutcome::Timeout, 0, 0};
        auto r = orchestrate_submit(ctx_);
        EXPECT_EQ(r.gate, OrchestratorGate::SubmitAmbiguous);
        ReconcileIngress in{};
        EXPECT_TRUE(to_reconcile_.try_pop(in));
        ReconcileEvent ev{};
        ev.handle = in.handle;
        ev.coid = r.order.client_order_id;
        ev.resulting_state = resolution;
        ev.prior_state = OrderState::Ambiguous;
        EXPECT_TRUE(reconcile_events_.try_push(ev));
        return r.order.client_order_id;
    }
};

TEST_F(LiveSubmitReconcileTest, TimeoutPushesToReconcileRing) {
    g_mock_response = {SubmitOutcome::Timeout, 0, 0};
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitAmbiguous);

    ReconcileIngress in{};
    ASSERT_TRUE(to_reconcile_.try_pop(in));
    EXPECT_EQ(in.record.client_order_id.view(), r.order.client_order_id.view());
    EXPECT_EQ(in.record.state, OrderState::Ambiguous);
    EXPECT_TRUE(in.handle.valid());
}

TEST_F(LiveSubmitReconcileTest, NetworkErrorPushesToReconcileRing) {
    g_mock_response = {SubmitOutcome::NetworkError, 0, 0};
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitNetworkError);

    ReconcileIngress in{};
    ASSERT_TRUE(to_reconcile_.try_pop(in));
    EXPECT_EQ(in.record.client_order_id.view(), r.order.client_order_id.view());
}

TEST_F(LiveSubmitReconcileTest, StaleRulesVersionGateDoesNotPushBeforeSubmit) {
    // Gate 1's fast-reject happens BEFORE any in-flight registration or submit
    // attempt -- it must never reach the to_reconcile push at all, regardless
    // of ctx_.to_reconcile being wired.
    g_mock_current_rules_version = 2;
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitStaleRulesVersion);

    ReconcileIngress in{};
    EXPECT_FALSE(to_reconcile_.try_pop(in));
}

TEST_F(LiveSubmitReconcileTest, AcceptedPushesToReconcileRingAndRejectedDoesNot) {
    // AUDIT EXEC-INFLIGHT-003: this test used to assert that Accepted ALSO stayed off
    // the reconcile ring. That was the defect. An Accepted order keeps its in-flight
    // slot (correctly -- it is resting on the exchange), and drain_reconcile_events()
    // releases a slot only on is_exchange_final(), so if nothing ever tracks it the
    // slot is unreleasable for the life of the process. Accepted must be handed to
    // the reconcile loop; Rejected genuinely must not, because it is already terminal
    // and orchestrate_submit() released its slot synchronously.
    auto r1 = orchestrate_submit(ctx_);
    ASSERT_EQ(r1.gate, OrchestratorGate::SubmitAccepted);
    ReconcileIngress in{};
    ASSERT_TRUE(to_reconcile_.try_pop(in))
        << "an accepted order must reach the reconcile loop, or its slot can never be released";
    EXPECT_EQ(in.record.state, OrderState::Accepted);
    EXPECT_EQ(in.record.client_order_id.view(), r1.order.client_order_id.view());
    EXPECT_TRUE(in.handle.valid());

    ctx_.sequence = 2;
    g_mock_response = {SubmitOutcome::Rejected, 0, -1013};
    auto r2 = orchestrate_submit(ctx_);
    ASSERT_EQ(r2.gate, OrchestratorGate::SubmitRejected);
    EXPECT_FALSE(to_reconcile_.try_pop(in)) << "Rejected is terminal; nothing left to reconcile";
}

TEST_F(LiveSubmitReconcileTest, PushedHandleReleasesTheSameSlotItRegistered) {
    g_mock_response = {SubmitOutcome::Timeout, 0, 0};
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitAmbiguous);
    ASSERT_EQ(in_flight_.count(), 1u);

    ReconcileIngress in{};
    ASSERT_TRUE(to_reconcile_.try_pop(in));
    EXPECT_TRUE(in_flight_.mark_resolved_handle(in.handle, r.order.client_order_id.view()))
        << "the handle pushed alongside an Ambiguous order must be the real "
           "handle from this order's own in-flight registration";
    EXPECT_EQ(in_flight_.count(), 0u);
}

TEST_F(LiveSubmitReconcileTest, ReconcileEventsDrainedAtTopOfNextCall) {
    // Submit order #1, let it go Ambiguous, then feed back a resolution via
    // reconcile_events_ as if the reconcile thread had found it -- the NEXT
    // orchestrate_submit() call (for an unrelated order #2) must drain it.
    g_mock_response = {SubmitOutcome::Timeout, 0, 0};
    auto r1 = orchestrate_submit(ctx_);
    ASSERT_EQ(r1.gate, OrchestratorGate::SubmitAmbiguous);
    ASSERT_EQ(in_flight_.count(), 1u);

    ReconcileIngress in{};
    ASSERT_TRUE(to_reconcile_.try_pop(in));

    ReconcileEvent ev{};
    ev.handle = in.handle;
    ev.coid = r1.order.client_order_id;
    ev.resulting_state = OrderState::Filled;
    ev.exchange_order_id = 777;
    ASSERT_TRUE(reconcile_events_.try_push(ev));

    ctx_.sequence = 2;
    g_mock_response = {SubmitOutcome::Accepted, 999, 0};
    auto r2 = orchestrate_submit(ctx_);
    EXPECT_EQ(r2.gate, OrchestratorGate::SubmitAccepted);

    // Order #1's slot must have been released by the drain at the top of this
    // second call, and its resolution audited -- leaving only order #2 in flight.
    EXPECT_FALSE(in_flight_.is_in_flight(r1.order.client_order_id.view()));
    EXPECT_EQ(in_flight_.count(), 1u);
    EXPECT_TRUE(in_flight_.is_in_flight(r2.order.client_order_id.view()));

    bool found_reconciled = false;
    for (std::size_t i = 0; i < audit_.count(); ++i) {
        auto* ar = audit_.at(i);
        if (ar && ar->event_type == AuditEventType::OrderReconciled) found_reconciled = true;
    }
    EXPECT_TRUE(found_reconciled);
}

TEST_F(LiveSubmitReconcileTest, DrainRunsEvenWhenThisCallFailsAnEarlyGate) {
    // The drain must not be gated behind THIS call's own success -- a resolved
    // slot from a previous order should not sit unreleased just because this
    // particular call happens to hit KillSwitchNotNormal.
    g_mock_response = {SubmitOutcome::Timeout, 0, 0};
    auto r1 = orchestrate_submit(ctx_);
    ASSERT_EQ(r1.gate, OrchestratorGate::SubmitAmbiguous);

    ReconcileIngress in{};
    ASSERT_TRUE(to_reconcile_.try_pop(in));
    ReconcileEvent ev{};
    ev.handle = in.handle;
    ev.coid = r1.order.client_order_id;
    ev.resulting_state = OrderState::Cancelled;
    ASSERT_TRUE(reconcile_events_.try_push(ev));

    kill_switch_.trigger();
    auto r2 = orchestrate_submit(ctx_);
    ASSERT_EQ(r2.gate, OrchestratorGate::KillSwitchNotNormal);

    EXPECT_FALSE(in_flight_.is_in_flight(r1.order.client_order_id.view()));
    EXPECT_EQ(in_flight_.count(), 0u);
}

// --- SAFE-03 slice 1 (audit P1-002): the drain at the top of orchestrate_submit() is durable-first ---
//
// Each call below stops at the kill switch (Gate 2) on purpose: the drain runs before the gates, and a call that
// stops there makes no durable append of its own, so every append the fake sees is the drain's.

TEST_F(LiveSubmitReconcileTest, ADurableFailureHoldsTheResolutionAndTheSlotUntilTheSinkAcknowledges) {
    const ClientOrderId coid = submit_ambiguous_and_queue_resolution(OrderState::Filled);
    ASSERT_TRUE(in_flight_.is_in_flight(coid.view()));

    kill_switch_.trigger();
    fake_durable_audit_.outcome_should_fail = true;
    auto r2 = orchestrate_submit(ctx_);
    ASSERT_EQ(r2.gate, OrchestratorGate::KillSwitchNotNormal);

    EXPECT_TRUE(in_flight_.is_in_flight(coid.view())) << "no durable ACK -> the slot is not released";
    EXPECT_TRUE(reconcile_durable_.blocked());
    EXPECT_FALSE(ring_has(AuditEventType::OrderReconciled)) << "no durable ACK -> nothing in the audit ring";

    fake_durable_audit_.outcome_should_fail = false;  // the sink is back
    auto r3 = orchestrate_submit(ctx_);
    ASSERT_EQ(r3.gate, OrchestratorGate::KillSwitchNotNormal);

    EXPECT_FALSE(in_flight_.is_in_flight(coid.view())) << "the very same resolution, retried, now released the slot";
    EXPECT_FALSE(reconcile_durable_.blocked());
    EXPECT_TRUE(ring_has(AuditEventType::OrderReconciled));
}

TEST_F(LiveSubmitReconcileTest, ResolutionsAreNotDrainedAtAllWhileNoDurablePublisherIsWired) {
    // reconcile_events wired but reconcile_durable not: the only other choice would be publishing to memory with
    // no durable record, which is exactly what the field exists to forbid. Fail closed: leave the ring alone.
    const ClientOrderId coid = submit_ambiguous_and_queue_resolution(OrderState::Filled);
    ctx_.reconcile_durable = nullptr;

    kill_switch_.trigger();
    auto r2 = orchestrate_submit(ctx_);
    ASSERT_EQ(r2.gate, OrchestratorGate::KillSwitchNotNormal);

    EXPECT_TRUE(in_flight_.is_in_flight(coid.view()));
    EXPECT_FALSE(ring_has(AuditEventType::OrderReconciled));
    ReconcileEvent still_queued{};
    EXPECT_TRUE(reconcile_events_.try_pop(still_queued)) << "the resolution waits in the ring";
}

TEST_F(LiveSubmitReconcileTest, TheDrainAtTheTopMakesEveryResolutionDurableOnTheSubmitPathsPort) {
    const ClientOrderId coid = submit_ambiguous_and_queue_resolution(OrderState::Cancelled);
    const std::uint64_t sequence_before = fake_durable_audit_.next_sequence;

    kill_switch_.trigger();
    auto r2 = orchestrate_submit(ctx_);
    ASSERT_EQ(r2.gate, OrchestratorGate::KillSwitchNotNormal);

    // Ambiguous -> Cancelled: the remap frame and the resolution itself.
    EXPECT_EQ(fake_durable_audit_.next_sequence - sequence_before, 2u);
    EXPECT_EQ(reconcile_durable_.acked_frames(), 2u);
    EXPECT_FALSE(in_flight_.is_in_flight(coid.view()));
}

// --- SAFE-03 path 1 (audit P1-002): the synchronous POST path, branch by branch ---
//
// orchestrate_submit() acknowledges every outcome durably BEFORE it transitions the order, appends the outcome to
// the audit ring, folds a fill into PositionTruth or releases the in-flight slot (Gate 13). Only the Rejected branch
// had a test for a failed ACK. These pin the other six, each as its own test so that a mutant which drops the ACK
// check of ONE branch is named by that branch's test -- and they assert every effect the ACK is supposed to guard,
// not only the gate: the order stays Submitting, the slot stays held, PositionTruth and the fill baseline are
// untouched, the ring has no outcome record, and nothing is handed to the reconcile loop.

class LiveSubmitOutcomeAckTest : public LiveSubmitReconcileTest {
protected:
    PositionTruth truth_;
    OrderFillContext fill_context_;

    void SetUp() override {
        LiveSubmitReconcileTest::SetUp();
        ctx_.position_truth = &truth_;
        ctx_.fill_context = &fill_context_;
    }

    static SubmitResponse filled_answer(OrderState status, std::int64_t filled_qty_ticks) {
        SubmitResponse r{};
        r.outcome = SubmitOutcome::Accepted;
        r.exchange_order_id = 555;
        r.exchange_status = status;
        r.filled_qty_ticks = filled_qty_ticks;
        r.avg_fill_price_ticks = 5000;
        return r;
    }

    // One submit whose OUTCOME frame (everything after Intent and Prepared) the durable sink does not acknowledge.
    void expect_an_unacknowledged_outcome_is_published_nowhere(const SubmitResponse& answer) {
        g_mock_response = answer;
        const ClientOrderId coid = make_client_order_id(ctx_.now_ms, ctx_.sequence, ctx_.symbol_id);
        fake_durable_audit_.outcome_should_fail = true;

        const OrchestratorResult r = orchestrate_submit(ctx_);

        EXPECT_EQ(r.gate, OrchestratorGate::AuditWriteNotAcked);
        EXPECT_EQ(r.order.state, OrderState::Submitting) << "the transition site never ran: still Submitting";
        EXPECT_EQ(r.order.filled_qty_ticks, 0) << "the fill was not written back to the order";
        EXPECT_TRUE(in_flight_.is_in_flight(coid.view())) << "no ACK -> the slot is not released";
        EXPECT_EQ(truth_.net_qty_ticks(ctx_.symbol_id), 0) << "no ACK -> no fill reaches PositionTruth";
        EXPECT_EQ(truth_.tracked_symbol_count(), 0u);
        EXPECT_NE(fill_context_.find(coid.view()), nullptr) << "no ACK -> the fill baseline is not removed";
        EXPECT_EQ(fill_context_.consume_delta(coid.view(), 40), 40) << "...and has not been advanced either";
        for (std::size_t i = 0; i < audit_.count(); ++i) {
            const AuditRecord* ar = audit_.at(i);
            ASSERT_NE(ar, nullptr);
            const AuditEventType t = ar->event_type;
            EXPECT_FALSE(t == AuditEventType::OrderAccepted || t == AuditEventType::OrderPartialFill ||
                         t == AuditEventType::OrderFilled || t == AuditEventType::OrderRejected ||
                         t == AuditEventType::OrderAmbiguous)
                << "no ACK -> no outcome record in the audit ring (record " << i << ")";
        }
        ReconcileIngress in{};
        EXPECT_FALSE(to_reconcile_.try_pop(in)) << "no ACK -> nothing is handed to the reconcile loop";
    }
};

TEST_F(LiveSubmitOutcomeAckTest, AnUnacknowledgedAcceptedOutcomeIsPublishedNowhere) {
    expect_an_unacknowledged_outcome_is_published_nowhere(SubmitResponse{SubmitOutcome::Accepted, 12345, 0});
}

TEST_F(LiveSubmitOutcomeAckTest, AnUnacknowledgedPartialFillOutcomeIsPublishedNowhere) {
    expect_an_unacknowledged_outcome_is_published_nowhere(filled_answer(OrderState::PartialFill, 40));
}

TEST_F(LiveSubmitOutcomeAckTest, AnUnacknowledgedFilledOutcomeIsPublishedNowhere) {
    expect_an_unacknowledged_outcome_is_published_nowhere(filled_answer(OrderState::Filled, 100));
}

TEST_F(LiveSubmitOutcomeAckTest, AnUnacknowledgedRejectedOutcomeIsPublishedNowhere) {
    expect_an_unacknowledged_outcome_is_published_nowhere(SubmitResponse{SubmitOutcome::Rejected, 0, -1013});
}

TEST_F(LiveSubmitOutcomeAckTest, AnUnacknowledgedTimeoutOutcomeIsPublishedNowhere) {
    expect_an_unacknowledged_outcome_is_published_nowhere(SubmitResponse{SubmitOutcome::Timeout, 0, 0});
}

TEST_F(LiveSubmitOutcomeAckTest, AnUnacknowledgedNetworkErrorOutcomeIsPublishedNowhere) {
    expect_an_unacknowledged_outcome_is_published_nowhere(SubmitResponse{SubmitOutcome::NetworkError, 0, 0});
}

TEST_F(LiveSubmitOutcomeAckTest, AnUnacknowledgedStaleRulesVersionAnswerIsPublishedNowhere) {
    // The defensive branch: unreachable through Gate 1, but a SubmitFn may still return it after the send.
    expect_an_unacknowledged_outcome_is_published_nowhere(SubmitResponse{SubmitOutcome::StaleRulesVersion, 0, 0});
}

TEST_F(LiveSubmitOutcomeAckTest, AnOrderStuckInSubmittingIsNeverSubmittedAgain) {
    // The outcome ACK failed, so the order sits in Submitting with its slot held (until a restart recovers it as
    // Ambiguous). When the sink is back, the very same intent must be refused as in flight -- not sent a second time.
    g_mock_response = SubmitResponse{SubmitOutcome::Accepted, 12345, 0};
    fake_durable_audit_.outcome_should_fail = true;
    const OrchestratorResult first = orchestrate_submit(ctx_);
    ASSERT_EQ(first.gate, OrchestratorGate::AuditWriteNotAcked);

    fake_durable_audit_.outcome_should_fail = false;
    const OrchestratorResult again = orchestrate_submit(ctx_);

    EXPECT_EQ(again.gate, OrchestratorGate::DuplicateInFlight);
    EXPECT_TRUE(in_flight_.is_in_flight(first.order.client_order_id.view()));
    EXPECT_EQ(in_flight_.count(), 1u);
}

// --- Phase 3: real-file-I/O integration (durable persistence, not just gate
// logic -- the fake port above covers gate logic; this proves the port
// adapter wiring and the actual on-disk frame sequence are correct). ---

class LiveSubmitDurableIntegrationTest : public LiveSubmitTest {
protected:
    std::string durable_audit_path_;
    std::unique_ptr<KeyRing> key_ring_;
    std::unique_ptr<DurableAuditSink> durable_audit_sink_;

    void SetUp() override {
        LiveSubmitTest::SetUp();

        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        durable_audit_path_ = std::string(tmp) + "hy_live_submit_durable_" +
                               std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter) + ".log";
#else
        durable_audit_path_ = "/tmp/hy_live_submit_durable_" + std::to_string(getpid()) + "_" +
                               std::to_string(counter) + ".log";
#endif
        remove_durable_files();

        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x40 + i);
        key_ring_ = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        std::vector<std::byte> key_material{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
        ASSERT_EQ(key_ring_->add_key(1, key_material, rec), KeyRingAddStatus::Ok);

        durable_audit_sink_ = std::make_unique<DurableAuditSink>(durable_audit_path_, *key_ring_, 1);
        ASSERT_TRUE(durable_audit_sink_->is_open());
        ctx_.durable_audit = make_durable_order_audit_port(*durable_audit_sink_);
    }

    void TearDown() override {
        durable_audit_sink_.reset();
        key_ring_.reset();
        remove_durable_files();
        LiveSubmitTest::TearDown();
    }

    void remove_durable_files() {
        std::remove(durable_audit_path_.c_str());
        std::remove((durable_audit_path_ + ".lock").c_str());
        std::remove((durable_audit_path_ + ".tip").c_str());
        std::remove((durable_audit_path_ + ".tip.tmp").c_str());
    }
};

TEST_F(LiveSubmitDurableIntegrationTest, HappyPathDurableAuditSequence) {
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitAccepted);

    // Simulate a process restart: destroy this instance, construct a fresh
    // one over the same path/KeyRing -- recovery runs in the constructor.
    durable_audit_sink_.reset();
    auto restarted = std::make_unique<DurableAuditSink>(durable_audit_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted->is_open());
    ASSERT_EQ(restarted->recovery_status(), RecoveryScanStatus::Recovered)
        << "Accepted is not exchange-final -- the 3 durably-written frames "
           "(Intent/Prepared/Accepted) must recover as one open checkpoint";
    auto cps = restarted->recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Accepted);
    EXPECT_EQ(cps[0].symbol_id, ctx_.symbol_id);
    EXPECT_EQ(cps[0].intended_price_ticks, ctx_.price_ticks);
    EXPECT_EQ(cps[0].intended_qty_ticks, ctx_.qty_ticks);
    EXPECT_EQ(cps[0].exchange_order_id, r.order.exchange_order_id);
    EXPECT_STREQ(cps[0].client_order_id.id, r.order.client_order_id.id);
}

TEST_F(LiveSubmitDurableIntegrationTest, RejectedPathReleasesInFlightOnlyAfterRealDurableAck) {
    g_mock_response = {SubmitOutcome::Rejected, 0, -1013};
    auto r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::SubmitRejected);
    EXPECT_EQ(in_flight_.count(), 0u);

    durable_audit_sink_.reset();
    auto restarted = std::make_unique<DurableAuditSink>(durable_audit_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted->is_open());
    EXPECT_EQ(restarted->recovery_status(), RecoveryScanStatus::Clean)
        << "Rejected is exchange-final -- nothing should need recovering";
    EXPECT_TRUE(restarted->recovered_checkpoints().empty());
}

// --- SAFE-03 slice 1 on a REAL durable log: what the drain wrote must replay ---

namespace {

QueryResult cancelled_query(const OrderExpectation&, void*) {
    return QueryResult{QueryOutcome::Found, OrderState::Cancelled, /*exchange_order_id=*/4242, /*filled_qty=*/0,
                       /*avg_price=*/0};
}

}  // namespace

TEST_F(LiveSubmitDurableIntegrationTest, AReconcileResolutionIsDurableBeforeItsSlotIsReleasedAndTheLogReplaysClean) {
    ToReconcileRing to_reconcile;
    ReconcileEventRing reconcile_events;
    ReconcileDurablePublisher reconcile_durable(ctx_.durable_audit);
    ctx_.to_reconcile = &to_reconcile;
    ctx_.reconcile_events = &reconcile_events;
    ctx_.reconcile_durable = &reconcile_durable;

    g_mock_response = {SubmitOutcome::Timeout, 0, 0};
    auto r1 = orchestrate_submit(ctx_);
    ASSERT_EQ(r1.gate, OrchestratorGate::SubmitAmbiguous);

    // The real reconcile loop, with an exchange that says the order was cancelled.
    OrderTracker tracker;
    const QueryPort query_port{&cancelled_query, nullptr};
    poll_once(tracker, to_reconcile, reconcile_events, query_port, ReconcilePollPolicy{}, ctx_.now_ms);
    ASSERT_TRUE(in_flight_.is_in_flight(r1.order.client_order_id.view())) << "found, but not yet drained";

    kill_switch_.trigger();  // the next call only drains (and stops at Gate 2)
    auto r2 = orchestrate_submit(ctx_);
    ASSERT_EQ(r2.gate, OrchestratorGate::KillSwitchNotNormal);
    EXPECT_FALSE(in_flight_.is_in_flight(r1.order.client_order_id.view()));
    EXPECT_EQ(reconcile_durable.acked_frames(), 2u) << "the Ambiguous re-record and the Cancelled resolution";
    EXPECT_EQ(reconcile_durable.failed_appends(), 0u);

    // Restart: Intent, Prepared, Ambiguous, Ambiguous, Cancelled -- every frame a legal repeat or transition, the
    // order exchange-final, so nothing to recover.
    durable_audit_sink_.reset();
    auto restarted = std::make_unique<DurableAuditSink>(durable_audit_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted->is_open());
    EXPECT_EQ(restarted->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(restarted->recovered_checkpoints().empty());
}

// --- SAFE-03 slice 2 on a REAL durable log: the Intent frame is a COID's FIRST frame, and a refusal after it is
// an Intent-only orphan (SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md round 9) ---

TEST_F(LiveSubmitDurableIntegrationTest, ADuplicateOfAnInFlightOrderLeavesALogThatStillReplays) {
    auto first = orchestrate_submit(ctx_);
    ASSERT_EQ(first.gate, OrchestratorGate::SubmitAccepted);
    ASSERT_TRUE(in_flight_.is_in_flight(first.order.client_order_id.view()));

    // The very same intent again (same now_ms / sequence / symbol, so the same client order id) while the first
    // order is still open.
    auto again = orchestrate_submit(ctx_);
    ASSERT_EQ(again.gate, OrchestratorGate::DuplicateInFlight);
    EXPECT_EQ(in_flight_.count(), 1u);

    durable_audit_sink_.reset();
    auto restarted = std::make_unique<DurableAuditSink>(durable_audit_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted->is_open());
    EXPECT_FALSE(restarted->fenced());
    EXPECT_EQ(restarted->recovery_status(), RecoveryScanStatus::Recovered)
        << "a second Intent frame after Accepted is an illegal transition: the refused duplicate must not have "
           "written one";
    auto cps = restarted->recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Accepted);
    EXPECT_STREQ(cps[0].client_order_id.id, first.order.client_order_id.id);
    EXPECT_EQ(restarted->recovered_intent_orphans(), 0u);
}

TEST_F(LiveSubmitDurableIntegrationTest, EveryLocalRefusalAfterTheIntentFrameLeavesAnOrphanAndNothingElse) {
    // Gate 10b makes the Intent frame durable BEFORE CONFIRM, the port check and the in-flight registration, so
    // each of these refusals leaves a client order id whose last durable frame is Intent. Nothing was sent.
    ctx_.confirmation.confirmed = false;  // 1. the operator never confirmed
    ctx_.sequence = 11;
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::OperatorNotConfirmed);
    bind_confirmation();

    ctx_.confirmation.qty_ticks += 10;  // 2. the confirmation authorizes another order
    ctx_.sequence = 12;
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::ConfirmationMismatch);
    bind_confirmation();

    const SubmitPort good_port = ctx_.submit_port;
    ctx_.submit_port = {nullptr, nullptr, mock_current_rules_version};  // 3. no usable submit port
    ctx_.sequence = 13;
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::SubmitPortInvalid);
    ctx_.submit_port = good_port;

    InFlightRegistry* const registry = ctx_.in_flight;
    ctx_.in_flight = nullptr;  // 4. no in-flight registry
    ctx_.sequence = 14;
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::InFlightRegistryUnavailable);
    ctx_.in_flight = registry;

    EXPECT_EQ(in_flight_.count(), 0u) << "none of them held a slot";
    EXPECT_EQ(g_mock_submit_calls, 0) << "and none of them reached the exchange";

    durable_audit_sink_.reset();
    auto restarted = std::make_unique<DurableAuditSink>(durable_audit_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted->is_open());
    EXPECT_FALSE(restarted->fenced());
    EXPECT_EQ(restarted->recovery_status(), RecoveryScanStatus::Clean) << "four orders that were never sent";
    EXPECT_TRUE(restarted->recovered_checkpoints().empty());
    EXPECT_EQ(restarted->recovered_intent_orphans(), 4u);
}

TEST_F(LiveSubmitDurableIntegrationTest, ARefusalBecauseTheRegistryIsFullLeavesAnOrphanNotAnOpenOrder) {
    // The commonest way an orphan arises: every in-flight slot is taken (by orders that are not in this log), the
    // next attempt writes its Intent frame at Gate 10b and finds no free slot at Gate 12b.
    for (std::size_t i = 0; i < kMaxInFlight; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "HY-FULL%02u", static_cast<unsigned>(i));
        ASSERT_TRUE(in_flight_.register_submit(id));
    }
    ASSERT_EQ(in_flight_.count(), kMaxInFlight);

    ctx_.sequence = 5;
    const OrchestratorResult r = orchestrate_submit(ctx_);
    ASSERT_EQ(r.gate, OrchestratorGate::DuplicateInFlight) << "capacity is reported through the same gate";
    EXPECT_FALSE(in_flight_.is_in_flight(r.order.client_order_id.view()));
    EXPECT_EQ(g_mock_submit_calls, 0);

    durable_audit_sink_.reset();
    auto restarted = std::make_unique<DurableAuditSink>(durable_audit_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted->is_open());
    EXPECT_FALSE(restarted->fenced());
    EXPECT_EQ(restarted->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(restarted->recovered_checkpoints().empty());
    EXPECT_EQ(restarted->recovered_intent_orphans(), 1u);
}

TEST_F(LiveSubmitDurableIntegrationTest, ARefusedAttemptRetriedUnderTheSameIdIsOneOpenOrderNotAnOrphan) {
    ctx_.confirmation.confirmed = false;
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::OperatorNotConfirmed);  // Intent frame, nothing sent

    bind_confirmation();  // the retry keeps now_ms / sequence / symbol, so it keeps the client order id
    const OrchestratorResult sent = orchestrate_submit(ctx_);
    ASSERT_EQ(sent.gate, OrchestratorGate::SubmitAccepted);

    durable_audit_sink_.reset();
    auto restarted = std::make_unique<DurableAuditSink>(durable_audit_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted->is_open());
    EXPECT_FALSE(restarted->fenced());
    EXPECT_EQ(restarted->recovery_status(), RecoveryScanStatus::Recovered)
        << "Intent, Intent, Prepared, Accepted: every frame a repeat or a legal transition";
    auto cps = restarted->recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Accepted);
    EXPECT_STREQ(cps[0].client_order_id.id, sent.order.client_order_id.id);
    EXPECT_EQ(restarted->recovered_intent_orphans(), 0u) << "an order that moved past Intent is not an orphan";
}

TEST_F(LiveSubmitDurableIntegrationTest, RefusalsAfterTheIntentFrameDoNotStopTheNextStartNoMatterHowMany) {
    // Seventy attempts the operator never confirmed -- more than the in-flight registry holds. Each wrote its
    // durable Intent frame and was refused at CONFIRM. Then one confirmed order goes out and stays open.
    constexpr std::size_t kRefused = kMaxInFlight + 6;
    ctx_.confirmation.confirmed = false;
    for (std::size_t i = 0; i < kRefused; ++i) {
        ctx_.sequence = static_cast<std::uint32_t>(100 + i);
        const OrchestratorResult r = orchestrate_submit(ctx_);
        ASSERT_EQ(r.gate, OrchestratorGate::OperatorNotConfirmed) << "attempt " << i;
    }
    EXPECT_EQ(in_flight_.count(), 0u) << "a refused attempt holds no slot";
    EXPECT_EQ(g_mock_submit_calls, 0) << "and none of them reached the exchange";

    bind_confirmation();
    ctx_.sequence = 1;
    const OrchestratorResult sent = orchestrate_submit(ctx_);
    ASSERT_EQ(sent.gate, OrchestratorGate::SubmitAccepted);

    durable_audit_sink_.reset();
    auto restarted = std::make_unique<DurableAuditSink>(durable_audit_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted->is_open());
    EXPECT_FALSE(restarted->fenced());
    EXPECT_EQ(restarted->recovery_status(), RecoveryScanStatus::Recovered)
        << "seventy-one open orders would have been CapacityExceeded";
    auto cps = restarted->recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u) << "only the order that was actually sent";
    EXPECT_EQ(cps[0].resulting_state, OrderState::Accepted);
    EXPECT_STREQ(cps[0].client_order_id.id, sent.order.client_order_id.id);
    EXPECT_EQ(restarted->recovered_intent_orphans(), kRefused);
}

// --- Engineering Mandate 1 (CLAUDE.md): the order path allocates nothing ---
//
// test_helpers/alloc_probe.hpp replaces the global operator new of this executable and counts the calls made while a Window is open (its header says
// why it may be included from one translation unit only: this file is the only one of test_live_submit_orchestrator). Every input is built before
// the window opens; inside it there is only the call under test. "Heap" is operator new here, as everywhere in this tree: a malloc() made inside a
// library is invisible to the probe.

namespace {

struct Allocations {
    std::size_t calls{0};
    std::size_t bytes{0};
};

template <class Fn>
Allocations count_allocations(Fn&& fn) {
    Allocations a;
    {
        const alloc_probe::Window window;
        fn();
        a.calls = window.calls();
        a.bytes = window.bytes();
    }
    return a;
}

std::string allocation_report(const char* what, const Allocations& a) {
    return std::string(what) + ": " + std::to_string(a.calls) + " call(s) to operator new, " + std::to_string(a.bytes) + " byte(s). ";
}

}  // namespace

TEST(OrderPathAllocation, TheProbeSeesTheAllocationsItIsMeantToSee) {
    // The positive control: a "0 allocations" result below would be true for the wrong reason if the counting functions were not the ones this
    // executable links, or the window never opened.
    const alloc_probe::SelfCheck c = alloc_probe::selfcheck();
    EXPECT_EQ(c.after_array_new, 1u);
    EXPECT_EQ(c.after_nothrow_array_new, 2u);
    EXPECT_EQ(c.after_string_growth, 3u);
    EXPECT_EQ(c.outside_window, 0u);
}

TEST_F(LiveSubmitTest, EveryOutcomeAndEveryRefusalOfAnOrderAllocatesNothing) {
    std::string problems;
    const auto probe = [&](const char* what, OrchestratorGate expected) {
        ++ctx_.sequence;
        OrchestratorResult r{};
        const Allocations a = count_allocations([&] { r = orchestrate_submit(ctx_); });
        if (r.gate != expected) {
            problems += std::string(what) + ": gate " + std::to_string(static_cast<int>(r.gate)) + ", expected " +
                        std::to_string(static_cast<int>(expected)) + ". ";
        }
        if (a.calls != 0) problems += allocation_report(what, a);
    };

    probe("accepted", OrchestratorGate::SubmitAccepted);
    probe("accepted, second order", OrchestratorGate::SubmitAccepted);
    g_mock_response = {SubmitOutcome::Rejected, 0, -1013};
    probe("rejected by the exchange", OrchestratorGate::SubmitRejected);
    g_mock_response = {SubmitOutcome::Timeout, 0, 0};
    probe("timeout (ambiguous)", OrchestratorGate::SubmitAmbiguous);
    g_mock_response = {SubmitOutcome::NetworkError, 0, 0};
    probe("network error", OrchestratorGate::SubmitNetworkError);
    g_mock_response = {SubmitOutcome::Accepted, 12345, 0};

    g_mock_current_rules_version = 2;  // the symbol rules moved on since the snapshot
    probe("stale rules version", OrchestratorGate::SubmitStaleRulesVersion);
    g_mock_current_rules_version = 1;

    const auto readiness = ctx_.readiness;
    ctx_.readiness = nullptr;  // not wired: a refusal at Gate 2b
    probe("no readiness wired", OrchestratorGate::NotReady);
    ctx_.readiness = readiness;

    ctx_.signer_ready = false;
    probe("signer not ready", OrchestratorGate::SignerNotReady);
    ctx_.signer_ready = true;

    kill_switch_.trigger();
    probe("kill switch tripped", OrchestratorGate::KillSwitchNotNormal);

    EXPECT_TRUE(problems.empty()) << problems;
}

TEST_F(LiveSubmitReconcileTest, DrainingAResolutionInsideTheNextOrderAllocatesNothing) {
    // The reconcile drain runs at the top of every orchestrate_submit(): durable publication through the port, the audit ring, PositionTruth is not
    // wired here, the in-flight slot release. One call that drains a resolution AND submits an order.
    const ClientOrderId coid = submit_ambiguous_and_queue_resolution(OrderState::Cancelled);
    ASSERT_EQ(in_flight_.count(), 1u);

    ++ctx_.sequence;
    g_mock_response = {SubmitOutcome::Accepted, 12345, 0};
    OrchestratorResult r{};
    const Allocations a = count_allocations([&] { r = orchestrate_submit(ctx_); });
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_FALSE(in_flight_.is_in_flight(coid.view())) << "the resolution was drained inside the measured call";
    EXPECT_EQ(a.calls, 0u) << allocation_report("drain + order", a);
}

TEST_F(LiveSubmitDurableIntegrationTest, AnOrderThroughTheRealDurableSinkAllocatesNothing) {
    // Three durable appends per order (Intent, Prepared, Outcome), each one a log append plus an atomic rewrite of the tip anchor. The anchor's
    // temporary name and (on POSIX) its directory used to be built per call with std::string: two or three allocations per append.
    OrchestratorResult r{};
    ASSERT_EQ(orchestrate_submit(ctx_).gate, OrchestratorGate::SubmitAccepted) << "the first order, outside any window";
    ++ctx_.sequence;
    const Allocations a = count_allocations([&] { r = orchestrate_submit(ctx_); });
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(a.calls, 0u) << allocation_report("one order, three durable appends", a);
}
