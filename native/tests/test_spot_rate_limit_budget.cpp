// TODOLIST Stage 1A · TODO 1A.2: spot_rate_limit_budget.hpp unit tests.
// All local (no network) -- pure tracker/budget-split logic.
#include <gtest/gtest.h>
#include <hengyuan/spot_rate_limit_budget.hpp>

using hy::EndpointWeightTable;
using hy::PartitionedRateBudget;
using hy::PrivateRestEndpoint;
using hy::RateLimitBudgetSplit;
using hy::RateLimitLane;
using hy::SpotRateLimitTracker;
using hy::endpoint_weight;

// --- RateLimitBudgetSplit::is_valid() ---

TEST(RateLimitBudgetSplit, DefaultSplitIsValid) {
    EXPECT_TRUE(RateLimitBudgetSplit::default_split().is_valid());
}

TEST(RateLimitBudgetSplit, DefaultIsEightyTenTen) {
    auto s = RateLimitBudgetSplit::default_split();
    EXPECT_EQ(s.strategy_pct, 80u);
    EXPECT_EQ(s.reconciliation_pct, 10u);
    EXPECT_EQ(s.emergency_pct, 10u);
}

TEST(RateLimitBudgetSplit, SumNotEqualToHundredIsInvalid) {
    RateLimitBudgetSplit s{70, 10, 10};  // sums to 90
    EXPECT_FALSE(s.is_valid());

    RateLimitBudgetSplit over{80, 10, 20};  // sums to 110
    EXPECT_FALSE(over.is_valid());
}

TEST(RateLimitBudgetSplit, AnyOtherValidPartitionIsAccepted) {
    RateLimitBudgetSplit s{50, 25, 25};
    EXPECT_TRUE(s.is_valid());
}

TEST(RateLimitBudgetSplit, IndividualFieldOverHundredRejected) {
    RateLimitBudgetSplit s{150, 0, 0};  // would "sum" to 150 via wraparound tricks if unchecked
    EXPECT_FALSE(s.is_valid());
}

// --- PartitionedRateBudget: floor-proportional split ---

TEST(PartitionedRateBudget, InvalidSplitZeroesEveryLaneAndFails) {
    PartitionedRateBudget b;
    RateLimitBudgetSplit bad{50, 10, 10};  // sums to 70
    EXPECT_FALSE(b.reset(1000, 0, bad));
    EXPECT_FALSE(b.can_send(RateLimitLane::Strategy, 1));
    EXPECT_FALSE(b.can_send(RateLimitLane::Reconciliation, 1));
    EXPECT_FALSE(b.can_send(RateLimitLane::Emergency, 1));
}

TEST(PartitionedRateBudget, FloorSplitNeverExceedsAvailableTotal) {
    // 101 is not evenly divisible by 80/10/10 -- proves the floor-division safety
    // property (sum(lane_limit) <= available_total), not just the exact-multiple case.
    PartitionedRateBudget b;
    ASSERT_TRUE(b.reset(101, 0));
    using Clock = PartitionedRateBudget::Clock;
    auto now = Clock::now();

    // available_total = 101, Strategy = floor(101*80/100) = 80,
    // Reconciliation = floor(101*10/100) = 10, Emergency = floor(101*10/100) = 10.
    // sum = 100 <= 101 -- one unit is conservatively unclaimed by any lane, exactly
    // the safety margin floor division buys.
    EXPECT_EQ(b.remaining(RateLimitLane::Strategy, now), 80u);
    EXPECT_EQ(b.remaining(RateLimitLane::Reconciliation, now), 10u);
    EXPECT_EQ(b.remaining(RateLimitLane::Emergency, now), 10u);
}

TEST(PartitionedRateBudget, SafetyMarginSubtractedOnlyOnceAtOuterLevel) {
    // limit=100, safety=20 -> available_total=80. Strategy lane = floor(80*80/100)=64,
    // NOT floor((100*0.8)-（20*0.8)) or any double-subtraction variant -- each inner
    // RequestWeightTracker gets safety_margin=0.
    PartitionedRateBudget b;
    ASSERT_TRUE(b.reset(100, 20));
    EXPECT_EQ(b.remaining(RateLimitLane::Strategy), 64u);
}

