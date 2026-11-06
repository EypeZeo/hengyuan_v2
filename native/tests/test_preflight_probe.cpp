// preflight_probe.hpp (private-stream migration M5b-1): the order the preflight harness asks orchestrate_submit() about, and the one place it is put into the
// context. Pure logic -- no network, no threads: the quantity rounding (overflow-checked), the side and price a suggested action calls for, and the property the
// harness exists to demonstrate -- that filling a context never leaves a confirmation bound and never leaves anything but the mock submit port in it.

#include <gtest/gtest.h>

#include <hengyuan/preflight_probe.hpp>

#include <cstdint>
#include <limits>
#include <optional>

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
