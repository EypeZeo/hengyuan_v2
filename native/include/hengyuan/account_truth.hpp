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

// L4 spec §4.2: Binance's GET /api/v3/account response reports every asset's free/locked
// balance as a decimal string at up to 8 fractional digits, regardless of that asset's own
// native precision -- unlike price/qty, which are symbol-specific (SymbolRules::price_scale/
// qty_scale below). AssetBalance::free_ticks/locked_ticks are always at this fixed scale;
// binance_private_rest.hpp's account-fetch parser is the sole producer of AssetBalance values
// and enforces this by construction (rejects, rather than truncates, an input with more than
// 8 fractional digits). See also §5.1.1's rescale_notional_ceil(), which normalizes a
// price*qty product to this same scale before comparing it against a balance.
static constexpr int kBalanceScale = 8;

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

    // SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md §2.1/§2.2/L4 §5.3: the version this
    // particular snapshot was read from a symbol registry at. 0 is reserved for
    // "unknown/unavailable" and never matches a real registry version (L4 §5.1's
    // registry versions start at 1) — a caller with no registry wired up
    // therefore fails closed by construction against live_submit_orchestrator's
    // Gate 1 stale-version check, not by an extra null check anyone has to
    // remember. SymbolRegistry (native/include/hengyuan/symbol_registry.hpp) is
    // the data-model's registry-side integration.
    std::uint32_t rules_version{0};

    // L4 §5.1: decimal places implied by tick_size_ticks/step_size_ticks respectively --
    // i.e. price_ticks/qty_ticks for THIS symbol are expressed at these scales, not at
    // kBalanceScale. Symbol-dependent (unlike AssetBalance, which is always kBalanceScale
    // per §4.2). See rescale_notional_ceil() below for why this matters and
    // symbol_registry.hpp for how a real fetch populates these from exchangeInfo's
    // PRICE_FILTER.tickSize / LOT_SIZE.stepSize.
    std::uint8_t price_scale{0};
    std::uint8_t qty_scale{0};
    // L4 §5.1 (round-6 P0 fix): exchangeInfo's quoteAssetPrecision, populated directly (an
    // integer field Binance already reports, not derived like price_scale/qty_scale above).
    // Explicitly NOT guaranteed to equal kBalanceScale(8) -- conflating the two was
    // "round-5's avg_fill_price_ticks bug" the spec names when scaling cummulativeQuoteQty.
    // Unused by validate_pre_trade() below (that function only needs price_scale/qty_scale);
    // carried here because it's part of the same per-symbol registry snapshot and has
    // nowhere else to live.
    std::uint8_t quote_scale{0};

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

// L4 §5.1.1: 10^exp for exp in [0,18] -- 10^19 overflows int64
// (9,223,372,036,854,775,807 < 10,000,000,000,000,000,000), so exp>=19 fails closed rather
// than computing a wrapped value. A plain lookup table, not a loop: the valid domain is
// small and fixed, and a table makes the exp==19 boundary a simple array-length check
// instead of a runtime multiply-and-compare that would itself need overflow checking.
inline bool pow10_i64(std::uint8_t exp, std::int64_t& out) noexcept {
    static constexpr std::int64_t kPow10[] = {
        1LL,
        10LL,
        100LL,
        1'000LL,
        10'000LL,
        100'000LL,
        1'000'000LL,
        10'000'000LL,
        100'000'000LL,
        1'000'000'000LL,
        10'000'000'000LL,
        100'000'000'000LL,
        1'000'000'000'000LL,
        10'000'000'000'000LL,
        100'000'000'000'000LL,
        1'000'000'000'000'000LL,
        10'000'000'000'000'000LL,
        100'000'000'000'000'000LL,
        1'000'000'000'000'000'000LL,  // 10^18
    };
    if (exp >= sizeof(kPow10) / sizeof(kPow10[0])) return false;
    out = kPow10[exp];
    return true;
}

