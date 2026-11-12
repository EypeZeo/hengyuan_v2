// Q23 slices S2/S3: never_sent_rules.hpp -- may THIS OrderNeverSent frame close THAT order?
// (docs/NOTSENT_CLOSURE_DESIGN.md 4.3 and 4.6)
//
// Pure logic, no logs: the order is a ClosureTarget (what the recovery scan knows of it) and the frame an AuditRecord. The
// same rule is applied to real logs by the recovery reader (test_never_sent_reader.cpp), and will be applied by the writers
// of slices S4 and S6 before they write -- so a frame a writer lets through is a frame the next start accepts.
//
// The verdicts come in a fixed order (the order first, then the frame: pairing, existence, identity): the order is part of the
// contract, because a caller that wants to say WHY an order cannot be closed asks the order's question first.

#include <gtest/gtest.h>
#include <hengyuan/never_sent_rules.hpp>

#include <cstdint>
#include <cstring>
#include <limits>

using namespace hy;
using namespace hy::never_sent;

namespace {

constexpr std::uint32_t kSymbol = 7;
constexpr std::int64_t kPrice = 500'000;
constexpr std::int64_t kQty = 100;

// The order as the scan knows it: Intent (or whatever state), nothing seen on the exchange.
ClosureTarget target(OrderState state = OrderState::Intent) {
    ClosureTarget t{};
    t.state = state;
    t.symbol_id = kSymbol;
    t.side = OrderSide::Sell;
    t.mode = ExecutionMode::Live;
    t.price_ticks = kPrice;
    t.qty_ticks = kQty;
    return t;
}

// A well-formed closing frame for that order.
AuditRecord frame() {
    AuditRecord r{};
    r.timestamp_ms = 1000;
    r.event_type = AuditEventType::OrderNeverSent;
    r.mode = ExecutionMode::Live;
    r.symbol_id = kSymbol;
    r.set_client_order_id("HY-NS-RULES");
    r.price_ticks = kPrice;
    r.qty_ticks = kQty;
    r.resulting_state = OrderState::NeverSent;
    r.side = OrderSide::Sell;
    r.detail_code = 3;
    return r;
}

constexpr OrderState kAllStates[] = {
    OrderState::Intent,          OrderState::Submitting, OrderState::Accepted,        OrderState::Rejected,
    OrderState::Ambiguous,       OrderState::PartialFill, OrderState::Filled,         OrderState::CancelRequested,
    OrderState::Cancelled,       OrderState::Expired,    OrderState::Reconciled,      OrderState::EscalatedToOperator,
    OrderState::NeverSent,
};

}  // namespace

TEST(NeverSentRules, AWellFormedClosureOfAnIntentOrAPreparedOrderIsOk) {
    EXPECT_EQ(check_closure(target(OrderState::Intent), frame()), ClosureCheck::Ok) << "class A: refused before Prepared";
    EXPECT_EQ(check_closure(target(OrderState::Submitting), frame()), ClosureCheck::Ok) << "class B: refused by the final gate";
}

TEST(NeverSentRules, NotCheckedIsNotAPass) {
    static_assert(static_cast<int>(ClosureCheck::NotChecked) == 0, "a result nobody filled in must be the zero value");
    static_assert(ClosureCheck::NotChecked != ClosureCheck::Ok);
    EXPECT_NE(ClosureCheck{}, ClosureCheck::Ok);
}

// --- the order's question ---------------------------------------------------------------------------------------------------

TEST(NeverSentRules, OnlyIntentAndSubmittingAreClosable) {
    for (const OrderState s : kAllStates) {
        const ClosureCheck expected = (s == OrderState::Intent || s == OrderState::Submitting) ? ClosureCheck::Ok
                                                                                               : ClosureCheck::NotInAClosableState;
        EXPECT_EQ(check_order_closable(target(s)), expected) << order_state_name(s);
        EXPECT_EQ(check_closure(target(s), frame()), expected) << order_state_name(s) << " with a perfectly good frame";
    }
}

TEST(NeverSentRules, AnOrderThatMayHaveCrossedIsNeverClosable) {
    for (const OrderState s : {OrderState::Ambiguous, OrderState::EscalatedToOperator, OrderState::Accepted,
                               OrderState::PartialFill, OrderState::CancelRequested, OrderState::Rejected,
                               OrderState::Filled, OrderState::Cancelled, OrderState::Expired, OrderState::Reconciled}) {
        EXPECT_EQ(check_closure(target(s), frame()), ClosureCheck::NotInAClosableState) << order_state_name(s);
    }
}

