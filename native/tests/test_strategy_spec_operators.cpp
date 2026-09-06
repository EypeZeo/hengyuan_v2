// Batch 5, 5b: strategy_spec_operators.cpp unit tests -- per-operator streaming step
// functions, exercised directly against small hand-computed sequences (mirrors
// py_core/tests/test_indicators_operators.py's role on the Python side). Warm-up boundary
// bars are checked explicitly for every windowed operator, not just the steady-state values.

#include <gtest/gtest.h>
#include <hengyuan/strategy_spec_operators.hpp>

#include <cmath>
#include <limits>

using namespace hy;
using namespace hy::detail;

namespace {
// kNaN comes from hy::detail (strategy_spec_operators.hpp) via the using-directive above --
// not redefined here to avoid shadowing/ambiguity.

WindowState make_window(std::uint16_t capacity) {
    WindowState state{};
    state.pool_offset = 0;
    state.capacity = capacity;
    state.head = 0;
    state.count = 0;
    return state;
}
}  // namespace

// --- sma ---

TEST(StrategySpecOperators, SmaWindow3RecomputesExactlyEveryBar) {
    WindowState state = make_window(3);
    double pool[3]{};
    const double inputs[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    EXPECT_TRUE(std::isnan(step_sma(state, pool, inputs[0])));
    EXPECT_TRUE(std::isnan(step_sma(state, pool, inputs[1])));
    EXPECT_DOUBLE_EQ(step_sma(state, pool, inputs[2]), 2.0);  // mean(1,2,3)
    EXPECT_DOUBLE_EQ(step_sma(state, pool, inputs[3]), 3.0);  // mean(2,3,4)
    EXPECT_DOUBLE_EQ(step_sma(state, pool, inputs[4]), 4.0);  // mean(3,4,5)
}

TEST(StrategySpecOperators, SmaAnyNaNInWindowProducesNaN) {
    WindowState state = make_window(3);
    double pool[3]{};
    EXPECT_TRUE(std::isnan(step_sma(state, pool, 1.0)));
    EXPECT_TRUE(std::isnan(step_sma(state, pool, kNaN)));  // upstream still warming up
    EXPECT_TRUE(std::isnan(step_sma(state, pool, 3.0)));   // window is now {1.0, NaN, 3.0}
    // Window is now {NaN, 3.0, 4.0} (the leading 1.0 has rolled out) -- still contains the
    // NaN pushed earlier, so still NaN. Only once the NaN itself rolls out of the window does
    // a real value reappear (matching pandas' full-window min_periods semantics).
    EXPECT_TRUE(std::isnan(step_sma(state, pool, 4.0)));
    EXPECT_DOUBLE_EQ(step_sma(state, pool, 5.0), 4.0);  // window {3.0, 4.0, 5.0}
}

// --- stddev ---

TEST(StrategySpecOperators, StddevWindow3MatchesTwoPassSampleStddev) {
    WindowState state = make_window(3);
    double pool[3]{};
    EXPECT_TRUE(std::isnan(step_stddev(state, pool, 1.0)));
    EXPECT_TRUE(std::isnan(step_stddev(state, pool, 2.0)));
    // window {1,2,3}: mean=2, sq-dev sum=(1)+(0)+(1)=2, /(3-1)=1, sqrt(1)=1.0
    EXPECT_DOUBLE_EQ(step_stddev(state, pool, 3.0), 1.0);
    // window {2,3,4}: mean=3, same shape -> 1.0
    EXPECT_DOUBLE_EQ(step_stddev(state, pool, 4.0), 1.0);
}

// --- rolling_max / rolling_min ---

TEST(StrategySpecOperators, RollingMaxMinWindow3) {
    WindowState max_state = make_window(3);
    WindowState min_state = make_window(3);
    double max_pool[3]{};
    double min_pool[3]{};
    const double inputs[] = {1.0, 3.0, 2.0, 5.0, 4.0};
    for (int i = 0; i < 2; ++i) {
        EXPECT_TRUE(std::isnan(step_rolling_max(max_state, max_pool, inputs[i])));
        EXPECT_TRUE(std::isnan(step_rolling_min(min_state, min_pool, inputs[i])));
    }
    EXPECT_DOUBLE_EQ(step_rolling_max(max_state, max_pool, inputs[2]), 3.0);  // {1,3,2}
    EXPECT_DOUBLE_EQ(step_rolling_min(min_state, min_pool, inputs[2]), 1.0);
    EXPECT_DOUBLE_EQ(step_rolling_max(max_state, max_pool, inputs[3]), 5.0);  // {3,2,5}
    EXPECT_DOUBLE_EQ(step_rolling_min(min_state, min_pool, inputs[3]), 2.0);
    EXPECT_DOUBLE_EQ(step_rolling_max(max_state, max_pool, inputs[4]), 5.0);  // {2,5,4}
    EXPECT_DOUBLE_EQ(step_rolling_min(min_state, min_pool, inputs[4]), 2.0);
}

// --- roc ---

TEST(StrategySpecOperators, RocWindow2) {
    WindowState state = make_window(2);
    double pool[2]{};
    const double inputs[] = {10.0, 20.0, 30.0, 40.0};
    EXPECT_TRUE(std::isnan(step_roc(state, pool, inputs[0])));
    EXPECT_TRUE(std::isnan(step_roc(state, pool, inputs[1])));
    EXPECT_DOUBLE_EQ(step_roc(state, pool, inputs[2]), 2.0);  // 30/10 - 1
    EXPECT_DOUBLE_EQ(step_roc(state, pool, inputs[3]), 1.0);  // 40/20 - 1
}

TEST(StrategySpecOperators, RocZeroDenominatorIsNaNNeverInf) {
    WindowState state = make_window(2);
    double pool[2]{};
    step_roc(state, pool, 0.0);
    step_roc(state, pool, 5.0);
    const double result = step_roc(state, pool, 10.0);  // 10 / x[t-2]=0.0 -> NaN, not inf
    EXPECT_TRUE(std::isnan(result));
}

// --- lag ---

TEST(StrategySpecOperators, LagN2DelaysExactly) {
    WindowState state = make_window(2);
    double pool[2]{};
    const double inputs[] = {10.0, 20.0, 30.0, 40.0};
    EXPECT_TRUE(std::isnan(step_lag(state, pool, inputs[0])));
    EXPECT_TRUE(std::isnan(step_lag(state, pool, inputs[1])));
    EXPECT_DOUBLE_EQ(step_lag(state, pool, inputs[2]), 10.0);
    EXPECT_DOUBLE_EQ(step_lag(state, pool, inputs[3]), 20.0);
}

// --- ema ---

TEST(StrategySpecOperators, EmaWindow3SeedsWithSmaThenRecurses) {
    EmaState state{};
    const double window = 3.0;
    EXPECT_TRUE(std::isnan(step_ema(state, window, 1.0)));
    EXPECT_TRUE(std::isnan(step_ema(state, window, 2.0)));
    EXPECT_DOUBLE_EQ(step_ema(state, window, 3.0), 2.0);  // seed = mean(1,2,3)
    // alpha = 2/(3+1) = 0.5; ema[3] = 0.5*4 + 0.5*2.0 = 3.0
    EXPECT_DOUBLE_EQ(step_ema(state, window, 4.0), 3.0);
    // ema[4] = 0.5*5 + 0.5*3.0 = 4.0
    EXPECT_DOUBLE_EQ(step_ema(state, window, 5.0), 4.0);
}

// --- rsi ---

TEST(StrategySpecOperators, RsiWindow3SeedsFromFirstThreeDeltas) {
    RsiState state{};
    const double window = 3.0;
    // prices: 10, 11, 10, 11 -> deltas: +1, -1, +1 (gains {1,0,1}, losses {0,1,0})
    EXPECT_TRUE(std::isnan(step_rsi(state, window, 10.0)));  // first observation, no delta yet
    EXPECT_TRUE(std::isnan(step_rsi(state, window, 11.0)));  // delta 1 of 3
    EXPECT_TRUE(std::isnan(step_rsi(state, window, 10.0)));  // delta 2 of 3
    const double avg_gain = (1.0 + 0.0 + 1.0) / 3.0;
    const double avg_loss = (0.0 + 1.0 + 0.0) / 3.0;
    const double expected_seed = 100.0 - 100.0 / (1.0 + avg_gain / avg_loss);
    EXPECT_DOUBLE_EQ(step_rsi(state, window, 11.0), expected_seed);  // delta 3 of 3 -> seeds
}

TEST(StrategySpecOperators, RsiAvgLossZeroYieldsHundred) {
    RsiState state{};
    const double window = 2.0;
    step_rsi(state, window, 10.0);
    step_rsi(state, window, 11.0);            // delta +1
    const double result = step_rsi(state, window, 12.0);  // delta +1 -> avg_loss stays 0
    EXPECT_DOUBLE_EQ(result, 100.0);
}

// --- binary arithmetic ---

TEST(StrategySpecOperators, ArithmeticOpsPropagateNaNViaIeee754) {
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::Add, 2.0, 3.0), 5.0);
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::Sub, 5.0, 3.0), 2.0);
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::Mul, 2.0, 3.0), 6.0);
    EXPECT_TRUE(std::isnan(apply_binary(SpecOp::Add, kNaN, 3.0)));
}

