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

struct DepthSnapshot {
    std::uint64_t last_update_id{0};
    PriceLevel bids[1024];
    std::size_t bid_count{0};
    PriceLevel asks[1024];
    std::size_t ask_count{0};
};

struct DepthManagerStats {
    std::uint64_t events_applied{0};
    std::uint64_t events_buffered{0};
    std::uint64_t events_dropped{0};  // dropped during sync (u <= lastUpdateId)
    std::uint64_t snapshots{0};
    std::uint64_t resyncs{0};         // gap detected → back to Buffering
    std::uint64_t gap_events{0};      // events that arrived with U gap
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
        last_applied_u_ = 0;
        book_.clear();
    }

    // Feed a parsed depth event from WS. Behavior depends on state:
    //   Buffering: store in ring buffer
    //   Syncing:   should not be called (caller drains buffer first)
    //   Tracking:  apply to book, check for gaps
    // Returns true if the event was applied to the book.
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

            book_.apply_delta(ev.price_ticks, ev.qty_lots, ev.side);
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

            book_.apply_delta(buf_events_[i].price_ticks,
                              buf_events_[i].qty_lots,
                              buf_events_[i].side);
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
    DepthState state_{DepthState::Buffering};
    OrderBook book_{};
    DepthManagerStats stats_{};
    std::uint64_t last_applied_u_{0};

    // Circular buffer for events received during Buffering state.
    std::size_t buf_count_{0};
    std::array<BinanceMarketEvent, kMaxBuffered> buf_events_{};
    std::array<std::uint64_t, kMaxBuffered> buf_first_u_{};
    std::array<std::uint64_t, kMaxBuffered> buf_final_u_{};
};

}  // namespace hy
