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
//   4. Snapshot. Poll the SnapshotRefreshGate; a delivered snapshot is applied via
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

namespace hy {

// What happened during one tick -- the caller prints/logs from this; the driver never does I/O.
struct DepthFeedTickReport {
    std::uint32_t events_drained{0};
    std::uint32_t events_applied{0};   // DepthManager::on_depth_event() returned true (consumed,
                                        // not necessarily changed the book -- see that function)
    std::uint32_t events_rejected{0};  // InputValidator rejected (not a resync)
    bool generation_reset{false};      // a new generation fired this tick: DepthManager reset to Buffering
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
};

struct DepthFeedDriverConfig {
    // Same bound and same rationale as the harness's pre-existing manual drain loop
    // (live_submit_preflight_harness.cpp): large enough that a normal burst drains in one tick,
    // small enough that a runaway feed cannot starve the rest of the caller's loop indefinitely.
    std::uint32_t max_events_per_tick{4096};
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
            report.generation_reset = true;
            ++stats_.generation_resets;
        }

        drain(report);

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

private:
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
};

}  // namespace hy
