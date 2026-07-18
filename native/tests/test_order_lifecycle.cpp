// P2-EXEC-LIVE-01 D12-5: order lifecycle state machine + idempotency + reconciliation.
#include <gtest/gtest.h>
#include <hengyuan/order_lifecycle.hpp>

using hy::ClientOrderId;
using hy::OrderRecord;
using hy::OrderState;
using hy::ReconcileAction;
using hy::TransitionResult;
using hy::determine_reconcile_action;
using hy::is_terminal;
using hy::make_client_order_id;
using hy::order_state_name;
using hy::validate_transition;

// --- State names ---

TEST(OrderLifecycle, StateNamesNotNull) {
    for (int i = 0; i <= 11; ++i) {
        EXPECT_NE(order_state_name(static_cast<OrderState>(i)), nullptr);
    }
}

// --- Terminal states ---

TEST(OrderLifecycle, TerminalStates) {
    EXPECT_TRUE(is_terminal(OrderState::Rejected));
    EXPECT_TRUE(is_terminal(OrderState::Filled));
    EXPECT_TRUE(is_terminal(OrderState::Cancelled));
    EXPECT_TRUE(is_terminal(OrderState::Expired));
    EXPECT_TRUE(is_terminal(OrderState::Reconciled));
    EXPECT_TRUE(is_terminal(OrderState::EscalatedToOperator));
}

TEST(OrderLifecycle, NonTerminalStates) {
    EXPECT_FALSE(is_terminal(OrderState::Intent));
    EXPECT_FALSE(is_terminal(OrderState::Submitting));
    EXPECT_FALSE(is_terminal(OrderState::Accepted));
    EXPECT_FALSE(is_terminal(OrderState::Ambiguous));
    EXPECT_FALSE(is_terminal(OrderState::PartialFill));
    EXPECT_FALSE(is_terminal(OrderState::CancelRequested));
}

// --- Valid transitions ---