TEST(PartitionedRateBudget, LaneIsolationStrategyExhaustionDoesNotAffectOthers) {
    PartitionedRateBudget b;
    ASSERT_TRUE(b.reset(100, 0));  // Strategy=80, Reconciliation=10, Emergency=10
    ASSERT_TRUE(b.try_consume(RateLimitLane::Strategy, 80));
    EXPECT_FALSE(b.can_send(RateLimitLane::Strategy, 1));
    EXPECT_TRUE(b.can_send(RateLimitLane::Reconciliation, 10));
    EXPECT_TRUE(b.can_send(RateLimitLane::Emergency, 10));
}

TEST(PartitionedRateBudget, EmergencyLaneUnreachableThroughOtherLanes) {
    PartitionedRateBudget b;
    ASSERT_TRUE(b.reset(100, 0));
    // Draining Strategy and Reconciliation entirely must never touch Emergency.
    ASSERT_TRUE(b.try_consume(RateLimitLane::Strategy, 80));
    ASSERT_TRUE(b.try_consume(RateLimitLane::Reconciliation, 10));
    EXPECT_EQ(b.remaining(RateLimitLane::Emergency), 10u);
    EXPECT_TRUE(b.can_send(RateLimitLane::Emergency, 10));
}

TEST(PartitionedRateBudget, RollbackOnlyAffectsItsOwnLane) {
    PartitionedRateBudget b;
    ASSERT_TRUE(b.reset(100, 0));
    ASSERT_TRUE(b.try_consume(RateLimitLane::Strategy, 80));
    ASSERT_TRUE(b.try_consume(RateLimitLane::Reconciliation, 10));

    b.rollback(RateLimitLane::Strategy, 80);
    EXPECT_TRUE(b.can_send(RateLimitLane::Strategy, 80));
    EXPECT_FALSE(b.can_send(RateLimitLane::Reconciliation, 1))
        << "rollback on Strategy must not have touched Reconciliation";
}

// --- endpoint_weight() ---

TEST(EndpointWeight, PinnedDefaultsMatchSpecSection74) {
    EndpointWeightTable t;
    EXPECT_EQ(endpoint_weight(t, PrivateRestEndpoint::PostOrder), 1u);
    EXPECT_EQ(endpoint_weight(t, PrivateRestEndpoint::GetOrder), 4u);
    EXPECT_EQ(endpoint_weight(t, PrivateRestEndpoint::GetAccount), 20u);
    EXPECT_EQ(endpoint_weight(t, PrivateRestEndpoint::GetExchangeInfo), 20u);
    EXPECT_EQ(endpoint_weight(t, PrivateRestEndpoint::GetServerTime), 1u);
}

TEST(EndpointWeight, OutOfRangeEndpointRefusedNotGuessed) {
    EndpointWeightTable t;
    auto bogus = static_cast<PrivateRestEndpoint>(200);
    EXPECT_EQ(endpoint_weight(t, bogus), std::numeric_limits<std::uint32_t>::max());
}

// --- SpotRateLimitTracker ---

TEST(SpotRateLimitTracker, ConfigureRejectsAnInvalidSplit) {
    SpotRateLimitTracker tr;
    RateLimitBudgetSplit bad{1, 1, 1};
    EXPECT_FALSE(tr.configure(1000, 0, 1000, 0, 1000, 0, bad));
}

TEST(SpotRateLimitTracker, TryReserveOrderReservesAllThreeDimensionsAtomically) {
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(/*weight*/ 100, 0, /*raw*/ 100, 0, /*orders*/ 100, 0));
    // Each dimension's Strategy lane = floor(100*80/100) = 80.
    EXPECT_TRUE(tr.can_send_order(RateLimitLane::Strategy, 80));
    ASSERT_TRUE(tr.try_reserve_order(RateLimitLane::Strategy, 80));
    EXPECT_FALSE(tr.can_send_order(RateLimitLane::Strategy, 1))
        << "weight lane exhausted -- must block even though raw/orders still have headroom";
}

