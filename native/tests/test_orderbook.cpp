// P2-CORE-OB-01: OrderBook unit tests.
#include <gtest/gtest.h>
#include <hengyuan/orderbook.hpp>

using hy::OrderBook;
using hy::PriceLevel;
using hy::Side;

TEST(OrderBook, EmptyTopOfBook) {
    OrderBook ob;
    EXPECT_FALSE(ob.top_of_book().has_value());
}

TEST(OrderBook, SnapshotBasic) {
    OrderBook ob;
    PriceLevel asks[] = {{100, 10}, {101, 20}, {102, 30}};
    PriceLevel bids[] = {{99, 15}, {98, 25}};
    ob.apply_snapshot(asks, 3, bids, 2);

    EXPECT_EQ(ob.ask_count(), 3u);
    EXPECT_EQ(ob.bid_count(), 2u);

    auto tob = ob.top_of_book();
    ASSERT_TRUE(tob.has_value());
    EXPECT_EQ(tob->first, 99);   // best bid
    EXPECT_EQ(tob->second, 100); // best ask
}

TEST(OrderBook, DeltaUpdateExistingLevel) {
    OrderBook ob;
    PriceLevel asks[] = {{100, 10}};
    PriceLevel bids[] = {{99, 15}};
    ob.apply_snapshot(asks, 1, bids, 1);

    // Update ask qty
    auto r = ob.apply_delta(100, 50, Side::Sell);
    EXPECT_EQ(r, OrderBook::ApplyResult::Ok);
    EXPECT_EQ(ob.asks()[0].qty_lots, 50);
}

TEST(OrderBook, DeltaRemoveLevel) {
    OrderBook ob;
    PriceLevel asks[] = {{100, 10}, {101, 20}};
    PriceLevel bids[] = {{99, 15}};
    ob.apply_snapshot(asks, 2, bids, 1);

    // Remove ask at 100 (qty=0)
    auto r = ob.apply_delta(100, 0, Side::Sell);
    EXPECT_EQ(r, OrderBook::ApplyResult::Ok);
    EXPECT_EQ(ob.ask_count(), 1u);
    EXPECT_EQ(ob.asks()[0].price_ticks, 101);
}

TEST(OrderBook, DeltaInsertNewLevel) {
    OrderBook ob;
    PriceLevel asks[] = {{100, 10}, {102, 30}};
    PriceLevel bids[] = {{99, 15}};
    ob.apply_snapshot(asks, 2, bids, 1);

    // Insert ask at 101
    auto r = ob.apply_delta(101, 20, Side::Sell);
    EXPECT_EQ(r, OrderBook::ApplyResult::Ok);
    EXPECT_EQ(ob.ask_count(), 3u);
    EXPECT_EQ(ob.asks()[1].price_ticks, 101);
    EXPECT_EQ(ob.asks()[1].qty_lots, 20);
}

TEST(OrderBook, DeltaInsertBid) {
    OrderBook ob;
    PriceLevel asks[] = {{200, 10}};
    PriceLevel bids[] = {{100, 15}};
    ob.apply_snapshot(asks, 1, bids, 1);

    // Insert new bid explicitly
    auto r = ob.apply_delta(120, 25, Side::Buy);
    EXPECT_EQ(r, OrderBook::ApplyResult::Ok);
    EXPECT_EQ(ob.bid_count(), 2u);
    // Bids sorted descending: 120, 100
    EXPECT_EQ(ob.bids()[0].price_ticks, 120);
    EXPECT_EQ(ob.bids()[1].price_ticks, 100);
}

TEST(OrderBook, RejectsInvalidInput) {
    OrderBook ob;
    EXPECT_EQ(ob.apply_delta(-1, 10, Side::Buy), OrderBook::ApplyResult::InvalidInput);
    EXPECT_EQ(ob.apply_delta(0, 10, Side::Buy), OrderBook::ApplyResult::InvalidInput);
    EXPECT_EQ(ob.apply_delta(100, -5, Side::Buy), OrderBook::ApplyResult::InvalidInput);
}

