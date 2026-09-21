// 批次 6 6b-0f-3b: kline_feed_sync.hpp tests -- Boost-free, no network, no threads.
//
// The property under test is one sentence: the evaluator never sees a hole, a duplicate or a
// malformed bar, and whenever it might have it is rebuilt from history. Almost every test therefore
// compares the synced evaluator against a REFERENCE evaluator that was simply fed the same contiguous
// bars in order -- if the two agree on seen_bars() and every node value, nothing was lost, repeated
// or reordered on the way. The worked-example spec from docs/STRATEGY_SPEC.md §7 (sma20/sma50/rsi14,
// warm-up 50) is used throughout, the same one the evaluator's own tests use.

#include <gtest/gtest.h>
#include <hengyuan/kline_feed_sync.hpp>
#include <hengyuan/strategy_spec_toml_parser.hpp>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

using hy::BackfillApplyStatus;
using hy::KlineFeedSync;
using hy::KlineSyncInvalidReason;
using hy::KlineSyncState;
using hy::KlineWsEvent;
using hy::LiveBarAction;
using hy::StreamingEvaluator;

namespace {

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
mode = "scaled"

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

// Same shape as the worked example but a single 2000-bar window: loads, but cannot fit the
// evaluator's history pool (kHistoryPoolCapacity = 1024), so init() refuses it.
constexpr std::string_view kOversized = R"(
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

constexpr std::int64_t kHour = 3'600'000;

// Bar `i` of a deterministic, contiguous 1h series that actually moves (so windows, RSI and the
// crossover all take non-trivial values).
KlineWsEvent bar(std::size_t i) {
    KlineWsEvent e;
    const double x = static_cast<double>(i);
    e.open_time_ms = static_cast<std::int64_t>(i) * kHour;
    e.close_time_ms = e.open_time_ms + kHour - 1;
    e.close = 100.0 + 10.0 * std::sin(x * 0.3) + static_cast<double>(i % 5) * 0.1;
    e.open = 100.0 + 10.0 * std::sin((x - 1.0) * 0.3);
    e.high = (e.open > e.close ? e.open : e.close) + 0.5;
    e.low = (e.open < e.close ? e.open : e.close) - 0.5;
    e.volume = 10.0 + static_cast<double>(i % 3);
    e.symbol_id = 7;
    e.is_closed = true;
    return e;
}

std::vector<KlineWsEvent> bars(std::size_t first, std::size_t count) {
    std::vector<KlineWsEvent> v;
    v.reserve(count);
    for (std::size_t i = 0; i < count; ++i) v.push_back(bar(first + i));
    return v;
}

bool same_double(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

bool same_state(const StreamingEvaluator& a, const StreamingEvaluator& b, std::size_t node_count) {
    if (a.is_initialized() != b.is_initialized() || a.seen_bars() != b.seen_bars() ||
        a.warmup_complete() != b.warmup_complete()) {
        return false;
    }
    for (std::size_t i = 0; i < node_count; ++i) {
        if (!same_double(a.node_value(i), b.node_value(i))) return false;
    }
    return true;
}

struct Rig {
    explicit Rig(std::string_view spec = kWorkedExample)
        : load(hy::load_strategy_spec(spec)), sync(evaluator, load.dag) {}

    std::size_t nodes() const { return load.dag.node_count; }

    // A reference evaluator fed bars [first, first+count) straight through.
    StreamingEvaluator reference(std::size_t first, std::size_t count) const {
        StreamingEvaluator ref;
        EXPECT_TRUE(ref.init(load.dag));
        for (std::size_t i = 0; i < count; ++i) {
            const KlineWsEvent b = bar(first + i);
            (void)ref.step(hy::Bar{b.open, b.high, b.low, b.close, b.volume});
        }
        return ref;
    }

    hy::SpecLoadResult load;  // declared first: the evaluator and the sync refer to it
    StreamingEvaluator evaluator;
    KlineFeedSync sync;
};

}  // namespace

// --- startup ---------------------------------------------------------------------------------------

TEST(KlineFeedSync, StartsNeedingABackfillWithAnEmptyEvaluator) {
    Rig rig;
    ASSERT_TRUE(rig.load.ok());
    EXPECT_EQ(rig.sync.state(), KlineSyncState::NeedsBackfill);
    EXPECT_TRUE(rig.sync.needs_backfill());
    EXPECT_FALSE(rig.sync.live());
    EXPECT_EQ(rig.sync.last_invalid_reason(), KlineSyncInvalidReason::Startup);
    EXPECT_EQ(rig.sync.last_close_time_ms(), 0);
    EXPECT_FALSE(rig.evaluator.is_initialized());
    EXPECT_EQ(rig.evaluator.seen_bars(), 0U);
    EXPECT_FALSE(rig.evaluator.warmup_complete());
}

TEST(KlineFeedSync, TheConstructorTakesOverAnEvaluatorThatWasAlreadyRunning) {
    hy::SpecLoadResult load = hy::load_strategy_spec(kWorkedExample);
    ASSERT_TRUE(load.ok());
    StreamingEvaluator evaluator;
    ASSERT_TRUE(evaluator.init(load.dag));
    for (const KlineWsEvent& b : bars(0, 70)) (void)evaluator.step(hy::Bar{b.open, b.high, b.low, b.close, b.volume});
    ASSERT_TRUE(evaluator.warmup_complete());

    KlineFeedSync sync(evaluator, load.dag);
    EXPECT_FALSE(evaluator.warmup_complete()) << "stale state survived the takeover";
    EXPECT_EQ(evaluator.seen_bars(), 0U);
}

TEST(KlineFeedSync, BackfillBarsWantedCoversWarmupPlusOnePlusTheSlack) {
    Rig rig;
    ASSERT_TRUE(rig.load.ok());
    // warm-up 50 -> 51 bars make warmup_complete() true (seen > 50), +1 forming candle, +1 bar that
    // closed inside the REST safety margin.
    EXPECT_EQ(hy::compute_effective_warmup(rig.load.dag), 50U);
    EXPECT_EQ(rig.sync.backfill_bars_wanted(), 50U + 1U + KlineFeedSync::kBackfillSlackBars);
    EXPECT_EQ(KlineFeedSync::kBackfillSlackBars, 2U);
}

TEST(KlineFeedSync, LiveBarsBeforeTheFirstBackfillAreDroppedNotConsumed) {
    Rig rig;
    for (std::size_t i = 0; i < 5; ++i) {
        const auto r = rig.sync.on_live_bar(bar(i));
        EXPECT_EQ(r.action, LiveBarAction::NotLive);
    }
    EXPECT_EQ(rig.sync.stats().dropped_not_live, 5U);
    EXPECT_EQ(rig.sync.stats().bars_stepped, 0U);
    EXPECT_EQ(rig.evaluator.seen_bars(), 0U);
    EXPECT_FALSE(rig.sync.live());
}

// --- backfill ---------------------------------------------------------------------------------------------

TEST(KlineFeedSync, ABackfillRebuildsTheEvaluatorExactlyAsAStraightFeedWould) {
    Rig rig;
    ASSERT_TRUE(rig.load.ok());
    const auto history = bars(0, 60);
    const auto r = rig.sync.apply_backfill(history);

    ASSERT_EQ(r.status, BackfillApplyStatus::Applied);
    EXPECT_EQ(r.bars_applied, 60U);
    EXPECT_TRUE(r.warmup_complete);
    EXPECT_EQ(r.last_close_time_ms, history.back().close_time_ms);
    EXPECT_TRUE(rig.sync.live());
    EXPECT_EQ(rig.sync.last_close_time_ms(), history.back().close_time_ms);
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 60), rig.nodes()));
    // The trap: reset() clears the DAG, so a rebuild must have re-initialised it.
    EXPECT_TRUE(rig.evaluator.is_initialized());
    EXPECT_EQ(rig.evaluator.effective_warmup(), 50U);
    EXPECT_EQ(rig.sync.stats().backfills_applied, 1U);
    EXPECT_EQ(rig.sync.stats().backfill_bars_replayed, 60U);
}

