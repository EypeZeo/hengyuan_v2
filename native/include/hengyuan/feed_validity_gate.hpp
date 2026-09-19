// SPDX-License-Identifier: proprietary
// feed_validity_gate.hpp — 批次 6 6b-0a: the single answer to "is the market-data feed currently
// trustworthy enough to derive a new order intent from?".
//
// Why this exists (外部复核 P0-04/P0-05, verified against the merged 6a-1/6b-1 code): both public
// sessions are fail-stop (binance_kline_ws_session.hpp / binance_ws_session.hpp -- no reconnect),
// the kline ring can lose a closed bar (fixed by ingest_closed_bar()), and the depth book is only
// meaningful while DepthManager is in Tracking. Before this file each of those facts was checked
// (or not) ad hoc at each call site; a new order path would have had to remember all of them.
// This gate is the one place they are combined, so every consumer (the planner in 6b-0c, the
// authorization re-check in 6b-0e, the demo/harness main loops) asks one function.
//
// Pure logic, no Boost, no I/O -- callers gather the booleans from their own session/manager
// objects and pass them in, which is also what makes it exhaustively unit-testable.
//
// NOT a recovery mechanism: "invalid" only ever means "no new intents". Getting out of invalid
// after a suspension needs the backfill protocol of 6b-0f; until that lands, the demo/harness
// treat a suspension or a stopped session as terminal (exit non-zero, restart into recovery).

#pragma once

#include <cstdint>

namespace hy {

// Default upper bound on one process run. Binance closes a WebSocket connection after 24h (the
// external review's citation of the official docs -- not independently re-fetched this session,
// to be confirmed when 6b-0f implements the rollover); a fail-stop session would then end the run
// abruptly, so stop cleanly well before that. Overridable by the caller, never larger than the
// caller's own request.
inline constexpr int kDefaultMaxRunSeconds = 20 * 60 * 60;

struct FeedHealthInputs {
    bool kline_session_stopped{false};      // BinanceKlineWsSession::stopped()
    bool kline_session_suspended{false};    // BinanceKlineWsSession::is_suspended()
    bool depth_session_stopped{false};      // BinanceWsSession::stopped()
    bool depth_tracking{false};             // DepthManager::state() == DepthState::Tracking
    bool consumer_continuity_broken{false}; // the consumer-side KlineBarGapGuard::is_suspended()
};

enum class FeedInvalidReason : std::uint8_t {
    None = 0,  // feed valid
    KlineSessionStopped = 1,
    DepthSessionStopped = 2,
    KlineSessionSuspended = 3,
    ConsumerContinuityBroken = 4,
    DepthNotTracking = 5,
};

// Priority order is fixed and meaningful: a stopped session is reported ahead of a merely
// suspended/untracked one, because "stopped" is the terminal, restart-required condition.
inline constexpr FeedInvalidReason evaluate_feed_validity(const FeedHealthInputs& in) noexcept {
    if (in.kline_session_stopped) return FeedInvalidReason::KlineSessionStopped;
    if (in.depth_session_stopped) return FeedInvalidReason::DepthSessionStopped;
    if (in.kline_session_suspended) return FeedInvalidReason::KlineSessionSuspended;
    if (in.consumer_continuity_broken) return FeedInvalidReason::ConsumerContinuityBroken;
    if (!in.depth_tracking) return FeedInvalidReason::DepthNotTracking;
    return FeedInvalidReason::None;
}

// True for the reasons that cannot heal without a process restart (until 6b-0f exists): callers
// exit non-zero on these. DepthNotTracking is transient (initial sync, or a resync in progress)
// and is NOT terminal -- the gate simply stays closed until DepthManager gets back to Tracking.
inline constexpr bool feed_invalid_reason_is_terminal(FeedInvalidReason r) noexcept {
    return r == FeedInvalidReason::KlineSessionStopped ||
           r == FeedInvalidReason::DepthSessionStopped ||
           r == FeedInvalidReason::KlineSessionSuspended ||
           r == FeedInvalidReason::ConsumerContinuityBroken;
}

inline constexpr const char* feed_invalid_reason_name(FeedInvalidReason r) noexcept {
    switch (r) {
        case FeedInvalidReason::None: return "None";
        case FeedInvalidReason::KlineSessionStopped: return "KlineSessionStopped";
        case FeedInvalidReason::DepthSessionStopped: return "DepthSessionStopped";
        case FeedInvalidReason::KlineSessionSuspended: return "KlineSessionSuspended";
        case FeedInvalidReason::ConsumerContinuityBroken: return "ConsumerContinuityBroken";
        case FeedInvalidReason::DepthNotTracking: return "DepthNotTracking";
    }
    return "?";
}

}  // namespace hy
