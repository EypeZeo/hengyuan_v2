// P2-EXEC-LIVE-01 D12-4: account truth + pre-trade validation tests.
// All synthetic data, no network, no real balances.
#include <gtest/gtest.h>
#include <hengyuan/account_truth.hpp>

#include <cstring>

using hy::AccountSnapshot;
using hy::AssetBalance;
using hy::ExposureLimits;
using hy::FreshnessStatus;
using hy::PreTradeCheck;
using hy::SymbolRules;
using hy::check_freshness;
using hy::validate_pre_trade;

static AssetBalance make_balance(const char* name, std::int64_t free, std::int64_t locked) {
    AssetBalance b{};
    std::strncpy(b.asset, name, sizeof(b.asset) - 1);
    b.free_ticks = free;
    b.locked_ticks = locked;
    return b;
}

static AccountSnapshot make_account(std::int64_t ts_ms, bool can_trade) {
    AccountSnapshot a{};
    a.timestamp_ms = ts_ms;
    a.can_trade = can_trade;
    a.assets[0] = make_balance("USDT", 100000, 0);
    a.assets[1] = make_balance("BTC", 5000, 500);
    a.asset_count = 2;
    return a;
}

static SymbolRules make_btcusdt_rules() {
    SymbolRules r{};
    std::strncpy(r.symbol, "BTCUSDT", sizeof(r.symbol) - 1);
    r.is_trading = true;
    r.min_price_ticks = 100;
    r.max_price_ticks = 10000000;
    r.tick_size_ticks = 100;
    r.min_qty_ticks = 10;
    r.max_qty_ticks = 1000000;
    r.step_size_ticks = 10;
    r.min_notional_ticks = 1000;
    return r;
}

static ExposureLimits make_limits() {
    ExposureLimits l{};
    l.single_order_notional_cap = 50000;
    l.total_exposure_notional_cap = 200000;
    l.current_exposure_notional = 0;
    l.freshness_max_age_ms = 30000;
    return l;
}

// --- Freshness ---

TEST(Freshness, FreshWithinWindow) {
    EXPECT_EQ(check_freshness(1000000, 1020000, 30000), FreshnessStatus::Fresh);
}

TEST(Freshness, StaleAfterWindow) {
    EXPECT_EQ(check_freshness(1000000, 1040000, 30000), FreshnessStatus::Stale);
}

TEST(Freshness, NeverFetchedOnZeroTimestamp) {
    EXPECT_EQ(check_freshness(0, 1000000, 30000), FreshnessStatus::NeverFetched);
}

TEST(Freshness, ExactBoundaryIsFresh) {
    EXPECT_EQ(check_freshness(1000000, 1030000, 30000), FreshnessStatus::Fresh);
}

// --- AccountSnapshot ---

TEST(AccountSnapshot, FindExistingAsset) {
    auto a = make_account(1000, true);
    auto* b = a.find("USDT");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->free_ticks, 100000);
}

TEST(AccountSnapshot, FindMissingAssetReturnsNull) {
    auto a = make_account(1000, true);
    EXPECT_EQ(a.find("ETH"), nullptr);
}

TEST(AccountSnapshot, TotalIsFreePlusLocked) {
    auto b = make_balance("BTC", 5000, 500);
    EXPECT_EQ(b.total(), 5500);
}

// --- Pre-trade validation ---

TEST(PreTrade, ValidOrderPasses) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::Ok);
}

TEST(PreTrade, AccountCannotTrade) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, false);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::AccountCannotTrade);
}

TEST(PreTrade, BalanceStaleRejects) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 50000, limits);
    EXPECT_EQ(r, PreTradeCheck::BalanceStale);
}

TEST(PreTrade, BalanceNeverFetchedRejects) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(0, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 1000, limits);
    EXPECT_EQ(r, PreTradeCheck::BalanceStale);
}

TEST(PreTrade, SymbolNotTrading) {
    auto rules = make_btcusdt_rules();
    rules.is_trading = false;
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::SymbolNotTrading);
}

TEST(PreTrade, PriceBelowMin) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 50, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::PriceBelowMin);
}

TEST(PreTrade, PriceAboveMax) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 20000000, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::PriceAboveMax);
}

TEST(PreTrade, PriceStepViolation) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 150, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::PriceStepViolation);
}

TEST(PreTrade, QtyBelowMin) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, 5, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::QtyBelowMin);
}

TEST(PreTrade, QtyAboveMax) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, 2000000, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::QtyAboveMax);
}

TEST(PreTrade, QtyStepViolation) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, 15, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::QtyStepViolation);
}

TEST(PreTrade, BelowMinNotional) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    // price=100 * qty=10 = 1000 → exactly at min, should pass
    auto r1 = validate_pre_trade(rules, 100, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r1, PreTradeCheck::Ok);
    // price=100 * qty=9 → not valid qty step. Let's use qty=10, price slightly lower
    // Actually min_notional=1000, price=100*qty=10=1000 passes. But just at boundary.
    // Let's test below: we need notional < 1000. But smallest valid price=100, qty=10, notional=1000.
    // Set min_notional higher to test.
    rules.min_notional_ticks = 2000;
    auto r2 = validate_pre_trade(rules, 100, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r2, PreTradeCheck::BelowMinNotional);
}

TEST(PreTrade, ExceedsSingleOrderCap) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    account.assets[0].free_ticks = 999999999;  // enough balance
    auto limits = make_limits();
    limits.single_order_notional_cap = 5000;
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 1010, limits);
    // notional = 1000 * 10 = 10000 > 5000
    EXPECT_EQ(r, PreTradeCheck::ExceedsSingleOrderCap);
}