TEST(KlineFeedSync, AShortBackfillGoesLiveWithWarmupIncompleteAndLiveBarsFinishIt) {
    Rig rig;
    const auto r = rig.sync.apply_backfill(bars(0, 10));
    ASSERT_EQ(r.status, BackfillApplyStatus::Applied);
    EXPECT_FALSE(r.warmup_complete);
    EXPECT_TRUE(rig.sync.live());
    EXPECT_FALSE(rig.evaluator.warmup_complete());

    // 41 more bars: seen goes 10 -> 51 > 50.
    for (std::size_t i = 10; i < 51; ++i) {
        ASSERT_EQ(rig.sync.on_live_bar(bar(i)).action, LiveBarAction::Stepped) << "bar " << i;
    }
    EXPECT_TRUE(rig.evaluator.warmup_complete());
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 51), rig.nodes()));
}

TEST(KlineFeedSync, LiveBarsContinueTheSequenceAndReturnTheEvaluatorsOwnTargetPosition) {
    Rig rig;
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);
    StreamingEvaluator ref = rig.reference(0, 60);
    for (std::size_t i = 60; i < 140; ++i) {
        const KlineWsEvent b = bar(i);
        const auto live = rig.sync.on_live_bar(b);
        const double expected = ref.step(hy::Bar{b.open, b.high, b.low, b.close, b.volume});
        ASSERT_EQ(live.action, LiveBarAction::Stepped) << "bar " << i;
        ASSERT_TRUE(same_double(live.target_position, expected)) << "bar " << i;
    }
    EXPECT_TRUE(same_state(rig.evaluator, ref, rig.nodes()));
    EXPECT_EQ(rig.sync.stats().bars_stepped, 80U);
    EXPECT_EQ(rig.sync.last_close_time_ms(), bar(139).close_time_ms);
}