// A failed try_reserve_order() at the ORDERS stage must roll back BOTH of the two
// earlier stages (weight and raw) -- exercises the full 3-stage cascade, not just one
// rollback call in isolation (that's already covered directly at the
// RequestWeightTracker/PartitionedRateBudget level above).
//
// Reconfiguring between phases would trivially reset every lane regardless of whether
// a leak occurred, so it can't discriminate "rolled back correctly" from "leaked" --
// this test instead sizes the weight lane to an EXACT boundary so a leaked (not
// rolled-back) reservation from the failed attempt would push a later, otherwise-valid
// probe over the limit, while a correctly-rolled-back one leaves it comfortably inside.
// orders is deliberately left with capacity for exactly ONE successful order (permanently
// exhausted after that) so a later probe must go through try_reserve_weight_only() --
// the one public method that touches weight+raw but never orders -- to observe weight/
// raw state without orders' permanent exhaustion masking the result.
TEST(SpotRateLimitTracker, FailureAtOrdersStageRollsBackBothWeightAndRaw) {
    SpotRateLimitTracker tr;
    // weight: available_total=88 -> Strategy lane = floor(88*80/100) = 70.
    // raw: huge headroom (not the dimension under test here).
    // orders: available_total=2 -> Strategy lane = floor(2*80/100) = 1 (exactly one
    // successful order, ever).
    ASSERT_TRUE(tr.configure(/*weight*/ 88, 0, /*raw*/ 1000, 0, /*orders*/ 2, 0));

    ASSERT_TRUE(tr.try_reserve_order(RateLimitLane::Strategy, 40))
        << "consumes weight=40 (30 remaining of 70), raw=1, orders=1 (0 remaining -- exhausted)";

    // weight.try_consume(30) succeeds (40+30=70, exactly fills the lane) and
    // raw.try_consume(1) succeeds, but orders.try_consume(1) fails (already exhausted
    // by the first call) -- forcing the cascade to roll back BOTH raw and weight.
    ASSERT_FALSE(tr.try_reserve_order(RateLimitLane::Strategy, 30));

    EndpointWeightTable table;  // GetAccount = weight 20
    // If weight/raw were correctly rolled back, weight sits at 40/70 used (30
    // remaining): 40+20=60 <= 70 -- succeeds. If the failed attempt's weight
    // consumption LEAKED (left at 70/70 used, 0 remaining): 70+20=90 > 70 -- would
    // fail. orders is never touched by try_reserve_weight_only, so its permanent
    // exhaustion cannot mask this result either way.
    EXPECT_TRUE(tr.try_reserve_weight_only(RateLimitLane::Strategy, PrivateRestEndpoint::GetAccount,
                                            table))
        << "weight/raw must have been rolled back to their post-first-call state, "
           "not left at the failed second attempt's partial consumption";
}

TEST(SpotRateLimitTracker, TryReserveWeightOnlyNeverTouchesOrdersLane) {
    SpotRateLimitTracker tr;
    // orders: Strategy lane = 0 (would fail try_reserve_order's 3rd stage), but
    // try_reserve_weight_only must not even look at it.
    ASSERT_TRUE(tr.configure(/*weight*/ 100000, 0, /*raw*/ 100000, 0, /*orders*/ 1, 0));
    EndpointWeightTable table;
    EXPECT_TRUE(tr.try_reserve_weight_only(RateLimitLane::Strategy,
                                            PrivateRestEndpoint::GetAccount, table));
}

TEST(SpotRateLimitTracker, TryReserveWeightOnlyRejectsUnknownEndpoint) {
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(100000, 0, 100000, 0, 100000, 0));
    EndpointWeightTable table;
    auto bogus = static_cast<PrivateRestEndpoint>(200);
    EXPECT_FALSE(tr.try_reserve_weight_only(RateLimitLane::Strategy, bogus, table));
}
