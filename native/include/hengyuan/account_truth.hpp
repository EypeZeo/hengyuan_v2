// SPDX-License-Identifier: proprietary
// account_truth.hpp — Binance account truth data model + freshness + pre-trade validation.
//
// Governance: L1 (data structures + validation logic, no network, no secret).
// Network fetch is in binance_private_rest.hpp (L4). This file is pure logic.
//
// ADR-019 D6 + D7: account truth + order legality / pre-trade risk input.
//   ✅ Balance / available / locked per asset
//   ✅ Freshness window (stale → fail-closed)
//   ✅ Symbol trading status check
//   ✅ Price / quantity step size validation (tickSize, stepSize)
//   ✅ Min notional check
//   ✅ Single-order notional cap
//   ✅ Total exposure cap
//   ✅ Single frozen order type for first path

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace hy {

// --- Account balance snapshot ---

static constexpr std::size_t kMaxAssets = 32;
static constexpr std::size_t kAssetNameLen = 12;

struct AssetBalance {
    char asset[kAssetNameLen]{};
    std::int64_t free_ticks{0};
    std::int64_t locked_ticks{0};

    std::int64_t total() const noexcept { return free_ticks + locked_ticks; }

    std::string_view asset_name() const noexcept {
        return {asset, std::strlen(asset)};
    }
};

struct AccountSnapshot {
    std::int64_t timestamp_ms{0};
    std::size_t asset_count{0};
    std::array<AssetBalance, kMaxAssets> assets{};
    bool can_trade{false};

    const AssetBalance* find(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < asset_count; ++i) {
            if (assets[i].asset_name() == name) return &assets[i];
        }
        return nullptr;
    }
};

// --- Freshness gate ---

enum class FreshnessStatus : std::uint8_t {
    Fresh = 0,
    Stale = 1,
    NeverFetched = 2,
};

inline FreshnessStatus check_freshness(
    std::int64_t snapshot_ts_ms,
    std::int64_t now_ms,
    std::int64_t max_age_ms) noexcept {

    if (snapshot_ts_ms <= 0) return FreshnessStatus::NeverFetched;
    if (now_ms - snapshot_ts_ms > max_age_ms) return FreshnessStatus::Stale;
    return FreshnessStatus::Fresh;
}

// --- Symbol trading rules (from exchangeInfo) ---

static constexpr std::size_t kSymbolNameLen = 20;

struct SymbolRules {
    char symbol[kSymbolNameLen]{};
    bool is_trading{false};

    // LOT_SIZE filter
    std::int64_t min_qty_ticks{0};
    std::int64_t max_qty_ticks{0};
    std::int64_t step_size_ticks{0};

    // PRICE_FILTER
    std::int64_t min_price_ticks{0};
    std::int64_t max_price_ticks{0};
    std::int64_t tick_size_ticks{0};

    // MIN_NOTIONAL / NOTIONAL filter
    std::int64_t min_notional_ticks{0};

    std::string_view symbol_name() const noexcept {
        return {symbol, std::strlen(symbol)};
    }
};

// --- Order side / type (ADR-019 D7 M7; first path frozen to LIMIT per spec 6.8) ---

enum class OrderSide : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

// First live path supports exactly one frozen order type (spec 6.8 / ADR-019 D7 M7).
// The enum has a single member on purpose — adding another requires an ADR change.
enum class OrderType : std::uint8_t {
    Limit = 0,
};

inline const char* order_side_name(OrderSide s) noexcept {
    return s == OrderSide::Buy ? "BUY" : "SELL";
}

// --- Pre-trade validation (ADR-019 D7) ---

enum class PreTradeCheck : std::uint8_t {
    Ok = 0,
    SymbolNotTrading = 1,
    PriceBelowMin = 2,
    PriceAboveMax = 3,
    PriceStepViolation = 4,
    QtyBelowMin = 5,
    QtyAboveMax = 6,
    QtyStepViolation = 7,
    BelowMinNotional = 8,
    ExceedsSingleOrderCap = 9,
    ExceedsTotalExposureCap = 10,
    InsufficientBalance = 11,
    BalanceStale = 12,
    AccountCannotTrade = 13,
    InvalidInput = 14,      // negative/zero price or qty
    NotionalOverflow = 15,  // price*qty or exposure sum would overflow int64 (fail-closed)
};

// Overflow-safe multiply for notional = price_ticks * qty_ticks.
// Both operands are expected non-negative (validated by caller). Returns false
// on negative input or when the product would exceed INT64_MAX — caller must
// fail-closed. Portable (no __builtin_mul_overflow: MSVC lacks it).
inline bool checked_notional(std::int64_t price_ticks,
                             std::int64_t qty_ticks,
                             std::int64_t& out) noexcept {
    if (price_ticks < 0 || qty_ticks < 0) return false;
    if (price_ticks != 0 &&
        qty_ticks > (std::numeric_limits<std::int64_t>::max() / price_ticks)) {
        return false;  // would overflow
    }
    out = price_ticks * qty_ticks;
    return true;
}