// --- duplicates & the ring join ------------------------------------------------------------------------------

TEST(KlineFeedSync, DuplicateAndOlderBarsAreSkippedAndChangeNothing) {
    Rig rig;
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);

    // The ring may still hold bars the backfill already contains.
    EXPECT_EQ(rig.sync.on_live_bar(bar(59)).action, LiveBarAction::Duplicate);  // the baseline itself
    EXPECT_EQ(rig.sync.on_live_bar(bar(58)).action, LiveBarAction::Duplicate);
    EXPECT_EQ(rig.sync.on_live_bar(bar(0)).action, LiveBarAction::Duplicate);
    EXPECT_TRUE(rig.sync.live());
    EXPECT_EQ(rig.sync.stats().duplicates_skipped, 3U);
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 60), rig.nodes())) << "a duplicate was consumed";

    EXPECT_EQ(rig.sync.on_live_bar(bar(60)).action, LiveBarAction::Stepped);  // the join continues
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 61), rig.nodes()));
}

TEST(KlineFeedSync, ABarThatOverlapsTheBaselineButEndsAfterItIsAGapNotADuplicate) {
    Rig rig;
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);
    KlineWsEvent misaligned = bar(60);
    misaligned.open_time_ms += 1000;  // not exactly adjacent to the baseline, and it ends after it
    misaligned.close_time_ms += 1000;
    EXPECT_EQ(rig.sync.on_live_bar(misaligned).action, LiveBarAction::Gap);
    EXPECT_FALSE(rig.sync.live());
}

// --- gaps & recovery --------------------------------------------------------------------------------------------

TEST(KlineFeedSync, AHoleInvalidatesTheStateAndDropsTheOffendingBar) {
    Rig rig;
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);

    const auto r = rig.sync.on_live_bar(bar(61));  // bar 60 is missing
    EXPECT_EQ(r.action, LiveBarAction::Gap);
    EXPECT_EQ(rig.sync.state(), KlineSyncState::NeedsBackfill);
    EXPECT_EQ(rig.sync.last_invalid_reason(), KlineSyncInvalidReason::ConsumerGap);
    EXPECT_EQ(rig.sync.stats().gaps_detected, 1U);
    EXPECT_EQ(rig.sync.last_close_time_ms(), 0);
    // The planner is blocked from this instant: nothing stale is left to read.
    EXPECT_FALSE(rig.evaluator.warmup_complete());
    EXPECT_EQ(rig.evaluator.seen_bars(), 0U);

    EXPECT_EQ(rig.sync.on_live_bar(bar(62)).action, LiveBarAction::NotLive);  // the caller must not pop now
    EXPECT_EQ(rig.sync.stats().dropped_not_live, 1U);
    EXPECT_EQ(rig.sync.stats().bars_stepped, 0U);
}

TEST(KlineFeedSync, RecoveryAfterAGapRebuildsFromAFreshBackfillAndLeavesNoTrace) {
    Rig rig;
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);
    ASSERT_EQ(rig.sync.on_live_bar(bar(62)).action, LiveBarAction::Gap);

    // The new history contains the missed bars AND the bar that revealed the gap.
    const auto r = rig.sync.apply_backfill(bars(0, 63));
    ASSERT_EQ(r.status, BackfillApplyStatus::Applied);
    EXPECT_TRUE(r.warmup_complete);
    EXPECT_TRUE(rig.sync.live());
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 63), rig.nodes()));

    EXPECT_EQ(rig.sync.on_live_bar(bar(63)).action, LiveBarAction::Stepped);
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 64), rig.nodes()));
}

