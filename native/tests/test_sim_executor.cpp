// P2-EXEC-SIM-01: SimExecutor dry-run simulation.
#include <gtest/gtest.h>
#include <hengyuan/sim_executor.hpp>

#include <cstdint>
#include <limits>

using hy::ExecutionIntent;
using hy::FillStatus;
using hy::KillState;
using hy::OrderSide;
using hy::OrderType;
using hy::RiskLimits;
using hy::SimConfig;
using hy::SimExecutor;

static ExecutionIntent mkt(OrderSide side, std::int64_t qty, std::uint32_t sym = 0) {
    ExecutionIntent in{};
    in.symbol_id = sym;
    in.side = side;
    in.type = OrderType::Market;
    in.qty_lots = qty;
    return in;
}

static SimConfig cfg_no_fee(std::int64_t max_pos = 0) {
    return SimConfig{.limits = RiskLimits{.max_position_lots = max_pos, .max_notional_ticks = 0},
                     .fee_ppm = 0,
                     .max_drawdown = 0};
}

TEST(SimExecutor, MarketBuyFillsAtAsk) {
    SimExecutor<64> ex(cfg_no_fee());
    auto f = ex.execute(mkt(OrderSide::Buy, 10), 99, 100);
    EXPECT_EQ(f.status, FillStatus::Filled);
    EXPECT_EQ(f.fill_price_ticks, 100);
    EXPECT_EQ(f.filled_qty_lots, 10);
    EXPECT_EQ(ex.position(0).net_qty_lots, 10);
    EXPECT_EQ(ex.position(0).avg_entry_price_ticks, 100);
}

TEST(SimExecutor, MarketSellFillsAtBid) {
    SimExecutor<64> ex(cfg_no_fee());
    auto f = ex.execute(mkt(OrderSide::Sell, 10), 99, 100);
    EXPECT_EQ(f.status, FillStatus::Filled);
    EXPECT_EQ(f.fill_price_ticks, 99);
    EXPECT_EQ(ex.position(0).net_qty_lots, -10);
}

TEST(SimExecutor, EmptyBookNoLiquidity) {
    SimExecutor<64> ex(cfg_no_fee());
    auto f = ex.execute(mkt(OrderSide::Buy, 10), 0, 0);
    EXPECT_EQ(f.status, FillStatus::NoLiquidity);
    EXPECT_EQ(ex.stats().no_liquidity, 1u);
}

TEST(SimExecutor, LimitNotMarketable) {
    SimExecutor<64> ex(cfg_no_fee());
    ExecutionIntent in{};
    in.side = OrderSide::Buy;
    in.type = OrderType::Limit;
    in.qty_lots = 10;
    in.limit_price_ticks = 99;  // below ask 100 -> not marketable
    auto f = ex.execute(in, 98, 100);
    EXPECT_EQ(f.status, FillStatus::NoLiquidity);
}

TEST(SimExecutor, LimitMarketableFillsAtBook) {
    SimExecutor<64> ex(cfg_no_fee());
    ExecutionIntent in{};
    in.side = OrderSide::Buy;
    in.type = OrderType::Limit;
    in.qty_lots = 10;
    in.limit_price_ticks = 101;  // >= ask 100 -> marketable, price-improves to 100
    auto f = ex.execute(in, 99, 100);
    EXPECT_EQ(f.status, FillStatus::Filled);
    EXPECT_EQ(f.fill_price_ticks, 100);
}

TEST(SimExecutor, RejectedByMaxPosition) {
    SimExecutor<64> ex(cfg_no_fee(/*max_pos=*/5));
    auto f = ex.execute(mkt(OrderSide::Buy, 10), 99, 100);
    EXPECT_EQ(f.status, FillStatus::Rejected);
    EXPECT_EQ(ex.stats().rejected, 1u);
    EXPECT_EQ(ex.position(0).net_qty_lots, 0);
}

TEST(SimExecutor, RejectedByKillSwitch) {
    SimExecutor<64> ex(cfg_no_fee(100));
    ex.kill_switch().arm();
    auto f = ex.execute(mkt(OrderSide::Buy, 10), 99, 100);
    EXPECT_EQ(f.status, FillStatus::Rejected);
}

TEST(SimExecutor, PositionAccountingLongIncreaseReduce) {
    SimExecutor<64> ex(cfg_no_fee(1000));
    ex.execute(mkt(OrderSide::Buy, 10), 100, 100);   // net 10 @ 100
    ex.execute(mkt(OrderSide::Buy, 10), 110, 110);   // net 20 @ avg 105
    EXPECT_EQ(ex.position(0).net_qty_lots, 20);
    EXPECT_EQ(ex.position(0).avg_entry_price_ticks, 105);

    ex.execute(mkt(OrderSide::Sell, 5), 120, 120);   // reduce 5; realized (120-105)*5=75
    EXPECT_EQ(ex.position(0).net_qty_lots, 15);
    EXPECT_EQ(ex.position(0).realized_pnl, 75);
}

