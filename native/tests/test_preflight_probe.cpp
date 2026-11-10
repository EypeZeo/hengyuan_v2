// preflight_probe.hpp (private-stream migration M5b-1): the order the preflight harness asks orchestrate_submit() about, and the one place it is put into the
// context. Pure logic -- no network, no threads: the quantity rounding (overflow-checked), the side and price a suggested action calls for, and the property the
// harness exists to demonstrate -- that filling a context never leaves a confirmation bound and never leaves anything but the mock submit port in it.

#include <gtest/gtest.h>

#include "test_helpers/account_snapshot_fixtures.hpp"
#include <hengyuan/preflight_probe.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <string>

using namespace hy;

namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

SymbolRules rules_with(std::int64_t min_qty, std::int64_t step) {
    SymbolRules r{};
    r.min_qty_ticks = min_qty;
    r.step_size_ticks = step;
    return r;
}

std::optional<std::int64_t> some(std::int64_t v) { return v; }

// The Testnet's BTCUSDT as its public exchangeInfo reports it (read 2026-11-07, the day of the first joint run), converted the way parse_exchange_info_response()
// converts it: PRICE_FILTER tickSize/minPrice 0.01, maxPrice 1000000; LOT_SIZE stepSize/minQty 0.00001, maxQty 9000; NOTIONAL minNotional 5.0 (at kBalanceScale).
// The synthetic rules the other tests use carry min_notional_ticks = 1000 (0.00001 USDT), which is why no test ever saw a probe fall under the real minimum.
SymbolRules testnet_btcusdt() {
    SymbolRules r{};
    std::strncpy(r.symbol, "BTCUSDT", sizeof(r.symbol) - 1);
    r.is_trading = true;
    r.price_scale = 2;
    r.tick_size_ticks = 1;
    r.min_price_ticks = 1;
    r.max_price_ticks = 100'000'000;
    r.qty_scale = 5;
    r.step_size_ticks = 1;
    r.min_qty_ticks = 1;
    r.max_qty_ticks = 900'000'000;
    r.min_notional_ticks = 500'000'000;
    r.quote_scale = 8;
    return r;
}

// The gate's own verdict on an order of `side`, for an account holding far more of everything than any order below needs and limits that do not bind (caps 0 = off):
// what is left is the symbol's own filters -- the ones a probe has to pass.
PreTradeCheck gate_verdict(const SymbolRules& rules, OrderSide side, std::int64_t price_ticks, std::int64_t qty_ticks) {
    constexpr std::int64_t kRich = 9'200'000'000'000'000'000;  // just under INT64_MAX: more than any order below comes to
    static const AccountSnapshot account = hy::test_helpers::make_account(900, /*can_trade=*/true, {{"USDT", kRich, 0}, {"BTC", kRich, 0}});
    ExposureLimits limits;
    limits.freshness_max_age_ms = 30000;
    return validate_pre_trade(rules, side, price_ticks, qty_ticks, account, "BTC", "USDT", /*now_ms=*/1000, limits);
}

int g_mock_calls = 0;

SubmitResponse mock_submit(const char*, std::uint32_t, OrderSide, OrderType, std::int64_t, std::int64_t, const SymbolRules&, void*) noexcept {
    ++g_mock_calls;
    return SubmitResponse{SubmitOutcome::NetworkError, 0, -1};
}
std::uint32_t mock_version(void*) { return 7; }

SubmitResponse other_submit(const char*, std::uint32_t, OrderSide, OrderType, std::int64_t, std::int64_t, const SymbolRules&, void*) noexcept {
    return SubmitResponse{SubmitOutcome::NetworkError, 0, -1};
}
std::uint32_t other_version(void*) { return 9; }

}  // namespace

// ---- preflight_quantity_ticks() ------------------------------------------------------------------------------------------------------------

TEST(PreflightQuantity, TheMinimumItselfWhenItIsOnTheStepGrid) {
    EXPECT_EQ(preflight_quantity_ticks(rules_with(10, 5)), some(10));
    EXPECT_EQ(preflight_quantity_ticks(rules_with(10, 10)), some(10));
    EXPECT_EQ(preflight_quantity_ticks(rules_with(1, 1)), some(1));
}

TEST(PreflightQuantity, RoundsUpToTheNextStep) {
    EXPECT_EQ(preflight_quantity_ticks(rules_with(10, 4)), some(12));
    EXPECT_EQ(preflight_quantity_ticks(rules_with(1, 1000)), some(1000));
    EXPECT_EQ(preflight_quantity_ticks(rules_with(999, 1000)), some(1000));
    EXPECT_EQ(preflight_quantity_ticks(rules_with(1001, 1000)), some(2000));
    EXPECT_EQ(preflight_quantity_ticks(rules_with(7, 3)), some(9));
}

