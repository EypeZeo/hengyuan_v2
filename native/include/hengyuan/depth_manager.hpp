// SPDX-License-Identifier: proprietary
// depth_manager.hpp — Binance depth snapshot bootstrap + incremental tracking.
//
// Implements the official Binance depth management protocol:
//   1. Buffer WS depthUpdate events on connect
//   2. Fetch REST snapshot (GET /api/v3/depth?limit=1000)
//   3. Drop buffered events where u <= lastUpdateId
//   4. Verify first applied event has U <= lastUpdateId+1 <= u
//   5. Track incrementally; on gap → re-buffer + re-snapshot
//
// Governance: L2 (state machine + buffer logic, no network).
// The actual REST fetch is performed by the caller and injected via
// apply_snapshot(). This keeps depth_manager network-free and testable.

#pragma once

#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/orderbook.hpp>
#include <array>
#include <cstdint>

namespace hy {

enum class DepthState : std::uint8_t {
    Buffering = 0,  // WS connected, buffering events, waiting for snapshot
    Syncing = 1,    // Snapshot received, replaying buffered events
    Tracking = 2,   // Fully synced, applying events in real-time
};

// Per-side capacity of a REST depth snapshot. Bounds Binance's largest allowed
// `limit` value that this codebase accepts (1000, see binance_rest_snapshot.hpp's
// kAllowedLimits -- 5000 is deliberately excluded precisely because of this bound).
inline constexpr std::size_t kDepthSnapshotLevels = 1024;

// AUDIT MD-BOOK-002: this relationship used to be implicit, and getting it wrong in
// the OTHER direction (OrderBook silently truncating a 1000-level snapshot into 256
// slots and then refusing every subsequent insert) froze top_of_book(). OrderBook is
// a bounded top-N VIEW of this snapshot, so kMaxLevels <= kDepthSnapshotLevels is the
// intended direction; pin it so a future capacity change has to think about it.
static_assert(kMaxLevels <= kDepthSnapshotLevels,
              "OrderBook is a top-N view of DepthSnapshot; it must not claim more "
              "levels per side than a snapshot can carry");

struct DepthSnapshot {
    std::uint64_t last_update_id{0};
    PriceLevel bids[kDepthSnapshotLevels];
    std::size_t bid_count{0};
    PriceLevel asks[kDepthSnapshotLevels];
    std::size_t ask_count{0};
};

struct DepthManagerStats {
    std::uint64_t events_applied{0};
    std::uint64_t events_buffered{0};
    std::uint64_t events_dropped{0};  // dropped during sync (u <= lastUpdateId)
    std::uint64_t snapshots{0};
    std::uint64_t resyncs{0};         // gap detected → back to Buffering
    std::uint64_t gap_events{0};      // events that arrived with U gap
    std::uint64_t buffer_overflow_count{0};  // events dropped because the Buffering-state
                                              // ring (kMaxBuffered) was already full
    // AUDIT MD-BOOK-002: OrderBook::apply_delta()'s return value used to be discarded
    // here and in hot_thread.hpp, so a delta that never reached the book was
    // indistinguishable from one that did. These two make the outcome observable.
    // deltas_outside_window is EXPECTED and benign (a level worse than the 256th
    // best simply isn't in the maintained window); deltas_invalid is not -- it means
    // a malformed price/qty reached the book layer.
    std::uint64_t deltas_outside_window{0};
    std::uint64_t deltas_invalid{0};
};

class DepthManager {
public:
    static constexpr std::size_t kMaxBuffered = 4096;

    DepthManager() = default;

    DepthState state() const noexcept { return state_; }
    const DepthManagerStats& stats() const noexcept { return stats_; }
    const OrderBook& book() const noexcept { return book_; }

    // Call when WS connects or when a resync is needed.
    // Transitions to Buffering state, clears book.
    void start_buffering() noexcept {
        state_ = DepthState::Buffering;
        buf_count_ = 0;
        buf_overflowed_ = false;
        last_applied_u_ = 0;
        book_.clear();
    }