TEST(OrderBook, ClearsBook) {
    OrderBook ob;
    PriceLevel asks[] = {{100, 10}};
    PriceLevel bids[] = {{99, 15}};
    ob.apply_snapshot(asks, 1, bids, 1);
    ob.clear();
    EXPECT_EQ(ob.ask_count(), 0u);
    EXPECT_EQ(ob.bid_count(), 0u);
    EXPECT_FALSE(ob.top_of_book().has_value());
}

TEST(OrderBook, LevelsFull) {
    OrderBook ob;
    ob.clear();
    std::array<PriceLevel, hy::kMaxLevels> full_asks{};
    for (std::size_t i = 0; i < hy::kMaxLevels; ++i) {
        full_asks[i] = {static_cast<std::int64_t>(1000 + i * 2), 10};
    }
    PriceLevel one_bid{500, 10};
    ob.apply_snapshot(full_asks.data(), hy::kMaxLevels, &one_bid, 1);
    EXPECT_EQ(ob.ask_count(), hy::kMaxLevels);

    // Insert new ask should fail
    auto r = ob.apply_delta(1001, 5, Side::Sell);
    EXPECT_EQ(r, OrderBook::ApplyResult::LevelsFull);
}

TEST(OrderBook, UpdateCount) {
    OrderBook ob;
    PriceLevel asks[] = {{100, 10}};
    PriceLevel bids[] = {{99, 15}};
    ob.apply_snapshot(asks, 1, bids, 1);

    ob.apply_delta(100, 20, Side::Sell);
    ob.apply_delta(101, 30, Side::Sell);
    EXPECT_EQ(ob.update_count(), 2u);
}

TEST(OrderBook, RemoveNonexistentLevel) {
    OrderBook ob;
    PriceLevel asks[] = {{100, 10}};
    PriceLevel bids[] = {{99, 15}};
    ob.apply_snapshot(asks, 1, bids, 1);

    // Remove price that doesn't exist — should be no-op
    auto r = ob.apply_delta(105, 0, Side::Sell);
    EXPECT_EQ(r, OrderBook::ApplyResult::Ok);
    EXPECT_EQ(ob.ask_count(), 1u);
    EXPECT_EQ(ob.bid_count(), 1u);
}

TEST(OrderBook, WidePriceRange) {
    OrderBook ob;
    // Meme coin: 0.0000001 → with 1e8 multiplier = 10 ticks
    // BTC: 100000 → with 1e8 multiplier = 10'000'000'000'000 ticks
    PriceLevel asks[] = {{10, 1'000'000'000}, {10'000'000'000'000LL, 1}};
    PriceLevel bids[] = {{5, 2'000'000'000}};
    ob.apply_snapshot(asks, 2, bids, 1);

    auto tob = ob.top_of_book();
    ASSERT_TRUE(tob.has_value());
    EXPECT_EQ(tob->first, 5);
    EXPECT_EQ(tob->second, 10);
}

TEST(OrderBook, DeltaOnEmptyBook) {
    OrderBook ob;
    // Explicit side allows correct placement even with empty book
    ob.apply_delta(100, 10, Side::Buy);
    ob.apply_delta(101, 20, Side::Sell);
    EXPECT_EQ(ob.bid_count(), 1u);
    EXPECT_EQ(ob.ask_count(), 1u);

    auto tob = ob.top_of_book();
    ASSERT_TRUE(tob.has_value());
    EXPECT_EQ(tob->first, 100);   // best bid
    EXPECT_EQ(tob->second, 101);  // best ask
}

TEST(OrderBook, RemoveBidLevel) {
    OrderBook ob;
    PriceLevel asks[] = {{100, 10}};
    PriceLevel bids[] = {{99, 15}, {98, 25}};
    ob.apply_snapshot(asks, 1, bids, 2);

    auto r = ob.apply_delta(99, 0, Side::Buy);
    EXPECT_EQ(r, OrderBook::ApplyResult::Ok);
    EXPECT_EQ(ob.bid_count(), 1u);
    EXPECT_EQ(ob.bids()[0].price_ticks, 98);
}