TEST(NeverSentRules, ADoubleClosureIsNotClosable) {
    EXPECT_EQ(check_closure(target(OrderState::NeverSent), frame()), ClosureCheck::NotInAClosableState)
        << "an order already closed as never sent is terminal";
}

TEST(NeverSentRules, AnOrderThatShowedAnyTraceOfExistingOnTheExchangeIsNotClosable) {
    for (const OrderState s : {OrderState::Intent, OrderState::Submitting}) {
        ClosureTarget with_id = target(s);
        with_id.exchange_order_id = 77;
        EXPECT_EQ(check_order_closable(with_id), ClosureCheck::OrderShowsExistence) << order_state_name(s);
        EXPECT_EQ(check_closure(with_id, frame()), ClosureCheck::OrderShowsExistence) << order_state_name(s);

        ClosureTarget with_fill = target(s);
        with_fill.filled_qty_ticks = 1;
        EXPECT_EQ(check_order_closable(with_fill), ClosureCheck::OrderShowsExistence) << order_state_name(s);

        ClosureTarget negative_id = target(s);
        negative_id.exchange_order_id = -1;
        EXPECT_EQ(check_order_closable(negative_id), ClosureCheck::OrderShowsExistence)
            << "any non-zero id is a sign; the sign of the number does not matter";
    }
}

// --- the frame's questions ---------------------------------------------------------------------------------------------------

TEST(NeverSentRules, PairingHoldsOnlyWhenTheFrameSaysNeverSentInBothPlacesOrNeither) {
    AuditRecord both = frame();
    AuditRecord neither = frame();
    neither.event_type = AuditEventType::OrderRejected;
    neither.resulting_state = OrderState::Rejected;
    AuditRecord event_only = frame();
    event_only.resulting_state = OrderState::Submitting;
    AuditRecord state_only = frame();
    state_only.event_type = AuditEventType::OrderRejected;
    EXPECT_TRUE(pairing_holds(both));
    EXPECT_TRUE(pairing_holds(neither)) << "an ordinary frame says it in neither place";
    EXPECT_FALSE(pairing_holds(event_only));
    EXPECT_FALSE(pairing_holds(state_only));
}

TEST(NeverSentRules, TheFrameMustSayNeverSentInBothPlaces) {
    for (const OrderState s : {OrderState::Intent, OrderState::Submitting}) {
        AuditRecord event_only = frame();
        event_only.resulting_state = OrderState::Ambiguous;
        EXPECT_EQ(check_closure(target(s), event_only), ClosureCheck::FramePairingBroken) << order_state_name(s);

        AuditRecord state_only = frame();
        state_only.event_type = AuditEventType::OrderRejected;
        EXPECT_EQ(check_closure(target(s), state_only), ClosureCheck::FramePairingBroken) << order_state_name(s);

        AuditRecord neither = frame();
        neither.event_type = AuditEventType::OrderRejected;
        neither.resulting_state = OrderState::Rejected;
        EXPECT_EQ(check_closure(target(s), neither), ClosureCheck::FramePairingBroken)
            << "an ordinary frame is not a closing frame: check_closing_frame() is asked about closing frames only";
    }
}

TEST(NeverSentRules, TheFrameMayNotClaimToHaveExistedOrFilled) {
    {
        AuditRecord r = frame();
        r.exchange_order_id = 5;
        EXPECT_EQ(check_closure(target(), r), ClosureCheck::FrameClaimsExistence) << "an exchange order id";
    }
    {
        AuditRecord r = frame();
        r.filled_qty_ticks = 1;
        EXPECT_EQ(check_closure(target(), r), ClosureCheck::FrameClaimsExistence) << "a fill";
    }
    {
        AuditRecord r = frame();
        r.avg_fill_price_ticks = 1;
        EXPECT_EQ(check_closure(target(), r), ClosureCheck::FrameClaimsExistence) << "an average fill price";
    }
}