TEST(PreflightQuantity, NoStepMeansNoGrid) {
    EXPECT_EQ(preflight_quantity_ticks(rules_with(25, 0)), some(25));
}

TEST(PreflightQuantity, RulesThatCannotProduceAQuantityProduceNone) {
    EXPECT_FALSE(preflight_quantity_ticks(rules_with(0, 5)).has_value()) << "a zero minimum is no quantity";
    EXPECT_FALSE(preflight_quantity_ticks(rules_with(0, 0)).has_value());
    EXPECT_FALSE(preflight_quantity_ticks(rules_with(-5, 5)).has_value()) << "a negative minimum is no quantity";
    EXPECT_FALSE(preflight_quantity_ticks(rules_with(10, -1)).has_value()) << "a negative step is no grid";
}

TEST(PreflightQuantity, ARoundingThatDoesNotFitProducesNone) {
    constexpr std::int64_t kLastGridPoint = (kMax / 4) * 4;  // the largest multiple of 4 that fits in 64 bits
    EXPECT_EQ(preflight_quantity_ticks(rules_with(kLastGridPoint - 3, 4)), some(kLastGridPoint)) << "lands exactly on the last point that fits";
    EXPECT_EQ(preflight_quantity_ticks(rules_with(kLastGridPoint, 4)), some(kLastGridPoint)) << "already on the grid";
    EXPECT_FALSE(preflight_quantity_ticks(rules_with(kLastGridPoint + 1, 4)).has_value()) << "the next grid point is above INT64_MAX";
    EXPECT_FALSE(preflight_quantity_ticks(rules_with(kMax, 2)).has_value());
    EXPECT_EQ(preflight_quantity_ticks(rules_with(kMax, 1)), some(kMax)) << "a step of 1 never rounds";
}

// ---- preflight_quantity_ticks(rules, price): the notional half -----------------------------------------------------------------------------

namespace {

SymbolRules scaled_rules(int price_scale, int qty_scale, std::int64_t min_qty, std::int64_t step, std::int64_t min_notional) {
    SymbolRules r = rules_with(min_qty, step);
    r.is_trading = true;
    r.price_scale = static_cast<std::uint8_t>(price_scale);
    r.qty_scale = static_cast<std::uint8_t>(qty_scale);
    r.min_notional_ticks = min_notional;
    return r;
}

// `qty` is THE smallest quantity the gate accepts at `price` on `rules`: the planner says so, the gate says Ok to it, and not to the grid point before it (when
// there is one above the lot).
void expect_smallest(const SymbolRules& rules, std::int64_t price, std::int64_t qty) {
    SCOPED_TRACE("price_ticks=" + std::to_string(price) + " expected qty_ticks=" + std::to_string(qty));
    EXPECT_EQ(preflight_quantity_ticks(rules, price), some(qty));
    EXPECT_EQ(gate_verdict(rules, OrderSide::Buy, price, qty), PreTradeCheck::Ok);
    const std::int64_t inc = rules.step_size_ticks > 0 ? rules.step_size_ticks : 1;
    if (qty - inc >= rules.min_qty_ticks) {
        EXPECT_NE(gate_verdict(rules, OrderSide::Buy, price, qty - inc), PreTradeCheck::Ok);
    }
}

}  // namespace

TEST(PreflightQuantityNotional, WithoutAMinimumNotionalItIsTheLot) {
    EXPECT_EQ(preflight_quantity_ticks(rules_with(10, 5), 123), some(10));
    EXPECT_EQ(preflight_quantity_ticks(rules_with(25, 0), 123), some(25));
    SymbolRules negative = rules_with(10, 5);
    negative.min_notional_ticks = -1;
    EXPECT_EQ(preflight_quantity_ticks(negative, 123), some(10)) << "a negative minimum is no minimum, as in the gate";
}

// The joint run of 2026-11-07: price 7592032 (75 920.32), the bare minimum lot 0.00001 BTC -- 0.7592 USDT against a minimum of 5. The log said PreTradeFailed and
// nothing else; the verdict was BelowMinNotional.
TEST(PreflightQuantityNotional, TheTestnetsBtcusdtAtTheJointRunsPrice) {
    const SymbolRules rules = testnet_btcusdt();
    EXPECT_EQ(preflight_quantity_ticks(rules), some(1)) << "the lot alone: what the joint run sent";
    EXPECT_EQ(gate_verdict(rules, OrderSide::Buy, 7'592'032, 1), PreTradeCheck::BelowMinNotional);
    expect_smallest(rules, 7'592'032, 7);  // 7 lots = 5.3144224 USDT; 6 lots = 4.5552192
}