TEST(SimExecutor, PositionFlip) {
    SimExecutor<64> ex(cfg_no_fee(1000));
    ex.execute(mkt(OrderSide::Buy, 10), 100, 100);   // long 10 @ 100
    ex.execute(mkt(OrderSide::Sell, 25), 120, 120);  // close 10 (pnl (120-100)*10=200), flip short 15
    EXPECT_EQ(ex.position(0).net_qty_lots, -15);
    EXPECT_EQ(ex.position(0).avg_entry_price_ticks, 120);
    EXPECT_EQ(ex.position(0).realized_pnl, 200);
}

TEST(SimExecutor, FeeReducesRealized) {
    SimConfig c{.limits = RiskLimits{.max_position_lots = 1000, .max_notional_ticks = 0},
                .fee_ppm = 10000,  // 1% = 10000 ppm
                .max_drawdown = 0};
    SimExecutor<64> ex(c);
    // buy 10 @ 1000: notional 10000, fee = 10000*10000/1000000 = 100
    auto f = ex.execute(mkt(OrderSide::Buy, 10), 1000, 1000);
    EXPECT_EQ(f.status, FillStatus::Filled);
    EXPECT_EQ(f.fee_ticks, 100);
    EXPECT_EQ(ex.position(0).realized_pnl, -100);
}

TEST(SimExecutor, DefaultFee750Ppm) {
    SimConfig c{.limits = RiskLimits{.max_position_lots = 1000, .max_notional_ticks = 0},
                .max_drawdown = 0};
    // Default fee_ppm is 750 (0.075%)
    EXPECT_EQ(c.fee_ppm, 750);
    SimExecutor<64> ex(c);
    // buy 100 @ 10000: notional 1000000, fee = 1000000*750/1000000 = 750
    auto f = ex.execute(mkt(OrderSide::Buy, 100), 10000, 10000);
    EXPECT_EQ(f.status, FillStatus::Filled);
    EXPECT_EQ(f.fee_ticks, 750);
}

TEST(SimExecutor, DrawdownArmsKillSwitch) {
    SimConfig c{.limits = RiskLimits{.max_position_lots = 1000, .max_notional_ticks = 0},
                .fee_ppm = 0,
                .max_drawdown = 100};
    SimExecutor<64> ex(c);
    ex.execute(mkt(OrderSide::Buy, 10), 100, 100);  // long 10 @ 100
    EXPECT_EQ(ex.kill_switch().state(), KillState::Normal);
    // sell 1 at a crashed book -> big unrealized loss on remaining 9
    ex.execute(mkt(OrderSide::Sell, 1), 10, 10);
    // equity = realized(-90) + unrealized((10-100)*9=-810) = -900 < -100 -> armed
    EXPECT_EQ(ex.kill_switch().state(), KillState::Armed);
}

// Audit F-A-006. The checked multiplications of a fill used to be ignored: price 2 x quantity INT64_MAX/2 + 1 overflowed, the fill was still reported as
// Filled, the position took the whole quantity and its average entry price stayed 0. A fill whose arithmetic does not fit int64 is now refused whole.
TEST(SimExecutor, AFillWhoseNotionalOverflowsIsRefusedAndLeavesThePositionAlone) {
    SimExecutor<64> ex(cfg_no_fee());  // no fee, no risk limits: nothing but the arithmetic can refuse it
    constexpr std::int64_t kHuge = std::numeric_limits<std::int64_t>::max() / 2 + 1;
    const auto f = ex.execute(mkt(OrderSide::Buy, kHuge), 1, 2);  // ask 2: 2 * kHuge = INT64_MAX + 1
    EXPECT_EQ(f.status, FillStatus::Rejected);
    EXPECT_EQ(f.filled_qty_lots, 0);
    EXPECT_EQ(f.fill_price_ticks, 0);
    EXPECT_EQ(ex.position(0).net_qty_lots, 0);
    EXPECT_EQ(ex.position(0).avg_entry_price_ticks, 0);
    EXPECT_EQ(ex.position(0).realized_pnl, 0);
    EXPECT_EQ(ex.stats().filled, 0u);
    EXPECT_EQ(ex.stats().rejected, 1u);
    EXPECT_EQ(ex.stats().overflow_guard, 1u);
}

