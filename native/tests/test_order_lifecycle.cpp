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

// docs/SPEC_INVARIANTS.md's "Reconciled" entry: SUBMITPORT spec round 24 is
// authoritative that no live transition ever targets Reconciled. Code used to
// disagree (Ambiguous -> Reconciled was Ok) until this was resolved by fixing
// the code side, not the spec side. Pinned as permanently InvalidTransition so
// a future "helpful" re-add is caught here.
TEST(OrderLifecycle, AmbiguousToReconciledIsInvalid) {
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::Reconciled),
              TransitionResult::InvalidTransition);
}

// Reconciliation resolves Ambiguous directly to whichever exchange-final state
// the query actually discovered — never to a generic bucket that would discard
// which outcome it was.
TEST(OrderLifecycle, AmbiguousResolvesToDiscoveredExchangeFinalState) {
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::Filled),
              TransitionResult::Ok);
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::Cancelled),
              TransitionResult::Ok);
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::Rejected),
              TransitionResult::Ok);
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::Expired),
              TransitionResult::Ok);
}

// SUBMITPORT spec line 961's full Ambiguous target set also includes the two
// "actually still live" outcomes -- order_tracker.hpp's poll_once() is the first
// caller that can discover these via reconciliation. Not exchange-final
// (is_exchange_final() stays false for both), so a caller reaching one of these
// must keep tracking the order under the separate open-order-polling mechanism
// (spec L4 §6.6) -- out of scope here, this test only pins the transition itself.
TEST(OrderLifecycle, AmbiguousResolvesToStillLiveState) {
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::Accepted),
              TransitionResult::Ok);
    EXPECT_EQ(validate_transition(OrderState::Ambiguous, OrderState::PartialFill),
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

    // Reconciliation query discovered the order was actually filled — resolves
    // directly to Filled, not to a generic Reconciled bucket.
    EXPECT_EQ(rec.transition_to(OrderState::Filled), TransitionResult::Ok);
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

// ===========================================================================
// EXHAUSTIVE TRANSITION RELATION — all 12 x 12 = 144 (from, to) pairs
// ===========================================================================
//
// WHY: before this table, the tests above pinned 19 of 144 pairs (13.2%). The
// gaps were not harmless. Every one of the four VALID Expired transitions was
// unpinned, so deleting `to == OrderState::Expired` from validate_transition()
// — which would strand every expiring order in a live state — passed the entire
// suite. So did 70 of the 72 terminal-state pairs.
//
// This table states the COMPLETE relation in one place, so the state machine's
// behaviour can be diffed against the spec by reading rather than by inferring
// it from scattered examples. A change to validate_transition() must now be
// accompanied by a deliberate change here.
//
// Expected values are derived from order_lifecycle.hpp's own contract:
//   * is_terminal(from) short-circuits to AlreadyTerminal for all 12 targets,
//     regardless of `to`. Six terminal states x 12 = 72 pairs.
//   * The 22 Ok transitions are listed explicitly in kValidTransitions below.
//   * Everything else is InvalidTransition.
namespace {

constexpr OrderState kAllStates[] = {
    OrderState::Intent,          OrderState::Submitting,
    OrderState::Accepted,        OrderState::Rejected,
    OrderState::Ambiguous,       OrderState::PartialFill,
    OrderState::Filled,          OrderState::CancelRequested,
    OrderState::Cancelled,       OrderState::Expired,
    OrderState::Reconciled,      OrderState::EscalatedToOperator,
};
static_assert(sizeof(kAllStates) / sizeof(kAllStates[0]) == 12,
              "OrderState gained or lost a value — update this table deliberately, "
              "do not just resize it");

struct Transition {
    OrderState from;
    OrderState to;
};

// The complete Ok set. Anything not here (and not from a terminal state) must be
// InvalidTransition.
constexpr Transition kValidTransitions[] = {
    {OrderState::Intent,          OrderState::Submitting},
    // Submitting: exchange responded, or the response was ambiguous.
    {OrderState::Submitting,      OrderState::Accepted},
    {OrderState::Submitting,      OrderState::Rejected},
    {OrderState::Submitting,      OrderState::Ambiguous},
    {OrderState::Submitting,      OrderState::Filled},   // immediate fill
    // Accepted: resting on the book.
    {OrderState::Accepted,        OrderState::PartialFill},
    {OrderState::Accepted,        OrderState::Filled},
    {OrderState::Accepted,        OrderState::CancelRequested},
    {OrderState::Accepted,        OrderState::Expired},
    // Ambiguous: reconciliation resolves to whichever state the query discovered
    // (exchange-final, or still-live Accepted/PartialFill per spec line 961), or
    // escalation — never a blind resubmit, and never the generic Reconciled
    // bucket (docs/SPEC_INVARIANTS.md's "Reconciled" entry).
    {OrderState::Ambiguous,       OrderState::Accepted},
    {OrderState::Ambiguous,       OrderState::PartialFill},
    {OrderState::Ambiguous,       OrderState::Filled},
    {OrderState::Ambiguous,       OrderState::Cancelled},
    {OrderState::Ambiguous,       OrderState::Rejected},
    {OrderState::Ambiguous,       OrderState::Expired},
    {OrderState::Ambiguous,       OrderState::EscalatedToOperator},
    // PartialFill: still open.
    {OrderState::PartialFill,     OrderState::Filled},
    {OrderState::PartialFill,     OrderState::CancelRequested},
    {OrderState::PartialFill,     OrderState::Expired},
    // CancelRequested: cancel may lose the race to a fill or an expiry.
    {OrderState::CancelRequested, OrderState::Cancelled},
    {OrderState::CancelRequested, OrderState::Filled},
    {OrderState::CancelRequested, OrderState::Expired},
};
constexpr std::size_t kValidCount = sizeof(kValidTransitions) / sizeof(kValidTransitions[0]);
static_assert(kValidCount == 22, "the Ok set changed size — update deliberately");

bool is_listed_valid(OrderState from, OrderState to) {
    for (std::size_t i = 0; i < kValidCount; ++i) {
        if (kValidTransitions[i].from == from && kValidTransitions[i].to == to) return true;
    }
    return false;
}

TransitionResult expected_result(OrderState from, OrderState to) {
    if (is_terminal(from)) return TransitionResult::AlreadyTerminal;
    return is_listed_valid(from, to) ? TransitionResult::Ok : TransitionResult::InvalidTransition;
}

}  // namespace

TEST(TransitionRelation, EveryOneOf144PairsMatchesTheTable) {
    int ok_count = 0, invalid_count = 0, terminal_count = 0;
    for (OrderState from : kAllStates) {
        for (OrderState to : kAllStates) {
            const TransitionResult expected = expected_result(from, to);
            const TransitionResult actual = validate_transition(from, to);
            EXPECT_EQ(actual, expected)
                << "validate_transition(" << order_state_name(from) << ", "
                << order_state_name(to) << ") disagrees with the exhaustive table";
            switch (expected) {
                case TransitionResult::Ok: ++ok_count; break;
                case TransitionResult::InvalidTransition: ++invalid_count; break;
                case TransitionResult::AlreadyTerminal: ++terminal_count; break;
            }
        }
    }
    // Guards against the table silently going vacuous (e.g. a future edit that
    // empties kAllStates would otherwise "pass" with zero comparisons).
    EXPECT_EQ(ok_count + invalid_count + terminal_count, 144);
    EXPECT_EQ(ok_count, 22);
    EXPECT_EQ(terminal_count, 72) << "6 terminal states x 12 targets";
    EXPECT_EQ(invalid_count, 50);
}

// Called out separately because these four were the specific blind spot: no test
// anywhere used Expired as a destination, so the branches enabling them could be
// deleted with the whole suite still green.
TEST(TransitionRelation, ExpiryIsReachableFromEveryLiveRestingState) {
    EXPECT_EQ(validate_transition(OrderState::Accepted, OrderState::Expired), TransitionResult::Ok);
    EXPECT_EQ(validate_transition(OrderState::PartialFill, OrderState::Expired), TransitionResult::Ok);
    EXPECT_EQ(validate_transition(OrderState::CancelRequested, OrderState::Expired), TransitionResult::Ok);
    // ...but not from Submitting: the exchange cannot expire an order it has not
    // yet acknowledged.
    EXPECT_EQ(validate_transition(OrderState::Submitting, OrderState::Expired),
              TransitionResult::InvalidTransition);
}

// No path back into Submitting from anywhere. This is the state-machine half of
// the no-blind-resubmit guarantee (InFlightRegistry is the other half).
TEST(TransitionRelation, NothingEverReturnsToSubmitting) {
    for (OrderState from : kAllStates) {
        if (from == OrderState::Intent) continue;  // the one legal entry
        EXPECT_NE(validate_transition(from, OrderState::Submitting), TransitionResult::Ok)
            << order_state_name(from) << " -> Submitting must never be Ok: a second POST for a "
            << "client-order-id that may already rest on the exchange is a duplicate order";
    }
}

// Self-loops are never Ok. Same-state re-assertion is handled at the replay layer
// as an accepted no-op, NOT by transitioning through the state machine.
TEST(TransitionRelation, NoSelfLoopIsOk) {
    for (OrderState s : kAllStates) {
        EXPECT_NE(validate_transition(s, s), TransitionResult::Ok)
            << order_state_name(s) << " -> itself must not be a valid transition";
    }
}
