// P2-EXEC-SIM-01: RiskGate pre-trade checks.
#include <gtest/gtest.h>
#include <hengyuan/risk_gate.hpp>

#include <cstdint>
#include <limits>

using hy::ExecutionIntent;
using hy::KillState;
using hy::KillSwitch;
using hy::OrderSide;
using hy::RiskDecision;
using hy::RiskGate;
using hy::RiskLimits;

static ExecutionIntent buy(std::int64_t qty) {
    ExecutionIntent in{};
    in.symbol_id = 0;
    in.side = OrderSide::Buy;
    in.qty_lots = qty;
    return in;
}

static ExecutionIntent sell(std::int64_t qty) {
    ExecutionIntent in{};
    in.symbol_id = 0;
    in.side = OrderSide::Sell;
    in.qty_lots = qty;
    return in;
}

TEST(RiskGate, AllowsValidIntent) {
    RiskGate g(RiskLimits{.max_position_lots = 100, .max_notional_ticks = 0});
    KillSwitch ks;
    EXPECT_EQ(g.check(buy(10), 0, 50000, ks), RiskDecision::Allow);
}

TEST(RiskGate, BlocksInvalidQty) {
    RiskGate g(RiskLimits{});
    KillSwitch ks;
    EXPECT_EQ(g.check(buy(0), 0, 50000, ks), RiskDecision::BlockInvalidQty);
    EXPECT_EQ(g.check(buy(-5), 0, 50000, ks), RiskDecision::BlockInvalidQty);
}

TEST(RiskGate, BlocksOnArmedWhenOpening) {
    RiskGate g(RiskLimits{.max_position_lots = 100, .max_notional_ticks = 0});
    KillSwitch ks;
    ks.arm();
    EXPECT_EQ(g.check(buy(10), 0, 50000, ks), RiskDecision::BlockKillSwitch);
}

TEST(RiskGate, AllowsReducingWhenArmed) {
    RiskGate g(RiskLimits{.max_position_lots = 100, .max_notional_ticks = 0});
    KillSwitch ks;
    ks.arm();
    // long 20, selling reduces -> allowed even while armed
    EXPECT_EQ(g.check(sell(10), 20, 50000, ks), RiskDecision::Allow);
}

TEST(RiskGate, BlocksAllWhenTriggered) {
    RiskGate g(RiskLimits{.max_position_lots = 100, .max_notional_ticks = 0});
    KillSwitch ks;
    ks.trigger();
    EXPECT_EQ(g.check(sell(10), 20, 50000, ks), RiskDecision::BlockKillSwitch);
}

TEST(RiskGate, BlocksMaxPosition) {
    RiskGate g(RiskLimits{.max_position_lots = 100, .max_notional_ticks = 0});
    KillSwitch ks;
    // current 95 long, buying 10 -> 105 > 100
    EXPECT_EQ(g.check(buy(10), 95, 50000, ks), RiskDecision::BlockMaxPosition);
    // buying 5 -> 100, OK
    EXPECT_EQ(g.check(buy(5), 95, 50000, ks), RiskDecision::Allow);
}

TEST(RiskGate, MaxPositionAppliesToShortSide) {
    RiskGate g(RiskLimits{.max_position_lots = 100, .max_notional_ticks = 0});
    KillSwitch ks;
    // current -95 (short), selling 10 -> -105, abs 105 > 100
    EXPECT_EQ(g.check(sell(10), -95, 50000, ks), RiskDecision::BlockMaxPosition);
}

TEST(RiskGate, BlocksMaxNotional) {
    RiskGate g(RiskLimits{.max_position_lots = 0, .max_notional_ticks = 1'000'000});
    KillSwitch ks;
    // 50000 * 30 = 1,500,000 > 1,000,000
    EXPECT_EQ(g.check(buy(30), 0, 50000, ks), RiskDecision::BlockMaxNotional);
    // 50000 * 20 = 1,000,000 not > 1,000,000 -> allow
    EXPECT_EQ(g.check(buy(20), 0, 50000, ks), RiskDecision::Allow);
}

// --- AUDIT RISK-INTOVF-032 / RISK-REFPRICE-033 regressions ---
//
// Both defects made the gate answer Allow when it should have blocked, which
// is the worst direction for a risk gate to fail in. These tests pin the
// direction, not just the enum value.

TEST(RiskGate, NonPositiveReferencePriceIsRejectedNotIgnored) {
    // Before: product_exceeds() answers "does not exceed" for a non-positive
    // operand, so a 1-tick notional cap let an INT64_MAX quantity through.
    RiskGate gate(RiskLimits{.max_position_lots = 0, .max_notional_ticks = 1});
    KillSwitch ks;
    auto intent = buy(std::numeric_limits<std::int64_t>::max());

    EXPECT_EQ(gate.check(intent, 0, /*ref_price_ticks=*/0, ks), RiskDecision::BlockInvalidPrice);
    EXPECT_EQ(gate.check(intent, 0, /*ref_price_ticks=*/-1, ks), RiskDecision::BlockInvalidPrice);
}

TEST(RiskGate, ProjectedPositionOverflowFailsClosed) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    RiskGate gate(RiskLimits{.max_position_lots = 1000, .max_notional_ticks = 0});
    KillSwitch ks;

    // Positive-side overflow: kMax + kMax used to wrap to -2, whose magnitude
    // (2) compared as WELL UNDER the 1000-lot cap -> Allow.
    EXPECT_EQ(gate.check(buy(kMax), kMax, 100, ks), RiskDecision::BlockPositionOverflow);

    // Negative-side overflow: kMin + (-1).
    EXPECT_EQ(gate.check(sell(1), kMin, 100, ks), RiskDecision::BlockPositionOverflow);
}

TEST(RiskGate, ProjectedPositionAtInt64MinIsBlockedNotAllowed) {
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    RiskGate gate(RiskLimits{.max_position_lots = 1000, .max_notional_ticks = 0});
    KillSwitch ks;

    // projected == INT64_MIN exactly. The old `-projected` was UB and wrapped
    // back to INT64_MIN, i.e. a NEGATIVE "magnitude" that passed `> 1000`.
    // The magnitude is now taken in the unsigned domain, where it is 2^63 and
    // correctly exceeds any int64 cap.
    const auto d = gate.check(sell(1), kMin + 1, 100, ks);
    EXPECT_NE(d, RiskDecision::Allow);
    EXPECT_EQ(d, RiskDecision::BlockMaxPosition);
}

TEST(RiskGate, LargeButRepresentablePositionStillBlocksNormally) {
    // Guards against "fixed the overflow by blocking everything": a position
    // that is merely over the cap, with no overflow anywhere, must still come
    // back as BlockMaxPosition rather than BlockPositionOverflow.
    RiskGate gate(RiskLimits{.max_position_lots = 1000, .max_notional_ticks = 0});
    KillSwitch ks;
    EXPECT_EQ(gate.check(buy(500), 900, 100, ks), RiskDecision::BlockMaxPosition);
    EXPECT_EQ(gate.check(buy(50), 100, 100, ks), RiskDecision::Allow);
}
