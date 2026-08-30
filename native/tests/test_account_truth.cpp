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
    // AUDIT L4-SYMBOLRULES-SCALE (§5.1.1): price_scale=0, qty_scale=8 makes BOTH
    // rescale_notional_ceil() calls inside validate_pre_trade() an identity transform
    // for every test in this file that predates scale-awareness (notional's native scale
    // = price_scale+qty_scale = 8 = kBalanceScale already; the Sell-side qty rescale's
    // native scale = qty_scale = 8 = kBalanceScale already) -- chosen deliberately so none
    // of this file's existing numeric expectations needed to change when the rescale was
    // introduced, not because 0/8 is a realistic BTCUSDT precision pair.
    r.price_scale = 0;
    r.qty_scale = 8;
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

// --- L4 §5.1.1: pow10_i64 / rescale_notional_ceil ---

TEST(Pow10I64, ValidExponentsMatchExpectedPowersOfTen) {
    std::int64_t out = 0;
    ASSERT_TRUE(hy::pow10_i64(0, out));
    EXPECT_EQ(out, 1);
    ASSERT_TRUE(hy::pow10_i64(4, out));
    EXPECT_EQ(out, 10000);
    ASSERT_TRUE(hy::pow10_i64(18, out));
    EXPECT_EQ(out, 1'000'000'000'000'000'000LL);
}

TEST(Pow10I64, Exponent19FailsClosedOnOverflow) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::pow10_i64(19, out));
    EXPECT_FALSE(hy::pow10_i64(255, out));
}

TEST(RescaleNotionalCeil, IdentityWhenScalesEqual) {
    std::int64_t out = 0;
    ASSERT_TRUE(hy::rescale_notional_ceil(12345, 8, 8, out));
    EXPECT_EQ(out, 12345);
}

TEST(RescaleNotionalCeil, ScalesUpByExactPowerOfTen) {
    std::int64_t out = 0;
    ASSERT_TRUE(hy::rescale_notional_ceil(100, 4, 8, out));  // *10^4
    EXPECT_EQ(out, 1'000'000);
}

TEST(RescaleNotionalCeil, ScalesDownWithCeilingNotTruncation) {
    // raw=100 at scale 2 -> scale 0 is dividing by 10^2=100. 100/100 = 1 exactly (no
    // rounding needed) -- pick a value that actually exercises the ceiling.
    std::int64_t out = 0;
    ASSERT_TRUE(hy::rescale_notional_ceil(150, 2, 0, out));
    // 150/100 = 1.5 -> ceiling is 2, NOT a truncating 1. This is the specific behavior
    // the fail-closed "must not exceed" direction depends on.
    EXPECT_EQ(out, 2);

    ASSERT_TRUE(hy::rescale_notional_ceil(100, 2, 0, out));
    EXPECT_EQ(out, 1);  // exact division still gives the exact (non-inflated) answer
}

TEST(RescaleNotionalCeil, RejectsOutOfRangeScales) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::rescale_notional_ceil(100, -1, 8, out));
    EXPECT_FALSE(hy::rescale_notional_ceil(100, 19, 8, out));
    EXPECT_FALSE(hy::rescale_notional_ceil(100, 8, -1, out));
    EXPECT_FALSE(hy::rescale_notional_ceil(100, 8, 19, out));
    // The spec's own concrete attack: two out-of-range uint8_t scales (200 each) summed
    // as `int` -- correctly rejected here (400 > 18), not silently accepted after
    // wrapping to 144 the way summing them as uint8_t first would have.
    EXPECT_FALSE(hy::rescale_notional_ceil(100, 200 + 200, 8, out));
}

TEST(RescaleNotionalCeil, RejectsNegativeInput) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::rescale_notional_ceil(-1, 4, 8, out));
}

TEST(RescaleNotionalCeil, ScaleUpOverflowFailsClosed) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::rescale_notional_ceil(std::numeric_limits<std::int64_t>::max(), 0, 8, out));
}

// --- L4 §6.1.1: checked_scaled_mul_div ---

TEST(CheckedScaledMulDiv, ZeroExponentIsPlainDivision) {
    std::int64_t out = 0;
    ASSERT_TRUE(hy::checked_scaled_mul_div(100, 0, 4, out));
    EXPECT_EQ(out, 25);
}

TEST(CheckedScaledMulDiv, PositiveExponentScalesUpBeforeDividing) {
    std::int64_t out = 0;
    ASSERT_TRUE(hy::checked_scaled_mul_div(5, 3, 2, out));  // (5 * 1000) / 2
    EXPECT_EQ(out, 2500);
}

TEST(CheckedScaledMulDiv, NegativeExponentScalesDivisorUp) {
    std::int64_t out = 0;
    ASSERT_TRUE(hy::checked_scaled_mul_div(500, -2, 1, out));  // 500 / (1 * 100)
    EXPECT_EQ(out, 5);
}