TEST(KlineFeedSync, ABackfillThatMissesTheLatestBarsJustProducesAnotherRound) {
    // The REST fetch's safety margin can drop a bar that closed moments ago; the live stream's first
    // bar then does not join. That is a Gap, not corruption: back to NeedsBackfill.
    Rig rig;
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);
    EXPECT_EQ(rig.sync.on_live_bar(bar(62)).action, LiveBarAction::Gap);  // 60 and 61 never arrived
    EXPECT_FALSE(rig.sync.live());
    EXPECT_EQ(rig.sync.stats().gaps_detected, 1U);
}

TEST(KlineFeedSync, InvalidatingFromLiveResetsTheEvaluatorImmediately) {
    Rig rig;
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);
    ASSERT_TRUE(rig.evaluator.warmup_complete());

    rig.sync.invalidate(KlineSyncInvalidReason::SessionSuspended);
    EXPECT_EQ(rig.sync.state(), KlineSyncState::NeedsBackfill);
    EXPECT_FALSE(rig.evaluator.warmup_complete());
    EXPECT_EQ(rig.evaluator.seen_bars(), 0U);
    EXPECT_EQ(rig.sync.last_invalid_reason(), KlineSyncInvalidReason::SessionSuspended);

    rig.sync.invalidate(KlineSyncInvalidReason::Manual);  // idempotent in effect, counted each time
    rig.sync.invalidate(KlineSyncInvalidReason::Manual);
    EXPECT_EQ(rig.sync.stats().invalidations, 3U);
    EXPECT_EQ(rig.sync.stats().session_suspended_invalidations, 1U);
    EXPECT_EQ(rig.sync.stats().manual_invalidations, 2U);
    EXPECT_EQ(rig.sync.stats().gaps_detected, 0U);
    EXPECT_FALSE(rig.sync.live());

    // And the very same object recovers.
    ASSERT_EQ(rig.sync.apply_backfill(bars(100, 60)).status, BackfillApplyStatus::Applied);
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(100, 60), rig.nodes()));
}

// --- malformed input -----------------------------------------------------------------------------------------------

TEST(KlineFeedSync, MalformedLiveBarsAreDroppedWithoutInvalidatingOrBeingConsumed) {
    Rig rig;
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);

    const double kNan = std::numeric_limits<double>::quiet_NaN();
    const double kInf = std::numeric_limits<double>::infinity();
    std::vector<KlineWsEvent> bad(12, bar(60));
    bad[0].close = kNan;
    bad[1].high = kInf;
    bad[2].open = 0.0;
    bad[3].low = -1.0;
    bad[4].is_closed = false;
    bad[5].close_time_ms = bad[5].open_time_ms;  // does not move forward
    bad[6].high = bad[6].low - 1.0;              // high < low
    bad[7].open = bad[7].high + 5.0;             // open outside [low, high]
    bad[8].close = bad[8].low - 5.0;             // close outside [low, high]
    bad[9].volume = -1.0;
    bad[10].volume = kNan;
    bad[11].volume = kInf;
    for (std::size_t i = 0; i < bad.size(); ++i) {
        EXPECT_EQ(rig.sync.on_live_bar(bad[i]).action, LiveBarAction::Malformed) << "case " << i;
    }
    EXPECT_TRUE(rig.sync.live()) << "a malformed bar must not invalidate; the next good bar decides";
    EXPECT_EQ(rig.sync.stats().malformed_bars, bad.size());
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 60), rig.nodes()));
    EXPECT_EQ(rig.sync.on_live_bar(bar(60)).action, LiveBarAction::Stepped);
}

TEST(KlineFeedSync, ABackfillWhileLiveIsIgnoredUntouched) {
    Rig rig;
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);
    const auto r = rig.sync.apply_backfill(bars(500, 60));
    EXPECT_EQ(r.status, BackfillApplyStatus::NotNeeded);
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 60), rig.nodes()));
    EXPECT_EQ(rig.sync.last_close_time_ms(), bar(59).close_time_ms);
    EXPECT_EQ(rig.sync.stats().backfills_applied, 1U);
}

