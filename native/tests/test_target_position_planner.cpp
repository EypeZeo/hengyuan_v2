// 批次 6 6b-0c: target_position_planner.hpp unit tests -- pure logic, no network.
//
// The scenarios are the ones 外部复核 P0-02 named as breaking HoldingStateTracker: rejection,
// UNKNOWN, restart with a holding, partial fill -- plus the warm-up hazard found while designing
// the fix (after a restart the evaluator's undefined signal maps to 0.0, which a level-triggered
// planner would misread as "hold nothing").

#include <gtest/gtest.h>
#include <hengyuan/target_position_planner.hpp>

#include <cstdint>
#include <iterator>
#include <limits>

using hy::PlannedSide;
using hy::PlanReason;
using hy::PlannedIntent;
using hy::PlannerConfig;
using hy::PlannerInputs;
using hy::TargetPositionPlanner;

namespace {

// min lot 10, step 10 (so tradable sizes are 10, 20, 30, ...), target position 100.
PlannerConfig base_config() {
    PlannerConfig c;
    c.target_qty_ticks = 100;
    c.min_qty_ticks = 10;
    c.step_size_ticks = 10;
    c.max_qty_ticks = 0;
    c.base_reserve_ticks = 0;
    c.cooldown_bars = 1;
    return c;
}

// Everything trusted; caller sets signal/holdings per test.
PlannerInputs ready(double signal, std::int64_t total, std::int64_t free_qty, std::uint64_t bar = 100) {
    PlannerInputs in;
    in.target_signal = signal;
    in.base_total_ticks = total;
    in.base_free_ticks = free_qty;
    in.bar_index = bar;
    in.feed_valid = true;
    in.warmup_complete = true;
    in.order_in_flight_or_unknown = false;
    in.position_consistent = true;
    return in;
}

}  // namespace

// --- fail-closed defaults --------------------------------------------------------------------

TEST(TargetPositionPlanner, DefaultConstructedInputsNeverProduceAnIntent) {
    TargetPositionPlanner p(base_config());
    const PlannedIntent i = p.plan(PlannerInputs{});
    EXPECT_FALSE(i.has_intent());
    EXPECT_EQ(i.reason, PlanReason::FeedInvalid);
}

TEST(TargetPositionPlanner, EachTrustInputIsIndividuallyRequired) {
    TargetPositionPlanner p(base_config());
    {
        auto in = ready(1.0, 0, 0);
        in.feed_valid = false;
        EXPECT_EQ(p.plan(in).reason, PlanReason::FeedInvalid);
    }
    {
        auto in = ready(1.0, 0, 0);
        in.warmup_complete = false;
        EXPECT_EQ(p.plan(in).reason, PlanReason::WarmupIncomplete);
    }
    {
        auto in = ready(1.0, 0, 0);
        in.position_consistent = false;
        EXPECT_EQ(p.plan(in).reason, PlanReason::PositionDiverged);
    }
    {
        auto in = ready(1.0, 0, 0);
        in.order_in_flight_or_unknown = true;
        EXPECT_EQ(p.plan(in).reason, PlanReason::OrderInFlight);
    }
}

// --- the P0-02 scenarios ---------------------------------------------------------------------

TEST(TargetPositionPlanner, FlatAndSignalOnBuysTheTargetQuantity) {
    TargetPositionPlanner p(base_config());
    const auto i = p.plan(ready(1.0, 0, 0));
    EXPECT_EQ(i.side, PlannedSide::Buy);
    EXPECT_EQ(i.qty_ticks, 100);
    EXPECT_EQ(i.reason, PlanReason::Intent);
}

// The tracker's restart failure: a holding exists but the tracker starts Flat and would BUY again.
TEST(TargetPositionPlanner, RestartWithTheTargetAlreadyHeldDoesNotBuyAgain) {
    TargetPositionPlanner p(base_config());
    const auto i = p.plan(ready(1.0, 100, 100));
    EXPECT_FALSE(i.has_intent());
    EXPECT_EQ(i.reason, PlanReason::AtTarget);
}

