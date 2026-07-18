// P2-EXEC-SIM-04: Intent channel + HotThread executor integration.
#include <gtest/gtest.h>
#include <hengyuan/hot_thread.hpp>
#include <hengyuan/intent_channel.hpp>

using hy::BinanceMarketEvent;
using hy::EventType;
using hy::ExecutionIntent;
using hy::FillStatus;
using hy::HotThread;
using hy::IntentChannel;
using hy::OrderSide;
using hy::OrderType;
using hy::RiskLimits;
using hy::Side;
using hy::SimConfig;
using hy::SimExecutor;
using hy::SimFill;
using hy::SpscRing;
using hy::TimestampedIntent;

static BinanceMarketEvent depth(std::int64_t price, std::int64_t qty,
                                 Side side, std::uint64_t id = 1) {
    BinanceMarketEvent ev{};
    ev.event_id = id;
    ev.price_ticks = price;
    ev.qty_lots = qty;
    ev.ts_event_ms = 1000;
    ev.type = EventType::DepthDelta;
    ev.side = side;
    return ev;
}

TEST(IntentChannel, PushPopBasic) {
    IntentChannel<> ch;
    TimestampedIntent ti{};
    ti.intent.symbol_id = 0;
    ti.intent.side = OrderSide::Buy;
    ti.intent.type = OrderType::Market;
    ti.intent.qty_lots = 100;
    ti.strategy_id = 42;

    EXPECT_TRUE(ch.try_push(ti));
    TimestampedIntent out{};
    EXPECT_TRUE(ch.try_pop(out));
    EXPECT_EQ(out.intent.qty_lots, 100);
    EXPECT_EQ(out.strategy_id, 42u);
}

TEST(IntentChannel, HotThreadDrainsAndExecutes) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    // Build book
    ring.try_push(depth(100, 50, Side::Buy));
    ring.try_push(depth(101, 50, Side::Sell));
    ht.run_once();

    // Wire intent channel + executor
    IntentChannel<> intent_ch;
    SimConfig cfg{
        .limits = RiskLimits{.max_position_lots = 10000, .max_notional_ticks = 0},
        .fee_ppm = 0,
        .max_drawdown = 0,
    };
    SimExecutor<> sim(cfg);

    std::vector<FillStatus> fills;
    ht.set_intent_executor(&intent_ch, &sim,
        [&](const TimestampedIntent&, const SimFill& f) {
            fills.push_back(f.status);
        });

    // Push intent
    TimestampedIntent ti{};
    ti.intent.symbol_id = 0;
    ti.intent.side = OrderSide::Buy;
    ti.intent.type = OrderType::Market;
    ti.intent.qty_lots = 10;
    ti.strategy_id = 1;
    intent_ch.try_push(ti);

    // Drain — should execute the intent
    ht.run_once();

    EXPECT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0], FillStatus::Filled);
    EXPECT_EQ(sim.position(0).net_qty_lots, 10);
}

TEST(IntentChannel, NoBookNoLiquidity) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);
    // No book — empty

    IntentChannel<> intent_ch;
    SimConfig cfg{};
    SimExecutor<> sim(cfg);

    FillStatus last_status = FillStatus::Filled;
    ht.set_intent_executor(&intent_ch, &sim,
        [&](const TimestampedIntent&, const SimFill& f) {
            last_status = f.status;
        });

    TimestampedIntent ti{};
    ti.intent.symbol_id = 0;
    ti.intent.side = OrderSide::Buy;
    ti.intent.type = OrderType::Market;
    ti.intent.qty_lots = 10;
    intent_ch.try_push(ti);
    ht.run_once();

    EXPECT_EQ(last_status, FillStatus::NoLiquidity);
}

TEST(IntentChannel, MultipleIntentsProcessed) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    ring.try_push(depth(100, 50, Side::Buy));
    ring.try_push(depth(101, 50, Side::Sell));
    ht.run_once();

    IntentChannel<> intent_ch;
    SimConfig cfg{
        .limits = RiskLimits{.max_position_lots = 10000, .max_notional_ticks = 0},
        .fee_ppm = 0,
        .max_drawdown = 0,
    };
    SimExecutor<> sim(cfg);

    int fill_count = 0;
    ht.set_intent_executor(&intent_ch, &sim,
        [&](const TimestampedIntent&, const SimFill&) { ++fill_count; });

    // Push 3 intents
    for (int i = 0; i < 3; ++i) {
        TimestampedIntent ti{};
        ti.intent.symbol_id = 0;
        ti.intent.side = (i % 2 == 0) ? OrderSide::Buy : OrderSide::Sell;
        ti.intent.type = OrderType::Market;
        ti.intent.qty_lots = 10;
        ti.strategy_id = static_cast<std::uint32_t>(i);
        intent_ch.try_push(ti);
    }

    ht.run_once();

    EXPECT_EQ(fill_count, 3);
    EXPECT_EQ(sim.stats().filled, 3u);
}
