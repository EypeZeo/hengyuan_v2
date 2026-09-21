// SPDX-License-Identifier: proprietary
// kline_feed_driver.hpp — 批次 6 6b-0f-3c: everything the main loop does for the kline feed in one
// tick, in one testable place. It wires the pieces that were built and tested in isolation:
//
//   PublicFeedSupervisor  (which session is current; reconnects)            -- 6b-0f-2
//   SingleFlightFetchGate (a blocking REST backfill off the hot loop)       -- 6b-0f-3a
//   KlineFeedSync         (the only path from a bar to the evaluator)       -- 6b-0f-3b
//   KlineWsEventRing      (closed bars from the live session)
//
// Nothing here does I/O or logging (callers print from the returned report), and it is generic over
// the session and the backfill outcome, so the whole orchestration -- which is where the recovery
// bugs would actually live -- runs against fakes in unit tests. The harness `main()` cannot be
// tested; this can.
//
// THE TICK, in this order:
//   1. The live session's OWN guard. If the session reports itself suspended (a gap it saw, or a
//      closed bar it had to drop because the ring was full) bars were lost, so a live sync is
//      invalidated. The session is then told to resume_after_gap() -- ONCE per suspension, with a
//      retry timeout: the call is asynchronous (posted to the session's strand), so is_suspended()
//      keeps reading true for a moment after it, and re-posting every tick would be noise; but if the
//      session flaps suspended again before this thread ever observes it clear, a "once" flag would
//      wait forever, so an unanswered resume is repeated after resume_retry_ms. Resuming EARLY, at the
//      start of recovery rather than after it, is deliberate: bars that close during the fetch then
//      keep flowing into the ring instead of being discarded by a still-suspended session, and the
//      consumer-side join (duplicate filter + continuity guard in KlineFeedSync) decides what to keep.
//   2. Recovery. While the sync needs a backfill the gate is asked to fetch (wanted=true). An
//      outcome that arrives is applied; a fetch that failed leaves the cooldown to the gate's own
//      outcome-driven policy (a 429/418 can back off far longer than a timeout), and a backfill the
//      sync REJECTED (malformed / not contiguous -- the REST codec should never produce one) gets
//      rejected_backfill_cooldown_ms so a bad source cannot be hammered.
//   3. Drain. Only while the sync is live, and at most max_bars_per_tick bars, so a burst cannot
//      starve the rest of the loop. While NOT live the ring is left alone on purpose: it is the buffer
//      that holds the bars closing during the fetch.
//
// KNOWN LIMIT: the ring holds 16 bars. A fetch that outlasts 16 bar intervals (a 1s timeframe with a
// slow REST call) overflows it; the session then suspends and the whole recovery repeats. Sensible
// timeframes (>= 1m) are far from that; it fails safe (the feed stays invalid), never wrong.
//
// THREADING: hot-thread-only (the gate's worker touches only its own mailbox).

#pragma once

#include <hengyuan/kline_bar.hpp>
#include <hengyuan/kline_feed_sync.hpp>
#include <hengyuan/public_feed_supervisor.hpp>
#include <hengyuan/single_flight_fetch_gate.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace hy {

// The driver needs only two things from a fetch outcome, so the concrete type (which carries the
// Boost-dependent transport status) lives with the REST fetcher, not here.
template <typename O>
concept KlinesBackfillOutcomeLike = requires(const O& o) {
    { o.ok() } -> std::convertible_to<bool>;
    { o.bars() } -> std::convertible_to<std::span<const KlineWsEvent>>;
};

template <typename S>
concept KlineFeedSession = SupervisedFeedSession<S> && requires(S& s, const S& cs) {
    { cs.is_suspended() } -> std::convertible_to<bool>;
    s.resume_after_gap();
};

struct KlineFeedDriverConfig {
    std::uint32_t max_bars_per_tick{64};
    // Applied on top of the gate's own policy when a fetch succeeded but the sync refused the bars.
    std::int64_t rejected_backfill_cooldown_ms{5000};
    // An unanswered resume_after_gap() is repeated after this long.
    std::int64_t resume_retry_ms{2000};
    // Cap on the request's limit: Binance's own maximum (kline_bar.hpp).
    std::uint32_t max_backfill_bars{static_cast<std::uint32_t>(kMaxBackfillBars)};
};

// What happened during one tick -- the caller prints from this; the driver never does I/O.
struct KlineFeedTickReport {
    std::uint32_t bars_stepped{0};
    std::uint32_t duplicates_skipped{0};
    std::uint32_t malformed_dropped{0};
    bool gap_detected{false};       // a live bar revealed a hole; the sync invalidated itself
    bool session_suspended{false};  // the session reported its own guard suspended this tick
    bool resume_posted{false};      // resume_after_gap() was called this tick
    bool backfill_started{false};   // the gate started a fetch this tick
    bool backfill_collected{false}; // a fetch outcome arrived this tick
    bool backfill_fetch_ok{false};  // ... and it was a successful fetch
    bool backfill_applied{false};   // ... and the sync rebuilt from it
    BackfillApplyStatus apply_status{BackfillApplyStatus::NotNeeded};
    bool warmup_complete_after_apply{false};
    std::size_t bars_applied{0};
};

struct KlineFeedDriverStats {
    std::uint64_t session_suspensions_seen{0};  // ticks that saw a suspended session
    std::uint64_t resumes_posted{0};
    std::uint64_t backfills_collected{0};
    std::uint64_t backfill_fetch_failures{0};
    std::uint64_t backfills_rejected{0};        // fetched fine, refused by the sync
    std::uint64_t backfills_applied{0};
    std::uint64_t stale_outcomes_ignored{0};    // an outcome that arrived while the sync was already live
};