TEST(KlineFeedSync, AnEmptyBackfillChangesNothing) {
    Rig rig;
    const auto r = rig.sync.apply_backfill(std::span<const KlineWsEvent>{});
    EXPECT_EQ(r.status, BackfillApplyStatus::Empty);
    EXPECT_FALSE(rig.sync.live());
    EXPECT_FALSE(rig.evaluator.is_initialized());
}

TEST(KlineFeedSync, ARejectedBackfillNeverTouchesTheEvaluator) {
    Rig rig;

    auto with_hole = bars(0, 60);
    with_hole.erase(with_hole.begin() + 30);  // a missing bar in the middle
    EXPECT_EQ(rig.sync.apply_backfill(with_hole).status, BackfillApplyStatus::NotContiguous);

    auto overlapping = bars(0, 60);
    overlapping[30] = overlapping[29];  // a repeated bar
    EXPECT_EQ(rig.sync.apply_backfill(overlapping).status, BackfillApplyStatus::NotContiguous);

    auto poisoned = bars(0, 60);
    poisoned[45].close = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(rig.sync.apply_backfill(poisoned).status, BackfillApplyStatus::Malformed);

    auto unclosed = bars(0, 60);
    unclosed[59].is_closed = false;  // the forming candle must never be replayed
    EXPECT_EQ(rig.sync.apply_backfill(unclosed).status, BackfillApplyStatus::Malformed);

    // Nothing was half-applied: still needing a backfill, evaluator never even initialised.
    EXPECT_FALSE(rig.sync.live());
    EXPECT_FALSE(rig.evaluator.is_initialized());
    EXPECT_EQ(rig.evaluator.seen_bars(), 0U);
    EXPECT_EQ(rig.sync.stats().backfills_applied, 0U);

    // ...and a good one still applies afterwards.
    EXPECT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);
}

TEST(KlineFeedSync, ATimestampAtTheEndOfTimeIsNotContiguousAndDoesNotOverflow) {
    Rig rig;
    std::vector<KlineWsEvent> v = bars(0, 2);
    v[0].close_time_ms = std::numeric_limits<std::int64_t>::max();
    v[1].open_time_ms = std::numeric_limits<std::int64_t>::min();  // what +1 would wrap to
    EXPECT_EQ(rig.sync.apply_backfill(v).status, BackfillApplyStatus::NotContiguous);
    EXPECT_FALSE(rig.sync.live());
}

TEST(KlineFeedSync, AnEvaluatorThatRefusesToInitialiseLeavesTheSyncNeedingABackfill) {
    Rig rig(kOversized);
    ASSERT_TRUE(rig.load.ok());  // the loader has no upper bound tied to the pool; init() does
    EXPECT_EQ(rig.sync.backfill_bars_wanted(), 2000U + 1U + KlineFeedSync::kBackfillSlackBars);

    const auto r = rig.sync.apply_backfill(bars(0, 20));
    EXPECT_EQ(r.status, BackfillApplyStatus::EvaluatorInitFailed);
    EXPECT_FALSE(rig.sync.live());
    EXPECT_FALSE(rig.evaluator.is_initialized());
    EXPECT_EQ(rig.sync.stats().backfills_applied, 0U);
}

// --- the invariant, under a random walk ---------------------------------------------------------------------------------