TEST(PreflightQuantityNotional, ANotionalEqualToTheMinimumIsEnoughAndLessIsNot) {
    const SymbolRules rules = testnet_btcusdt();
    expect_smallest(rules, 50'000'000, 1);  // 500000.00 * 0.00001 = 5.00000000 exactly
    expect_smallest(rules, 49'999'999, 2);  // 499999.99 * 0.00001 = 4.9999999: just short, so the next lot
    expect_smallest(rules, 50'000'001, 1);
    EXPECT_EQ(gate_verdict(rules, OrderSide::Buy, 49'999'999, 1), PreTradeCheck::BelowMinNotional);
}

TEST(PreflightQuantityNotional, TheNotionalIsRescaledTheWayTheGateRescalesIt) {
    // Native scale (price_scale + qty_scale) below kBalanceScale (8): the product is multiplied up.
    expect_smallest(scaled_rules(0, 0, 1, 1, 10'000'000'000), 7, 15);  // 7 * 14 * 1e8 = 9.8e9 is short, 7 * 15 * 1e8 = 1.05e10 reaches
    // At it: no rescale.
    expect_smallest(scaled_rules(3, 5, 1, 1, 1000), 125, 8);  // 125 * 8 = 1000 exactly
    // Above it: divided down and judged on the value rounded DOWN, as the gate judges a minimum (audit F-A-001: a lower bound must be reached by the exact
    // product, and rounding up lets a product just under it through). This case used to expect 3333 "because the ceiling reaches 5000"; the exact product of 3333
    // lots, 4999.5 ticks, is below the 5000 minimum, so the exchange would have refused that order. The first quantity that really reaches it is 3334.
    expect_smallest(scaled_rules(6, 5, 1, 1, 5000), 1500, 3334);  // floor(1500 * 3333 / 1000) = 4999 is short; floor(1500 * 3334 / 1000) = 5001 reaches
}

// Audit F-A-001, the case the report gave: price_scale 4 + qty_scale 5 = 9 above kBalanceScale, a product of 4.999999991 against a minimum of 5.0. Rounded up it
// read as 5.00000000 and the lot of 1 passed; the exact product is under the minimum, so the smallest quantity that reaches it is 2.
TEST(PreflightQuantityNotional, AProductJustUnderTheMinimumIsShortEvenWhenItRoundsUpToIt) {
    const SymbolRules rules = scaled_rules(4, 5, 1, 1, 500'000'000);
    EXPECT_EQ(gate_verdict(rules, OrderSide::Buy, 4'999'999'991, 1), PreTradeCheck::BelowMinNotional);
    expect_smallest(rules, 4'999'999'991, 2);
    expect_smallest(rules, 5'000'000'000, 1);  // 5.000000000 exactly: reaches
}

TEST(PreflightQuantityNotional, OnlyPointsOfTheStepGridAreAnswers) {
    expect_smallest(scaled_rules(0, 0, 1000, 1000, 1'000'000'000'000), 3, 4000);  // 3 * 3000 * 1e8 = 9e11 is short, 3 * 4000 * 1e8 = 1.2e12 reaches
}

TEST(PreflightQuantityNotional, WithoutAStepEveryTickIsAQuantity) {
    expect_smallest(scaled_rules(0, 0, 25, 0, 100'000'000'000), 10, 100);  // 10 * q * 1e8 >= 1e11 needs q >= 100; the lot (25) is short
}

TEST(PreflightQuantityNotional, TheLotItselfWhenItAlreadyReachesTheMinimum) {
    expect_smallest(scaled_rules(0, 0, 25, 5, 100), 10, 25);
}

TEST(PreflightQuantityNotional, AnAnswerFarFromTheLotIsFound) {
    expect_smallest(scaled_rules(0, 0, 1, 1, 100'000'300'000'000), 1, 1'000'003);  // 1000003.0 at price 1: a million lots and three
}

TEST(PreflightQuantityNotional, NothingWhenNoRepresentableQuantityReachesTheMinimum) {
    // 3e17 * qty at scale 7, rescaled by 10: 3 lots are 9e18 (short of 9.2e18), 4 lots overflow the rescale -- the gate says NotionalOverflow there.
    const SymbolRules rules = scaled_rules(2, 5, 1, 1, 9'200'000'000'000'000'000);
    constexpr std::int64_t kPrice = 300'000'000'000'000'000;
    EXPECT_FALSE(preflight_quantity_ticks(rules, kPrice).has_value());
    EXPECT_EQ(gate_verdict(rules, OrderSide::Buy, kPrice, 3), PreTradeCheck::BelowMinNotional);
    EXPECT_EQ(gate_verdict(rules, OrderSide::Buy, kPrice, 4), PreTradeCheck::NotionalOverflow);
    // Not even the lot fits (the product is INT64_MAX, the rescale by 10 overflows).
    EXPECT_FALSE(preflight_quantity_ticks(testnet_btcusdt(), kMax).has_value());
    // A minimum that is itself the largest value there is: the notional overflows the rescale just before it could reach it.
    EXPECT_FALSE(preflight_quantity_ticks(scaled_rules(0, 0, 1, 1, kMax), 1).has_value());
}

TEST(PreflightQuantityNotional, NothingWhenTheAnswerIsAboveMaxQty) {
    SymbolRules rules = testnet_btcusdt();
    rules.max_qty_ticks = 6;
    EXPECT_FALSE(preflight_quantity_ticks(rules, 7'592'032).has_value()) << "7 lots are needed, 6 are allowed";
    rules.max_qty_ticks = 7;
    EXPECT_EQ(preflight_quantity_ticks(rules, 7'592'032), some(7)) << "the maximum itself is allowed";
    // The gate judges the lot filter only for rules that carry a step; so does the planner.
    rules.max_qty_ticks = 6;
    rules.step_size_ticks = 0;
    EXPECT_EQ(preflight_quantity_ticks(rules, 7'592'032), some(7));
    EXPECT_EQ(gate_verdict(rules, OrderSide::Buy, 7'592'032, 7), PreTradeCheck::Ok);
    // With no minimum notional the lot is the answer -- unless the lot is itself above the maximum.
    SymbolRules lot_only = rules_with(10, 5);
    lot_only.max_qty_ticks = 5;
    EXPECT_FALSE(preflight_quantity_ticks(lot_only, 123).has_value());
    lot_only.max_qty_ticks = 10;
    EXPECT_EQ(preflight_quantity_ticks(lot_only, 123), some(10));
    // Likewise when the lot already reaches a (small) minimum notional but is itself above the maximum: found at once, and still refused.
    SymbolRules lot_reaches = scaled_rules(0, 0, 10, 5, 100);
    EXPECT_EQ(preflight_quantity_ticks(lot_reaches, 10), some(10));
    lot_reaches.max_qty_ticks = 5;
    EXPECT_FALSE(preflight_quantity_ticks(lot_reaches, 10).has_value());
}

TEST(PreflightQuantityNotional, NoPriceNoQuantity) {
    const SymbolRules rules = testnet_btcusdt();
    EXPECT_FALSE(preflight_quantity_ticks(rules, 0).has_value());
    EXPECT_FALSE(preflight_quantity_ticks(rules, -1).has_value());
    EXPECT_FALSE(preflight_quantity_ticks(rules, std::numeric_limits<std::int64_t>::min()).has_value());
    // ...whether or not the rules carry a minimum notional: a price that is not a price is no order.
    EXPECT_FALSE(preflight_quantity_ticks(rules_with(10, 5), 0).has_value());
    EXPECT_FALSE(preflight_quantity_ticks(rules_with(10, 5), -1).has_value());
}

// The doubling search finds an answer tens of billions of steps from the lot in a few dozen evaluations; a walk one step at a time would not finish.
TEST(PreflightQuantityNotional, AnAnswerOfTensOfBillionsIsFoundInAFewDozenSteps) {
    expect_smallest(scaled_rules(0, 0, 1, 1, 9'000'000'000'000'000'000), 1, 90'000'000'000);  // q * 1e8 >= 9e18: 90 billion units, the largest notional there is room for
}

// The answer sits in a window just below the overflow edge: the doubling jumps from "short" (2^60 units) straight to "the product overflows" (2^61 units, 4 * 2^61 =
// 2^63) and the bisection must still find the 2.3e18 in between -- an overflow is not "short of the minimum".
TEST(PreflightQuantityNotional, AnAnswerJustBelowTheOverflowEdgeIsNotMissed) {
    expect_smallest(scaled_rules(4, 4, 1, 1, 9'200'000'000'000'000'000), 4, 2'300'000'000'000'000'000);  // native scale 8: no rescale; 4 * q >= 9.2e18
}

// A minimum no quantity can reach, though every quantity up to the 64-bit limit is representable: the doubling stops at the limit instead of overflowing.
TEST(PreflightQuantityNotional, NothingWhenTheMinimumIsNeverReached) {
    // native scale 18: the notional is divided by 1e10 on the way to the balance scale, so even 2^62 units at price 1 come to well under INT64_MAX ticks.
    const SymbolRules rules = scaled_rules(9, 9, 1, 1, kMax);
    EXPECT_FALSE(preflight_quantity_ticks(rules, 1).has_value());
}

TEST(PreflightQuantityNotional, RulesWithoutALotGiveNoQuantity) {
    SymbolRules rules = testnet_btcusdt();
    rules.min_qty_ticks = 0;
    EXPECT_FALSE(preflight_quantity_ticks(rules, 7'592'032).has_value());
    rules = testnet_btcusdt();
    rules.step_size_ticks = -1;
    EXPECT_FALSE(preflight_quantity_ticks(rules, 7'592'032).has_value());
}

// The oracle is the gate itself: the first point of the lot grid, walking up from the lot, that validate_pre_trade() answers Ok to -- for random rules (on the
// step grid, as every real symbol is), prices, and minimum notionals placed within a few ticks of what some number of lots comes to, so the boundary is hit often.
TEST(PreflightQuantityNotional, AgreesWithTheGateOnRandomRules) {
    std::mt19937_64 rng(0x7E57CA5EULL);
    const auto pick = [&rng](std::int64_t lo, std::int64_t hi) { return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng); };
    constexpr std::int64_t kSteps[] = {0, 1, 2, 3, 5, 10, 100};
    int searched = 0;     // answers above the lot: the search had work to do
    int capped = 0;       // rounds where a maximum quantity was in the way
    int no_minimum = 0;   // rounds with no (or a zero) minimum notional
    for (int round = 0; round < 2000; ++round) {
        const int price_scale = static_cast<int>(pick(0, 6));
        const int qty_scale = static_cast<int>(pick(0, 6));
        const std::int64_t step = kSteps[pick(0, 6)];
        const std::int64_t min_qty = step > 0 ? step * pick(1, 5) : pick(1, 5);
        const std::int64_t price = pick(1, 2000);
        std::int64_t native = 0;
        ASSERT_TRUE(checked_notional(price, min_qty * pick(1, 1500), native));
        std::int64_t at_balance_scale = 0;
        ASSERT_TRUE(rescale_notional_ceil(native, price_scale + qty_scale, kBalanceScale, at_balance_scale));
        std::int64_t min_notional = std::max<std::int64_t>(0, at_balance_scale + pick(-3, 3));
        if (pick(0, 9) == 0) min_notional = 0;  // one round in ten has no minimum at all: the lot is the answer
        SymbolRules rules = scaled_rules(price_scale, qty_scale, min_qty, step, min_notional);
        if (step > 0 && pick(0, 3) == 0) rules.max_qty_ticks = min_qty * pick(1, 4000);

        const std::optional<std::int64_t> planned = preflight_quantity_ticks(rules, price);

        std::optional<std::int64_t> oracle;
        const std::int64_t inc = step > 0 ? step : 1;
        std::int64_t qty = min_qty;
        for (int i = 0; i < 20'000; ++i, qty += inc) {
            const PreTradeCheck verdict = gate_verdict(rules, OrderSide::Buy, price, qty);
            if (verdict == PreTradeCheck::Ok) {
                oracle = qty;
                break;
            }
            if (verdict == PreTradeCheck::QtyAboveMax) break;  // nothing larger can pass
        }
        ASSERT_TRUE(oracle.has_value() || rules.max_qty_ticks > 0) << "round " << round << ": the oracle's bound was too small for this round";
        EXPECT_EQ(planned, oracle) << "round " << round << ": price_scale=" << price_scale << " qty_scale=" << qty_scale << " min_qty=" << min_qty << " step=" << step
                                   << " max_qty=" << rules.max_qty_ticks << " min_notional=" << min_notional << " price=" << price;
        if (planned && *planned > min_qty) ++searched;
        if (!oracle && rules.max_qty_ticks > 0) ++capped;
        if (min_notional == 0) ++no_minimum;
    }
    // The generator must have exercised what the test claims to.
    EXPECT_GT(searched, 1000);
    EXPECT_GT(capped, 20);
    EXPECT_GT(no_minimum, 0);
}

// ---- plan_preflight_probe() ----------------------------------------------------------------------------------------------------------------

TEST(PreflightPlan, AnOpenBuysAtTheBestAsk) {
    const auto plan = plan_preflight_probe(SuggestedAction::Open, {99, 101}, rules_with(10, 4));
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->side, OrderSide::Buy);
    EXPECT_EQ(plan->price_ticks, 101) << "the ask, the price that crosses";
    EXPECT_EQ(plan->qty_ticks, 12);
}

TEST(PreflightPlan, ACloseSellsAtTheBestBid) {
    const auto plan = plan_preflight_probe(SuggestedAction::Close, {99, 101}, rules_with(10, 4));
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->side, OrderSide::Sell);
    EXPECT_EQ(plan->price_ticks, 99) << "the bid, the price that crosses";
    EXPECT_EQ(plan->qty_ticks, 12);
}

TEST(PreflightPlan, NoActionNoProbe) {
    EXPECT_FALSE(plan_preflight_probe(SuggestedAction::None, {99, 101}, rules_with(10, 4)).has_value());
    EXPECT_FALSE(plan_preflight_probe(static_cast<SuggestedAction>(7), {99, 101}, rules_with(10, 4)).has_value()) << "a value that is none of the three";
}

TEST(PreflightPlan, ASideOfTheBookThatIsNotAPriceIsNoProbe) {
    const SymbolRules rules = rules_with(10, 4);
    EXPECT_FALSE(plan_preflight_probe(SuggestedAction::Open, {99, 0}, rules).has_value());
    EXPECT_FALSE(plan_preflight_probe(SuggestedAction::Open, {99, -1}, rules).has_value());
    EXPECT_FALSE(plan_preflight_probe(SuggestedAction::Close, {0, 101}, rules).has_value());
    EXPECT_FALSE(plan_preflight_probe(SuggestedAction::Close, {-5, 101}, rules).has_value());
}

TEST(PreflightPlan, EachActionLooksOnlyAtItsOwnSideOfTheBook) {
    const SymbolRules rules = rules_with(10, 4);
    const auto open = plan_preflight_probe(SuggestedAction::Open, {0, 101}, rules);
    ASSERT_TRUE(open.has_value()) << "an open does not need a bid";
    EXPECT_EQ(open->price_ticks, 101);
    const auto close = plan_preflight_probe(SuggestedAction::Close, {99, 0}, rules);
    ASSERT_TRUE(close.has_value()) << "a close does not need an ask";
    EXPECT_EQ(close->price_ticks, 99);
}

TEST(PreflightPlan, ASymbolWithoutAQuantityIsNoProbe) {
    EXPECT_FALSE(plan_preflight_probe(SuggestedAction::Open, {99, 101}, rules_with(0, 4)).has_value());
    EXPECT_FALSE(plan_preflight_probe(SuggestedAction::Close, {99, 101}, rules_with(10, -1)).has_value());
}

TEST(PreflightPlan, TheQuantityIsSizedAtTheProbesOwnPrice) {
    const SymbolRules rules = testnet_btcusdt();
    // The best bid is 0.01 under 500000.00 and the best ask is 500000.00: one lot at the ask is exactly the 5 USDT minimum, at the bid it is just short.
    const auto open = plan_preflight_probe(SuggestedAction::Open, {49'999'999, 50'000'000}, rules);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->price_ticks, 50'000'000);
    EXPECT_EQ(open->qty_ticks, 1);
    const auto close = plan_preflight_probe(SuggestedAction::Close, {49'999'999, 50'000'000}, rules);
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->price_ticks, 49'999'999);
    EXPECT_EQ(close->qty_ticks, 2) << "a SELL at the bid needs the next lot to reach the same minimum";
}

// The book of the 2026-11-07 joint run (best ask 75 920.32): the order the probe now asks about is one the gate's symbol filters accept, for either side.
TEST(PreflightPlan, TheJointRunsBookGivesOrdersTheGateAccepts) {
    const SymbolRules rules = testnet_btcusdt();
    const auto open = plan_preflight_probe(SuggestedAction::Open, {7'592'031, 7'592'032}, rules);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->side, OrderSide::Buy);
    EXPECT_EQ(open->price_ticks, 7'592'032);
    EXPECT_EQ(open->qty_ticks, 7);
    EXPECT_EQ(gate_verdict(rules, open->side, open->price_ticks, open->qty_ticks), PreTradeCheck::Ok);
    const auto close = plan_preflight_probe(SuggestedAction::Close, {7'592'031, 7'592'032}, rules);
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->side, OrderSide::Sell);
    EXPECT_EQ(close->price_ticks, 7'592'031);
    EXPECT_EQ(close->qty_ticks, 7);
    EXPECT_EQ(gate_verdict(rules, close->side, close->price_ticks, close->qty_ticks), PreTradeCheck::Ok);
}

TEST(PreflightPlan, ASymbolWhoseMinimumCannotBeReachedIsNoProbe) {
    SymbolRules rules = testnet_btcusdt();
    rules.max_qty_ticks = 6;  // 7 lots are needed at this price
    EXPECT_FALSE(plan_preflight_probe(SuggestedAction::Open, {7'592'031, 7'592'032}, rules).has_value());
}

// ---- fill_preflight_order() ----------------------------------------------------------------------------------------------------------------

namespace {

struct FillFixture {
    DryRunEvidenceChain evidence;
    SymbolRules rules = rules_with(10, 4);
    PreflightEnvironment env;
    PreflightProbe probe{OrderSide::Sell, 99, 12};

    FillFixture() {
        env.evidence = &evidence;
        env.rules = &rules;
        env.base_asset = "BTC";
        env.quote_asset = "USDT";
        env.signer_ready = true;
        env.depth_synced = true;
        env.symbol_id = 3;
        env.sequence = 9;
        env.now_ms = 12345;
        env.submit_port = {&mock_submit, nullptr, &mock_version};
    }
};

}  // namespace

TEST(PreflightFill, PutsTheOrderAndTheEnvironmentIntoTheContext) {
    FillFixture fx;
    OrchestratorContext ctx{};
    fill_preflight_order(ctx, fx.probe, fx.env);

    EXPECT_EQ(ctx.evidence, &fx.evidence);
    EXPECT_TRUE(ctx.signer_ready);
    EXPECT_TRUE(ctx.depth_synced);
    EXPECT_EQ(ctx.symbol_rules, &fx.rules);
    EXPECT_EQ(ctx.side, OrderSide::Sell);
    EXPECT_EQ(ctx.order_type, OrderType::Limit);
    EXPECT_EQ(ctx.base_asset, "BTC");
    EXPECT_EQ(ctx.quote_asset, "USDT");
    EXPECT_EQ(ctx.now_ms, 12345);
    EXPECT_EQ(ctx.price_ticks, 99);
    EXPECT_EQ(ctx.qty_ticks, 12);
    EXPECT_EQ(ctx.symbol_id, 3U);
    EXPECT_EQ(ctx.sequence, 9U);
    EXPECT_EQ(ctx.mode, ExecutionMode::Live);
    EXPECT_EQ(ctx.order_weight, 1U);
}

TEST(PreflightFill, TheEnvironmentsFlagsAreTheContextsFlags) {
    FillFixture fx;
    fx.env.signer_ready = false;
    fx.env.depth_synced = false;
    OrchestratorContext ctx{};
    ctx.signer_ready = true;
    ctx.depth_synced = true;
    fill_preflight_order(ctx, fx.probe, fx.env);
    EXPECT_FALSE(ctx.signer_ready) << "a harness without a signer says so; the fill never claims one";
    EXPECT_FALSE(ctx.depth_synced) << "a book that does not track says so";
}

TEST(PreflightFill, TheWiringItWasGivenIsNotTouched) {
    FillFixture fx;
    AuditRingSink audit;
    KillSwitch kill_switch;
    SpotRateLimitTracker rate_limiter;
    InFlightRegistry in_flight;
    AccountSnapshot account{};
    OrchestratorContext ctx{};
    ctx.audit = &audit;
    ctx.kill_switch = &kill_switch;
    ctx.rate_limiter = &rate_limiter;
    ctx.in_flight = &in_flight;
    ctx.account = &account;
    ctx.exposure_limits.freshness_max_age_ms = 777;
    ctx.exposure_limits.single_order_notional_cap = 555;
    fill_preflight_order(ctx, fx.probe, fx.env);

    EXPECT_EQ(ctx.audit, &audit);
    EXPECT_EQ(ctx.kill_switch, &kill_switch);
    EXPECT_EQ(ctx.rate_limiter, &rate_limiter);
    EXPECT_EQ(ctx.in_flight, &in_flight);
    EXPECT_EQ(ctx.account, &account);
    EXPECT_EQ(ctx.exposure_limits.freshness_max_age_ms, 777);
    EXPECT_EQ(ctx.exposure_limits.single_order_notional_cap, 555);
    EXPECT_EQ(ctx.readiness, nullptr) << "the fill does not invent a readiness: whatever the pipeline wired (or did not) is what Gate 2b asks";
}

TEST(PreflightFill, ABoundConfirmationIsResetNotKept) {
    FillFixture fx;
    OrchestratorContext ctx{};
    // A context that arrives with the operator's confirmation already bound to exactly this order -- the one thing that would let it through.
    ctx.confirmation.confirmed = true;
    ctx.confirmation.symbol_id = fx.env.symbol_id;
    ctx.confirmation.side = fx.probe.side;
    ctx.confirmation.type = OrderType::Limit;
    ctx.confirmation.price_ticks = fx.probe.price_ticks;
    ctx.confirmation.qty_ticks = fx.probe.qty_ticks;
    ctx.confirmation.max_notional = 1'000'000;
    ctx.confirmation.valid_until_ms = fx.env.now_ms + 60'000;
    ASSERT_TRUE(ctx.confirmation.authorizes(fx.env.symbol_id, fx.probe.side, OrderType::Limit, fx.probe.price_ticks, fx.probe.qty_ticks, 1000,
                                            fx.env.now_ms));

    fill_preflight_order(ctx, fx.probe, fx.env);

    EXPECT_FALSE(ctx.confirmation.confirmed);
    EXPECT_FALSE(ctx.confirmation.authorizes(fx.env.symbol_id, fx.probe.side, OrderType::Limit, fx.probe.price_ticks, fx.probe.qty_ticks, 1000,
                                             fx.env.now_ms));
    EXPECT_EQ(ctx.confirmation.max_notional, 0) << "the whole confirmation is the default one, not just its flag";
    EXPECT_EQ(ctx.confirmation.valid_until_ms, 0);
}

TEST(PreflightFill, AnotherSubmitPortIsReplacedByTheMock) {
    FillFixture fx;
    int marker = 0;
    OrchestratorContext ctx{};
    ctx.submit_port = {&other_submit, &marker, &other_version};

    fill_preflight_order(ctx, fx.probe, fx.env);

    EXPECT_EQ(ctx.submit_port.fn, &mock_submit);
    EXPECT_EQ(ctx.submit_port.user_data, nullptr);
    EXPECT_EQ(ctx.submit_port.current_rules_version_fn, &mock_version);
    EXPECT_EQ(g_mock_calls, 0) << "installing the port calls nothing";
}

// ---- pre_trade_check_name() ----------------------------------------------------------------------------------------------------------------

TEST(PreTradeCheckName, EveryVerdictHasItsOwnName) {
    const std::pair<PreTradeCheck, const char*> table[] = {
        {PreTradeCheck::Ok, "Ok"},
        {PreTradeCheck::SymbolNotTrading, "SymbolNotTrading"},
        {PreTradeCheck::PriceBelowMin, "PriceBelowMin"},
        {PreTradeCheck::PriceAboveMax, "PriceAboveMax"},
        {PreTradeCheck::PriceStepViolation, "PriceStepViolation"},
        {PreTradeCheck::QtyBelowMin, "QtyBelowMin"},
        {PreTradeCheck::QtyAboveMax, "QtyAboveMax"},
        {PreTradeCheck::QtyStepViolation, "QtyStepViolation"},
        {PreTradeCheck::BelowMinNotional, "BelowMinNotional"},
        {PreTradeCheck::ExceedsSingleOrderCap, "ExceedsSingleOrderCap"},
        {PreTradeCheck::ExceedsTotalExposureCap, "ExceedsTotalExposureCap"},
        {PreTradeCheck::InsufficientBalance, "InsufficientBalance"},
        {PreTradeCheck::BalanceStale, "BalanceStale"},
        {PreTradeCheck::AccountCannotTrade, "AccountCannotTrade"},
        {PreTradeCheck::InvalidInput, "InvalidInput"},
        {PreTradeCheck::NotionalOverflow, "NotionalOverflow"},
        {PreTradeCheck::BalanceFromFuture, "BalanceFromFuture"},
    };
    std::set<std::string> seen;
    for (const auto& [check, name] : table) {
        EXPECT_STREQ(pre_trade_check_name(check), name);
        seen.insert(name);
    }
    EXPECT_EQ(seen.size(), 17U) << "every name is its own";
    // The table covers the whole enumeration: nothing from 0 to its last value falls through to "Unknown".
    for (int value = 0; value <= static_cast<int>(PreTradeCheck::BalanceFromFuture); ++value) {
        EXPECT_STRNE(pre_trade_check_name(static_cast<PreTradeCheck>(value)), "Unknown") << "value " << value;
    }
    EXPECT_STREQ(pre_trade_check_name(static_cast<PreTradeCheck>(200)), "Unknown") << "a value outside the enumeration";
}
