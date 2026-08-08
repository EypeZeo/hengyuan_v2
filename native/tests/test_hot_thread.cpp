// HotThread unit tests — feed events through ring → validator → orderbook.
#include <gtest/gtest.h>
#include <hengyuan/hot_thread.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

using hy::BinanceMarketEvent;
using hy::EventType;
using hy::HotThread;
using hy::Side;
using hy::SpscRing;

static BinanceMarketEvent make_depth(std::uint64_t id, std::int64_t price,
                                      std::int64_t qty, std::uint64_t ts_ms,
                                      Side side = Side::Buy) {
    BinanceMarketEvent ev{};
    ev.event_id = id;
    ev.price_ticks = price;
    ev.qty_lots = qty;
    ev.ts_event_ms = ts_ms;
    ev.type = EventType::DepthDelta;
    ev.side = side;
    return ev;
}

static BinanceMarketEvent make_trade(std::uint64_t id, std::int64_t price,
                                      std::int64_t qty, std::uint64_t ts_ms) {
    BinanceMarketEvent ev{};
    ev.event_id = id;
    ev.price_ticks = price;
    ev.qty_lots = qty;
    ev.ts_event_ms = ts_ms;
    ev.type = EventType::Trade;
    ev.side = Side::Buy;
    return ev;
}

TEST(HotThread, ProcessesDepthAndUpdatesBook) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    // Build a book: asks at 100,101; bids at 99,98
    ring.try_push(make_depth(1, 100, 10, 1000, Side::Sell));  // ask
    ring.try_push(make_depth(1, 101, 20, 1001, Side::Sell));  // ask (same event_id OK for depth)
    ring.try_push(make_depth(1, 99, 15, 1002, Side::Buy));    // bid
    ring.try_push(make_depth(1, 98, 25, 1003, Side::Buy));    // bid

    ht.run_once();

    EXPECT_EQ(ht.stats().events_processed, 4u);
    EXPECT_EQ(ht.stats().depth_updates, 4u);

    auto tob = ht.book().top_of_book();
    ASSERT_TRUE(tob.has_value());
    EXPECT_EQ(tob->first, 99);   // best bid
    EXPECT_EQ(tob->second, 100); // best ask
}

TEST(HotThread, CountsTrades) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    ring.try_push(make_trade(1, 50000, 100, 1000));
    ring.try_push(make_trade(2, 50001, 200, 1001));

    ht.run_once();

    EXPECT_EQ(ht.stats().trades, 2u);
    EXPECT_EQ(ht.stats().events_processed, 2u);
}

TEST(HotThread, RejectsInvalidEvents) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    auto bad = make_trade(1, -100, 10, 1000);  // negative price
    ring.try_push(bad);

    ht.run_once();

    EXPECT_EQ(ht.stats().rejected, 1u);
    EXPECT_EQ(ht.stats().events_processed, 0u);
}

TEST(HotThread, HandlesResync) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    ring.try_push(make_depth(1, 100, 10, 1000, Side::Sell));
    ht.run_once();

    BinanceMarketEvent resync{};
    resync.event_id = 2;
    resync.price_ticks = 100;
    resync.qty_lots = 10;
    resync.ts_event_ms = 1001;
    resync.flags = hy::event_flag::kResyncRequired;
    ring.try_push(resync);

    ht.run_once();

    EXPECT_EQ(ht.stats().resync_requests, 1u);
    EXPECT_EQ(ht.book().ask_count(), 0u);
    EXPECT_EQ(ht.book().bid_count(), 0u);
}

TEST(HotThread, TopOfBookCallback) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    std::int64_t last_bid = 0, last_ask = 0;
    ht.set_on_top_of_book([&](std::uint32_t, std::int64_t bid, std::int64_t ask) {
        last_bid = bid;
        last_ask = ask;
    });

    ring.try_push(make_depth(1, 200, 10, 1000, Side::Sell));  // ask
    ring.try_push(make_depth(1, 100, 15, 1001, Side::Buy));   // bid

    ht.run_once();

    auto tob = ht.book().top_of_book();
    ASSERT_TRUE(tob.has_value());
    EXPECT_EQ(last_bid, 100);
    EXPECT_EQ(last_ask, 200);
}

TEST(HotThread, EmptyRingNoOp) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    ht.run_once();  // should not crash

    EXPECT_EQ(ht.stats().events_processed, 0u);
}

TEST(HotThread, StopFlag) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    EXPECT_FALSE(ht.should_stop());
    ht.request_stop();
    EXPECT_TRUE(ht.should_stop());
}

TEST(HotThread, OnEventCallback) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    std::size_t callback_count = 0;
    ht.set_on_event([&](const BinanceMarketEvent&) { ++callback_count; });

    ring.try_push(make_trade(1, 50000, 100, 1000));
    ring.try_push(make_depth(2, 100, 10, 1001, Side::Sell));
    ht.run_once();

    EXPECT_EQ(callback_count, 2u);
}

