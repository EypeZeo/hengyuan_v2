// Q23 slices S2/S3: never_sent_rules.hpp -- may THIS OrderNeverSent frame close THAT order?
// (docs/NOTSENT_CLOSURE_DESIGN.md 4.3 and 4.6)
//
// Pure logic, no logs: the order is a ClosureTarget (what the recovery scan knows of it) and the frame an AuditRecord. The
// same rule is applied to real logs by the recovery reader (test_never_sent_reader.cpp) and by the writer
// DurableAuditSink::append_never_sent() before it writes (test_never_sent_writer.cpp) -- so a frame the writer lets through is
// a frame the next start accepts.
//
// The verdicts come in a fixed order (the order first, then the frame: pairing, existence, identity): the order is part of the
// contract, because a caller that wants to say WHY an order cannot be closed asks the order's question first.

#include <gtest/gtest.h>
#include <hengyuan/never_sent_rules.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

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

// --- class A (Q23 S6): the pure builders ------------------------------------------------------------------------------------

namespace {

// The Intent frame of the order every class A test below refuses.
AuditRecord intent_frame_of_the_order() {
    AuditRecord r{};
    r.timestamp_ms = 1000;
    r.event_type = AuditEventType::OrderIntentCreated;
    r.mode = ExecutionMode::Live;
    r.symbol_id = kSymbol;
    r.set_client_order_id("HY-NS-INTENT");
    r.price_ticks = kPrice;
    r.qty_ticks = kQty;
    r.resulting_state = OrderState::Intent;
    r.side = OrderSide::Sell;
    return r;
}

constexpr Reason kAllReasons[] = {Reason::ConfirmationMissing,        Reason::ConfirmationMismatch, Reason::SubmitPortInvalid,
                                  Reason::InFlightRegistryUnavailable, Reason::RateLimitExhausted,   Reason::DuplicateInFlight};

}  // namespace

TEST(NeverSentRules, EveryClassAReasonYieldsAFrameTheRuleAcceptsForTheOrderItWasBuiltFrom) {
    const AuditRecord intent = intent_frame_of_the_order();
    const ClosureTarget t = class_a_target_of(intent);
    for (const Reason reason : kAllReasons) {
        const AuditRecord closing = class_a_closure_of(intent, reason, 9000);
        EXPECT_EQ(check_closure(t, closing), ClosureCheck::Ok) << static_cast<long long>(reason);
        EXPECT_EQ(closing.detail_code, static_cast<std::int64_t>(reason)) << "the reason is the frame's detail_code";
        EXPECT_EQ(closing.detail_msg[0], '\0') << "and nothing is in detail_msg (design 4.3)";
        EXPECT_EQ(closing.timestamp_ms, 9000);
        EXPECT_EQ(closing.event_type, AuditEventType::OrderNeverSent);
        EXPECT_EQ(closing.resulting_state, OrderState::NeverSent);
        EXPECT_STREQ(closing.client_order_id, "HY-NS-INTENT");
    }
}

TEST(NeverSentRules, TheReasonCodesAreTheDocumentedNumbers) {
    // Stable for whoever reads the log (docs/NOTSENT_CLOSURE_DESIGN.md 4.3): changing one is changing the log's meaning.
    EXPECT_EQ(static_cast<int>(Reason::ConfirmationMissing), 1);
    EXPECT_EQ(static_cast<int>(Reason::ConfirmationMismatch), 2);
    EXPECT_EQ(static_cast<int>(Reason::SubmitPortInvalid), 3);
    EXPECT_EQ(static_cast<int>(Reason::InFlightRegistryUnavailable), 4);
    EXPECT_EQ(static_cast<int>(Reason::RateLimitExhausted), 5);
    EXPECT_EQ(static_cast<int>(Reason::DuplicateInFlight), 6);
}

TEST(NeverSentRules, ATargetBuiltFromAnIntentIsAnIntentWithNothingSeen) {
    const ClosureTarget t = class_a_target_of(intent_frame_of_the_order());
    EXPECT_EQ(t.state, OrderState::Intent);
    EXPECT_EQ(t.exchange_order_id, 0);
    EXPECT_EQ(t.filled_qty_ticks, 0);
    EXPECT_EQ(t.symbol_id, kSymbol);
    EXPECT_EQ(t.side, OrderSide::Sell);
    EXPECT_EQ(t.mode, ExecutionMode::Live);
    EXPECT_EQ(t.price_ticks, kPrice);
    EXPECT_EQ(t.qty_ticks, kQty);
}

