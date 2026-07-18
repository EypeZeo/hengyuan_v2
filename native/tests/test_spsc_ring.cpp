// P2-CORE-BUILD-01: SpscRing unit tests — single-threaded correctness.
// Multi-threaded stress + TSAN coverage deferred to P2-CORE-05/BUILD follow-up.
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/spsc_ring.hpp>
#include <hengyuan/binance_market_event.hpp>

using hy::SpscRing;
using hy::BinanceMarketEvent;

TEST(SpscRing, Capacity) {
    SpscRing<BinanceMarketEvent, 1024> ring;
    EXPECT_EQ(ring.capacity(), 1024u);
}

TEST(SpscRing, EmptyOnConstruction) {
    SpscRing<BinanceMarketEvent, 64> ring;
    EXPECT_TRUE(ring.empty_approx());
    EXPECT_EQ(ring.size_approx(), 0u);
}

TEST(SpscRing, PopFromEmptyReturnsFalse) {
    SpscRing<BinanceMarketEvent, 64> ring;
    BinanceMarketEvent out{};
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, PushPopSingle) {
    SpscRing<BinanceMarketEvent, 64> ring;
    BinanceMarketEvent ev{};
    ev.event_id = 42;
    ev.price_ticks = 123456789;
    ev.qty_lots = 100;
    ev.symbol_id = 3;

    EXPECT_TRUE(ring.try_push(ev));
    EXPECT_EQ(ring.size_approx(), 1u);

    BinanceMarketEvent out{};
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.event_id, 42u);
    EXPECT_EQ(out.price_ticks, 123456789);
    EXPECT_EQ(out.qty_lots, 100);
    EXPECT_EQ(out.symbol_id, 3u);

    EXPECT_TRUE(ring.empty_approx());
}

TEST(SpscRing, FIFO) {
    SpscRing<BinanceMarketEvent, 64> ring;
    for (uint64_t i = 0; i < 10; ++i) {
        BinanceMarketEvent ev{};
        ev.event_id = i;
        EXPECT_TRUE(ring.try_push(ev));
    }

    for (uint64_t i = 0; i < 10; ++i) {
        BinanceMarketEvent out{};
        EXPECT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out.event_id, i);
    }
}

TEST(SpscRing, FullReturnsFalse) {
    constexpr std::size_t N = 8;
    SpscRing<BinanceMarketEvent, N> ring;

    for (std::size_t i = 0; i < N; ++i) {
        BinanceMarketEvent ev{};
        ev.event_id = i;
        EXPECT_TRUE(ring.try_push(ev));
    }

    BinanceMarketEvent overflow{};
    overflow.event_id = 999;
    EXPECT_FALSE(ring.try_push(overflow));
}

TEST(SpscRing, WrapAround) {
    constexpr std::size_t N = 4;
    SpscRing<BinanceMarketEvent, N> ring;

    for (uint64_t round = 0; round < 10; ++round) {
        for (uint64_t i = 0; i < N; ++i) {
            BinanceMarketEvent ev{};
            ev.event_id = round * N + i;
            EXPECT_TRUE(ring.try_push(ev));
        }

        BinanceMarketEvent extra{};
        EXPECT_FALSE(ring.try_push(extra));

        for (uint64_t i = 0; i < N; ++i) {
            BinanceMarketEvent out{};
            EXPECT_TRUE(ring.try_pop(out));
            EXPECT_EQ(out.event_id, round * N + i);
        }

        EXPECT_TRUE(ring.empty_approx());
    }
}

TEST(SpscRing, InterleavedPushPop) {
    SpscRing<BinanceMarketEvent, 8> ring;
    uint64_t write_seq = 0;
    uint64_t read_seq = 0;

    for (int batch = 0; batch < 100; ++batch) {
        std::size_t to_push = static_cast<std::size_t>(batch % 5 + 1);
        for (std::size_t i = 0; i < to_push; ++i) {
            BinanceMarketEvent ev{};
            ev.event_id = write_seq++;
            if (!ring.try_push(ev)) {
                --write_seq;
                break;
            }
        }

        std::size_t to_pop = static_cast<std::size_t>(batch % 3 + 1);
        for (std::size_t i = 0; i < to_pop; ++i) {
            BinanceMarketEvent out{};
            if (ring.try_pop(out)) {
                EXPECT_EQ(out.event_id, read_seq);
                ++read_seq;
            }
        }
    }

    while (true) {
        BinanceMarketEvent out{};
        if (!ring.try_pop(out)) break;
        EXPECT_EQ(out.event_id, read_seq);
        ++read_seq;
    }

    EXPECT_EQ(read_seq, write_seq);
}

TEST(SpscRing, PreservesAllFields) {
    SpscRing<BinanceMarketEvent, 4> ring;
    BinanceMarketEvent ev{};
    ev.event_id = 0xDEADBEEF;
    ev.price_ticks = -1'000'000'000;
    ev.qty_lots = 999'999;
    ev.ts_event_ms = 1700000000000ULL;
    ev.ts_recv_ns = 0xFFFFFFFFFFFFFFFFULL;
    ev.symbol_id = 42;
    ev.type = hy::EventType::AggTrade;
    ev.side = hy::Side::Sell;
    ev.flags = hy::event_flag::kSnapshot | hy::event_flag::kStale;

    EXPECT_TRUE(ring.try_push(ev));

    BinanceMarketEvent out{};
    EXPECT_TRUE(ring.try_pop(out));

    EXPECT_EQ(out.event_id, 0xDEADBEEFu);
    EXPECT_EQ(out.price_ticks, -1'000'000'000);
    EXPECT_EQ(out.qty_lots, 999'999);
    EXPECT_EQ(out.ts_event_ms, 1700000000000ULL);
    EXPECT_EQ(out.ts_recv_ns, 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_EQ(out.symbol_id, 42u);
    EXPECT_EQ(out.type, hy::EventType::AggTrade);
    EXPECT_EQ(out.side, hy::Side::Sell);
    EXPECT_EQ(out.flags, hy::event_flag::kSnapshot | hy::event_flag::kStale);
}

struct TrivialPod {
    int a;
    int b;
};
static_assert(std::is_trivially_copyable_v<TrivialPod>);

TEST(SpscRing, WorksWithOtherTrivialTypes) {
    SpscRing<TrivialPod, 16> ring;
    TrivialPod item{.a = 10, .b = 20};
    EXPECT_TRUE(ring.try_push(item));
    TrivialPod out{};
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.a, 10);
    EXPECT_EQ(out.b, 20);
}