template <KlineFeedSession Session, KlinesBackfillOutcomeLike Outcome>
class KlineFeedDriver {
public:
    using Gate = SingleFlightFetchGate<KlinesBackfillRequest, Outcome>;
    using Supervisor = PublicFeedSupervisor<Session>;

    // All references must outlive the driver; the driver owns none of them.
    KlineFeedDriver(KlineFeedSync& sync, Gate& gate, Supervisor& supervisor, KlineWsEventRing& ring,
                    KlinesBackfillRequest request, KlineFeedDriverConfig config = {})
        : sync_(sync)
        , gate_(gate)
        , supervisor_(supervisor)
        , ring_(ring)
        , request_(std::move(request))
        , config_(config) {}

    KlineFeedDriver(const KlineFeedDriver&) = delete;
    KlineFeedDriver& operator=(const KlineFeedDriver&) = delete;

    // `on_bar(const KlineWsEvent&, double target_position)` is called for every bar that reached the
    // evaluator, in order. Must not call back into this driver.
    template <typename OnBar>
    KlineFeedTickReport tick(std::int64_t now_ms, OnBar&& on_bar) {
        KlineFeedTickReport report;
        watch_session(now_ms, report);
        recover(now_ms, report);
        drain(report, on_bar);
        return report;
    }

    const KlineFeedDriverStats& stats() const noexcept { return stats_; }
    const KlinesBackfillRequest& request() const noexcept { return request_; }

private:
    void watch_session(std::int64_t now_ms, KlineFeedTickReport& report) {
        const std::shared_ptr<Session> session = supervisor_.current();
        if (!session || !session->is_suspended()) {
            resume_pending_ = false;  // nothing to answer: the next suspension gets a fresh resume
            return;
        }
        report.session_suspended = true;
        ++stats_.session_suspensions_seen;
        if (sync_.live()) sync_.invalidate(KlineSyncInvalidReason::SessionSuspended);

        const bool retry_due =
            resume_pending_ && detail::feed_elapsed_ms(now_ms, resume_posted_at_ms_) >= config_.resume_retry_ms;
        if (!resume_pending_ || retry_due) {
            session->resume_after_gap();
            resume_pending_ = true;
            resume_posted_at_ms_ = now_ms;
            report.resume_posted = true;
            ++stats_.resumes_posted;
        }
    }

    void recover(std::int64_t now_ms, KlineFeedTickReport& report) {
        // Kept current every tick: the gate copies it only when it actually starts a fetch.
        const std::uint32_t wanted = sync_.backfill_bars_wanted();
        request_.limit = wanted < config_.max_backfill_bars ? wanted : config_.max_backfill_bars;

        const std::uint64_t started_before = gate_.stats().fetches_started;
        std::optional<Outcome> outcome = gate_.poll(sync_.needs_backfill(), request_, now_ms);
        report.backfill_started = gate_.stats().fetches_started != started_before;
        if (!outcome) return;

        report.backfill_collected = true;
        ++stats_.backfills_collected;
        report.backfill_fetch_ok = outcome->ok();
        if (!outcome->ok()) {
            ++stats_.backfill_fetch_failures;  // the gate's outcome-driven cooldown already applies
            return;
        }

        const BackfillApplyResult result = sync_.apply_backfill(outcome->bars());
        report.apply_status = result.status;
        report.bars_applied = result.bars_applied;
        report.warmup_complete_after_apply = result.warmup_complete;
        switch (result.status) {
            case BackfillApplyStatus::Applied:
                report.backfill_applied = true;
                ++stats_.backfills_applied;
                break;
            case BackfillApplyStatus::NotNeeded:
                ++stats_.stale_outcomes_ignored;  // the sync was already live: nothing to do, nothing to punish
                break;
            case BackfillApplyStatus::Empty:
            case BackfillApplyStatus::Malformed:
            case BackfillApplyStatus::NotContiguous:
            case BackfillApplyStatus::EvaluatorInitFailed:
                ++stats_.backfills_rejected;
                gate_.impose_cooldown(now_ms, config_.rejected_backfill_cooldown_ms);
                break;
        }
    }

    template <typename OnBar>
    void drain(KlineFeedTickReport& report, OnBar& on_bar) {
        KlineWsEvent bar{};
        std::uint32_t taken = 0;
        // live() is re-read every iteration: a Gap ends the drain, and what is left stays in the ring.
        while (sync_.live() && taken < config_.max_bars_per_tick && ring_.try_pop(bar)) {
            ++taken;
            const LiveBarResult r = sync_.on_live_bar(bar);
            switch (r.action) {
                case LiveBarAction::Stepped:
                    ++report.bars_stepped;
                    on_bar(bar, r.target_position);
                    break;
                case LiveBarAction::Duplicate:
                    ++report.duplicates_skipped;
                    break;
                case LiveBarAction::Malformed:
                    ++report.malformed_dropped;
                    break;
                case LiveBarAction::Gap:
                    report.gap_detected = true;
                    break;
                case LiveBarAction::NotLive:
                    break;  // unreachable: the loop condition checks live()
            }
        }
    }

    KlineFeedSync& sync_;
    Gate& gate_;
    Supervisor& supervisor_;
    KlineWsEventRing& ring_;
    KlinesBackfillRequest request_;
    KlineFeedDriverConfig config_;
    KlineFeedDriverStats stats_{};
    bool resume_pending_{false};
    std::int64_t resume_posted_at_ms_{0};
};

}  // namespace hy