TEST(NeverSentRules, TheClosingFrameClaimsNoExistenceEvenWhenTheRecordItWasBuiltFromDid) {
    // The builder is only ever given an Intent frame, which claims nothing. Given a dirty record it must still not pass an
    // existence claim on: such a frame would be refused by the rule (FrameClaimsExistence), and a closure must never say it.
    AuditRecord dirty = intent_frame_of_the_order();
    dirty.exchange_order_id = 9;
    dirty.filled_qty_ticks = 4;
    dirty.avg_fill_price_ticks = 500'000;
    dirty.detail_code = 77;
    dirty.set_detail("a message that must not survive");
    const AuditRecord closing = class_a_closure_of(dirty, Reason::RateLimitExhausted, 9000);
    EXPECT_EQ(closing.exchange_order_id, 0);
    EXPECT_EQ(closing.filled_qty_ticks, 0);
    EXPECT_EQ(closing.avg_fill_price_ticks, 0);
    EXPECT_EQ(closing.detail_code, static_cast<std::int64_t>(Reason::RateLimitExhausted)) << "the reason replaces whatever was there";
    EXPECT_EQ(closing.detail_msg[0], '\0');
    EXPECT_EQ(check_closure(class_a_target_of(intent_frame_of_the_order()), closing), ClosureCheck::Ok);
}

// --- the recently closed ids ------------------------------------------------------------------------------------------------

TEST(NeverSentRulesRecentlyClosed, RemembersWhatItWasToldAndNothingElse) {
    RecentlyClosedIds ids;
    EXPECT_EQ(ids.size(), 0u);
    EXPECT_FALSE(ids.contains("HY-1-1-0007"));
    ids.remember("HY-1-1-0007");
    EXPECT_EQ(ids.size(), 1u);
    EXPECT_TRUE(ids.contains("HY-1-1-0007"));
    EXPECT_FALSE(ids.contains("HY-1-2-0007"));
    EXPECT_FALSE(ids.contains("HY-1-1-0008"));
}

TEST(NeverSentRulesRecentlyClosed, AnIdThatIsOnlyAPrefixOfARememberedOneIsNotThatOne) {
    // The comparison is of whole ids: HY-12 must not be refused because HY-123 was closed.
    RecentlyClosedIds ids;
    ids.remember("HY-1700000000000-12-0007");
    EXPECT_FALSE(ids.contains("HY-1700000000000-1-0007"));
    EXPECT_FALSE(ids.contains("HY-1700000000000-12"));
    EXPECT_FALSE(ids.contains("HY-1700000000000-12-00070"));
    EXPECT_TRUE(ids.contains("HY-1700000000000-12-0007"));
}

TEST(NeverSentRulesRecentlyClosed, EmptyAndTooLongIdsAreIgnoredAndTheLongestLegalOneIsKept) {
    RecentlyClosedIds ids;
    ids.remember("");
    EXPECT_EQ(ids.size(), 0u) << "an empty id names nothing";
    EXPECT_FALSE(ids.contains("")) << "and nothing is ever 'the empty id'";
    const std::string longest(kClientOrderIdLen, 'x');
    const std::string too_long(kClientOrderIdLen + 1, 'y');
    ids.remember(too_long);
    EXPECT_EQ(ids.size(), 0u) << "an id that does not fit a ClientOrderId cannot be one";
    EXPECT_FALSE(ids.contains(too_long));
    ids.remember(longest);
    EXPECT_EQ(ids.size(), 1u);
    EXPECT_TRUE(ids.contains(longest)) << "an id exactly as long as a ClientOrderId can hold is legal";
}

TEST(NeverSentRulesRecentlyClosed, TheOldestIdIsForgottenFirstWhenTheRingIsFull) {
    RecentlyClosedIds ids;
    auto name = [](std::size_t i) { return "HY-ring-" + std::to_string(i); };
    for (std::size_t i = 0; i < RecentlyClosedIds::kCapacity; ++i) ids.remember(name(i));
    EXPECT_EQ(ids.size(), RecentlyClosedIds::kCapacity);
    for (std::size_t i = 0; i < RecentlyClosedIds::kCapacity; ++i) EXPECT_TRUE(ids.contains(name(i))) << i;
    ids.remember(name(RecentlyClosedIds::kCapacity));  // the 65th id evicts the 1st, and only the 1st
    EXPECT_EQ(ids.size(), RecentlyClosedIds::kCapacity) << "a full ring does not grow";
    EXPECT_FALSE(ids.contains(name(0)));
    for (std::size_t i = 1; i <= RecentlyClosedIds::kCapacity; ++i) EXPECT_TRUE(ids.contains(name(i))) << i;
    ids.remember(name(RecentlyClosedIds::kCapacity + 1));  // ...then the 2nd
    EXPECT_FALSE(ids.contains(name(1)));
    EXPECT_TRUE(ids.contains(name(2)));
    // a long run keeps exactly the last kCapacity ids
    for (std::size_t i = RecentlyClosedIds::kCapacity + 2; i < 5 * RecentlyClosedIds::kCapacity + 3; ++i) ids.remember(name(i));
    const std::size_t last = 5 * RecentlyClosedIds::kCapacity + 2;
    for (std::size_t i = last + 1 - RecentlyClosedIds::kCapacity; i <= last; ++i) EXPECT_TRUE(ids.contains(name(i))) << i;
    EXPECT_FALSE(ids.contains(name(last - RecentlyClosedIds::kCapacity)));
}