TEST(OrderLifecycle, IntentToSubmitting) {
    EXPECT_EQ(validate_transition(OrderState::Intent, OrderState::Submitting),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, SubmittingToAccepted) {
    EXPECT_EQ(validate_transition(OrderState::Submitting, OrderState::Accepted),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, SubmittingToRejected) {
    EXPECT_EQ(validate_transition(OrderState::Submitting, OrderState::Rejected),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, SubmittingToAmbiguous) {
    EXPECT_EQ(validate_transition(OrderState::Submitting, OrderState::Ambiguous),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, SubmittingToFilledImmediate) {
    EXPECT_EQ(validate_transition(OrderState::Submitting, OrderState::Filled),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, AcceptedToPartialFill) {
    EXPECT_EQ(validate_transition(OrderState::Accepted, OrderState::PartialFill),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, AcceptedToFilled) {
    EXPECT_EQ(validate_transition(OrderState::Accepted, OrderState::Filled),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, AcceptedToCancelRequested) {
    EXPECT_EQ(validate_transition(OrderState::Accepted, OrderState::CancelRequested),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, PartialFillToFilled) {
    EXPECT_EQ(validate_transition(OrderState::PartialFill, OrderState::Filled),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, CancelRequestedToCancelled) {
    EXPECT_EQ(validate_transition(OrderState::CancelRequested, OrderState::Cancelled),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, CancelRequestedToFilled) {
    EXPECT_EQ(validate_transition(OrderState::CancelRequested, OrderState::Filled),
              TransitionResult::Ok);
}

// --- Ambiguous transitions (M6) ---

TEST(OrderLifecycle, AmbiguousToReconciled) {
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::Reconciled),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, AmbiguousToEscalated) {
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::EscalatedToOperator),
              TransitionResult::Ok);
}

TEST(OrderLifecycle, AmbiguousCannotRetry) {
    // Ambiguous → Submitting is NOT valid (no blind retry, M6)
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::Submitting),
              TransitionResult::InvalidTransition);
}

TEST(OrderLifecycle, AmbiguousCannotDirectAccept) {
    // Must go through Reconciled, not directly to Accepted
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::Accepted),
              TransitionResult::InvalidTransition);
}

// --- Invalid transitions ---

TEST(OrderLifecycle, IntentCannotSkipToAccepted) {
    EXPECT_EQ(validate_transition(OrderState::Intent, OrderState::Accepted),
              TransitionResult::InvalidTransition);
}

TEST(OrderLifecycle, TerminalCannotTransition) {
    EXPECT_EQ(validate_transition(OrderState::Filled, OrderState::Cancelled),
              TransitionResult::AlreadyTerminal);
    EXPECT_EQ(validate_transition(OrderState::Rejected, OrderState::Submitting),
              TransitionResult::AlreadyTerminal);
}

// --- OrderRecord transition_to ---

TEST(OrderRecord, HappyPath) {
    OrderRecord rec{};
    EXPECT_EQ(rec.state, OrderState::Intent);
    EXPECT_EQ(rec.transition_to(OrderState::Submitting), TransitionResult::Ok);
    EXPECT_EQ(rec.state, OrderState::Submitting);
    EXPECT_EQ(rec.transition_to(OrderState::Accepted), TransitionResult::Ok);
    EXPECT_EQ(rec.state, OrderState::Accepted);
    EXPECT_EQ(rec.transition_to(OrderState::Filled), TransitionResult::Ok);
    EXPECT_EQ(rec.state, OrderState::Filled);
}

TEST(OrderRecord, InvalidTransitionDoesNotChangeState) {
    OrderRecord rec{};
    EXPECT_EQ(rec.transition_to(OrderState::Filled), TransitionResult::InvalidTransition);
    EXPECT_EQ(rec.state, OrderState::Intent);  // unchanged
}

TEST(OrderRecord, AmbiguousReconciliationPath) {
    OrderRecord rec{};
    rec.transition_to(OrderState::Submitting);
    rec.transition_to(OrderState::Ambiguous);
    EXPECT_EQ(rec.state, OrderState::Ambiguous);

    // Cannot retry
    EXPECT_EQ(rec.transition_to(OrderState::Submitting), TransitionResult::InvalidTransition);

    // Can reconcile
    EXPECT_EQ(rec.transition_to(OrderState::Reconciled), TransitionResult::Ok);
}

// --- Client order ID ---

TEST(ClientOrderId, FormatIsCorrect) {
    auto coid = make_client_order_id(1719600000000LL, 1, 0x0042);
    auto v = coid.view();
    EXPECT_FALSE(coid.empty());
    EXPECT_TRUE(v.starts_with("HY-"));
    EXPECT_NE(v.find("0042"), std::string_view::npos);
}

TEST(ClientOrderId, DeterministicSameInputSameOutput) {
    auto a = make_client_order_id(1000, 1, 42);
    auto b = make_client_order_id(1000, 1, 42);
    EXPECT_EQ(a.view(), b.view());
}

TEST(ClientOrderId, DifferentInputsDifferentOutput) {
    auto a = make_client_order_id(1000, 1, 42);
    auto b = make_client_order_id(1000, 2, 42);
    EXPECT_NE(a.view(), b.view());
}

TEST(ClientOrderId, EmptyByDefault) {
    ClientOrderId coid{};
    EXPECT_TRUE(coid.empty());
}

// --- Reconciliation action ---

TEST(Reconcile, AmbiguousQueriesOrder) {
    OrderRecord rec{};
    rec.state = OrderState::Ambiguous;
    rec.query_attempts = 0;
    EXPECT_EQ(determine_reconcile_action(rec), ReconcileAction::QueryOrder);
}

TEST(Reconcile, MaxQueriesEscalates) {
    OrderRecord rec{};
    rec.state = OrderState::Ambiguous;
    rec.query_attempts = OrderRecord::kMaxQueryAttempts;
    EXPECT_EQ(determine_reconcile_action(rec), ReconcileAction::EscalateToOperator);
}

TEST(Reconcile, ShouldEscalateFlag) {
    OrderRecord rec{};
    rec.state = OrderState::Ambiguous;
    rec.query_attempts = OrderRecord::kMaxQueryAttempts;
    EXPECT_TRUE(rec.should_escalate());
}

TEST(Reconcile, NonAmbiguousNoAction) {
    OrderRecord rec{};
    rec.state = OrderState::Accepted;
    EXPECT_EQ(determine_reconcile_action(rec), ReconcileAction::NoAction);
}
