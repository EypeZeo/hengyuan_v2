// SPDX-License-Identifier: proprietary
// kline_feed_sync.hpp — 批次 6 6b-0f-3b: the consumer side of the kline feed. The ONLY path by which
// a bar reaches StreamingEvaluator::step(), so that one invariant can be tested in one place:
//
//   the evaluator never sees a hole, a duplicate, or a bar that is not a sane closed candle --
//   and whenever it might have, it is rebuilt from history before anything is read from it again.
//
// Why it is needed: SMA/EMA/RSI/lag/crosses are all defined over an EQUALLY SPACED, gap-free series.
// One missing bar silently shifts every window; one repeated bar double-counts it. The state cannot
// heal by itself, and a wrong signal on a live account is worse than none. The same rebuild is also
// what makes the strategy usable at all after a restart: a fresh evaluator needs effective_warmup+1
// bars before warmup_complete() -- days of wall clock at a 1h timeframe -- unless it is warmed from
// REST history (binance_klines_rest.hpp) instead of waiting.
//
// PROTOCOL (the caller -- the feed harness -- drives the I/O; this class holds no session, no thread
// and no network):
//   1. Starts in NeedsBackfill. While !live() the caller does NOT pop the kline ring: the ring is the
//      buffer, so bars that close during the fetch are kept, not lost. (on_live_bar() called anyway
//      drops the bar and counts it.)
//   2. The caller fetches backfill_bars_wanted() bars and calls apply_backfill(). That rebuilds the
//      evaluator with init(dag) and replays every bar; the state becomes Live with the continuity
//      baseline at the last bar's close_time.
//   3. Live bars go through on_live_bar(): a bar at or before the baseline is a Duplicate (the ring
//      may still hold bars the backfill already contains -- skipped, not an error); the next bar in
//      sequence is stepped; anything else is a Gap, which invalidates the state again (the offending
//      bar is dropped; the next backfill will contain it once it is old enough to be closed).
//   4. The caller also calls invalidate() when it learns bars were lost some other way (the session's
//      own guard suspended, a ring overflow).
//   A backfill that ends just before the live stream's first bar simply produces a Gap on that bar and
//   another round; the fetch gate's cooldown keeps that from hammering the exchange.
//
// TWO TRAPS THIS HIDES FROM ITS CALLERS:
//   * StreamingEvaluator::reset() also CLEARS THE DAG and marks the evaluator uninitialized (it is
//     "reset + reload"); stepping it afterwards silently returns 0.0 forever. Rebuilding therefore
//     goes through init(dag), which is why this class is handed the loaded SpecDag and requires that
//     it outlives it.
//   * A bar counts as usable only if it is a sane closed candle. The live parser (KlineJsonParser)
//     checks only that std::from_chars reported no error, so "nan"/"inf" or a partially-parsed number
//     can reach the ring; the REST codec is stricter. This class checks both paths the same way.
//
// THREADING: hot-thread-only, like every other piece of the feed pipeline. Zero heap allocation.
// The evaluator must not be stepped, init()-ed or reset() by anyone else while this object owns it.

#pragma once

#include <hengyuan/kline_bar.hpp>
#include <hengyuan/strategy_spec_evaluator.hpp>
#include <hengyuan/strategy_spec_operators.hpp>
#include <hengyuan/strategy_spec_types.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace hy {

enum class KlineSyncState : std::uint8_t {
    NeedsBackfill = 0,  // the evaluator is empty/invalid; live bars are not consumed
    Live = 1,           // the evaluator is warmed to the continuity baseline; live bars are checked and stepped
};

enum class KlineSyncInvalidReason : std::uint8_t {
    Startup = 0,           // never applied
    ConsumerGap = 1,       // a live bar after a hole (detected here)
    SessionSuspended = 2,  // the session's own guard suspended: a gap or a ring overflow lost bars
    Manual = 3,            // the caller's decision (e.g. the feed supervisor gave up, an operator reset)
    BarOverdue = 4,        // the next closed bar did not arrive within its grace: a silent or stalled stream
};

inline constexpr const char* kline_sync_invalid_reason_name(KlineSyncInvalidReason r) noexcept {
    switch (r) {
        case KlineSyncInvalidReason::Startup: return "Startup";
        case KlineSyncInvalidReason::ConsumerGap: return "ConsumerGap";
        case KlineSyncInvalidReason::SessionSuspended: return "SessionSuspended";
        case KlineSyncInvalidReason::Manual: return "Manual";
        case KlineSyncInvalidReason::BarOverdue: return "BarOverdue";
    }
    return "?";
}

enum class BackfillApplyStatus : std::uint8_t {
    Applied = 0,
    NotNeeded = 1,            // called while Live: a stale/duplicate delivery, ignored untouched
    Empty = 2,                // no bars to rebuild from
    Malformed = 3,            // a bar is not a sane closed candle (the REST codec never produces this)
    NotContiguous = 4,        // bars are not exactly adjacent (likewise)
    EvaluatorInitFailed = 5,  // init(dag) refused; cannot happen for a spec that has already loaded
};