// The tracker's silent-after-failure bug: an Open was emitted, the order failed, the signal stays
// on. A level-triggered planner keeps proposing (after its cooldown) until the position is right.
TEST(TargetPositionPlanner, AFailedOpenIsProposedAgainOnALaterBar) {
    TargetPositionPlanner p(base_config());
    ASSERT_EQ(p.plan(ready(1.0, 0, 0, 10)).side, PlannedSide::Buy);
    p.note_not_executed(10);                                             // operator rejected / POST failed
    EXPECT_EQ(p.plan(ready(1.0, 0, 0, 10)).reason, PlanReason::CooldownActive);
    const auto later = p.plan(ready(1.0, 0, 0, 11));                     // cooldown_bars == 1
    EXPECT_EQ(later.side, PlannedSide::Buy);
    EXPECT_EQ(later.qty_ticks, 100);
}

// The tracker's never-closes bug: a Close was emitted but the SELL never filled.
TEST(TargetPositionPlanner, AnUnfilledCloseIsProposedAgain) {
    TargetPositionPlanner p(base_config());
    const auto first = p.plan(ready(0.0, 100, 100, 20));
    ASSERT_EQ(first.side, PlannedSide::Sell);
    EXPECT_EQ(first.qty_ticks, 100);
    p.note_not_executed(20);
    const auto later = p.plan(ready(0.0, 100, 100, 21));  // still holding, signal still off
    EXPECT_EQ(later.side, PlannedSide::Sell);
    EXPECT_EQ(later.qty_ticks, 100);
}

TEST(TargetPositionPlanner, UnknownOrInFlightOrderSuppressesEverything) {
    TargetPositionPlanner p(base_config());
    auto in = ready(1.0, 0, 0);
    in.order_in_flight_or_unknown = true;
    EXPECT_FALSE(p.plan(in).has_intent());
    in.target_signal = 0.0;
    in.base_total_ticks = 100;
    in.base_free_ticks = 100;
    EXPECT_FALSE(p.plan(in).has_intent());  // no closing SELL on top of an unresolved order either
}

TEST(TargetPositionPlanner, PartialFillProposesOnlyTheRemainder) {
    TargetPositionPlanner p(base_config());
    const auto i = p.plan(ready(1.0, 40, 40));  // 40 of 100 filled so far
    EXPECT_EQ(i.side, PlannedSide::Buy);
    EXPECT_EQ(i.qty_ticks, 60);
}

// --- the warm-up hazard ----------------------------------------------------------------------

// After a restart the evaluator has seen no bars, its undefined signal is mapped to 0.0, and the
// account still holds the position. Without the warm-up gate this reads "target zero" -> SELL.
TEST(TargetPositionPlanner, WarmupIncompleteWithAHoldingAndSignalZeroDoesNotSell) {
    TargetPositionPlanner p(base_config());
    auto in = ready(0.0, 100, 100);
    in.warmup_complete = false;
    const auto i = p.plan(in);
    EXPECT_FALSE(i.has_intent());
    EXPECT_EQ(i.reason, PlanReason::WarmupIncomplete);
    in.warmup_complete = true;  // and once genuinely warmed up, the same input DOES sell
    EXPECT_EQ(p.plan(in).side, PlannedSide::Sell);
}

// --- lot lattice / caps / reserve ------------------------------------------------------------

TEST(TargetPositionPlanner, QuantityIsRoundedDownOntoTheMinPlusStepLattice) {
    TargetPositionPlanner p(base_config());
    const auto i = p.plan(ready(1.0, 45, 45));  // needs 55 -> lattice {10,20,...} -> 50
    EXPECT_EQ(i.side, PlannedSide::Buy);
    EXPECT_EQ(i.qty_ticks, 50);
    EXPECT_EQ((i.qty_ticks - 10) % 10, 0);  // exactly what validate_pre_trade() demands
}

TEST(TargetPositionPlanner, LatticeUsesMinQtyAsTheOriginNotZero) {
    auto c = base_config();
    c.min_qty_ticks = 15;  // lattice {15, 25, 35, ...}
    c.step_size_ticks = 10;
    TargetPositionPlanner p(c);
    const auto i = p.plan(ready(1.0, 0, 0));  // needs 100 -> 15 + 8*10 = 95
    EXPECT_EQ(i.qty_ticks, 95);
    EXPECT_EQ((i.qty_ticks - 15) % 10, 0);
}

TEST(TargetPositionPlanner, DifferenceSmallerThanOneLotIsDustNotAnOrder) {
    TargetPositionPlanner p(base_config());
    const auto i = p.plan(ready(1.0, 95, 95));  // 5 short of target, min lot is 10
    EXPECT_FALSE(i.has_intent());
    EXPECT_EQ(i.reason, PlanReason::Dust);
}

