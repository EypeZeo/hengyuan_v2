// P2-EXEC-LIVE-01 D12-9: live submit orchestrator integration tests.
// All gates tested with mock submit port. No real network. No real money.
#include <gtest/gtest.h>
#include <hengyuan/live_submit_orchestrator.hpp>

#include <array>
#include <cstdio>
#include <cstring>
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

// --- Mock submit port ---

static SubmitResponse g_mock_response{};

static SubmitResponse mock_submit(
    const char* /*coid*/, std::uint32_t /*sym*/,
    OrderSide /*side*/, OrderType /*type*/,
    std::int64_t /*price*/, std::int64_t /*qty*/,
    const SymbolRules& /*rules_snapshot*/, void* /*ud*/) {
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

        std::strncpy(account_.assets[0].asset, "USDT", 5);
        account_.assets[0].free_ticks = 999999;
        account_.asset_count = 1;
        account_.can_trade = true;
        account_.timestamp_ms = 900;

        ctx_.audit = &audit_;
        ctx_.kill_switch = &kill_switch_;
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
    std::strncpy(account_.assets[1].asset, "BTC", 4);
    account_.assets[1].free_ticks = 999999;  // enough base to deliver
    account_.asset_count = 2;
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
    account_.assets[0].free_ticks = 1;
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
    std::strncpy(account_.assets[1].asset, "BTC", 4);
    account_.assets[1].free_ticks = 999999;  // enough base to deliver
    account_.asset_count = 2;
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

    void SetUp() override {
        LiveSubmitTest::SetUp();
        ctx_.to_reconcile = &to_reconcile_;
        ctx_.reconcile_events = &reconcile_events_;
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
