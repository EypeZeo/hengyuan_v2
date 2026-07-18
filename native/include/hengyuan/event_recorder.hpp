// SPDX-License-Identifier: proprietary
// event_recorder.hpp — append-only binary recorder for BinanceMarketEvent.
// Writes a 64-byte header + raw fixed-size 64-byte POD records (.hyf format).
// Python slow path (tools/perf/hyf_to_parquet.py) converts to Parquet/DuckDB.
// Governance: L1, no network/token/order. Records public market data only.
//
// .hyf file layout:
//   [0..3]   magic "HYF1"
//   [4..7]   uint32 record_size (== sizeof(BinanceMarketEvent) == 64, LE)
//   [8..63]  reserved (zero)
//   [64..]   N x 64-byte BinanceMarketEvent records (LE)

#pragma once

#include <hengyuan/binance_market_event.hpp>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace hy {

class EventRecorder {
public:
    static constexpr std::size_t kBufferRecords = 1024;  // 64 KB buffer, no heap

    EventRecorder() = default;
    ~EventRecorder() { close(); }
    EventRecorder(const EventRecorder&) = delete;
    EventRecorder& operator=(const EventRecorder&) = delete;
    EventRecorder(EventRecorder&&) = delete;
    EventRecorder& operator=(EventRecorder&&) = delete;

    bool open(const std::string& path) noexcept {
        file_ = std::fopen(path.c_str(), "wb");
        if (file_ == nullptr) {
            return false;
        }
        std::array<unsigned char, 64> header{};
        header[0] = 'H';
        header[1] = 'Y';
        header[2] = 'F';
        header[3] = '1';
        const auto rec = static_cast<std::uint32_t>(sizeof(BinanceMarketEvent));
        std::memcpy(header.data() + 4, &rec, sizeof(rec));
        std::fwrite(header.data(), 1, header.size(), file_);
        buf_pos_ = 0;
        count_ = 0;
        return true;
    }

    void record(const BinanceMarketEvent& ev) noexcept {
        if (file_ == nullptr) {
            return;
        }
        buffer_[buf_pos_++] = ev;
        ++count_;
        if (buf_pos_ >= kBufferRecords) {
            flush();
        }
    }

    void flush() noexcept {
        if (file_ == nullptr || buf_pos_ == 0) {
            return;
        }
        std::fwrite(buffer_.data(), sizeof(BinanceMarketEvent), buf_pos_, file_);
        buf_pos_ = 0;
    }

    void close() noexcept {
        if (file_ != nullptr) {
            flush();
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    bool is_open() const noexcept { return file_ != nullptr; }
    std::uint64_t records_written() const noexcept { return count_; }

private:
    std::FILE* file_{nullptr};
    std::array<BinanceMarketEvent, kBufferRecords> buffer_{};
    std::size_t buf_pos_{0};
    std::uint64_t count_{0};
};

}  // namespace hy