TEST(SimExecutor, AFillThatWouldOverflowTheCostBasisOrTheQuantityLeavesAnEarlierPositionIntact) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    {
        // The cost basis: 1 lot at kMax/2 + 1 (2^62), then 1 more at the same price. Each notional fits, the sum of the two (2^63) does not.
        SimExecutor<64> ex(cfg_no_fee());
        const std::int64_t price = kMax / 2 + 1;
        ASSERT_EQ(ex.execute(mkt(OrderSide::Buy, 1), price - 1, price).status, FillStatus::Filled);
        const auto f = ex.execute(mkt(OrderSide::Buy, 1), price - 1, price);
        EXPECT_EQ(f.status, FillStatus::Rejected);
        EXPECT_EQ(ex.position(0).net_qty_lots, 1) << "the refused fill added nothing";
        EXPECT_EQ(ex.position(0).avg_entry_price_ticks, price) << "and did not touch the cost basis";
        EXPECT_EQ(ex.stats().overflow_guard, 1u);
    }
    {
        // The quantity: net kMax - 1 lots, then 2 more.
        SimExecutor<64> ex(cfg_no_fee());
        ASSERT_EQ(ex.execute(mkt(OrderSide::Buy, kMax - 1), 1, 1).status, FillStatus::Filled);
        const auto f = ex.execute(mkt(OrderSide::Buy, 2), 1, 1);
        EXPECT_EQ(f.status, FillStatus::Rejected);
        EXPECT_EQ(ex.position(0).net_qty_lots, kMax - 1);
        EXPECT_EQ(ex.stats().overflow_guard, 1u);
    }
}

TEST(SimExecutor, AFeeThatCannotBeRepresentedRefusesTheFill) {
    // fee_ppm so large that notional * fee_ppm overflows even after the notional is divided by 1000.
    SimConfig c{.limits = RiskLimits{.max_position_lots = 0, .max_notional_ticks = 0},
                .fee_ppm = std::numeric_limits<std::int64_t>::max(),
                .max_drawdown = 0};
    SimExecutor<64> ex(c);
    const auto f = ex.execute(mkt(OrderSide::Buy, 10'000), 1'000, 1'000);  // notional 1e7 -> 1e4 after the split, times INT64_MAX
    EXPECT_EQ(f.status, FillStatus::Rejected);
    EXPECT_EQ(ex.position(0).net_qty_lots, 0);
    EXPECT_EQ(ex.stats().overflow_guard, 1u);
}

TEST(SimExecutor, ABigFillWhoseFeeNeedsTheCoarseTierStillFills) {
    // notional x fee_ppm overflows (6e17 x 750), but the notional alone fits: the fee comes from the divided-first tier, as before.
    SimExecutor<64> ex(SimConfig{.limits = RiskLimits{.max_position_lots = 0, .max_notional_ticks = 0}, .fee_ppm = 750, .max_drawdown = 0});
    const auto f = ex.execute(mkt(OrderSide::Buy, 100'000), 6'000'000'000'000, 6'000'000'000'000);
    ASSERT_EQ(f.status, FillStatus::Filled);
    EXPECT_EQ(f.fee_ticks, (600'000'000'000'000'000LL / 1000) * 750 / 1000);
    EXPECT_EQ(ex.stats().overflow_guard, 0u);
}

TEST(SimExecutor, AnEquityThatCannotBeComputedArmsTheKillSwitchInsteadOfReadingAsNoLoss) {
    // A drawdown limit is set and the mark (bid + ask) / 2 cannot even be formed: the old code added them unchecked.
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    SimConfig c{.limits = RiskLimits{.max_position_lots = 0, .max_notional_ticks = 0}, .fee_ppm = 0, .max_drawdown = 100};
    SimExecutor<64> ex(c);
    const auto f = ex.execute(mkt(OrderSide::Buy, 1), kMax, kMax);  // the fill itself is representable (1 x kMax)
    EXPECT_EQ(f.status, FillStatus::Filled);
    EXPECT_EQ(ex.kill_switch().state(), KillState::Armed);
    EXPECT_EQ(ex.stats().overflow_guard, 1u);
}

TEST(SimExecutor, MultiSymbolIndependentPositions) {
    SimExecutor<64> ex(cfg_no_fee(1000));
    ex.execute(mkt(OrderSide::Buy, 10, 0), 100, 100);
    ex.execute(mkt(OrderSide::Buy, 5, 1), 200, 200);
    EXPECT_EQ(ex.position(0).net_qty_lots, 10);
    EXPECT_EQ(ex.position(1).net_qty_lots, 5);
}
