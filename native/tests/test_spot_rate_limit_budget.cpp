// TODOLIST Stage 1A · TODO 1A.2: spot_rate_limit_budget.hpp unit tests.
// All local (no network) -- pure tracker/budget-split logic.
#include <gtest/gtest.h>
#include <hengyuan/spot_rate_limit_budget.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>

using hy::EndpointWeightTable;
using hy::PartitionedRateBudget;
using hy::PrivateRestEndpoint;
using hy::RateLimitBudgetSplit;
using hy::RateLimitLane;
using hy::RateLimitPermit;
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
    EXPECT_EQ(endpoint_weight(t, PrivateRestEndpoint::GetRateLimitOrder), 40u);
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

// TODO 1A.4: try_reserve_weight_only_explicit() -- listenKey calls' rate-limit path, deliberately
// NOT routed through PrivateRestEndpoint (see the function's own header comment: adding a
// PrivateRestEndpoint enumerator with no spec basis is a build-failing NAME_CONFLICT per
// tools/spec_enum_diff.py).

TEST(SpotRateLimitTracker, TryReserveWeightOnlyExplicitConsumesTheGivenWeight) {
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(/*weight*/ 100, 0, /*raw*/ 100, 0, /*orders*/ 100, 0));
    // Strategy lane = floor(100*80/100) = 80.
    EXPECT_TRUE(tr.try_reserve_weight_only_explicit(RateLimitLane::Strategy, /*weight=*/2));
}

TEST(SpotRateLimitTracker, TryReserveWeightOnlyExplicitNeverTouchesOrdersLane) {
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(/*weight*/ 100000, 0, /*raw*/ 100000, 0, /*orders*/ 1, 0));
    EXPECT_TRUE(tr.try_reserve_weight_only_explicit(RateLimitLane::Strategy, /*weight=*/2));
}

TEST(SpotRateLimitTracker, TryReserveWeightOnlyExplicitRefusesWhenWeightExceedsBudget) {
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(/*weight*/ 1, 0, /*raw*/ 100000, 0, /*orders*/ 100000, 0));
    // Strategy lane = floor(1*80/100) = 0 -- any positive weight must be refused.
    EXPECT_FALSE(tr.try_reserve_weight_only_explicit(RateLimitLane::Strategy, /*weight=*/2));
}

TEST(SpotRateLimitTracker, TryReserveWeightOnlyExplicitFailsClosedWhenRawExhausted) {
    SpotRateLimitTracker tr;
    // raw: Strategy lane = floor(1*80/100) = 0 -- the raw-consume stage must fail even though
    // the weight budget alone would allow this call.
    ASSERT_TRUE(tr.configure(/*weight*/ 100000, 0, /*raw*/ 1, 0, /*orders*/ 100000, 0));
    EXPECT_FALSE(tr.try_reserve_weight_only_explicit(RateLimitLane::Strategy, /*weight=*/2));
}

// --- RateLimitPermit and the typed reservations (SAFE-01 slice 4) ---
//
// reserve_weight_only()/reserve_weight_only_explicit() are try_reserve_weight_only*() that hand back the proof of the
// reservation: the permit the private REST client's network coroutines require. Same budget arithmetic, same
// "a refusal consumes nothing", plus the lane and weight the permit stands for.

TEST(RateLimitPermit, AReservationYieldsAReservedPermitNamingTheLaneAndTheWeight) {
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(/*weight*/ 1000, 0, /*raw*/ 1000, 0, /*orders*/ 1000, 0));
    EndpointWeightTable table;
    const auto permit = tr.reserve_weight_only(RateLimitLane::Reconciliation, PrivateRestEndpoint::GetAccount, table);
    ASSERT_TRUE(permit.has_value());
    EXPECT_EQ(permit->basis(), RateLimitPermit::Basis::Reserved);
    EXPECT_EQ(permit->lane(), RateLimitLane::Reconciliation);
    EXPECT_EQ(permit->weight(), 20u) << "the permit carries the table's weight for the endpoint, not a guess";

    const auto explicit_permit = tr.reserve_weight_only_explicit(RateLimitLane::Strategy, /*weight=*/2);
    ASSERT_TRUE(explicit_permit.has_value());
    EXPECT_EQ(explicit_permit->basis(), RateLimitPermit::Basis::Reserved);
    EXPECT_EQ(explicit_permit->lane(), RateLimitLane::Strategy);
    EXPECT_EQ(explicit_permit->weight(), 2u);
}