// L4 §5.1.1 (a real, previously-shipped bug this closes): checked_notional()'s raw
// price*qty product is at (price_scale + qty_scale) decimal places -- a symbol-dependent
// scale -- but every threshold it used to be compared against directly
// (SymbolRules::min_notional_ticks, ExposureLimits::single_order_notional_cap/
// total_exposure_notional_cap/current_exposure_notional, and AssetBalance::free_ticks
// per §4.2) is fixed at kBalanceScale (8). For any symbol where
// price_scale+qty_scale != 8 that was comparing across two different scales. This
// normalizes a value at native_scale to target_scale (always kBalanceScale in this
// file's own callers) via lossless integer arithmetic.
//
// native_scale/target_scale are `int`, not `std::uint8_t`, deliberately: the spec's own
// concrete attack scenario is a caller summing two out-of-range SymbolRules::price_scale/
// qty_scale values (e.g. 200 each, themselves already invalid but not caught before this
// call) -- 200+200 is 400 in `int` (correctly rejected by the [0,18] check below), but
// summing them AS uint8_t first would wrap to 144 before this function ever saw the
// input. Taking `int` parameters removes that narrowing step from existing entirely on
// the caller's side.
//
// Rounds UP (ceiling) when scaling down (target_scale < native_scale): every caller uses
// the result in a "must not exceed" or "must cover" comparison, so rounding up is the
// conservative, fail-closed direction -- it can only make a real notional look larger
// than it is, never smaller, so it can only make this function MORE likely to reject a
// borderline order, never less.
inline bool rescale_notional_ceil(std::int64_t raw_notional_native_scale,
                                   int native_scale, int target_scale,
                                   std::int64_t& out) noexcept {
    if (raw_notional_native_scale < 0) return false;
    if (native_scale < 0 || native_scale > 18) return false;
    if (target_scale < 0 || target_scale > 18) return false;

    if (target_scale >= native_scale) {
        std::int64_t factor = 0;
        if (!pow10_i64(static_cast<std::uint8_t>(target_scale - native_scale), factor)) {
            return false;
        }
        if (raw_notional_native_scale > std::numeric_limits<std::int64_t>::max() / factor) {
            return false;  // would overflow
        }
        out = raw_notional_native_scale * factor;
        return true;
    }

    // Scaling down: ceiling division by 10^(native_scale - target_scale). divisor is
    // always >= 1 (pow10_i64(0) == 1), so no divide-by-zero; the "+ divisor - 1" ceiling
    // trick is valid because both operands are non-negative (checked above).
    std::int64_t divisor = 0;
    if (!pow10_i64(static_cast<std::uint8_t>(native_scale - target_scale), divisor)) {
        return false;
    }
    if (raw_notional_native_scale > std::numeric_limits<std::int64_t>::max() - (divisor - 1)) {
        return false;  // the ceiling adjustment itself would overflow
    }
    out = (raw_notional_native_scale + divisor - 1) / divisor;
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

    // L4 §5.1.1's own documented precondition risk (not defended against with new code
    // here, per the spec's own framing of it as "worth stating explicitly" rather than
    // requiring one): a default-constructed SymbolRules{} has price_scale=qty_scale=0,
    // which rescale_notional_ceil() below will treat as a genuine native_scale=0 and
    // scale UP by 10^8 -- silently inflating notional_native rather than failing. In
    // practice `is_trading` also defaults false on such a struct, so the
    // SymbolNotTrading check above already rejects an unpopulated `rules` before this
    // point is reached; this rescale step still assumes a real registry entry, not an
    // independent defense against one that was never populated.

    // Notional = price * qty, at this symbol's NATIVE scale (price_scale + qty_scale).
    // Overflow → fail-closed: a wrapped product must never pass the caps below.
    std::int64_t notional_native = 0;
    if (!checked_notional(price_ticks, qty_ticks, notional_native)) {
        return PreTradeCheck::NotionalOverflow;
    }

    // L4 §5.1.1: notional_native is symbol-dependent scale; min_notional_ticks/the two
    // ExposureLimits caps/AssetBalance::free_ticks (§4.2) are all fixed at kBalanceScale.
    // Rescale exactly once, here, and use ONLY `notional` (never notional_native again)
    // for every comparison below -- including the Buy-side balance check further down,
    // which also compares against a kBalanceScale-denominated AssetBalance::free_ticks.
    std::int64_t notional = 0;
    if (!rescale_notional_ceil(notional_native,
                               static_cast<int>(rules.price_scale) + static_cast<int>(rules.qty_scale),
                               kBalanceScale, notional)) {
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

    // Balance sufficiency — side-aware. AssetBalance::free_ticks is always at
    // kBalanceScale regardless of asset (§4.2) — Buy already has `notional` rescaled to
    // that above; Sell's qty_ticks is still at this symbol's native qty_scale and needs
    // its own rescale before comparing against the base asset's balance (same scale
    // mismatch as the notional one above, just for a plain quantity instead of a
    // price*qty product — found while implementing §5.1.1's fix, not called out by name
    // in the spec text, but the identical class of bug against the same §4.2 invariant).
    if (side == OrderSide::Buy) {
        const auto* bal = account.find(quote_asset);
        if (!bal || bal->free_ticks < notional) {
            return PreTradeCheck::InsufficientBalance;
        }
    } else {  // Sell: must hold enough base asset to deliver
        std::int64_t qty_at_balance_scale = 0;
        if (!rescale_notional_ceil(qty_ticks, static_cast<int>(rules.qty_scale), kBalanceScale,
                                   qty_at_balance_scale)) {
            return PreTradeCheck::NotionalOverflow;
        }
        const auto* bal = account.find(base_asset);
        if (!bal || bal->free_ticks < qty_at_balance_scale) {
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