TEST(PreTrade, ExceedsTotalExposureCap) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    account.assets[0].free_ticks = 999999999;
    auto limits = make_limits();
    limits.total_exposure_notional_cap = 15000;
    limits.current_exposure_notional = 10000;
    // notional = 1000 * 10 = 10000; total = 10000 + 10000 = 20000 > 15000
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::ExceedsTotalExposureCap);
}

TEST(PreTrade, InsufficientBalance) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    account.assets[0].free_ticks = 5000;  // only 5000 USDT free
    auto limits = make_limits();
    // notional = 1000 * 10 = 10000 > 5000 free
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::InsufficientBalance);
}

TEST(PreTrade, MissingQuoteAssetRejects) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, 10, account, "EUR", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::InsufficientBalance);
}

TEST(PreTrade, ZeroExposureCapsAreIgnored) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    limits.single_order_notional_cap = 0;  // disabled
    limits.total_exposure_notional_cap = 0;
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::Ok);
}

// --- F2 regression: notional overflow / invalid input must fail-closed ---

TEST(PreTrade, ZeroPriceRejected) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 0, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::InvalidInput);
}

TEST(PreTrade, NegativeQtyRejected) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, 1000, -10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::InvalidInput);
}

// The core F2 case: a price*qty that overflows int64 must NOT wrap into a small
// value that passes the notional/exposure caps — it must fail closed.
TEST(PreTrade, NotionalOverflowFailsClosed) {
    auto rules = make_btcusdt_rules();
    // Widen symbol/step limits so we reach the notional math with huge inputs.
    rules.max_price_ticks = 0;   // 0 disables the max-price ceiling
    rules.max_qty_ticks = 0;     // 0 disables the max-qty ceiling
    rules.tick_size_ticks = 0;   // skip price-step math for this overflow probe
    rules.step_size_ticks = 0;   // skip qty-step math
    rules.min_notional_ticks = 0;
    auto account = make_account(1000, true);
    auto limits = make_limits();
    limits.single_order_notional_cap = 0;      // disabled — prove overflow is caught first
    limits.total_exposure_notional_cap = 0;

    const std::int64_t big = std::int64_t{1} << 40;  // 2^40 * 2^40 = 2^80 >> INT64_MAX
    auto r = validate_pre_trade(rules, big, big, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::NotionalOverflow);
}

TEST(PreTrade, ExposureSumOverflowFailsClosed) {
    auto rules = make_btcusdt_rules();
    rules.max_price_ticks = 0;
    rules.max_qty_ticks = 0;
    rules.tick_size_ticks = 0;
    rules.step_size_ticks = 0;
    rules.min_notional_ticks = 0;
    auto account = make_account(1000, true);
    auto limits = make_limits();
    limits.single_order_notional_cap = 0;
    // A valid small order notional, but current exposure near INT64_MAX so the
    // projected sum overflows — must fail closed, not wrap.
    limits.total_exposure_notional_cap = std::numeric_limits<std::int64_t>::max();
    limits.current_exposure_notional = std::numeric_limits<std::int64_t>::max() - 5;
    auto r = validate_pre_trade(rules, 1000, 10, account, "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::NotionalOverflow);
}

// Direct unit tests on the overflow-safe primitives.
TEST(CheckedMath, NotionalOkOnNormalValues) {
    std::int64_t out = 0;
    EXPECT_TRUE(hy::checked_notional(1000, 10, out));
    EXPECT_EQ(out, 10000);
}

TEST(CheckedMath, NotionalDetectsOverflow) {
    std::int64_t out = -1;
    const std::int64_t big = std::int64_t{1} << 40;
    EXPECT_FALSE(hy::checked_notional(big, big, out));
}

TEST(CheckedMath, NotionalRejectsNegative) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::checked_notional(-1, 10, out));
}

TEST(CheckedMath, AddDetectsOverflow) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::checked_add(std::numeric_limits<std::int64_t>::max(), 1, out));
    EXPECT_TRUE(hy::checked_add(100, 200, out));
    EXPECT_EQ(out, 300);
}

// --- F1 regression: side-aware balance (buy→quote, sell→base) ---

TEST(PreTrade, SellChecksBaseBalancePasses) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);  // USDT 100000, BTC 5000/500
    auto limits = make_limits();
    // Sell qty=10 base ticks; BTC free=5000 >= 10 → Ok
    auto r = validate_pre_trade(rules, hy::OrderSide::Sell, 1000, 10, account,
                                "BTC", "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::Ok);
}

TEST(PreTrade, SellInsufficientBaseBalance) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    account.assets[1].free_ticks = 5;  // BTC free only 5 < qty 10
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, hy::OrderSide::Sell, 1000, 10, account,
                                "BTC", "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::InsufficientBalance);
}

TEST(PreTrade, SellIgnoresQuoteBalance) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    account.assets[0].free_ticks = 0;  // no USDT at all — irrelevant for a sell
    auto limits = make_limits();
    auto r = validate_pre_trade(rules, hy::OrderSide::Sell, 1000, 10, account,
                                "BTC", "USDT", 1010, limits);
    EXPECT_EQ(r, PreTradeCheck::Ok);
}

TEST(PreTrade, BuyOverloadEquivalentToExplicitBuy) {
    auto rules = make_btcusdt_rules();
    auto account = make_account(1000, true);
    auto limits = make_limits();
    auto r_old = validate_pre_trade(rules, 1000, 10, account, "USDT", 1010, limits);
    auto r_new = validate_pre_trade(rules, hy::OrderSide::Buy, 1000, 10, account,
                                    "BTC", "USDT", 1010, limits);
    EXPECT_EQ(r_old, r_new);
    EXPECT_EQ(r_new, PreTradeCheck::Ok);
}
