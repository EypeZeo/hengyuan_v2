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
    std::int64_t /*price*/, std::int64_t /*qty*/, void* /*ud*/) {
    return g_mock_response;
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

        g_mock_response = {SubmitOutcome::Accepted, 12345, 0};
        ctx_.submit_port = {mock_submit, nullptr};
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
    ctx_.submit_port = {nullptr, nullptr};
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
