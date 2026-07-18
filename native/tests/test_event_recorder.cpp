// P2-CORE-REC-01: EventRecorder unit tests — write .hyf, read back, verify.
#include <gtest/gtest.h>
#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/event_recorder.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

using hy::BinanceMarketEvent;
using hy::EventRecorder;
using hy::EventType;
using hy::Side;

namespace {
std::filesystem::path temp_hyf() {
    return std::filesystem::temp_directory_path() / "hy_test_recorder.hyf";
}
}  // namespace

TEST(EventRecorder, OpenWriteClose) {
    auto path = temp_hyf();
    std::filesystem::remove(path);

    EventRecorder rec;
    ASSERT_TRUE(rec.open(path.string()));
    EXPECT_TRUE(rec.is_open());

    for (std::uint64_t i = 0; i < 10; ++i) {
        BinanceMarketEvent ev{};
        ev.event_id = i;
        ev.price_ticks = 50000 + static_cast<std::int64_t>(i);
        ev.qty_lots = 100;
        ev.symbol_id = static_cast<std::uint32_t>(i % 3);
        ev.type = EventType::Trade;
        rec.record(ev);
    }
    EXPECT_EQ(rec.records_written(), 10u);
    rec.close();
    EXPECT_FALSE(rec.is_open());

    std::filesystem::remove(path);
}

TEST(EventRecorder, HeaderAndRecordsReadBack) {
    auto path = temp_hyf();
    std::filesystem::remove(path);

    {
        EventRecorder rec;
        ASSERT_TRUE(rec.open(path.string()));
        for (std::uint64_t i = 0; i < 5; ++i) {
            BinanceMarketEvent ev{};
            ev.event_id = i * 100;
            ev.price_ticks = 67000'00000000 + static_cast<std::int64_t>(i);
            ev.qty_lots = static_cast<std::int64_t>(i) + 1;
            ev.symbol_id = 7;
            ev.type = EventType::DepthDelta;
            ev.side = Side::Sell;
            rec.record(ev);
        }
        rec.close();
    }

    std::ifstream f(path, std::ios::binary);
    ASSERT_TRUE(f.good());

    char header[64] = {};
    f.read(header, 64);
    EXPECT_EQ(header[0], 'H');
    EXPECT_EQ(header[1], 'Y');
    EXPECT_EQ(header[2], 'F');
    EXPECT_EQ(header[3], '1');

    std::uint32_t rec_size = 0;
    std::memcpy(&rec_size, header + 4, sizeof(rec_size));
    EXPECT_EQ(rec_size, 64u);

    for (std::uint64_t i = 0; i < 5; ++i) {
        BinanceMarketEvent ev{};
        f.read(reinterpret_cast<char*>(&ev), sizeof(ev));
        EXPECT_EQ(ev.event_id, i * 100);
        EXPECT_EQ(ev.price_ticks, 67000'00000000 + static_cast<std::int64_t>(i));
        EXPECT_EQ(ev.qty_lots, static_cast<std::int64_t>(i) + 1);
        EXPECT_EQ(ev.symbol_id, 7u);
        EXPECT_EQ(ev.type, EventType::DepthDelta);
        EXPECT_EQ(ev.side, Side::Sell);
    }

    f.close();
    std::filesystem::remove(path);
}

TEST(EventRecorder, FlushAcrossBufferBoundary) {
    auto path = temp_hyf();
    std::filesystem::remove(path);

    const std::uint64_t n = EventRecorder::kBufferRecords * 2 + 17;
    {
        EventRecorder rec;
        ASSERT_TRUE(rec.open(path.string()));
        for (std::uint64_t i = 0; i < n; ++i) {
            BinanceMarketEvent ev{};
            ev.event_id = i;
            ev.price_ticks = 1;
            rec.record(ev);
        }
        EXPECT_EQ(rec.records_written(), n);
        rec.close();
    }

    auto file_size = std::filesystem::file_size(path);
    EXPECT_EQ(file_size, 64u + n * sizeof(BinanceMarketEvent));

    std::filesystem::remove(path);
}

TEST(EventRecorder, RecordWithoutOpenIsNoOp) {
    EventRecorder rec;
    EXPECT_FALSE(rec.is_open());
    BinanceMarketEvent ev{};
    rec.record(ev);  // must not crash
    EXPECT_EQ(rec.records_written(), 0u);
}

TEST(EventRecorder, OpenInvalidPathFails) {
    EventRecorder rec;
    EXPECT_FALSE(rec.open("/nonexistent_dir_xyz/sub/file.hyf"));
    EXPECT_FALSE(rec.is_open());
}