TEST(HotThread, MultiSymbolBooks) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    // BTC depth (symbol 0)
    auto btc_bid = make_depth(1, 5990000, 100, 1000, Side::Buy);
    btc_bid.symbol_id = 0;
    auto btc_ask = make_depth(1, 5990100, 50, 1001, Side::Sell);
    btc_ask.symbol_id = 0;
    ring.try_push(btc_bid);
    ring.try_push(btc_ask);

    // ETH depth (symbol 1)
    auto eth_bid = make_depth(1, 300000, 200, 1002, Side::Buy);
    eth_bid.symbol_id = 1;
    auto eth_ask = make_depth(1, 300100, 150, 1003, Side::Sell);
    eth_ask.symbol_id = 1;
    ring.try_push(eth_bid);
    ring.try_push(eth_ask);

    ht.run_once();

    auto btc_tob = ht.book(0).top_of_book();
    ASSERT_TRUE(btc_tob.has_value());
    EXPECT_EQ(btc_tob->first, 5990000);
    EXPECT_EQ(btc_tob->second, 5990100);

    auto eth_tob = ht.book(1).top_of_book();
    ASSERT_TRUE(eth_tob.has_value());
    EXPECT_EQ(eth_tob->first, 300000);
    EXPECT_EQ(eth_tob->second, 300100);

    EXPECT_FALSE(ht.book(2).top_of_book().has_value());
}

TEST(HotThread, TobCallbackIncludesSymbolId) {
    SpscRing<BinanceMarketEvent, 64> ring;
    HotThread<64> ht(ring);

    std::uint32_t last_sym = 999;
    ht.set_on_top_of_book([&](std::uint32_t sym, std::int64_t, std::int64_t) {
        last_sym = sym;
    });

    auto bid = make_depth(1, 100, 10, 1000, Side::Buy);
    bid.symbol_id = 3;
    auto ask = make_depth(1, 200, 10, 1001, Side::Sell);
    ask.symbol_id = 3;
    ring.try_push(bid);
    ring.try_push(ask);
    ht.run_once();

    EXPECT_EQ(last_sym, 3u);
}

// --- Bounded drain per run_once() (audit HOT-DRAIN-027) ---
//
// run_once() used to drain the ring to empty with no bound, so a burst could keep
// one call running for the whole 65536-slot backlog -- during which the snapshot
// gate, the intent channel and the watchdog kill check (all after the loop) do not
// run at all. That is a tail-latency and fail-closed-responsiveness problem.

TEST(HotThreadDrain, OneCallConsumesAtMostTheDrainLimit) {
    using Ring = hy::SpscRing<hy::BinanceMarketEvent, 65536>;
    auto ring = std::make_unique<Ring>();
    hy::HotThread<65536> hot(*ring);

    constexpr std::size_t kOverfill = hy::HotThread<65536>::kMaxEventsPerRun + 500;
    for (std::size_t i = 0; i < kOverfill; ++i) {
        hy::BinanceMarketEvent ev{};
        ev.type = hy::EventType::Trade;
        ev.symbol_id = 0;
        ev.event_id = i + 1;
        ev.price_ticks = 100;
        ev.qty_lots = 1;
        ASSERT_TRUE(ring->try_push(ev));
    }

    hot.run_once();
    EXPECT_EQ(hot.stats().events_processed, hy::HotThread<65536>::kMaxEventsPerRun)
        << "one call must not run away with the whole backlog";
    EXPECT_EQ(hot.stats().drain_limit_hits, 1u) << "hitting the bound must be observable";

    // The remainder is not lost -- it is simply picked up by the next call.
    hot.run_once();
    EXPECT_EQ(hot.stats().events_processed, kOverfill);
    EXPECT_EQ(hot.stats().drain_limit_hits, 1u);
}

TEST(HotThreadDrain, OrdinaryLoadDrainsFullyAndCountsNoLimitHit) {
    using Ring = hy::SpscRing<hy::BinanceMarketEvent, 65536>;
    auto ring = std::make_unique<Ring>();
    hy::HotThread<65536> hot(*ring);

    for (std::size_t i = 0; i < 64; ++i) {
        hy::BinanceMarketEvent ev{};
        ev.type = hy::EventType::Trade;
        ev.symbol_id = 0;
        ev.event_id = i + 1;
        ev.price_ticks = 100;
        ev.qty_lots = 1;
        ASSERT_TRUE(ring->try_push(ev));
    }
    hot.run_once();
    EXPECT_EQ(hot.stats().events_processed, 64u);
    EXPECT_EQ(hot.stats().drain_limit_hits, 0u)
        << "the bound must cost nothing in the steady state";
}
