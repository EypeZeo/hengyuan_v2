// Batch 5, 5b: strategy_spec_evaluator.cpp unit tests -- DAG-level warm-up composition,
// [signal] mode application, and StreamingEvaluator::init()'s capacity gate. Uses
// docs/STRATEGY_SPEC.md §7's worked example as the composed-DAG case, matching the exact
// hand-computed warm-up value (50) the doc itself states.

#include <gtest/gtest.h>
#include <hengyuan/strategy_spec_evaluator.hpp>
#include <hengyuan/strategy_spec_toml_parser.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace hy;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// docs/STRATEGY_SPEC.md §7's worked example, verbatim (same text test_strategy_spec_toml_
// parser.cpp uses) -- fast=sma(20), slow=sma(50), rsi14=rsi(14),
// not_overbought=lt(rsi14,70), trend_up=gt(fast,slow), entry=and(trend_up,not_overbought).
constexpr std::string_view kWorkedExample = R"(
spec_version = 1
name = "sma_crossover_btc_1h"

[market]
market    = "crypto_spot"
symbol    = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "fast"
op = "sma"
input = "close"
window = 20

[[indicators]]
id = "slow"
op = "sma"
input = "close"
window = 50

[[indicators]]
id = "rsi14"
op = "rsi"
input = "close"
window = 14

[[indicators]]
id = "not_overbought"
op = "lt"
left = "rsi14"
right = 70.0

[[indicators]]
id = "trend_up"
op = "gt"
left = "fast"
right = "slow"

[[indicators]]
id = "entry"
op = "and"
left = "trend_up"
right = "not_overbought"

[signal]
node = "entry"
mode = "boolean"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "walk_forward+cpcv"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0000000000000000000000000000000000000000000000000000000000000000"
oos_sharpe     = 1.34
pbo            = 0.21
trials         = 480
)";

}  // namespace

TEST(StrategySpecEvaluator, EffectiveWarmupMatchesWorkedExampleHandComputedValue) {
    const auto load = load_strategy_spec(kWorkedExample);
    ASSERT_TRUE(load.ok());

    StreamingEvaluator evaluator;
    ASSERT_TRUE(evaluator.init(load.dag));
    // docs/STRATEGY_SPEC.md §7: max(sma50=50, rsi14=15) = 50.
    EXPECT_EQ(evaluator.effective_warmup(), 50u);
}

TEST(StrategySpecEvaluator, EntryNodeStaysNaNUntilWarmupBoundaryThenProducesRealValues) {
    const auto load = load_strategy_spec(kWorkedExample);
    ASSERT_TRUE(load.ok());
    // "entry" is nodes[5] per the declared order (fast, slow, rsi14, not_overbought,
    // trend_up, entry) -- confirmed against test_strategy_spec_toml_parser.cpp's own
    // WorkedExampleLoadsSuccessfully assertion on the same fixture text.
    constexpr std::size_t kEntryIdx = 5;
    ASSERT_EQ(std::string_view(load.dag.nodes[kEntryIdx].id), "entry");

    StreamingEvaluator evaluator;
    ASSERT_TRUE(evaluator.init(load.dag));

    // A mildly upward-drifting synthetic close series -- real values only matter in that
    // they're never exactly flat (which would make rsi's avg_loss==0 degenerate case fire on
    // every bar) and never repeat exactly the pattern that would zero out a denominator.
    double close = 100.0;
    for (int i = 0; i < 60; ++i) {
        close += (i % 7 == 0) ? -0.5 : 0.3;
        Bar bar{};
        bar.open = close;
        bar.high = close + 0.1;
        bar.low = close - 0.1;
        bar.close = close;
        bar.volume = 1000.0;
        evaluator.step(bar);

        const double entry_value = evaluator.node_value(kEntryIdx);
        if (i < 49) {
            EXPECT_TRUE(std::isnan(entry_value)) << "bar " << i << " expected still warming up";
        } else {
            EXPECT_FALSE(std::isnan(entry_value)) << "bar " << i << " expected past warm-up";
        }
    }
}