TEST(StrategySpecOperators, DivByZeroIsNaNNeverInf) {
    const double result = apply_binary(SpecOp::Div, 5.0, 0.0);
    EXPECT_TRUE(std::isnan(result));
    EXPECT_FALSE(std::isinf(result));
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::Div, 6.0, 3.0), 2.0);
}

TEST(StrategySpecOperators, ComparisonOpsRequireExplicitNaNGuard) {
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::Gt, 5.0, 3.0), 1.0);
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::Gt, 3.0, 5.0), 0.0);
    EXPECT_TRUE(std::isnan(apply_binary(SpecOp::Gt, kNaN, 5.0)));
    EXPECT_TRUE(std::isnan(apply_binary(SpecOp::Lt, 5.0, kNaN)));
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::Ge, 5.0, 5.0), 1.0);
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::Le, 5.0, 5.0), 1.0);
}

TEST(StrategySpecOperators, LogicalOpsNoThreeValuedShortCircuit) {
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::And, 1.0, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::And, 1.0, 0.0), 0.0);
    EXPECT_TRUE(std::isnan(apply_binary(SpecOp::And, 0.0, kNaN)));  // NOT short-circuited to 0
    EXPECT_DOUBLE_EQ(apply_binary(SpecOp::Or, 0.0, 1.0), 1.0);
    EXPECT_TRUE(std::isnan(apply_binary(SpecOp::Or, 1.0, kNaN)));  // NOT short-circuited to 1
}