TEST(TargetPositionPlanner, SellIsCappedByFreeBalance) {
    TargetPositionPlanner p(base_config());
    const auto i = p.plan(ready(0.0, 100, 30));  // 70 locked in open orders
    EXPECT_EQ(i.side, PlannedSide::Sell);
    EXPECT_EQ(i.qty_ticks, 30);
}

TEST(TargetPositionPlanner, NothingFreeToSellIsDust) {
    TargetPositionPlanner p(base_config());
    const auto i = p.plan(ready(0.0, 100, 0));
    EXPECT_FALSE(i.has_intent());
}

TEST(TargetPositionPlanner, ReserveIsNeverSoldAndIsNotCountedAsTheStrategysPosition) {
    auto c = base_config();
    c.base_reserve_ticks = 50;
    TargetPositionPlanner p(c);
    // Account holds 150: 50 is the operator's reserve, 100 is the strategy's -> close exactly 100.
    const auto sell = p.plan(ready(0.0, 150, 150));
    EXPECT_EQ(sell.side, PlannedSide::Sell);
    EXPECT_EQ(sell.qty_ticks, 100);
    // Account holds only the reserve: the strategy is flat, so signal-on buys the full target.
    const auto buy = p.plan(ready(1.0, 50, 50));
    EXPECT_EQ(buy.side, PlannedSide::Buy);
    EXPECT_EQ(buy.qty_ticks, 100);
}

TEST(TargetPositionPlanner, HoldingsBelowTheDeclaredReserveCountAsFlatNotNegative) {
    auto c = base_config();
    c.base_reserve_ticks = 50;
    TargetPositionPlanner p(c);
    const auto i = p.plan(ready(0.0, 20, 20));  // less than the reserve
    EXPECT_FALSE(i.has_intent());
    EXPECT_EQ(i.reason, PlanReason::AtTarget);
}

TEST(TargetPositionPlanner, PerOrderCapLimitsTheProposal) {
    auto c = base_config();
    c.max_qty_ticks = 50;
    TargetPositionPlanner p(c);
    const auto i = p.plan(ready(1.0, 0, 0));
    EXPECT_EQ(i.side, PlannedSide::Buy);
    EXPECT_EQ(i.qty_ticks, 50);  // the rest is proposed on later bars
}

// --- signal handling -------------------------------------------------------------------------

TEST(TargetPositionPlanner, ScaledSignalScalesTheDesiredPosition) {
    TargetPositionPlanner p(base_config());
    const auto i = p.plan(ready(0.5, 0, 0));
    EXPECT_EQ(i.side, PlannedSide::Buy);
    EXPECT_EQ(i.qty_ticks, 50);
}

TEST(TargetPositionPlanner, NegativeAndUndefinedSignalsMeanHoldNothing) {
    TargetPositionPlanner p(base_config());
    EXPECT_EQ(p.plan(ready(-0.7, 100, 100)).side, PlannedSide::Sell);  // long-only: not a short
    EXPECT_EQ(p.plan(ready(std::numeric_limits<double>::quiet_NaN(), 100, 100)).side, PlannedSide::Sell);
    EXPECT_EQ(p.plan(ready(-0.7, 0, 0)).reason, PlanReason::AtTarget);
}

TEST(TargetPositionPlanner, SignalAboveOneIsClampedToTheTarget) {
    TargetPositionPlanner p(base_config());
    EXPECT_EQ(p.plan(ready(3.0, 0, 0)).qty_ticks, 100);
}

// --- cooldown --------------------------------------------------------------------------------

TEST(TargetPositionPlanner, CooldownSuppressesForExactlyTheConfiguredNumberOfBars) {
    auto c = base_config();
    c.cooldown_bars = 3;
    TargetPositionPlanner p(c);
    p.note_not_executed(10);
    EXPECT_EQ(p.plan(ready(1.0, 0, 0, 10)).reason, PlanReason::CooldownActive);
    EXPECT_EQ(p.plan(ready(1.0, 0, 0, 11)).reason, PlanReason::CooldownActive);
    EXPECT_EQ(p.plan(ready(1.0, 0, 0, 12)).reason, PlanReason::CooldownActive);
    EXPECT_EQ(p.plan(ready(1.0, 0, 0, 13)).side, PlannedSide::Buy);
}

// --- config validation -----------------------------------------------------------------------