// Overflow-safe addition for exposure accumulation.
inline bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (a < 0 || b < 0) return false;
    if (a > std::numeric_limits<std::int64_t>::max() - b) return false;
    out = a + b;
    return true;
}

struct ExposureLimits {
    std::int64_t single_order_notional_cap{0};
    std::int64_t total_exposure_notional_cap{0};
    std::int64_t current_exposure_notional{0};
    std::int64_t freshness_max_age_ms{30000};  // 30 seconds default
};

// Side-aware pre-trade validation (ADR-019 D6 + D7).
//   Buy  → settles in quote asset: require quote.free >= notional.
//   Sell → delivers base asset:    require base.free  >= qty_ticks.
inline PreTradeCheck validate_pre_trade(
    const SymbolRules& rules,
    OrderSide side,
    std::int64_t price_ticks,
    std::int64_t qty_ticks,
    const AccountSnapshot& account,
    std::string_view base_asset,
    std::string_view quote_asset,
    std::int64_t now_ms,
    const ExposureLimits& limits) noexcept {

    // Reject non-positive price/qty up front (defends the overflow math below
    // and prevents a zero/negative order from silently passing later checks).
    if (price_ticks <= 0 || qty_ticks <= 0) return PreTradeCheck::InvalidInput;

    // Account must be trade-enabled
    if (!account.can_trade) return PreTradeCheck::AccountCannotTrade;

    // Freshness check
    auto freshness = check_freshness(account.timestamp_ms, now_ms, limits.freshness_max_age_ms);
    if (freshness != FreshnessStatus::Fresh) return PreTradeCheck::BalanceStale;

    // Symbol must be TRADING
    if (!rules.is_trading) return PreTradeCheck::SymbolNotTrading;

    // Price filter
    if (rules.tick_size_ticks > 0) {
        if (price_ticks < rules.min_price_ticks) return PreTradeCheck::PriceBelowMin;
        if (rules.max_price_ticks > 0 && price_ticks > rules.max_price_ticks) {
            return PreTradeCheck::PriceAboveMax;
        }
        if ((price_ticks - rules.min_price_ticks) % rules.tick_size_ticks != 0) {
            return PreTradeCheck::PriceStepViolation;
        }
    }

    // LOT_SIZE filter
    if (rules.step_size_ticks > 0) {
        if (qty_ticks < rules.min_qty_ticks) return PreTradeCheck::QtyBelowMin;
        if (rules.max_qty_ticks > 0 && qty_ticks > rules.max_qty_ticks) {
            return PreTradeCheck::QtyAboveMax;
        }
        if ((qty_ticks - rules.min_qty_ticks) % rules.step_size_ticks != 0) {
            return PreTradeCheck::QtyStepViolation;
        }
    }

    // Notional = price * qty (in ticks*ticks, caller must ensure same scale).
    // Overflow → fail-closed: a wrapped product must never pass the caps below.
    std::int64_t notional = 0;
    if (!checked_notional(price_ticks, qty_ticks, notional)) {
        return PreTradeCheck::NotionalOverflow;
    }

    // MIN_NOTIONAL
    if (rules.min_notional_ticks > 0 && notional < rules.min_notional_ticks) {
        return PreTradeCheck::BelowMinNotional;
    }

    // Single order cap
    if (limits.single_order_notional_cap > 0 && notional > limits.single_order_notional_cap) {
        return PreTradeCheck::ExceedsSingleOrderCap;
    }

    // Total exposure cap (overflow in the sum also fails closed)
    if (limits.total_exposure_notional_cap > 0) {
        std::int64_t projected = 0;
        if (!checked_add(limits.current_exposure_notional, notional, projected)) {
            return PreTradeCheck::NotionalOverflow;
        }
        if (projected > limits.total_exposure_notional_cap) {
            return PreTradeCheck::ExceedsTotalExposureCap;
        }
    }

    // Balance sufficiency — side-aware.
    if (side == OrderSide::Buy) {
        const auto* bal = account.find(quote_asset);
        if (!bal || bal->free_ticks < notional) {
            return PreTradeCheck::InsufficientBalance;
        }
    } else {  // Sell: must hold enough base asset to deliver
        const auto* bal = account.find(base_asset);
        if (!bal || bal->free_ticks < qty_ticks) {
            return PreTradeCheck::InsufficientBalance;
        }
    }

    return PreTradeCheck::Ok;
}

// Backward-compatible buy-side overload (quote-settled). Preserves the original
// 7-arg call shape used across existing call sites and tests: it is exactly a
// BUY with no base-asset check needed.
inline PreTradeCheck validate_pre_trade(
    const SymbolRules& rules,
    std::int64_t price_ticks,
    std::int64_t qty_ticks,
    const AccountSnapshot& account,
    std::string_view quote_asset,
    std::int64_t now_ms,
    const ExposureLimits& limits) noexcept {
    return validate_pre_trade(rules, OrderSide::Buy, price_ticks, qty_ticks,
                              account, /*base_asset=*/std::string_view{},
                              quote_asset, now_ms, limits);
}

}  // namespace hy