TEST(NeverSentRules, TheFrameMustDescribeTheOrderTheIntentFrameDescribed) {
    const auto with = [](auto mutate, const char* why) {
        AuditRecord r = frame();
        mutate(r);
        EXPECT_EQ(check_closure(target(), r), ClosureCheck::NotTheSameOrder) << why;
        EXPECT_EQ(check_closure(target(OrderState::Submitting), r), ClosureCheck::NotTheSameOrder) << why << " (class B)";
    };
    with([](AuditRecord& r) { r.symbol_id += 1; }, "symbol_id");
    with([](AuditRecord& r) { r.side = OrderSide::Buy; }, "side");
    with([](AuditRecord& r) { r.mode = ExecutionMode::DryRun; }, "mode");
    with([](AuditRecord& r) { r.price_ticks += 1; }, "price_ticks");
    with([](AuditRecord& r) { r.qty_ticks += 1; }, "qty_ticks");
    with([](AuditRecord& r) { r.price_ticks = 0; }, "price_ticks zeroed");
    with([](AuditRecord& r) { r.qty_ticks = 0; }, "qty_ticks zeroed");
}

// --- the order of the verdicts ------------------------------------------------------------------------------------------------

TEST(NeverSentRules, TheOrderIsJudgedBeforeTheFrame) {
    AuditRecord broken = frame();
    broken.resulting_state = OrderState::Intent;  // the pairing is broken as well
    broken.exchange_order_id = 9;
    broken.symbol_id += 1;
    EXPECT_EQ(check_closure(target(OrderState::Ambiguous), broken), ClosureCheck::NotInAClosableState);

    ClosureTarget existing = target(OrderState::Submitting);
    existing.exchange_order_id = 5;
    EXPECT_EQ(check_closure(existing, broken), ClosureCheck::OrderShowsExistence)
        << "an order that showed existence is refused as such, whatever the frame says";
}

TEST(NeverSentRules, TheFrameChecksRunPairingThenExistenceThenIdentity) {
    AuditRecord r = frame();
    r.resulting_state = OrderState::Submitting;  // pairing broken
    r.exchange_order_id = 9;                      // also claims existence
    r.symbol_id += 1;                             // also another order
    EXPECT_EQ(check_closure(target(), r), ClosureCheck::FramePairingBroken);

    r.resulting_state = OrderState::NeverSent;  // pairing restored
    EXPECT_EQ(check_closure(target(), r), ClosureCheck::FrameClaimsExistence);

    r.exchange_order_id = 0;  // claims nothing any more
    EXPECT_EQ(check_closure(target(), r), ClosureCheck::NotTheSameOrder);

    r.symbol_id = kSymbol;
    EXPECT_EQ(check_closure(target(), r), ClosureCheck::Ok);
}

// --- what the rule deliberately does not look at ---------------------------------------------------------------------------

TEST(NeverSentRules, TheReasonCodeIsInformationalAndAnyValueIsAccepted) {
    // design 4.3: the machine field carries the REASON (a class A or class B refusal cause). A value this build does not know
    // is not corruption: the frame is MAC-protected, and a reader that failed the whole log on an unfamiliar reason would turn
    // an informational field into a way to make a store unopenable.
    for (const std::int64_t code : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}, std::int64_t{1} << 40,
                                    std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()}) {
        AuditRecord r = frame();
        r.detail_code = code;
        EXPECT_EQ(check_closure(target(), r), ClosureCheck::Ok) << code;
    }
}

TEST(NeverSentRules, TheHumanReadableDetailIsNotLookedAtEither) {
    // The record's text field is informational: a short text and a buffer filled to its last byte get the same verdict. The full
    // buffer is filled directly, not through set_detail(): GCC (-Werror=stringop-truncation, seen at -O2 under ASan) refuses a
    // constant longer than the buffer as an argument of the truncating copy.
    AuditRecord r = frame();
    r.set_detail("anything at all");
    EXPECT_EQ(check_closure(target(), r), ClosureCheck::Ok);
    std::memset(r.detail_msg, 'x', sizeof(r.detail_msg) - 1);
    r.detail_msg[sizeof(r.detail_msg) - 1] = '\0';
    EXPECT_EQ(check_closure(target(), r), ClosureCheck::Ok);
}

TEST(NeverSentRules, TheCallerEstablishesTheThreeFactsTheRuleDoesNotLookAt) {
    // That the frame is the LAST frame of its client order id, that it is written at format v6, and that it follows an Intent
    // frame are facts only a caller with the whole log can establish (see the header comment); the rule is a pure function of
    // an order and a frame. This pins that it carries no hidden state: the same inputs, the same verdict, any number of times.
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(check_closure(target(), frame()), ClosureCheck::Ok);
        EXPECT_EQ(check_closure(target(OrderState::Ambiguous), frame()), ClosureCheck::NotInAClosableState);
    }
}
