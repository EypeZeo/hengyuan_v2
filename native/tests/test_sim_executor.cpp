// P2-EXEC-SIM-01: SimExecutor dry-run simulation.
#include <gtest/gtest.h>
#include <hengyuan/sim_executor.hpp>

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

TEST(SimExecutor, MultiSymbolIndependentPositions) {
    SimExecutor<64> ex(cfg_no_fee(1000));
    ex.execute(mkt(OrderSide::Buy, 10, 0), 100, 100);
    ex.execute(mkt(OrderSide::Buy, 5, 1), 200, 200);
    EXPECT_EQ(ex.position(0).net_qty_lots, 10);
    EXPECT_EQ(ex.position(1).net_qty_lots, 5);
}
