// SPDX-License-Identifier: proprietary
// depth_feed_driver.hpp — 批次 6 6b-0f-6: everything the main loop does for the depth feed in one
// tick, in one testable place -- the depth-side mirror of kline_feed_driver.hpp. Wires the pieces
// that were built and tested in isolation:
//
//   PublicFeedSupervisor<Session>  (reconnects a real depth WS session)             -- 6b-0f-2
//   FeedGenerationTracker          (detects each new generation)                    -- 6b-0f-4/public_feed_policy.hpp
//   SnapshotRefreshGate            (non-blocking REST snapshot fetch, off the loop) -- pre-批次6
//   DepthManager                   (book state: Buffering -> Syncing -> Tracking)   -- pre-批次6
//   InputValidator                 (fail-closed malformed/duplicate/stale rejection)-- pre-批次6
//
// This is the exact sequence live_submit_preflight_harness.cpp (6b-1) currently performs inline
// in its main loop, packaged so the orchestration -- the actual place a recovery bug would live,
// per kline_feed_driver.hpp's own header comment -- runs against fakes in unit tests instead of
// only ever being exercised by a human running the harness against real testnet.
//
// THE TICK, in this order:
//   1. supervisor.poll(now_ms) -- drives reconnection/backoff/rollover exactly like the kline side.
//   2. Generation check. A NEW generation resets DepthManager to Buffering -- mandatory, not
//      optional: a fresh connection's update ids do not continue the previous one's sequence
//      (public_feed_policy.hpp's own header comment on FeedGenerationTracker spells out the
//      hazard of skipping this). Exactly one reset per generation change, including the first.
//   3. Drain. Pop up to max_events_per_tick raw events from the ring and run each one through
//      InputValidator first -- a ResyncRequired verdict forces Buffering (the same event that
//      caused it is not itself re-submitted to DepthManager); a plain rejection (bad price/qty,
//      sequence rollback, duplicate) is dropped; everything else reaches
//      DepthManager::on_depth_event(). Bounded per tick so a burst cannot starve the rest of the
//      caller's loop (clock resync, keepalive, the kline side) -- same discipline as
//      KlineFeedDriver::drain()'s own max_bars_per_tick.
//   4. Staleness (6b-0f-9). A connected feed whose depth events stop being ACCEPTED for
//      stale_after_ms is a frozen book that still looks healthy: the WS-level idle timeout is satisfied
//      by Binance's pings, and DepthManager stays Tracking. So the driver fails closed on the data
//      itself -- the manager goes back to Buffering, the current session is stopped, and the
//      supervisor's own failure path (backoff, a new generation, the reset in step 2) takes over. The
//      clock is re-armed by every new generation and by every event DepthManager consumes; events that are
//      only drained and rejected are not activity (a validator/manager that throws everything away is
//      exactly the frozen book this is for). No snapshot is fetched for a book that is about to be reset.
//   5. Snapshot. Poll the SnapshotRefreshGate; a delivered snapshot is applied via
//      DepthManager::apply_snapshot() and the outcome reported straight back to the gate, so its
//      own cooldown-on-rejection logic (snapshot_refresh_gate.hpp) still applies unchanged.
//
// THREADING: hot-thread-only, like every other piece of the feed pipeline. The ring, validator,
// depth manager and snapshot gate are all owned by the CALLER (references only, exactly like
// KlineFeedDriver) -- this class adds no new shared state of its own beyond the generation
// tracker, which is itself hot-thread-only, plain (non-atomic) state.

#pragma once

#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/depth_manager.hpp>
#include <hengyuan/input_validator.hpp>
#include <hengyuan/public_feed_policy.hpp>
#include <hengyuan/public_feed_supervisor.hpp>
#include <hengyuan/snapshot_refresh_gate.hpp>
#include <hengyuan/spsc_ring.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace hy {

// What happened during one tick -- the caller prints/logs from this; the driver never does I/O.
struct DepthFeedTickReport {
    std::uint32_t events_drained{0};
    std::uint32_t events_applied{0};   // DepthManager::on_depth_event() returned true (consumed,
                                        // not necessarily changed the book -- see that function)
    std::uint32_t events_rejected{0};  // InputValidator rejected (not a resync)
    bool generation_reset{false};      // a new generation fired this tick: DepthManager reset to Buffering
    bool stale_restart{false};         // the feed was judged stale this tick: book reset, session stopped
    bool resync_requested{false};      // InputValidator saw ResyncRequired this tick
    bool snapshot_applied{false};      // a snapshot was collected from the gate this tick
    bool snapshot_apply_ok{false};     // ... and DepthManager::apply_snapshot() accepted it
};

struct DepthFeedDriverStats {
    std::uint64_t generation_resets{0};
    std::uint64_t resyncs_requested{0};
    std::uint64_t events_applied{0};
    std::uint64_t events_rejected{0};
    std::uint64_t snapshots_applied{0};
    std::uint64_t snapshots_rejected{0};
    std::uint64_t stale_restarts{0};
};