TEST(CheckedScaledMulDiv, NegativeExponentDenomExceedsValueYieldsZero) {
    std::int64_t out = -1;
    ASSERT_TRUE(hy::checked_scaled_mul_div(5, -2, 1, out));  // denom=100 > value=5
    EXPECT_EQ(out, 0);
}

TEST(CheckedScaledMulDiv, ZeroValueShortCircuitsToZero) {
    std::int64_t out = -1;
    ASSERT_TRUE(hy::checked_scaled_mul_div(0, 5, 3, out));
    EXPECT_EQ(out, 0);
}

TEST(CheckedScaledMulDiv, ExponentBoundaryEighteenAccepted) {
    std::int64_t out = 0;
    ASSERT_TRUE(hy::checked_scaled_mul_div(1, 18, 1, out));
    EXPECT_EQ(out, 1'000'000'000'000'000'000LL);
    // exponent=-18 against divisor=1: denominator 10^18 vastly exceeds value=1 -> 0, not a
    // rejection -- exercises the negative-boundary path through the same accepted range.
    ASSERT_TRUE(hy::checked_scaled_mul_div(1, -18, 1, out));
    EXPECT_EQ(out, 0);
}

TEST(CheckedScaledMulDiv, ExponentBeyondEighteenRejectedBothDirections) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::checked_scaled_mul_div(1, 19, 1, out));
    EXPECT_FALSE(hy::checked_scaled_mul_div(1, -19, 1, out));
}

TEST(CheckedScaledMulDiv, NonPositiveDivisorRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::checked_scaled_mul_div(100, 0, 0, out));
    EXPECT_FALSE(hy::checked_scaled_mul_div(100, 0, -1, out));
}

TEST(CheckedScaledMulDiv, NegativeValueRejected) {
    std::int64_t out = 0;
    EXPECT_FALSE(hy::checked_scaled_mul_div(-1, 0, 1, out));
}

TEST(CheckedScaledMulDiv, PositiveExponentQuotientOverflowFailsClosed) {
    std::int64_t out = 0;
    // (INT64_MAX * 100) / 1 vastly exceeds INT64_MAX -- the widened multiply itself never
    // overflows (128-bit intermediate), but the quotient narrowing back to int64_t must reject.
    EXPECT_FALSE(hy::checked_scaled_mul_div(std::numeric_limits<std::int64_t>::max(), 2, 1, out));
}

TEST(CheckedScaledMulDiv, NegativeExponentHugeDivisorWidensPastValueYieldsZeroNotFailure) {
    std::int64_t out = -1;
    // divisor * 10^1 overflows 64 bits (high word nonzero on the MSVC path / denom > value on
    // the __int128 path) -- this is a defined, correct "quotient underflows to 0" result, not
    // a rejection: value <= INT64_MAX is always < an actually-overflowing 65-bit-plus denominator.
    ASSERT_TRUE(hy::checked_scaled_mul_div(std::numeric_limits<std::int64_t>::max(), -1,
                                            std::numeric_limits<std::int64_t>::max(), out));
    EXPECT_EQ(out, 0);
}

// --- L4 §5.1.1 regression: the real scale-mismatch bug this closes ---
//
// Both worked examples below use price_scale+qty_scale != kBalanceScale(8) deliberately
// (the exact precondition §5.1.1 names) and pick real-world-shaped numbers whose OLD
// (pre-fix, comparing checked_notional()'s raw native-scale product directly against a
// kBalanceScale-8 threshold) and NEW (rescaled first) outcomes provably differ -- these
// are not synthetic edge cases, they demonstrate the bug fails OPEN (lets a real
// violation through), which is the dangerous direction for a risk gate.

TEST(PreTradeScaleMismatch, NativeScaleAboveTargetPreviouslyMaskedBelowMinNotional) {
    // price_scale=6, qty_scale=6 -> native_scale=12 (> kBalanceScale=8, needs ceiling
    // scale-DOWN). price_ticks=500000 (=$0.50 at scale 6), qty_ticks=1000000 (=1.0 at
    // scale 6) -> a real $0.50 order.
    auto rules = make_btcusdt_rules();
    rules.price_scale = 6;
    rules.qty_scale = 6;
    rules.min_notional_ticks = 100'000'000;  // $1.00 at kBalanceScale=8
    rules.tick_size_ticks = 0;   // disable PRICE_FILTER -- not what this test is about
    rules.step_size_ticks = 0;   // disable LOT_SIZE -- not what this test is about
    auto account = make_account(1000, true);
    auto limits = make_limits();
    limits.single_order_notional_cap = 0;    // isolate MIN_NOTIONAL, don't hit the cap
    limits.total_exposure_notional_cap = 0;

    auto r = validate_pre_trade(rules, hy::OrderSide::Buy, 500000, 1000000, account,
                                "BTC", "USDT", 1010, limits);
    // notional_native = 500000*1000000 = 5e11 (scale 12) -- comparing that RAW number
    // directly against min_notional_ticks=1e8 would say "5e11 is not < 1e8", i.e. pass.
    // The real order is $0.50 < the $1.00 minimum and must be rejected once rescaled
    // (5e11 ceiling-divided by 10^4 = 5e7 = $0.50000000 < $1.00 minimum).
    EXPECT_EQ(r, PreTradeCheck::BelowMinNotional);
}

