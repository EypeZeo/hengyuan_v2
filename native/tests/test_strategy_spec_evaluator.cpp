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

// --- 批次 6 6b-0c: warmup_complete() ---------------------------------------------------------
// Builds a minimal valid spec whose [signal] node is the LAST indicator in `indicators_toml`.
namespace {

std::string spec_with_indicators(const std::string& indicators_toml, const std::string& signal_node) {
    return std::string(R"(
spec_version = 1
name = "warmup_probe"

[market]
market    = "crypto_spot"
symbol    = "BTCUSDT"
timeframe = "1h"
)") + indicators_toml + "\n[signal]\nnode = \"" + signal_node + R"("
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
}

// A deterministic, never-flat price series (flat data would make rsi/stddev degenerate).
Bar probe_bar(int i) {
    const double close = 100.0 + 0.7 * static_cast<double>(i % 11) - 0.3 * static_cast<double>(i % 5) +
                         0.05 * static_cast<double>(i);
    Bar b{};
    b.open = close - 0.2 + 0.1 * static_cast<double>(i % 3);
    b.high = close + 0.5;
    b.low = close - 0.5;
    b.close = close;
    b.volume = 1000.0 + static_cast<double>(i);
    return b;
}

struct ProbeResult {
    std::uint32_t warmup{0};
    int first_defined_seen{-1};   // seen_bars at the first bar whose signal node value is not NaN
    int first_complete_seen{-1};  // seen_bars at the first bar where warmup_complete() is true
    bool complete_but_undefined{false};  // the safety violation: complete while node value is NaN
};

ProbeResult probe(const std::string& indicators_toml, const std::string& signal_node) {
    const auto load = load_strategy_spec(spec_with_indicators(indicators_toml, signal_node));
    EXPECT_TRUE(load.ok());
    StreamingEvaluator evaluator;
    EXPECT_TRUE(evaluator.init(load.dag));
    ProbeResult r;
    r.warmup = evaluator.effective_warmup();
    EXPECT_FALSE(evaluator.warmup_complete());  // nothing fed yet
    const std::size_t sig = load.dag.signal_node_idx;
    for (int i = 0; i < static_cast<int>(r.warmup) + 40; ++i) {
        evaluator.step(probe_bar(i));
        const int seen = static_cast<int>(evaluator.seen_bars());
        const bool defined = !std::isnan(evaluator.node_value(sig));
        if (defined && r.first_defined_seen < 0) r.first_defined_seen = seen;
        if (evaluator.warmup_complete()) {
            if (r.first_complete_seen < 0) r.first_complete_seen = seen;
            if (!defined) r.complete_but_undefined = true;
        }
    }
    return r;
}

}  // namespace