TEST(RateLimitPermit, AReservationConsumesExactlyWhatTheBooleanVariantDoes) {
    // weight 250 -> Reconciliation lane = floor(250*10/100) = 25. A permit for GetAccount (20) leaves 5.
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(/*weight*/ 250, 0, /*raw*/ 1000, 0, /*orders*/ 1000, 0));
    EndpointWeightTable table;
    ASSERT_TRUE(tr.reserve_weight_only(RateLimitLane::Reconciliation, PrivateRestEndpoint::GetAccount, table));
    EXPECT_FALSE(tr.try_reserve_weight_only(RateLimitLane::Reconciliation, PrivateRestEndpoint::GetAccount, table))
        << "the first permit consumed 20 of 25: a second 20 does not fit, on either API";
    EXPECT_TRUE(tr.try_reserve_weight_only_explicit(RateLimitLane::Reconciliation, /*weight=*/5))
        << "...and exactly 5 is still there";
}

TEST(RateLimitPermit, ARefusedReservationYieldsNoPermitAndConsumesNothing) {
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(/*weight*/ 250, 0, /*raw*/ 1000, 0, /*orders*/ 1000, 0));
    EndpointWeightTable table;
    ASSERT_TRUE(tr.reserve_weight_only(RateLimitLane::Reconciliation, PrivateRestEndpoint::GetAccount, table));

    EXPECT_FALSE(tr.reserve_weight_only(RateLimitLane::Reconciliation, PrivateRestEndpoint::GetAccount, table))
        << "20 more does not fit into the 5 left";
    EXPECT_FALSE(tr.reserve_weight_only_explicit(RateLimitLane::Reconciliation, /*weight=*/6));
    // The two refusals above must not have taken the 5 that are left: exactly 5 still goes through, then nothing.
    EXPECT_TRUE(tr.reserve_weight_only_explicit(RateLimitLane::Reconciliation, /*weight=*/5).has_value());
    EXPECT_FALSE(tr.reserve_weight_only_explicit(RateLimitLane::Reconciliation, /*weight=*/1).has_value());
}

TEST(RateLimitPermit, AReservationChargesTheLaneItNamesAndNoOther) {
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(/*weight*/ 1000, 0, /*raw*/ 1000, 0, /*orders*/ 1000, 0));
    // Reconciliation lane = 100: drain it, then it refuses -- while the other two lanes are untouched.
    ASSERT_TRUE(tr.reserve_weight_only_explicit(RateLimitLane::Reconciliation, /*weight=*/100).has_value());
    EXPECT_FALSE(tr.reserve_weight_only_explicit(RateLimitLane::Reconciliation, /*weight=*/1).has_value());
    const auto strategy = tr.reserve_weight_only_explicit(RateLimitLane::Strategy, /*weight=*/1);
    ASSERT_TRUE(strategy.has_value());
    EXPECT_EQ(strategy->lane(), RateLimitLane::Strategy);
    const auto emergency = tr.reserve_weight_only_explicit(RateLimitLane::Emergency, /*weight=*/1);
    ASSERT_TRUE(emergency.has_value());
    EXPECT_EQ(emergency->lane(), RateLimitLane::Emergency);
}

TEST(RateLimitPermit, AnUnknownEndpointYieldsNoPermit) {
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(100000, 0, 100000, 0, 100000, 0));
    EndpointWeightTable table;
    const auto bogus = static_cast<PrivateRestEndpoint>(200);
    EXPECT_FALSE(tr.reserve_weight_only(RateLimitLane::Strategy, bogus, table).has_value());
}

