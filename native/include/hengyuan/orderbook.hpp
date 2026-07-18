// SPDX-License-Identifier: proprietary
// orderbook.hpp — P2-CORE-OB-01: int64 fixed-point OrderBook.
// Pre-allocated flat sorted arrays, zero heap allocation, cache-friendly.
// Governance: L1, no network/token/order.

#pragma once

#include <hengyuan/binance_market_event.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

namespace hy {

struct PriceLevel {
    std::int64_t price_ticks{0};
    std::int64_t qty_lots{0};
};

static constexpr std::size_t kMaxLevels = 256;

class OrderBook {
public:
    enum class ApplyResult : std::uint8_t {
        Ok = 0,
        LevelsFull = 1,
        InvalidInput = 2,
    };

    OrderBook() = default;

    ApplyResult apply_delta(std::int64_t price_ticks, std::int64_t qty_lots, Side side) noexcept;

    void apply_snapshot(const PriceLevel* asks, std::size_t ask_count,
                        const PriceLevel* bids, std::size_t bid_count) noexcept;

    void clear() noexcept;

    std::optional<std::pair<std::int64_t, std::int64_t>> top_of_book() const noexcept;

    const PriceLevel* asks() const noexcept { return asks_.data(); }
    std::size_t ask_count() const noexcept { return ask_count_; }

    const PriceLevel* bids() const noexcept { return bids_.data(); }
    std::size_t bid_count() const noexcept { return bid_count_; }

    std::uint64_t update_count() const noexcept { return update_count_; }

private:
    static std::size_t find_ask(const std::array<PriceLevel, kMaxLevels>& arr,
                                std::size_t count, std::int64_t price) noexcept;
    static std::size_t find_bid(const std::array<PriceLevel, kMaxLevels>& arr,
                                std::size_t count, std::int64_t price) noexcept;

    static void insert_at(std::array<PriceLevel, kMaxLevels>& arr,
                          std::size_t& count, std::size_t pos,
                          std::int64_t price, std::int64_t qty) noexcept;
    static void remove_at(std::array<PriceLevel, kMaxLevels>& arr,
                          std::size_t& count, std::size_t pos) noexcept;

    std::array<PriceLevel, kMaxLevels> asks_{};
    std::array<PriceLevel, kMaxLevels> bids_{};
    std::size_t ask_count_{0};
    std::size_t bid_count_{0};
    std::uint64_t update_count_{0};
};

}  // namespace hy