inline constexpr const char* backfill_apply_status_name(BackfillApplyStatus s) noexcept {
    switch (s) {
        case BackfillApplyStatus::Applied: return "Applied";
        case BackfillApplyStatus::NotNeeded: return "NotNeeded";
        case BackfillApplyStatus::Empty: return "Empty";
        case BackfillApplyStatus::Malformed: return "Malformed";
        case BackfillApplyStatus::NotContiguous: return "NotContiguous";
        case BackfillApplyStatus::EvaluatorInitFailed: return "EvaluatorInitFailed";
    }
    return "?";
}

struct BackfillApplyResult {
    BackfillApplyStatus status{BackfillApplyStatus::Empty};
    std::size_t bars_applied{0};
    // After a successful apply: whether the replay alone was enough to warm the evaluator. A short
    // history (a fresh listing, a spec whose warm-up exceeds what REST can return) leaves the state
    // Live with this false -- correct, and the planner's warm-up gate keeps it from trading on it.
    bool warmup_complete{false};
    std::int64_t last_close_time_ms{0};
};

enum class LiveBarAction : std::uint8_t {
    NotLive = 0,    // arrived while NeedsBackfill: DROPPED (the caller should not have popped it)
    Duplicate = 1,  // at or before the baseline: already contained in the state
    Gap = 2,        // a hole after the baseline: the state invalidated itself
    Malformed = 3,  // not a sane closed candle: dropped
    Stepped = 4,    // accepted; target_position is the evaluator's [signal]-mapped output
};

struct LiveBarResult {
    LiveBarAction action{LiveBarAction::NotLive};
    double target_position{0.0};  // meaningful only for Stepped
};

struct KlineSyncStats {
    std::uint64_t bars_stepped{0};
    std::uint64_t duplicates_skipped{0};
    std::uint64_t dropped_not_live{0};
    std::uint64_t malformed_bars{0};
    std::uint64_t backfills_applied{0};
    std::uint64_t backfill_bars_replayed{0};
    std::uint64_t invalidations{0};
    std::uint64_t gaps_detected{0};
    std::uint64_t session_suspended_invalidations{0};
    std::uint64_t manual_invalidations{0};
    std::uint64_t overdue_invalidations{0};
};

// A bar the evaluator may be fed: closed, forward in time, finite positive prices that are
// mutually consistent, finite non-negative volume. The same shape rules binance_klines_codec.hpp
// enforces per bar, applied here to the live path as well.
inline bool is_sane_closed_kline(const KlineWsEvent& b) noexcept {
    if (!b.is_closed || b.close_time_ms <= b.open_time_ms) return false;
    const double prices[] = {b.open, b.high, b.low, b.close};
    for (const double p : prices) {
        if (!std::isfinite(p) || p <= 0.0) return false;
    }
    if (!std::isfinite(b.volume) || b.volume < 0.0) return false;
    return b.high >= b.low && b.open >= b.low && b.open <= b.high && b.close >= b.low && b.close <= b.high;
}

class KlineFeedSync {
public:
    // Extra bars requested beyond what warm-up needs: one for the still-forming candle REST returns
    // as the last element, one for a bar that closed inside the fetch's safety margin
    // (binance_klines_rest.hpp) and is therefore treated as forming.
    static constexpr std::uint32_t kBackfillSlackBars = 2;

    // `dag` must outlive this object (see the header comment on reset()). Takes ownership of the
    // evaluator's lifecycle: it is reset here so nothing stale can be read before the first backfill.
    KlineFeedSync(StreamingEvaluator& evaluator, const SpecDag& dag) noexcept
        : evaluator_(evaluator), dag_(dag) {
        evaluator_.reset();
    }

    KlineFeedSync(const KlineFeedSync&) = delete;
    KlineFeedSync& operator=(const KlineFeedSync&) = delete;

    KlineSyncState state() const noexcept { return state_; }
    // True when the kline ring may be drained into on_live_bar(). While false, leave bars in the ring.
    bool live() const noexcept { return state_ == KlineSyncState::Live; }
    bool needs_backfill() const noexcept { return state_ == KlineSyncState::NeedsBackfill; }
    KlineSyncInvalidReason last_invalid_reason() const noexcept { return last_reason_; }

    // How many bars to ask REST for: enough for the evaluator to reach warmup_complete() (which needs
    // seen_bars > effective_warmup, i.e. warm-up + 1 bars) plus the slack for the forming candle and a
    // bar inside the safety margin. NOT capped here -- the caller caps it at kMaxBackfillBars, and a
    // spec that needs more than REST can return simply ends Live with warm-up still incomplete.
    std::uint32_t backfill_bars_wanted() const noexcept {
        return compute_effective_warmup(dag_) + 1U + kBackfillSlackBars;
    }