TEST(StrategySpecEvaluator, ApplySignalModeBooleanMapping) {
    EXPECT_DOUBLE_EQ(apply_signal_mode(SignalMode::Boolean, kNaN), 0.0);
    EXPECT_DOUBLE_EQ(apply_signal_mode(SignalMode::Boolean, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(apply_signal_mode(SignalMode::Boolean, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(apply_signal_mode(SignalMode::Boolean, -3.5), 1.0);  // any nonzero -> 1.0
}

TEST(StrategySpecEvaluator, ApplySignalModeScaledClampsToUnitRange) {
    EXPECT_DOUBLE_EQ(apply_signal_mode(SignalMode::Scaled, kNaN), 0.0);
    EXPECT_DOUBLE_EQ(apply_signal_mode(SignalMode::Scaled, 0.5), 0.5);
    EXPECT_DOUBLE_EQ(apply_signal_mode(SignalMode::Scaled, 3.0), 1.0);
    EXPECT_DOUBLE_EQ(apply_signal_mode(SignalMode::Scaled, -3.0), -1.0);
}

TEST(StrategySpecEvaluator, ResetClearsRuntimeStateProducingIdenticalReplay) {
    const auto load = load_strategy_spec(kWorkedExample);
    ASSERT_TRUE(load.ok());

    StreamingEvaluator evaluator;
    ASSERT_TRUE(evaluator.init(load.dag));

    auto feed_bars = [](StreamingEvaluator& ev, int count) {
        double close = 100.0;
        std::vector<double> outputs;
        for (int i = 0; i < count; ++i) {
            close += 0.3;
            Bar bar{};
            bar.open = bar.high = bar.low = bar.close = close;
            bar.volume = 1000.0;
            outputs.push_back(ev.step(bar));
        }
        return outputs;
    };

    const auto first_run = feed_bars(evaluator, 55);
    ASSERT_TRUE(evaluator.init(load.dag));  // re-init == fresh state, same as reset()+reload
    const auto second_run = feed_bars(evaluator, 55);
    ASSERT_EQ(first_run.size(), second_run.size());
    for (std::size_t i = 0; i < first_run.size(); ++i) {
        EXPECT_DOUBLE_EQ(first_run[i], second_run[i]) << "bar " << i;
    }

    // reset() alone (no re-init) must leave the evaluator uninitialized -- step() after a bare
    // reset() is a caller contract violation this test does not need to exercise further than
    // confirming is_initialized() reports it accurately.
    evaluator.reset();
    EXPECT_FALSE(evaluator.is_initialized());
}

TEST(StrategySpecEvaluator, InitRejectsSingleWindowExceedingHistoryPoolCapacity) {
    // kHistoryPoolCapacity is 1024 -- a single node asking for a 2000-bar window can never fit.
    constexpr std::string_view kOversizedSpec = R"(
spec_version = 1
name = "oversized_window"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "huge"
op = "sma"
input = "close"
window = 2000

[signal]
node = "huge"
mode = "scaled"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "x"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0"
oos_sharpe     = 1.0
pbo            = 0.1
trials         = 1
)";
    const auto load = load_strategy_spec(kOversizedSpec);
    ASSERT_TRUE(load.ok());  // 5a's own load gate has no upper bound tied to the pool

    StreamingEvaluator evaluator;
    EXPECT_FALSE(evaluator.init(load.dag));
    EXPECT_FALSE(evaluator.is_initialized());
}

TEST(StrategySpecEvaluator, InitRejectsWindowExceedingUint16Range) {
    constexpr std::string_view kSpec = R"(
spec_version = 1
name = "window_too_wide_for_u16"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "huge"
op = "sma"
input = "close"
window = 70000

[signal]
node = "huge"
mode = "scaled"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "x"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0"
oos_sharpe     = 1.0
pbo            = 0.1
trials         = 1
)";
    const auto load = load_strategy_spec(kSpec);
    ASSERT_TRUE(load.ok());

    StreamingEvaluator evaluator;
    EXPECT_FALSE(evaluator.init(load.dag));
}
