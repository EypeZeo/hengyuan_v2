// P2-EXEC-SIM-02: Dry-run pipeline integration test.
// Synthetic events → SPSC → HotThread → OrderBook → SimExecutor.
// No network, no Boost, no real WS — pure L2 simulation.
#include <gtest/gtest.h>
#include <hengyuan/hot_thread.hpp>
#include <hengyuan/sim_executor.hpp>

using hy::BinanceMarketEvent;
using hy::EventType;
using hy::ExecutionIntent;
using hy::FillStatus;
using hy::HotThread;
using hy::KillState;
using hy::OrderSide;
using hy::OrderType;
using hy::RiskLimits;
using hy::Side;
using hy::SimConfig;
using hy::SimExecutor;
using hy::SpscRing;

static BinanceMarketEvent depth_ev(std::uint64_t id, std::int64_t price,
                                    std::int64_t qty, Side side,
                                    std::uint64_t ts_ms = 1000) {
    BinanceMarketEvent ev{};
    ev.event_id = id;
    ev.price_ticks = price;
    ev.qty_lots = qty;
    ev.ts_event_ms = ts_ms;
    ev.type = EventType::DepthDelta;
    ev.side = side;
    return ev;
}

static BinanceMarketEvent trade_ev(std::uint64_t id, std::int64_t price,
                                    std::int64_t qty, std::uint64_t ts_ms = 1000) {
    BinanceMarketEvent ev{};
    ev.event_id = id;
    ev.price_ticks = price;
    ev.qty_lots = qty;
    ev.ts_event_ms = ts_ms;
    ev.type = EventType::Trade;
    ev.side = Side::Buy;
    return ev;
}

class DryRunPipeline : public ::testing::Test {
protected:
    static constexpr std::size_t kRing = 256;

    SpscRing<BinanceMarketEvent, kRing> ring;
    HotThread<kRing> hot{ring};
    SimConfig cfg{
        .limits = RiskLimits{.max_position_lots = 10'000'000, .max_notional_ticks = 0},
        .fee_ppm = 750,
        .max_drawdown = 0,
    };
    SimExecutor<> sim{cfg};

    void build_book(std::int64_t bid, std::int64_t ask) {
        ring.try_push(depth_ev(1, bid, 100'000'000, Side::Buy));
        ring.try_push(depth_ev(1, ask, 100'000'000, Side::Sell));
        hot.run_once();
    }
};

