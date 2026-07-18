// SPDX-License-Identifier: proprietary
#include <hengyuan/orderbook.hpp>
#include <algorithm>

namespace hy {

std::size_t OrderBook::find_ask(const std::array<PriceLevel, kMaxLevels>& arr,
                                std::size_t count, std::int64_t price) noexcept {
    std::size_t lo = 0, hi = count;
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (arr[mid].price_ticks < price)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

std::size_t OrderBook::find_bid(const std::array<PriceLevel, kMaxLevels>& arr,
                                std::size_t count, std::int64_t price) noexcept {
    std::size_t lo = 0, hi = count;
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (arr[mid].price_ticks > price)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void OrderBook::insert_at(std::array<PriceLevel, kMaxLevels>& arr,
                           std::size_t& count, std::size_t pos,
                           std::int64_t price, std::int64_t qty) noexcept {
    if (count >= kMaxLevels) return;
    if (pos < count) {
        std::memmove(&arr[pos + 1], &arr[pos],
                     (count - pos) * sizeof(PriceLevel));
    }
    arr[pos] = {price, qty};
    ++count;
}

void OrderBook::remove_at(std::array<PriceLevel, kMaxLevels>& arr,
                           std::size_t& count, std::size_t pos) noexcept {
    if (pos >= count) return;
    if (pos + 1 < count) {
        std::memmove(&arr[pos], &arr[pos + 1],
                     (count - pos - 1) * sizeof(PriceLevel));
    }
    --count;
}

OrderBook::ApplyResult OrderBook::apply_delta(std::int64_t price_ticks,
                                               std::int64_t qty_lots,
                                               Side side) noexcept {
    if (price_ticks <= 0) return ApplyResult::InvalidInput;

    ++update_count_;

    const bool is_bid = (side == Side::Buy);

    if (qty_lots == 0) {
        if (is_bid) {
            std::size_t pos = find_bid(bids_, bid_count_, price_ticks);
            if (pos < bid_count_ && bids_[pos].price_ticks == price_ticks) {
                remove_at(bids_, bid_count_, pos);
            }
        } else {
            std::size_t pos = find_ask(asks_, ask_count_, price_ticks);
            if (pos < ask_count_ && asks_[pos].price_ticks == price_ticks) {
                remove_at(asks_, ask_count_, pos);
            }
        }
        return ApplyResult::Ok;
    }

    if (qty_lots < 0) return ApplyResult::InvalidInput;

    if (!is_bid) {
        std::size_t pos = find_ask(asks_, ask_count_, price_ticks);
        if (pos < ask_count_ && asks_[pos].price_ticks == price_ticks) {
            asks_[pos].qty_lots = qty_lots;
        } else {
            if (ask_count_ >= kMaxLevels) return ApplyResult::LevelsFull;
            insert_at(asks_, ask_count_, pos, price_ticks, qty_lots);
        }
    } else {
        std::size_t pos = find_bid(bids_, bid_count_, price_ticks);
        if (pos < bid_count_ && bids_[pos].price_ticks == price_ticks) {
            bids_[pos].qty_lots = qty_lots;
        } else {
            if (bid_count_ >= kMaxLevels) return ApplyResult::LevelsFull;
            insert_at(bids_, bid_count_, pos, price_ticks, qty_lots);
        }
    }

    return ApplyResult::Ok;
}

void OrderBook::apply_snapshot(const PriceLevel* asks, std::size_t a_count,
                                const PriceLevel* bids, std::size_t b_count) noexcept {
    clear();
    std::size_t ac = std::min(a_count, kMaxLevels);
    std::size_t bc = std::min(b_count, kMaxLevels);
    std::memcpy(asks_.data(), asks, ac * sizeof(PriceLevel));
    std::memcpy(bids_.data(), bids, bc * sizeof(PriceLevel));
    ask_count_ = ac;
    bid_count_ = bc;
}

void OrderBook::clear() noexcept {
    ask_count_ = 0;
    bid_count_ = 0;
    update_count_ = 0;
}

std::optional<std::pair<std::int64_t, std::int64_t>> OrderBook::top_of_book() const noexcept {
    if (bid_count_ == 0 || ask_count_ == 0) return std::nullopt;
    return std::pair{bids_[0].price_ticks, asks_[0].price_ticks};
}

}  // namespace hy