TEST(PreTradeScaleMismatch, NativeScaleBelowTargetPreviouslyMaskedExceedsSingleOrderCap) {
    // price_scale=1, qty_scale=1 -> native_scale=2 (< kBalanceScale=8, needs scale-UP by
    // 10^6). price_ticks=100 (=$10.0 at scale 1), qty_ticks=100 (=10.0 at scale 1) -> a
    // real $100 order.
    auto rules = make_btcusdt_rules();
    rules.price_scale = 1;
    rules.qty_scale = 1;
    rules.min_notional_ticks = 0;  // isolate the single-order cap
    rules.tick_size_ticks = 0;     // disable PRICE_FILTER -- not what this test is about
    rules.step_size_ticks = 0;     // disable LOT_SIZE -- not what this test is about
    auto account = make_account(1000, true);
    auto limits = make_limits();
    limits.single_order_notional_cap = 5'000'000'000;  // $50.00 at kBalanceScale=8
    limits.total_exposure_notional_cap = 0;

    auto r = validate_pre_trade(rules, hy::OrderSide::Buy, 100, 100, account,
                                "BTC", "USDT", 1010, limits);
    // notional_native = 100*100 = 10000 (scale 2) -- comparing that RAW number directly
    // against a cap of 5e9 would say "10000 is not > 5e9", i.e. pass. The real order is
    // $100 > the $50 cap and must be rejected once rescaled (10000 * 10^6 = 1e10 > 5e9).
    EXPECT_EQ(r, PreTradeCheck::ExceedsSingleOrderCap);
}

TEST(PreTradeScaleMismatch, SellSideQtyIsRescaledBeforeComparingAgainstBaseBalance) {
    // Found while implementing the fix above, not named explicitly in the spec text:
    // AssetBalance::free_ticks is always at kBalanceScale (§4.2) regardless of which
    // asset it holds, so the Sell-side "enough base asset to deliver" check
    // (qty_ticks vs. free_ticks) has the identical scale-mismatch shape as the notional
    // checks above, just for a plain quantity instead of a price*qty product.
    auto rules = make_btcusdt_rules();
    rules.qty_scale = 1;         // native scale for the qty comparison, deliberately != 8
    rules.tick_size_ticks = 0;   // disable PRICE_FILTER -- not what this test is about
    rules.min_qty_ticks = 0;     // disable LOT_SIZE min/step -- the qty values below (1, 3)
    rules.step_size_ticks = 0;   // are deliberately small and wouldn't clear the fixture's
    rules.max_qty_ticks = 0;     // default min_qty_ticks=10/step_size_ticks=10
    rules.min_notional_ticks = 0;  // isolate the balance check from MIN_NOTIONAL
    auto account = make_account(1000, true);
    account.assets[1].free_ticks = 20'000'000;  // 0.2 BTC at kBalanceScale=8
    auto limits = make_limits();
    limits.single_order_notional_cap = 0;
    limits.total_exposure_notional_cap = 0;
    limits.freshness_max_age_ms = 30000;

    // qty_ticks=1 at qty_scale=1 means a real quantity of 0.1 BTC -- comfortably under
    // the 0.2 BTC held, so this must pass once qty is correctly rescaled to kBalanceScale
    // (1 * 10^7 = 10,000,000 <= 20,000,000). The OLD buggy comparison (1 vs 20,000,000)
    // would also happen to pass here by coincidence of magnitude -- the next test picks
    // numbers where the two disagree.
    auto ok = validate_pre_trade(rules, hy::OrderSide::Sell, 1000, 1, account,
                                 "BTC", "USDT", 1010, limits);
    EXPECT_EQ(ok, PreTradeCheck::Ok);

    // qty_ticks=3 at qty_scale=1 means a real quantity of 0.3 BTC -- exceeds the 0.2 BTC
    // held, so this must be rejected once correctly rescaled (3*10^7=30,000,000 >
    // 20,000,000). The OLD buggy comparison (raw 3 vs. 20,000,000) would have passed.
    auto insufficient = validate_pre_trade(rules, hy::OrderSide::Sell, 1000, 3, account,
                                           "BTC", "USDT", 1010, limits);
    EXPECT_EQ(insufficient, PreTradeCheck::InsufficientBalance);
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