TEST(StrategySpecOperators, NotFlipsAndPropagatesNaN) {
    EXPECT_DOUBLE_EQ(apply_not(1.0), 0.0);
    EXPECT_DOUBLE_EQ(apply_not(0.0), 1.0);
    EXPECT_TRUE(std::isnan(apply_not(kNaN)));
}

TEST(StrategySpecOperators, IfThenElseSelectsOnExactlyOnePointZero) {
    EXPECT_DOUBLE_EQ(apply_if_then_else(1.0, 10.0, 20.0), 10.0);
    EXPECT_DOUBLE_EQ(apply_if_then_else(0.0, 10.0, 20.0), 20.0);
    EXPECT_TRUE(std::isnan(apply_if_then_else(kNaN, 10.0, 20.0)));
}

// --- crosses_above / crosses_below ---

TEST(StrategySpecOperators, CrossesAboveTrueOnlyOnCrossingBar) {
    CrossesState state{};
    // a: 1, 3, 2 ; b: 2, 2, 2  -> a crosses above b exactly at bar 1 (1<=2 then 3>2).
    EXPECT_TRUE(std::isnan(step_crosses(SpecOp::CrossesAbove, state, 1.0, 2.0)));  // bar 0, no prev
    EXPECT_DOUBLE_EQ(step_crosses(SpecOp::CrossesAbove, state, 3.0, 2.0), 1.0);    // crossed
    EXPECT_DOUBLE_EQ(step_crosses(SpecOp::CrossesAbove, state, 2.0, 2.0), 0.0);    // not >, no cross
}

TEST(StrategySpecOperators, CrossesBelowMirrorsCrossesAbove) {
    CrossesState state{};
    EXPECT_TRUE(std::isnan(step_crosses(SpecOp::CrossesBelow, state, 3.0, 2.0)));
    EXPECT_DOUBLE_EQ(step_crosses(SpecOp::CrossesBelow, state, 1.0, 2.0), 1.0);  // 3>=2 then 1<2
}

TEST(StrategySpecOperators, CrossesAnyNaNAmongFourNeededValuesIsNaN) {
    CrossesState state{};
    step_crosses(SpecOp::CrossesAbove, state, 1.0, 2.0);
    EXPECT_TRUE(std::isnan(step_crosses(SpecOp::CrossesAbove, state, kNaN, 2.0)));
}
