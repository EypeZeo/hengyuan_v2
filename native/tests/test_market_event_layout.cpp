// P2-CORE-BUILD-01: BinanceMarketEvent compile-time + runtime layout verification.
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/binance_market_event.hpp>
#include <cstring>

using hy::BinanceMarketEvent;
using hy::EventType;
using hy::Side;

TEST(MarketEventLayout, SizeIs64) {
    EXPECT_EQ(sizeof(BinanceMarketEvent), 64u);
}

TEST(MarketEventLayout, AlignmentIs64) {
    EXPECT_EQ(alignof(BinanceMarketEvent), 64u);
}

TEST(MarketEventLayout, TriviallyCopyable) {
    EXPECT_TRUE(std::is_trivially_copyable_v<BinanceMarketEvent>);
}

TEST(MarketEventLayout, StandardLayout) {
    EXPECT_TRUE(std::is_standard_layout_v<BinanceMarketEvent>);
}

TEST(MarketEventLayout, FieldOffsets) {
    EXPECT_EQ(offsetof(BinanceMarketEvent, event_id), 0u);
    EXPECT_EQ(offsetof(BinanceMarketEvent, price_ticks), 8u);
    EXPECT_EQ(offsetof(BinanceMarketEvent, qty_lots), 16u);
    EXPECT_EQ(offsetof(BinanceMarketEvent, ts_event_ms), 24u);
    EXPECT_EQ(offsetof(BinanceMarketEvent, ts_recv_ns), 32u);
    EXPECT_EQ(offsetof(BinanceMarketEvent, symbol_id), 40u);
    EXPECT_EQ(offsetof(BinanceMarketEvent, flags), 46u);
}

TEST(MarketEventLayout, ZeroInitialized) {
    BinanceMarketEvent ev{};
    std::memset(&ev, 0, sizeof(ev));
    EXPECT_EQ(ev.event_id, 0u);
    EXPECT_EQ(ev.price_ticks, 0);
    EXPECT_EQ(ev.qty_lots, 0);
    EXPECT_EQ(ev.ts_event_ms, 0u);
    EXPECT_EQ(ev.ts_recv_ns, 0u);
    EXPECT_EQ(ev.symbol_id, 0u);
    EXPECT_EQ(ev.type, EventType::Trade);
    EXPECT_EQ(ev.side, Side::Buy);
    EXPECT_EQ(ev.flags, 0u);
}

TEST(MarketEventLayout, RoundTripCopy) {
    BinanceMarketEvent src{};
    src.event_id = 12345;
    src.price_ticks = -999'000'000;
    src.qty_lots = 42;
    src.ts_event_ms = 1700000000000ULL;
    src.ts_recv_ns = 98765432100ULL;
    src.symbol_id = 7;
    src.type = EventType::DepthDelta;
    src.side = Side::Sell;
    src.flags = hy::event_flag::kResyncRequired;

    BinanceMarketEvent dst{};
    std::memcpy(&dst, &src, sizeof(BinanceMarketEvent));

    EXPECT_EQ(dst.event_id, 12345u);
    EXPECT_EQ(dst.price_ticks, -999'000'000);
    EXPECT_EQ(dst.qty_lots, 42);
    EXPECT_EQ(dst.ts_event_ms, 1700000000000ULL);
    EXPECT_EQ(dst.ts_recv_ns, 98765432100ULL);
    EXPECT_EQ(dst.symbol_id, 7u);
    EXPECT_EQ(dst.type, EventType::DepthDelta);
    EXPECT_EQ(dst.side, Side::Sell);
    EXPECT_EQ(dst.flags, hy::event_flag::kResyncRequired);
}

TEST(MarketEventLayout, EventFlags) {
    EXPECT_EQ(hy::event_flag::kNone, 0u);
    EXPECT_NE(hy::event_flag::kSnapshot & hy::event_flag::kStale, hy::event_flag::kSnapshot);
    uint16_t combined = hy::event_flag::kSnapshot | hy::event_flag::kClockAnomaly;
    EXPECT_TRUE(combined & hy::event_flag::kSnapshot);
    EXPECT_TRUE(combined & hy::event_flag::kClockAnomaly);
    EXPECT_FALSE(combined & hy::event_flag::kStale);
}