// The safety property: whenever warmup_complete() is true, the signal node is actually defined.
// Checked across every operator shape whose warm-up arithmetic differs.
TEST(StrategySpecEvaluatorWarmup, CompleteImpliesSignalNodeIsDefinedForEveryOperatorShape) {
    const struct { const char* name; std::string toml; const char* signal; } cases[] = {
        {"sma", "\n[[indicators]]\nid = \"s\"\nop = \"sma\"\ninput = \"close\"\nwindow = 5\n", "s"},
        {"ema", "\n[[indicators]]\nid = \"s\"\nop = \"ema\"\ninput = \"close\"\nwindow = 5\n", "s"},
        {"stddev", "\n[[indicators]]\nid = \"s\"\nop = \"stddev\"\ninput = \"close\"\nwindow = 5\n", "s"},
        {"rolling_max", "\n[[indicators]]\nid = \"s\"\nop = \"rolling_max\"\ninput = \"close\"\nwindow = 5\n", "s"},
        {"rolling_min", "\n[[indicators]]\nid = \"s\"\nop = \"rolling_min\"\ninput = \"close\"\nwindow = 5\n", "s"},
        {"roc", "\n[[indicators]]\nid = \"s\"\nop = \"roc\"\ninput = \"close\"\nwindow = 5\n", "s"},
        {"rsi", "\n[[indicators]]\nid = \"s\"\nop = \"rsi\"\ninput = \"close\"\nwindow = 5\n", "s"},
        {"lag", "\n[[indicators]]\nid = \"s\"\nop = \"lag\"\ninput = \"close\"\nn = 3\n", "s"},
        {"crosses_above_raw",
         "\n[[indicators]]\nid = \"s\"\nop = \"crosses_above\"\nleft = \"close\"\nright = \"open\"\n", "s"},
        {"crosses_above_sma",
         "\n[[indicators]]\nid = \"f\"\nop = \"sma\"\ninput = \"close\"\nwindow = 3\n"
         "\n[[indicators]]\nid = \"g\"\nop = \"sma\"\ninput = \"close\"\nwindow = 6\n"
         "\n[[indicators]]\nid = \"s\"\nop = \"crosses_above\"\nleft = \"f\"\nright = \"g\"\n",
         "s"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const ProbeResult r = probe(c.toml, c.signal);
        EXPECT_FALSE(r.complete_but_undefined);
        EXPECT_GE(r.first_complete_seen, 1);
        // Never earlier than the node's first defined value.
        EXPECT_GE(r.first_complete_seen, r.first_defined_seen);
        // ...and conservative by AT MOST one bar past it (so this gate never costs more than one
        // extra bar of waiting).
        EXPECT_LE(r.first_complete_seen, r.first_defined_seen + 1);
    }
}

// Pins the two facts warmup_complete()'s comment relies on -- why the boundary is `>` and not `>=`.
TEST(StrategySpecEvaluatorWarmup, WindowAverageIsDefinedOnTheWarmupBarSoTheGateIsOneBarConservativeThere) {
    const ProbeResult r = probe("\n[[indicators]]\nid = \"s\"\nop = \"sma\"\ninput = \"close\"\nwindow = 5\n", "s");
    EXPECT_EQ(r.warmup, 5u);
    EXPECT_EQ(r.first_defined_seen, 5);   // defined exactly when seen_bars == W
    EXPECT_EQ(r.first_complete_seen, 6);  // the gate waits one extra bar
}

TEST(StrategySpecEvaluatorWarmup, LagAndCrossesOverRawFieldsUndercountByOneSoGreaterOrEqualWouldBeUnsafe) {
    const ProbeResult lag = probe("\n[[indicators]]\nid = \"s\"\nop = \"lag\"\ninput = \"close\"\nn = 3\n", "s");
    EXPECT_EQ(lag.warmup, 3u);
    EXPECT_EQ(lag.first_defined_seen, 4);   // still undefined when seen_bars == W
    EXPECT_EQ(lag.first_complete_seen, 4);  // `>` is exact here; `>=` would have fired one bar early

    const ProbeResult crosses = probe(
        "\n[[indicators]]\nid = \"s\"\nop = \"crosses_above\"\nleft = \"close\"\nright = \"open\"\n", "s");
    EXPECT_EQ(crosses.warmup, 1u);
    EXPECT_EQ(crosses.first_defined_seen, 2);
    EXPECT_EQ(crosses.first_complete_seen, 2);
}

TEST(StrategySpecEvaluatorWarmup, NotCompleteBeforeInitAndAfterReset) {
    const auto load = load_strategy_spec(kWorkedExample);
    ASSERT_TRUE(load.ok());
    StreamingEvaluator evaluator;
    EXPECT_FALSE(evaluator.warmup_complete());  // never init()'d
    ASSERT_TRUE(evaluator.init(load.dag));
    for (int i = 0; i < 80; ++i) evaluator.step(probe_bar(i));
    EXPECT_TRUE(evaluator.warmup_complete());
    evaluator.reset();  // the restart / gap-recovery path
    EXPECT_FALSE(evaluator.warmup_complete());
    EXPECT_EQ(evaluator.seen_bars(), 0u);
}