    // Drops all evaluator state and requires a fresh backfill. Safe to call in any state.
    void invalidate(KlineSyncInvalidReason reason) noexcept {
        evaluator_.reset();  // warmup_complete() is false from this instant: the planner is blocked
        guard_.resume();
        state_ = KlineSyncState::NeedsBackfill;
        last_close_ms_ = 0;
        last_reason_ = reason;
        ++stats_.invalidations;
        if (reason == KlineSyncInvalidReason::ConsumerGap) ++stats_.gaps_detected;
        if (reason == KlineSyncInvalidReason::SessionSuspended) ++stats_.session_suspended_invalidations;
        if (reason == KlineSyncInvalidReason::Manual) ++stats_.manual_invalidations;
        if (reason == KlineSyncInvalidReason::BarOverdue) ++stats_.overdue_invalidations;
    }

    // Rebuilds the evaluator from `bars` (oldest first; contiguous, closed, sane -- validated here
    // BEFORE anything is touched, so a bad input never leaves a half-applied state) and goes Live.
    BackfillApplyResult apply_backfill(std::span<const KlineWsEvent> bars) noexcept {
        BackfillApplyResult result;
        if (state_ == KlineSyncState::Live) {
            result.status = BackfillApplyStatus::NotNeeded;
            return result;
        }
        if (bars.empty()) {
            result.status = BackfillApplyStatus::Empty;
            return result;
        }
        for (std::size_t i = 0; i < bars.size(); ++i) {
            if (!is_sane_closed_kline(bars[i])) {
                result.status = BackfillApplyStatus::Malformed;
                return result;
            }
            if (i > 0) {
                const std::int64_t prev_close = bars[i - 1].close_time_ms;
                if (prev_close == std::numeric_limits<std::int64_t>::max() ||
                    bars[i].open_time_ms != prev_close + 1) {
                    result.status = BackfillApplyStatus::NotContiguous;
                    return result;
                }
            }
        }

        if (!evaluator_.init(dag_)) {  // init() resets first: a fresh state, DAG laid out again
            evaluator_.reset();
            result.status = BackfillApplyStatus::EvaluatorInitFailed;
            return result;
        }
        for (const KlineWsEvent& b : bars) (void)evaluator_.step(to_bar(b));

        guard_.resume();
        (void)guard_.accept(bars.back());  // the first bar after resume() is the baseline unconditionally
        last_close_ms_ = bars.back().close_time_ms;
        state_ = KlineSyncState::Live;
        ++stats_.backfills_applied;
        stats_.backfill_bars_replayed += bars.size();

        result.status = BackfillApplyStatus::Applied;
        result.bars_applied = bars.size();
        result.warmup_complete = evaluator_.warmup_complete();
        result.last_close_time_ms = last_close_ms_;
        return result;
    }

    // One closed bar from the live ring. See the header comment for the four outcomes.
    LiveBarResult on_live_bar(const KlineWsEvent& bar) noexcept {
        LiveBarResult result;
        if (state_ != KlineSyncState::Live) {
            ++stats_.dropped_not_live;
            result.action = LiveBarAction::NotLive;
            return result;
        }
        if (!is_sane_closed_kline(bar)) {
            ++stats_.malformed_bars;
            result.action = LiveBarAction::Malformed;
            return result;
        }
        if (bar.close_time_ms <= last_close_ms_) {
            ++stats_.duplicates_skipped;
            result.action = LiveBarAction::Duplicate;
            return result;
        }
        if (!guard_.accept(bar)) {  // not exactly adjacent to the baseline (a hole, or a misaligned bar)
            invalidate(KlineSyncInvalidReason::ConsumerGap);
            result.action = LiveBarAction::Gap;
            return result;
        }
        last_close_ms_ = bar.close_time_ms;
        result.target_position = evaluator_.step(to_bar(bar));
        result.action = LiveBarAction::Stepped;
        ++stats_.bars_stepped;
        return result;
    }

    // close_time of the last bar the evaluator has consumed (0 when NeedsBackfill).
    std::int64_t last_close_time_ms() const noexcept { return last_close_ms_; }
    KlineSyncStats stats() const noexcept { return stats_; }

private:
    static Bar to_bar(const KlineWsEvent& e) noexcept {
        return Bar{e.open, e.high, e.low, e.close, e.volume};
    }

    StreamingEvaluator& evaluator_;
    const SpecDag& dag_;
    KlineBarGapGuard guard_;
    KlineSyncState state_{KlineSyncState::NeedsBackfill};
    KlineSyncInvalidReason last_reason_{KlineSyncInvalidReason::Startup};
    std::int64_t last_close_ms_{0};
    KlineSyncStats stats_{};
};

}  // namespace hy