TEST_F(DryRunPipeline, FullBuySellCycle) {
    // Build book: bid=5990000000000 ask=5990100000000 (BTC ~$59,901)
    const std::int64_t bid = 5990000000000LL;
    const std::int64_t ask = 5990100000000LL;
    build_book(bid, ask);

    auto tob = hot.book().top_of_book();
    ASSERT_TRUE(tob.has_value());
    EXPECT_EQ(tob->first, bid);
    EXPECT_EQ(tob->second, ask);

    // Buy 0.001 BTC (100000 lots at 1e8 multiplier)
    ExecutionIntent buy{};
    buy.symbol_id = 0;
    buy.side = OrderSide::Buy;
    buy.type = OrderType::Market;
    buy.qty_lots = 100'000;

    auto fill = sim.execute(buy, tob->first, tob->second);
    EXPECT_EQ(fill.status, FillStatus::Filled);
    EXPECT_EQ(fill.fill_price_ticks, ask);  // market buy fills at ask
    EXPECT_EQ(fill.filled_qty_lots, 100'000);
    EXPECT_GT(fill.fee_ticks, 0);

    auto pos = sim.position(0);
    EXPECT_EQ(pos.net_qty_lots, 100'000);
    EXPECT_EQ(pos.avg_entry_price_ticks, ask);
    EXPECT_LT(pos.realized_pnl, 0);  // fee is negative realized

    // Sell same qty to close
    ExecutionIntent sell{};
    sell.symbol_id = 0;
    sell.side = OrderSide::Sell;
    sell.type = OrderType::Market;
    sell.qty_lots = 100'000;

    auto fill2 = sim.execute(sell, tob->first, tob->second);
    EXPECT_EQ(fill2.status, FillStatus::Filled);
    EXPECT_EQ(fill2.fill_price_ticks, bid);  // market sell fills at bid

    pos = sim.position(0);
    EXPECT_EQ(pos.net_qty_lots, 0);  // flat
    // realized PnL = sell - buy spread loss + 2x fees < 0
    EXPECT_LT(pos.realized_pnl, 0);
}

TEST_F(DryRunPipeline, BookUpdatesAffectFillPrice) {
    build_book(100, 101);

    ExecutionIntent buy{};
    buy.symbol_id = 0;
    buy.side = OrderSide::Buy;
    buy.type = OrderType::Market;
    buy.qty_lots = 10;

    auto tob1 = hot.book().top_of_book();
    auto fill1 = sim.execute(buy, tob1->first, tob1->second);
    EXPECT_EQ(fill1.fill_price_ticks, 101);

    // Book moves: new ask at 105
    ring.try_push(depth_ev(2, 101, 0, Side::Sell));    // remove old ask
    ring.try_push(depth_ev(2, 105, 50, Side::Sell));   // new ask
    hot.run_once();

    auto tob2 = hot.book().top_of_book();
    ASSERT_TRUE(tob2.has_value());
    EXPECT_EQ(tob2->second, 105);

    buy.qty_lots = 10;
    auto fill2 = sim.execute(buy, tob2->first, tob2->second);
    EXPECT_EQ(fill2.fill_price_ticks, 105);  // fills at new ask
}

TEST_F(DryRunPipeline, RiskGateBlocksOversize) {
    build_book(100, 101);

    SimConfig strict_cfg{
        .limits = RiskLimits{.max_position_lots = 50, .max_notional_ticks = 0},
        .fee_ppm = 0,
        .max_drawdown = 0,
    };
    SimExecutor<> strict_sim(strict_cfg);

    auto tob = hot.book().top_of_book();
    ExecutionIntent big_buy{};
    big_buy.symbol_id = 0;
    big_buy.side = OrderSide::Buy;
    big_buy.type = OrderType::Market;
    big_buy.qty_lots = 100;  // exceeds max_position_lots=50

    auto fill = strict_sim.execute(big_buy, tob->first, tob->second);
    EXPECT_EQ(fill.status, FillStatus::Rejected);
    EXPECT_EQ(strict_sim.stats().rejected, 1u);
}

TEST_F(DryRunPipeline, DrawdownArmsKillSwitch) {
    SimConfig dd_cfg{
        .limits = RiskLimits{.max_position_lots = 10'000'000, .max_notional_ticks = 0},
        .fee_ppm = 0,
        .max_drawdown = 1,  // very tight drawdown
    };
    SimExecutor<> dd_sim(dd_cfg);

    build_book(100, 200);  // wide spread

    ExecutionIntent buy{};
    buy.symbol_id = 0;
    buy.side = OrderSide::Buy;
    buy.type = OrderType::Market;
    buy.qty_lots = 100;

    // Buy at 200, mark is (100+200)/2=150, unrealized = (150-200)*100 = -5000 < -1
    auto fill = dd_sim.execute(buy, 100, 200);
    EXPECT_EQ(fill.status, FillStatus::Filled);
    EXPECT_EQ(dd_sim.kill_switch().state(), KillState::Armed);

    // Next buy should be rejected (armed blocks opening)
    auto fill2 = dd_sim.execute(buy, 100, 200);
    EXPECT_EQ(fill2.status, FillStatus::Rejected);
}

TEST_F(DryRunPipeline, EmptyBookNoLiquidity) {
    // Don't build book — TOB is empty
    auto tob = hot.book().top_of_book();
    EXPECT_FALSE(tob.has_value());

    ExecutionIntent buy{};
    buy.symbol_id = 0;
    buy.side = OrderSide::Buy;
    buy.type = OrderType::Market;
    buy.qty_lots = 100;

    auto fill = sim.execute(buy, 0, 0);
    EXPECT_EQ(fill.status, FillStatus::NoLiquidity);
}

TEST_F(DryRunPipeline, TradeEventsPassThrough) {
    build_book(100, 101);
    ring.try_push(trade_ev(10, 100, 50, 2000));
    ring.try_push(trade_ev(11, 101, 30, 2001));
    hot.run_once();

    EXPECT_EQ(hot.stats().trades, 2u);
    EXPECT_EQ(hot.stats().depth_updates, 2u);  // from build_book
}

TEST_F(DryRunPipeline, BtcScaleBuySellBuySellBuy) {
    // Reproduce the exact VPS scenario: 5 alternating trades at BTC prices.
    // Prices: $59,901 ask / $59,900 bid with ~$1 spread.
    const std::int64_t bid1 = 5990000000000LL;  // $59,900.00
    const std::int64_t ask1 = 5990100000000LL;  // $59,901.00
    build_book(bid1, ask1);

    SimConfig btc_cfg{
        .limits = RiskLimits{.max_position_lots = 10'000'000, .max_notional_ticks = 0},
        .fee_ppm = 750,
        .max_drawdown = 0,
    };
    SimExecutor<> btc_sim(btc_cfg);
    const std::int64_t qty = 100'000;  // 0.001 BTC

    ExecutionIntent buy{};
    buy.symbol_id = 0;
    buy.side = OrderSide::Buy;
    buy.type = OrderType::Market;
    buy.qty_lots = qty;

    ExecutionIntent sell{};
    sell.symbol_id = 0;
    sell.side = OrderSide::Sell;
    sell.type = OrderType::Market;
    sell.qty_lots = qty;

    auto tob = hot.book().top_of_book();

    // Trade 1: BUY
    auto f1 = btc_sim.execute(buy, tob->first, tob->second);
    EXPECT_EQ(f1.status, FillStatus::Filled);
    EXPECT_EQ(f1.fill_price_ticks, ask1);
    EXPECT_GT(f1.fee_ticks, 0);  // fee must not be zero
    EXPECT_EQ(btc_sim.position(0).net_qty_lots, qty);

    // Trade 2: SELL (close)
    auto f2 = btc_sim.execute(sell, tob->first, tob->second);
    EXPECT_EQ(f2.status, FillStatus::Filled);
    EXPECT_EQ(f2.fill_price_ticks, bid1);
    EXPECT_EQ(btc_sim.position(0).net_qty_lots, 0);

    // Trade 3: BUY
    auto f3 = btc_sim.execute(buy, tob->first, tob->second);
    EXPECT_EQ(f3.status, FillStatus::Filled);
    EXPECT_EQ(btc_sim.position(0).net_qty_lots, qty);

    // Trade 4: SELL (close)
    auto f4 = btc_sim.execute(sell, tob->first, tob->second);
    EXPECT_EQ(f4.status, FillStatus::Filled);
    EXPECT_EQ(btc_sim.position(0).net_qty_lots, 0);

    // Trade 5: BUY (open)
    auto f5 = btc_sim.execute(buy, tob->first, tob->second);
    EXPECT_EQ(f5.status, FillStatus::Filled);
    EXPECT_EQ(btc_sim.position(0).net_qty_lots, qty);

    EXPECT_EQ(btc_sim.stats().filled, 5u);
    EXPECT_EQ(btc_sim.stats().overflow_guard, 0u);

    // Total PnL sanity: 5 fees (~$0.045 each = ~$0.225) + 2 round-trip losses
    // In ticks*lots: each fee ≈ 4.5e14, spread loss ≈ 1e11 per round-trip
    // Total ≈ 5 × 4.5e14 + 2 × 1e11 ≈ 2.25e15
    const auto& pos = btc_sim.position(0);
    const std::int64_t total_pnl = pos.realized_pnl;

    // PnL must be negative (fees + spread losses)
    EXPECT_LT(total_pnl, 0);
    // PnL magnitude must be reasonable: at most ~1e16 (≈$1 USD).
    // The VPS showed -2.49e18 (≈-$249), which is 1000× too large.
    EXPECT_GT(total_pnl, -1'000'000'000'000'000'0LL);  // > -1e16 (> -$1)
}

TEST_F(DryRunPipeline, FeePpmCalculation) {
    build_book(100, 100);  // tight spread

    SimConfig fee_cfg{
        .limits = RiskLimits{.max_position_lots = 10'000'000, .max_notional_ticks = 0},
        .fee_ppm = 750,  // 0.075%
        .max_drawdown = 0,
    };
    SimExecutor<> fee_sim(fee_cfg);

    auto tob = hot.book().top_of_book();
    ExecutionIntent buy{};
    buy.symbol_id = 0;
    buy.side = OrderSide::Buy;
    buy.type = OrderType::Market;
    buy.qty_lots = 1'000'000;

    auto fill = fee_sim.execute(buy, tob->first, tob->second);
    EXPECT_EQ(fill.status, FillStatus::Filled);
    // notional = 100 * 1000000 = 100000000, fee = 100000000 * 750 / 1000000 = 75000
    EXPECT_EQ(fill.fee_ticks, 75'000);
}
