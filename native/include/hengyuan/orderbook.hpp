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

// Per-side capacity of the maintained book. This is a bounded TOP-N VIEW, not an
// attempt to hold the exchange's full book: apply_snapshot() keeps the best
// kMaxLevels of whatever it is given, and apply_delta() maintains that window by
// evicting the worst level when a better one arrives.
//
// It is deliberately SMALLER than DepthSnapshot's 1024-per-side capacity
// (depth_manager.hpp) and than RestSnapshotConfig::limit's 1000 default
// (binance_rest_snapshot.hpp) -- see the static_assert in depth_manager.hpp that
// pins that relationship, and the audit note on insert_at() in orderbook.cpp for
// what went wrong when the window semantics were "reject when full" instead.
static constexpr std::size_t kMaxLevels = 256;

class OrderBook {
public:
    enum class ApplyResult : std::uint8_t {
        Ok = 0,
        // The incoming level is worse than every level currently held, so it falls
        // outside the maintained top-N window and was deliberately not stored. The
        // book is unchanged and CORRECT -- this is the normal, expected outcome for
        // deep levels, NOT an error and NOT a reason to resync. (Formerly named
        // LevelsFull, back when a full book rejected every insert including ones
        // that belonged at the top; see insert_at() in orderbook.cpp.)
        OutsideWindow = 1,
        // price_ticks <= 0 or qty_lots < 0 -- malformed input, book unchanged.
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

    // Returns false iff the level falls outside the maintained window (see
    // ApplyResult::OutsideWindow); true means it was stored, evicting the worst
    // held level first when the side was already at kMaxLevels.
    static bool insert_at(std::array<PriceLevel, kMaxLevels>& arr,
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