// Whatever mix of live bars, duplicates, gaps, backfills and invalidations arrives, whenever the sync
// is Live the evaluator must equal a reference fed the contiguous history it claims to hold. The walk
// is deterministic (fixed-seed LCG), so a failure is reproducible.
TEST(KlineFeedSync, TheEvaluatorAlwaysEqualsAContiguousReferenceUnderARandomWalk) {
    Rig rig;
    ASSERT_TRUE(rig.load.ok());

    std::uint64_t rng = 0x243F6A8885A308D3ULL;
    auto next = [&rng](std::uint64_t bound) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        return (rng >> 33) % bound;
    };

    std::size_t first = 0;  // start index of the history the evaluator currently holds
    std::size_t last = 0;   // index of the last bar it consumed (valid while live)
    std::size_t newest_seen = 0;
    int applied = 0;
    int gaps = 0;
    int dups = 0;

    for (int step = 0; step < 4000; ++step) {
        if (!rig.sync.live()) {
            // Recover from a random start (long enough to matter, sometimes shorter than warm-up).
            const std::size_t count = 1 + static_cast<std::size_t>(next(90));
            const std::size_t end = newest_seen + static_cast<std::size_t>(next(3));
            const std::size_t start = end + 1 > count ? end + 1 - count : 0;
            const auto v = bars(start, end + 1 - start);
            ASSERT_EQ(rig.sync.apply_backfill(v).status, BackfillApplyStatus::Applied) << "step " << step;
            first = start;
            last = end;
            newest_seen = end;
            ++applied;
        } else {
            switch (next(10)) {
                case 0:
                case 1:
                case 2:
                case 3:
                case 4:
                case 5: {  // the next bar in sequence
                    ASSERT_EQ(rig.sync.on_live_bar(bar(last + 1)).action, LiveBarAction::Stepped) << "step " << step;
                    ++last;
                    newest_seen = last > newest_seen ? last : newest_seen;
                    break;
                }
                case 6:
                case 7: {  // a duplicate/older bar from the ring
                    const std::size_t idx = first + static_cast<std::size_t>(next(last - first + 1));
                    ASSERT_EQ(rig.sync.on_live_bar(bar(idx)).action, LiveBarAction::Duplicate) << "step " << step;
                    ++dups;
                    break;
                }
                case 8: {  // a hole
                    const std::size_t skip = 2 + static_cast<std::size_t>(next(4));
                    ASSERT_EQ(rig.sync.on_live_bar(bar(last + skip)).action, LiveBarAction::Gap) << "step " << step;
                    newest_seen = last + skip;
                    ++gaps;
                    break;
                }
                default:  // the caller learns bars were lost some other way
                    rig.sync.invalidate(KlineSyncInvalidReason::SessionSuspended);
                    break;
            }
        }

        if (rig.sync.live()) {
            ASSERT_TRUE(same_state(rig.evaluator, rig.reference(first, last - first + 1), rig.nodes()))
                << "step " << step << " history [" << first << ", " << last << "]";
        } else {
            ASSERT_FALSE(rig.evaluator.warmup_complete()) << "step " << step << ": stale state readable while not live";
            ASSERT_EQ(rig.evaluator.seen_bars(), 0U) << "step " << step;
        }
    }
    // The walk really exercised every branch, not just one.
    EXPECT_GT(applied, 20);
    EXPECT_GT(gaps, 20);
    EXPECT_GT(dups, 100);
}

// --- vocabulary --------------------------------------------------------------------------------------------------------------

TEST(KlineFeedSync, SaneClosedKlineAcceptsTheOrdinaryCaseAndZeroVolume) {
    KlineWsEvent e = bar(3);
    EXPECT_TRUE(hy::is_sane_closed_kline(e));
    e.volume = 0.0;  // a genuinely empty bar
    EXPECT_TRUE(hy::is_sane_closed_kline(e));
    e.open = e.high;  // boundary cases of the range checks
    e.close = e.low;
    EXPECT_TRUE(hy::is_sane_closed_kline(e));
}

TEST(KlineFeedSync, NamesAreDistinct) {
    const KlineSyncInvalidReason reasons[] = {KlineSyncInvalidReason::Startup, KlineSyncInvalidReason::ConsumerGap,
                                              KlineSyncInvalidReason::SessionSuspended,
                                              KlineSyncInvalidReason::Manual};
    for (std::size_t i = 0; i < std::size(reasons); ++i) {
        EXPECT_STRNE(hy::kline_sync_invalid_reason_name(reasons[i]), "?");
        for (std::size_t j = i + 1; j < std::size(reasons); ++j) {
            EXPECT_STRNE(hy::kline_sync_invalid_reason_name(reasons[i]), hy::kline_sync_invalid_reason_name(reasons[j]));
        }
    }
    const BackfillApplyStatus statuses[] = {BackfillApplyStatus::Applied,       BackfillApplyStatus::NotNeeded,
                                            BackfillApplyStatus::Empty,         BackfillApplyStatus::Malformed,
                                            BackfillApplyStatus::NotContiguous, BackfillApplyStatus::EvaluatorInitFailed};
    for (std::size_t i = 0; i < std::size(statuses); ++i) {
        EXPECT_STRNE(hy::backfill_apply_status_name(statuses[i]), "?");
        for (std::size_t j = i + 1; j < std::size(statuses); ++j) {
            EXPECT_STRNE(hy::backfill_apply_status_name(statuses[i]), hy::backfill_apply_status_name(statuses[j]));
        }
    }
}
