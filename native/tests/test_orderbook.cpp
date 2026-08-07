// P2-CORE-OB-01: OrderBook unit tests.
#include <gtest/gtest.h>
#include <hengyuan/orderbook.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

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

// --- Bounded top-N window semantics (audit MD-BOOK-002) ---
//
// This group replaces the previous `LevelsFull` test, which asserted that a full
// book rejects EVERY insert. That was the bug: after apply_snapshot() truncates a
// real 1000-level Binance snapshot to kMaxLevels the book is instantly full, so
// under the old rule every new price level -- including a new BEST bid/ask -- was
// dropped and top_of_book() froze. The book is a top-N view: a level that belongs
// inside the window must displace the worst held level; only a level worse than all
// of them is OutsideWindow.

namespace {
// A full ask side at 1000, 1002, ... (kMaxLevels levels, strictly ascending).
std::array<PriceLevel, hy::kMaxLevels> make_full_asks() {
    std::array<PriceLevel, hy::kMaxLevels> a{};
    for (std::size_t i = 0; i < hy::kMaxLevels; ++i) {
        a[i] = {static_cast<std::int64_t>(1000 + static_cast<std::int64_t>(i) * 2), 10};
    }
    return a;
}
}  // namespace

TEST(OrderBook, FullBookAcceptsBetterAskAndEvictsWorst) {
    OrderBook ob;
    auto full_asks = make_full_asks();
    PriceLevel one_bid{500, 10};
    ob.apply_snapshot(full_asks.data(), hy::kMaxLevels, &one_bid, 1);
    ASSERT_EQ(ob.ask_count(), hy::kMaxLevels);
    const std::int64_t worst_before = ob.asks()[hy::kMaxLevels - 1].price_ticks;

    // A new BEST ask (better than the current best of 1000).
    auto r = ob.apply_delta(999, 7, Side::Sell);
    EXPECT_EQ(r, OrderBook::ApplyResult::Ok);
    EXPECT_EQ(ob.ask_count(), hy::kMaxLevels) << "window size must stay fixed";
    EXPECT_EQ(ob.asks()[0].price_ticks, 999);
    EXPECT_EQ(ob.asks()[0].qty_lots, 7);
    EXPECT_LT(ob.asks()[hy::kMaxLevels - 1].price_ticks, worst_before)
        << "the worst level must have been evicted, not the new best dropped";
}

TEST(OrderBook, FullBookAcceptsMidWindowAsk) {
    OrderBook ob;
    auto full_asks = make_full_asks();
    PriceLevel one_bid{500, 10};
    ob.apply_snapshot(full_asks.data(), hy::kMaxLevels, &one_bid, 1);

    // 1001 sits between the existing 1000 and 1002 -- inside the window.
    auto r = ob.apply_delta(1001, 5, Side::Sell);
    EXPECT_EQ(r, OrderBook::ApplyResult::Ok);
    EXPECT_EQ(ob.ask_count(), hy::kMaxLevels);
    EXPECT_EQ(ob.asks()[1].price_ticks, 1001);
    EXPECT_EQ(ob.asks()[1].qty_lots, 5);
}

TEST(OrderBook, FullBookReportsOutsideWindowForWorseThanAllAsk) {
    OrderBook ob;
    auto full_asks = make_full_asks();
    PriceLevel one_bid{500, 10};
    ob.apply_snapshot(full_asks.data(), hy::kMaxLevels, &one_bid, 1);
    const std::int64_t worst = ob.asks()[hy::kMaxLevels - 1].price_ticks;

    auto r = ob.apply_delta(worst + 100, 5, Side::Sell);
    EXPECT_EQ(r, OrderBook::ApplyResult::OutsideWindow);
    EXPECT_EQ(ob.ask_count(), hy::kMaxLevels);
    EXPECT_EQ(ob.asks()[hy::kMaxLevels - 1].price_ticks, worst) << "book must be unchanged";
}

TEST(OrderBook, FullBookAcceptsBetterBidAndEvictsWorst) {
    OrderBook ob;
    std::array<PriceLevel, hy::kMaxLevels> full_bids{};
    for (std::size_t i = 0; i < hy::kMaxLevels; ++i) {
        full_bids[i] = {static_cast<std::int64_t>(50'000 - static_cast<std::int64_t>(i)), 10};
    }
    PriceLevel one_ask{60'000, 10};
    ob.apply_snapshot(&one_ask, 1, full_bids.data(), hy::kMaxLevels);
    ASSERT_EQ(ob.bid_count(), hy::kMaxLevels);
    const std::int64_t worst_before = ob.bids()[hy::kMaxLevels - 1].price_ticks;

    auto r = ob.apply_delta(50'001, 42, Side::Buy);
    EXPECT_EQ(r, OrderBook::ApplyResult::Ok);
    EXPECT_EQ(ob.bid_count(), hy::kMaxLevels);
    EXPECT_EQ(ob.bids()[0].price_ticks, 50'001);
    EXPECT_GT(ob.bids()[hy::kMaxLevels - 1].price_ticks, worst_before)
        << "bids are best-first descending; evicting the worst raises the tail price";
}

TEST(OrderBook, FullBookStillSortedAfterManyEvictions) {
    OrderBook ob;
    auto full_asks = make_full_asks();
    PriceLevel one_bid{500, 10};
    ob.apply_snapshot(full_asks.data(), hy::kMaxLevels, &one_bid, 1);

    // Hammer the top of the book the way a live market does.
    for (std::int64_t p = 999; p > 900; --p) {
        ASSERT_EQ(ob.apply_delta(p, 3, Side::Sell), OrderBook::ApplyResult::Ok);
    }
    ASSERT_EQ(ob.ask_count(), hy::kMaxLevels);
    for (std::size_t i = 1; i < ob.ask_count(); ++i) {
        ASSERT_LT(ob.asks()[i - 1].price_ticks, ob.asks()[i].price_ticks)
            << "asks must stay strictly ascending at index " << i;
    }
    EXPECT_EQ(ob.asks()[0].price_ticks, 901);
}

TEST(OrderBook, RealisticSnapshotDoesNotFreezeTopOfBook) {
    // The exact shape the audit repro used: a default-config REST snapshot
    // (limit=1000, binance_rest_snapshot.hpp) truncated into the 256-level window,
    // followed by the single most common live event -- a new best bid.
    OrderBook ob;
    std::array<PriceLevel, 1000> bids{};
    std::array<PriceLevel, 1000> asks{};
    for (std::size_t i = 0; i < 1000; ++i) {
        bids[i] = {static_cast<std::int64_t>(50'000 - static_cast<std::int64_t>(i)), 10};
        asks[i] = {static_cast<std::int64_t>(50'001 + static_cast<std::int64_t>(i)), 10};
    }
    ob.apply_snapshot(asks.data(), asks.size(), bids.data(), bids.size());
    ASSERT_EQ(ob.bid_count(), hy::kMaxLevels);
    ASSERT_EQ(ob.ask_count(), hy::kMaxLevels);

    auto before = ob.top_of_book();
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->first, 50'000);

    ASSERT_EQ(ob.apply_delta(50'000 + 1, 42, Side::Buy), OrderBook::ApplyResult::Ok);

    auto after = ob.top_of_book();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->first, 50'001) << "top of book must track the new best bid";
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
