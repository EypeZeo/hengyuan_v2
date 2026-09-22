// SPDX-License-Identifier: proprietary
// public_feed_policy.hpp — 批次 6 6b-0f-3c-3: the numbers, and the one predicate, that the harnesses give
// the public-feed supervisors. public_feed_supervisor.hpp is MECHANISM; the harness main() cannot be
// tested. Keeping the policy here, as small pure functions, is what lets tests pin it -- in particular
// the property that matters most: a healthy connection is rolled over BEFORE Binance's 24h limit.
//
// The 24h limit, from Binance's official docs (github.com/binance/binance-spot-api-docs,
// web-socket-streams.md, "General WSS information"; verified 2026-09-21): "A single connection to
// stream.binance.com is only valid for 24 hours; expect to be disconnected at the 24 hour mark."

#pragma once

#include <hengyuan/public_feed_supervisor.hpp>

#include <cstdint>

namespace hy {

inline constexpr std::int64_t kBinanceWsConnectionLimitMs = 24LL * 60 * 60 * 1000;
// One hour inside the limit: a reconnect that is slow (backoff, a slow handshake, a REST backfill
// after it) must still land before the exchange closes the old connection on us.
inline constexpr std::int64_t kPublicFeedMaxConnectionAgeMs = 23LL * 60 * 60 * 1000;
// At the 60s backoff cap this is about ten minutes of a feed that never comes back -- long enough to
// ride out a real outage or a maintenance window, short enough that the operator hears about it (the
// harness exits non-zero) instead of trading blind.
inline constexpr std::uint32_t kPublicFeedMaxConsecutiveFailures = 10;

// The policy both public feeds (kline and depth) get. Everything not named keeps
// FeedSupervisorPolicy's own defaults. `jitter_seed` should come from real entropy in production and be
// fixed in tests.
inline FeedSupervisorPolicy make_public_feed_policy(std::uint64_t jitter_seed) noexcept {
    FeedSupervisorPolicy policy;
    policy.jitter_seed = jitter_seed;
    policy.max_consecutive_failures = kPublicFeedMaxConsecutiveFailures;
    policy.max_connection_age_ms = kPublicFeedMaxConnectionAgeMs;
    return policy;
}

// When a PLANNED rollover of the KLINE connection may go ahead. Replacing a connection leaves it
// briefly without a live session, so do it just after a bar has closed, when the next close is as far
// away as it will get and the reconnect gap cannot swallow a bar.
//   * only while the sync is live -- a feed that is already recovering is left alone;
//   * only within the first min(span/4, 60s) after the last consumed bar's close_time (span 0 -- the
//     variable-length "1M" -- uses 60s). Measured from the last bar actually consumed rather than from an
//     epoch-modulo bar grid, which is wrong for intervals Binance does not align to the epoch (1w opens on
//     Mondays, and epoch day zero is a Thursday).
// `now_epoch_ms` and `last_bar_close_ms` are both epoch milliseconds (exchange time for the bar, wall
// clock for now; the seconds-scale skew between them is far below the window).
inline bool kline_rollover_window_open(bool sync_live, std::int64_t last_bar_close_ms, std::int64_t now_epoch_ms,
                                       std::int64_t interval_span_ms) noexcept {
    if (!sync_live || last_bar_close_ms <= 0 || now_epoch_ms < last_bar_close_ms) return false;
    constexpr std::int64_t kMaxWindowMs = 60'000;
    const std::int64_t quarter = interval_span_ms > 0 ? interval_span_ms / 4 : kMaxWindowMs;
    const std::int64_t window = quarter < kMaxWindowMs ? quarter : kMaxWindowMs;
    return now_epoch_ms - last_bar_close_ms < window;  // both operands >= 0 here: no overflow
}

// 批次 6 6b-0f-4: the depth consumer's half of "a public feed can now reconnect". A fresh
// PublicFeedSupervisor<BinanceWsSession<N>> generation is a BRAND NEW WebSocket connection --
// its first depthUpdate carries update ids that do not continue whatever sequence the PREVIOUS
// generation was tracking (the old connection's last-seen updateId and the new one's first are
// unrelated numbers from the exchange's point of view, not a continuous stream). Feeding the new
// generation's events into a DepthManager that still thinks it is Tracking the old generation's
// book would apply deltas against a snapshot the current stream was never actually consistent
// with, either landing on a false Gap resync (harmless but wasteful) or -- worse, if update ids
// happen to satisfy DepthManager::on_depth_event()'s own gap check by pure coincidence -- being
// silently accepted onto a book that no longer corresponds to any real, consistent exchange
// state. The caller must therefore call DepthManager::start_buffering() once, synchronously,
// before feeding it a single event from a new generation. FeedGenerationTracker is the pure
// logic that detects "new generation" so that reset is never missed and never repeated:
// generation 0 (PublicFeedSupervisor's own "no session has ever started yet" sentinel --
// FeedSupervisorStats::generation) never fires, since there is nothing to reset FROM.
class FeedGenerationTracker {
public:
    // Call once per tick with PublicFeedSupervisor<...>::generation(). Returns true exactly once
    // per new (non-zero) generation value, including the first one ever observed -- the caller
    // must respond by resetting whatever per-connection consumer state it owns.
    bool observe(std::uint64_t generation) noexcept {
        if (generation == 0) return false;  // no session has started yet: nothing changed
        if (seen_ && generation == last_seen_) return false;
        seen_ = true;
        last_seen_ = generation;
        return true;
    }

private:
    bool seen_{false};
    std::uint64_t last_seen_{0};
};

}  // namespace hy