struct DepthFeedDriverConfig {
    // Same bound and same rationale as the harness's pre-existing manual drain loop
    // (live_submit_preflight_harness.cpp): large enough that a normal burst drains in one tick,
    // small enough that a runaway feed cannot starve the rest of the caller's loop indefinitely.
    std::uint32_t max_events_per_tick{4096};
    // How long a CONNECTED feed may go without a single depth event being accepted before it is judged
    // stale (see the header comment). 0 = never. BTCUSDT's 100 ms stream updates several times a second, so
    // a minute is far above any quiet spell; a symbol that can genuinely be silent for longer needs a larger
    // value, not 0. It equals the supervisor's stable_after_ms on purpose: a connection that delivered and
    // then went quiet has been up long enough for its end to count as a long healthy run that ended (the
    // failure streak restarts) rather than escalating towards Terminal on a merely quiet market -- while one
    // that never delivers a single event still escalates, as a dead feed should.
    std::int64_t stale_after_ms{60'000};
};

template <SupervisedFeedSession Session, std::size_t RingCapacity>
class DepthFeedDriver {
public:
    using Supervisor = PublicFeedSupervisor<Session>;
    using Ring = SpscRing<BinanceMarketEvent, RingCapacity>;

    // All references must outlive the driver; the driver owns none of them -- same discipline as
    // KlineFeedDriver. `snapshot_request` is copied (SnapshotRefreshGate's own worker thread needs
    // a value it can capture, not a reference into the caller's stack).
    DepthFeedDriver(Supervisor& supervisor, Ring& ring, InputValidator& validator, DepthManager& depth_mgr,
                    SnapshotRefreshGate& snapshot_gate, SnapshotRequest snapshot_request,
                    DepthFeedDriverConfig config = {})
        : supervisor_(supervisor)
        , ring_(ring)
        , validator_(validator)
        , depth_mgr_(depth_mgr)
        , snapshot_gate_(snapshot_gate)
        , snapshot_request_(std::move(snapshot_request))
        , config_(config) {}

    DepthFeedDriver(const DepthFeedDriver&) = delete;
    DepthFeedDriver& operator=(const DepthFeedDriver&) = delete;

    DepthFeedTickReport tick(std::int64_t now_ms) {
        DepthFeedTickReport report;
        supervisor_.poll(now_ms);

        if (gen_tracker_.observe(supervisor_.generation())) {
            depth_mgr_.start_buffering();
            // The new connection's update ids need not continue the old one's (an exchange-side reset
            // restarts them): forget the old sequence, or all of the new one is rejected as a rollback.
            validator_.reset_sequences();
            stale_ = false;
            last_activity_ms_ = now_ms;  // a new connection gets a whole stale_after_ms to deliver
            report.generation_reset = true;
            ++stats_.generation_resets;
        }

        drain(report);
        if (report.events_applied > 0) last_activity_ms_ = now_ms;
        judge_staleness(now_ms, report);

        // Not for a book that is about to be reset by the new generation anyway.
        if (stale_) return report;

        if (auto snap = snapshot_gate_.poll(depth_mgr_.needs_snapshot(), snapshot_request_)) {
            const bool ok = depth_mgr_.apply_snapshot(*snap);
            snapshot_gate_.notify_apply_result(ok);
            report.snapshot_applied = true;
            report.snapshot_apply_ok = ok;
            if (ok) {
                ++stats_.snapshots_applied;
            } else {
                ++stats_.snapshots_rejected;
            }
        }
        return report;
    }

    const DepthFeedDriverStats& stats() const noexcept { return stats_; }

    // True from the moment the feed is judged stale until the supervisor's next generation begins.
    bool stale() const noexcept { return stale_; }
    // The caller's clock at the last event DepthManager consumed (or at the start of the current
    // generation): the age of the book, for whoever prices an order from it.
    std::int64_t last_activity_ms() const noexcept { return last_activity_ms_; }

private:
    void judge_staleness(std::int64_t now_ms, DepthFeedTickReport& report) {
        if (config_.stale_after_ms <= 0 || stale_ || supervisor_.state() != FeedState::Connected) return;
        if (detail::feed_elapsed_ms(now_ms, last_activity_ms_) <= config_.stale_after_ms) return;
        stale_ = true;
        depth_mgr_.start_buffering();  // fail closed: the book is no longer trusted
        if (const std::shared_ptr<Session> session = supervisor_.current()) session->stop();
        report.stale_restart = true;
        ++stats_.stale_restarts;
    }

    void drain(DepthFeedTickReport& report) {
        BinanceMarketEvent ev{};
        std::uint32_t taken = 0;
        while (taken < config_.max_events_per_tick && ring_.try_pop(ev)) {
            ++taken;
            ++report.events_drained;
            const ValidationResult vr = validator_.validate(ev);
            if (vr == ValidationResult::ResyncRequired) {
                depth_mgr_.start_buffering();
                report.resync_requested = true;
                ++stats_.resyncs_requested;
                continue;
            }
            if (vr == ValidationResult::RejectNegativePrice || vr == ValidationResult::RejectZeroPrice ||
                vr == ValidationResult::RejectNegativeQty || vr == ValidationResult::RejectSeqRollback ||
                vr == ValidationResult::DropDuplicate) {
                ++report.events_rejected;
                ++stats_.events_rejected;
                continue;
            }
            if (ev.type == EventType::DepthDelta && depth_mgr_.on_depth_event(ev, ev.aux_id, ev.event_id)) {
                ++report.events_applied;
                ++stats_.events_applied;
            }
        }
    }

    Supervisor& supervisor_;
    Ring& ring_;
    InputValidator& validator_;
    DepthManager& depth_mgr_;
    SnapshotRefreshGate& snapshot_gate_;
    SnapshotRequest snapshot_request_;
    DepthFeedDriverConfig config_;
    FeedGenerationTracker gen_tracker_;
    DepthFeedDriverStats stats_{};
    bool stale_{false};
    std::int64_t last_activity_ms_{0};
};

}  // namespace hy
