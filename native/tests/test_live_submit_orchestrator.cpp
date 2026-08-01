// P2-EXEC-LIVE-01 D12-9: live submit orchestrator integration tests.
// All gates tested with mock submit port. No real network. No real money.
#include <gtest/gtest.h>
#include <hengyuan/live_submit_orchestrator.hpp>

#include <cstring>

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

// --- Test fixture ---

class LiveSubmitTest : public ::testing::Test {
protected:
    AuditRingSink audit_;
    KillSwitch kill_switch_;
    DryRunEvidenceChain evidence_;
    RequestWeightTracker rate_tracker_;
    InFlightRegistry in_flight_;
    SymbolRules rules_{};
    AccountSnapshot account_{};
    OrchestratorContext ctx_{};

    void SetUp() override {
        // Default: all gates pass
        audit_.set_available(true);
        kill_switch_.operator_reset();  // Normal
        fill_evidence();
        rate_tracker_.reset(6000, 500);

        std::strncpy(rules_.symbol, "BTCUSDT", sizeof(rules_.symbol) - 1);
        rules_.is_trading = true;
        rules_.min_price_ticks = 100;
        rules_.max_price_ticks = 10000000;
        rules_.tick_size_ticks = 100;
        rules_.min_qty_ticks = 10;
        rules_.max_qty_ticks = 1000000;
        rules_.step_size_ticks = 10;
        rules_.min_notional_ticks = 1000;

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
        ctx_.rate_tracker = &rate_tracker_;
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
    rate_tracker_.reset(10, 5);  // only 5 effective
    rate_tracker_.try_consume(5);
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

TEST_F(LiveSubmitReconcileTest, AcceptedAndRejectedDoNotPushToReconcileRing) {
    auto r1 = orchestrate_submit(ctx_);
    ASSERT_EQ(r1.gate, OrchestratorGate::SubmitAccepted);
    ReconcileIngress in{};
    EXPECT_FALSE(to_reconcile_.try_pop(in));

    ctx_.sequence = 2;
    g_mock_response = {SubmitOutcome::Rejected, 0, -1013};
    auto r2 = orchestrate_submit(ctx_);
    ASSERT_EQ(r2.gate, OrchestratorGate::SubmitRejected);
    EXPECT_FALSE(to_reconcile_.try_pop(in));
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