    // Feed a parsed depth event from WS. Behavior depends on state:
    //   Buffering: store in ring buffer
    //   Syncing:   should not be called (caller drains buffer first)
    //   Tracking:  apply to book, check for gaps
    // Returns true iff the event was consumed in Tracking state without a detected
    // gap. That is NOT the same as "changed the book": a level outside the
    // maintained top-N window (OrderBook::ApplyResult::OutsideWindow) is consumed
    // correctly and still returns true -- see stats().deltas_outside_window for that
    // distinction, which used to be invisible because apply_delta()'s return value
    // was discarded here (audit MD-BOOK-002).
    bool on_depth_event(const BinanceMarketEvent& ev,
                        std::uint64_t first_update_id,
                        std::uint64_t final_update_id) noexcept {
        if (state_ == DepthState::Buffering) {
            if (buf_count_ < kMaxBuffered) {
                buf_events_[buf_count_] = ev;
                buf_first_u_[buf_count_] = first_update_id;
                buf_final_u_[buf_count_] = final_update_id;
                ++buf_count_;
                ++stats_.events_buffered;
            } else {
                // The buffered array holds a contiguous prefix of events since Buffering
                // started; once full, every subsequent event is silently lost until the next
                // apply_snapshot() -- previously this was invisible (no counter, no flag).
                // Recording it lets apply_snapshot() refuse to trust a replay that we know is
                // missing events, instead of silently transitioning to Tracking on a book that
                // may already have a real gap right after the buffered prefix ends.
                buf_overflowed_ = true;
                ++stats_.buffer_overflow_count;
            }
            return false;
        }

        if (state_ == DepthState::Tracking) {
            // Gap detection: next event's U should be <= last_applied_u_ + 1
            if (last_applied_u_ > 0 && first_update_id > last_applied_u_ + 1) {
                ++stats_.gap_events;
                ++stats_.resyncs;
                start_buffering();
                // Re-buffer this event
                on_depth_event(ev, first_update_id, final_update_id);
                return false;
            }

            record_apply_result(book_.apply_delta(ev.price_ticks, ev.qty_lots, ev.side));
            last_applied_u_ = final_update_id;
            ++stats_.events_applied;
            return true;
        }

        return false;
    }

    // Apply a REST snapshot. Call this after fetching the snapshot while in
    // Buffering state. Transitions through Syncing → Tracking.
    // Returns true if sync succeeded (found valid continuation in buffer).
    bool apply_snapshot(const DepthSnapshot& snap) noexcept {
        if (state_ != DepthState::Buffering) return false;

        if (buf_overflowed_) {
            // The buffered prefix is known-incomplete for this episode -- replaying it and
            // trusting the result would silently assume sequence continuity across a gap we
            // know exists. Discard this snapshot attempt and force a fresh resync instead of
            // quietly entering Tracking on data we can't vouch for.
            ++stats_.resyncs;
            start_buffering();
            return false;
        }

        ++stats_.snapshots;
        state_ = DepthState::Syncing;

        // Initialize book from snapshot
        book_.apply_snapshot(snap.asks, snap.ask_count,
                             snap.bids, snap.bid_count);
        last_applied_u_ = snap.last_update_id;

        // Replay buffered events:
        //  - Drop events where final_update_id <= lastUpdateId
        //  - First valid event must have U <= lastUpdateId+1 <= u
        bool found_first = false;
        for (std::size_t i = 0; i < buf_count_; ++i) {
            const std::uint64_t U = buf_first_u_[i];
            const std::uint64_t u = buf_final_u_[i];

            if (u <= snap.last_update_id) {
                ++stats_.events_dropped;
                continue;
            }

            if (!found_first) {
                // First event after snapshot: verify U <= lastUpdateId+1 <= u
                if (U > snap.last_update_id + 1) {
                    // Gap between snapshot and first buffered event → resync
                    ++stats_.resyncs;
                    start_buffering();
                    return false;
                }
                found_first = true;
            }

            record_apply_result(book_.apply_delta(buf_events_[i].price_ticks,
                                                   buf_events_[i].qty_lots,
                                                   buf_events_[i].side));
            last_applied_u_ = u;
            ++stats_.events_applied;
        }

        buf_count_ = 0;
        state_ = DepthState::Tracking;
        return true;
    }

    // Check if the manager needs a snapshot fetch (caller should GET /api/v3/depth).
    bool needs_snapshot() const noexcept {
        return state_ == DepthState::Buffering;
    }

private:
    // Folds OrderBook::apply_delta()'s outcome into stats_ instead of discarding it.
    // Deliberately does NOT change any control flow: OutsideWindow is the normal
    // outcome for a deep level and must never trigger a resync, and InvalidInput must
    // still advance the sequence number (refusing to would manufacture a false gap on
    // the very next event).
    void record_apply_result(OrderBook::ApplyResult r) noexcept {
        if (r == OrderBook::ApplyResult::OutsideWindow) {
            ++stats_.deltas_outside_window;
        } else if (r == OrderBook::ApplyResult::InvalidInput) {
            ++stats_.deltas_invalid;
        }
    }

    DepthState state_{DepthState::Buffering};
    OrderBook book_{};
    DepthManagerStats stats_{};
    std::uint64_t last_applied_u_{0};

    // Circular buffer for events received during Buffering state.
    std::size_t buf_count_{0};
    bool buf_overflowed_{false};  // true if any event was dropped due to kMaxBuffered this episode
    std::array<BinanceMarketEvent, kMaxBuffered> buf_events_{};
    std::array<std::uint64_t, kMaxBuffered> buf_first_u_{};
    std::array<std::uint64_t, kMaxBuffered> buf_final_u_{};
};

}  // namespace hy