TEST(TargetPositionPlanner, InvalidConfigFailsClosed) {
    {
        auto c = base_config();
        c.step_size_ticks = 0;
        EXPECT_EQ(TargetPositionPlanner(c).plan(ready(1.0, 0, 0)).reason, PlanReason::InvalidConfig);
    }
    {
        auto c = base_config();
        c.min_qty_ticks = 0;
        EXPECT_EQ(TargetPositionPlanner(c).plan(ready(1.0, 0, 0)).reason, PlanReason::InvalidConfig);
    }
    {
        auto c = base_config();
        c.target_qty_ticks = -1;
        EXPECT_EQ(TargetPositionPlanner(c).plan(ready(1.0, 0, 0)).reason, PlanReason::InvalidConfig);
    }
    {
        auto c = base_config();
        c.max_qty_ticks = 5;  // below min lot: no order could ever satisfy it
        EXPECT_EQ(TargetPositionPlanner(c).plan(ready(1.0, 0, 0)).reason, PlanReason::InvalidConfig);
    }
    {
        auto c = base_config();
        c.base_reserve_ticks = -1;
        EXPECT_EQ(TargetPositionPlanner(c).plan(ready(1.0, 0, 0)).reason, PlanReason::InvalidConfig);
    }
}

TEST(TargetPositionPlanner, NegativeBalancesFailClosed) {
    TargetPositionPlanner p(base_config());
    EXPECT_EQ(p.plan(ready(1.0, -1, 0)).reason, PlanReason::InvalidConfig);
    EXPECT_EQ(p.plan(ready(1.0, 0, -1)).reason, PlanReason::InvalidConfig);
}

// --- helpers ---------------------------------------------------------------------------------

TEST(BalanceToQtyTicks, FloorsFromBalanceScaleToTheSymbolQtyScale) {
    std::int64_t out = -1;
    ASSERT_TRUE(hy::balance_to_qty_ticks(123'456'789, 5, out));  // 1e8 -> 1e5 scale: divide by 1e3
    EXPECT_EQ(out, 123'456);
    ASSERT_TRUE(hy::balance_to_qty_ticks(999, 5, out));
    EXPECT_EQ(out, 0);  // floor, never round up
    ASSERT_TRUE(hy::balance_to_qty_ticks(123'456'789, 8, out));
    EXPECT_EQ(out, 123'456'789);  // same scale: identity
}

TEST(BalanceToQtyTicks, RejectsNegativeBalanceAndOutOfDomainScale) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::balance_to_qty_ticks(-1, 5, out));
    EXPECT_FALSE(hy::balance_to_qty_ticks(100, 9, out));  // finer than kBalanceScale
}

TEST(PositionConsistent, WithinToleranceAgrees) {
    EXPECT_TRUE(hy::position_consistent(100, 100, 0));
    EXPECT_TRUE(hy::position_consistent(99, 100, 1));    // e.g. a fee taken in the base asset
    EXPECT_TRUE(hy::position_consistent(-40, -40, 0));
}

TEST(PositionConsistent, BeyondToleranceDiverges) {
    EXPECT_FALSE(hy::position_consistent(98, 100, 1));
    EXPECT_FALSE(hy::position_consistent(100, 0, 5));    // the account moved, no fill explains it
    EXPECT_FALSE(hy::position_consistent(0, 100, 5));    // a fill the account never reflected
}

TEST(PositionConsistent, NegativeToleranceAndOverflowFailClosed) {
    EXPECT_FALSE(hy::position_consistent(0, 0, -1));
    EXPECT_FALSE(hy::position_consistent(std::numeric_limits<std::int64_t>::max(), -1, 1'000));
    EXPECT_FALSE(hy::position_consistent(std::numeric_limits<std::int64_t>::min(), 1, 1'000));
}

TEST(PlanReasonName, EveryReasonHasADistinctName) {
    const PlanReason all[] = {PlanReason::Intent,           PlanReason::AtTarget,
                              PlanReason::Dust,             PlanReason::FeedInvalid,
                              PlanReason::WarmupIncomplete, PlanReason::OrderInFlight,
                              PlanReason::CooldownActive,   PlanReason::InvalidConfig,
                              PlanReason::PositionDiverged};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        EXPECT_STRNE(hy::plan_reason_name(all[i]), "?");
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            EXPECT_STRNE(hy::plan_reason_name(all[i]), hy::plan_reason_name(all[j]));
        }
    }
}