TEST(RateLimitPermit, AnExhaustedRawRequestBudgetRefusesEvenWhenTheWeightFits) {
    // raw: Strategy lane = floor(1*80/100) = 0 -- the raw-request stage fails closed, and the typed variants
    // report it as "no permit", exactly like the boolean ones.
    SpotRateLimitTracker tr;
    ASSERT_TRUE(tr.configure(/*weight*/ 100000, 0, /*raw*/ 1, 0, /*orders*/ 100000, 0));
    EndpointWeightTable table;
    EXPECT_FALSE(tr.reserve_weight_only(RateLimitLane::Strategy, PrivateRestEndpoint::GetAccount, table).has_value());
    EXPECT_FALSE(tr.reserve_weight_only_explicit(RateLimitLane::Strategy, /*weight=*/2).has_value());
}

TEST(RateLimitPermit, TheTwoNamedPermitsSayWhoCountsTheRequest) {
    const RateLimitPermit by_caller = RateLimitPermit::reserved_by_caller();
    EXPECT_EQ(by_caller.basis(), RateLimitPermit::Basis::ReservedByCaller);
    EXPECT_EQ(by_caller.lane(), RateLimitLane::Strategy) << "POST /order is reserved by the orchestrator under Strategy";
    EXPECT_EQ(by_caller.weight(), 0u) << "nothing is charged by the client for it";

    const RateLimitPermit none = RateLimitPermit::unaccounted();
    EXPECT_EQ(none.basis(), RateLimitPermit::Basis::Unaccounted);
    EXPECT_EQ(none.lane(), RateLimitLane::Reconciliation);
    EXPECT_EQ(none.weight(), 0u);
}

TEST(RateLimitPermit, ThePermitIsAPlainValue) {
    static_assert(std::is_trivially_copyable_v<RateLimitPermit>);
    static_assert(!std::is_default_constructible_v<RateLimitPermit>);
    static_assert(sizeof(RateLimitPermit) <= 8, "a permit travels by value into a coroutine frame");
    const RateLimitPermit a = RateLimitPermit::unaccounted();
    const RateLimitPermit b = a;
    EXPECT_EQ(b.basis(), a.basis());
}

// Not part of the normal run (--gtest_also_run_disabled_tests): what one reservation costs, with and without the
// typed permit around it. The private REST client takes one per call, in front of a TLS round trip of tens of
// milliseconds, so this only has to show that the permit adds nothing worth looking at.
TEST(RateLimitPermit, DISABLED_AReservationCostsNanoseconds) {
    SpotRateLimitTracker tr;
    // Reconciliation lane = 10% of a window large enough for every call below.
    ASSERT_TRUE(tr.configure(2'000'000'000u, 0, 2'000'000'000u, 0, 2'000'000'000u, 0));
    EndpointWeightTable table;
    constexpr int kRounds = 2'000'000;
    std::uint64_t sink = 0;

    auto time_it = [&](const char* name, auto&& one_call) {
        double best = 1e18;
        for (int rep = 0; rep < 5; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kRounds; ++i) sink += one_call();
            const auto t1 = std::chrono::steady_clock::now();
            const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kRounds;
            if (ns < best) best = ns;
        }
        std::printf("%-44s %7.1f ns/call (best of 5 x %d)\n", name, best, kRounds);
    };

    time_it("try_reserve_weight_only (bool)", [&] {
        return tr.try_reserve_weight_only(RateLimitLane::Reconciliation, PrivateRestEndpoint::GetServerTime, table) ? 1u : 0u;
    });
    time_it("reserve_weight_only (optional<permit>)", [&] {
        const auto p = tr.reserve_weight_only(RateLimitLane::Reconciliation, PrivateRestEndpoint::GetServerTime, table);
        return p ? p->weight() : 0u;
    });
    time_it("reserve_weight_only_explicit", [&] {
        const auto p = tr.reserve_weight_only_explicit(RateLimitLane::Reconciliation, 1);
        return p ? p->weight() : 0u;
    });
    EXPECT_NE(sink, 0u);
}
