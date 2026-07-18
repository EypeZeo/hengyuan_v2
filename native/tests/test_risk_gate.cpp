// P2-EXEC-SIM-01: RiskGate pre-trade checks.
#include <gtest/gtest.h>
#include <hengyuan/risk_gate.hpp>

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
