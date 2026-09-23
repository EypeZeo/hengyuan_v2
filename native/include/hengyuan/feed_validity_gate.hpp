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
//
// 批次 6 6b-0f-5: FeedHealthInputs/evaluate_feed_validity above assume a bare, fail-stop session --
// stopped() meaning "dead, the whole process must restart". Once a session is wrapped in
// PublicFeedSupervisor (kline: 6b-0f-2 onward; depth: 6b-0f-4's generation reset is the consumer-side
// half of the same move), stopped() on the underlying session is no longer the right signal: it is
// legitimately true between generations even in a healthy, actively-reconnecting feed. See
// SupervisedFeedInputs/evaluate_supervised_feed_validity further down for the supervisor-aware
// replacement. The two vocabularies deliberately COEXIST rather than one replacing the other in
// place: live_submit_preflight_harness.cpp (6b-1) still drives raw, unsupervised sessions and keeps
// compiling against the old names until it is rewritten to use the supervisors -- at which point
// FeedHealthInputs/evaluate_feed_validity/FeedInvalidReason have no remaining caller and can be
// deleted, rather than being half-migrated underneath a file that still needs the old behavior.

#pragma once

#include <hengyuan/public_feed_supervisor.hpp>

#include <cstdint>

namespace hy {

// Default upper bound on one process run -- NOT a workaround for Binance's 24h WebSocket
// connection limit any more. That limit is real (confirmed 2026-09-21 against the official docs,
// github.com/binance/binance-spot-api-docs, web-socket-streams.md, "General WSS information";
// see public_feed_policy.hpp's own citation), but a PublicFeedSupervisor-wrapped feed now rolls
// over well before it hits on its own (public_feed_policy.hpp's kPublicFeedMaxConnectionAgeMs /
// kline_rollover_window_open()) -- a session that reconnects on schedule does not "end the run
// abruptly" the way the old fail-stop-only sessions this comment used to describe did. This is
// now a plain operational bound (log rotation, a predictable restart cadence) a caller may choose
// to enforce, orthogonal to feed correctness. Overridable by the caller, never larger than the
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

// --- 6b-0f-5: the supervised-feed vocabulary (see the header comment above) --------------------

struct SupervisedFeedInputs {
    // PublicFeedSupervisor<BinanceKlineWsSession>::state() / PublicFeedSupervisor<BinanceWsSession<N>>::state()
    FeedState kline_feed_state{FeedState::Idle};
    FeedState depth_feed_state{FeedState::Idle};
    bool kline_sync_live{false};  // KlineFeedSync::live() -- the CONSUMER side: a connected session
                                   // can still be NeedsBackfill (a gap was just detected, or the
                                   // initial backfill after startup hasn't landed yet)
    bool depth_tracking{false};   // DepthManager::state() == DepthState::Tracking
};

enum class SupervisedFeedInvalidReason : std::uint8_t {
    None = 0,             // feed valid
    KlineFeedGaveUp = 1,  // supervisor reached FeedState::Terminal: unrecoverable without a process restart
    DepthFeedGaveUp = 2,
    KlineDisconnected = 3,  // supervisor is reconnecting (Connecting/Draining/Backoff) -- transient
    DepthDisconnected = 4,
    KlineNotSynced = 5,   // session connected, but the consumer-side sync is not live yet
    DepthNotTracking = 6, // session connected, but DepthManager has not (yet) reached Tracking
};

// Priority order, kline before depth throughout (matching evaluate_feed_validity's own kline-first
// convention above): a Terminal supervisor outranks a merely-reconnecting one, which outranks a
// connected-but-not-yet-synced one -- the same "how bad is it" ordering as the unsupervised gate,
// just restated in terms a reconnecting feed can actually be in.
inline constexpr SupervisedFeedInvalidReason evaluate_supervised_feed_validity(const SupervisedFeedInputs& in) noexcept {
    if (in.kline_feed_state == FeedState::Terminal) return SupervisedFeedInvalidReason::KlineFeedGaveUp;
    if (in.depth_feed_state == FeedState::Terminal) return SupervisedFeedInvalidReason::DepthFeedGaveUp;
    if (in.kline_feed_state != FeedState::Connected) return SupervisedFeedInvalidReason::KlineDisconnected;
    if (in.depth_feed_state != FeedState::Connected) return SupervisedFeedInvalidReason::DepthDisconnected;
    if (!in.kline_sync_live) return SupervisedFeedInvalidReason::KlineNotSynced;
    if (!in.depth_tracking) return SupervisedFeedInvalidReason::DepthNotTracking;
    return SupervisedFeedInvalidReason::None;
}

// True only for the reasons a supervisor itself has given up on -- everything else is the
// supervisor actively working (reconnecting, or a session connected but not yet synced), which
// heals on its own without any caller action. This is the whole point of wrapping a feed in
// PublicFeedSupervisor: unlike feed_invalid_reason_is_terminal() above, a mere disconnect is no
// longer terminal.
inline constexpr bool supervised_feed_invalid_reason_is_terminal(SupervisedFeedInvalidReason r) noexcept {
    return r == SupervisedFeedInvalidReason::KlineFeedGaveUp || r == SupervisedFeedInvalidReason::DepthFeedGaveUp;
}

inline constexpr const char* supervised_feed_invalid_reason_name(SupervisedFeedInvalidReason r) noexcept {
    switch (r) {
        case SupervisedFeedInvalidReason::None: return "None";
        case SupervisedFeedInvalidReason::KlineFeedGaveUp: return "KlineFeedGaveUp";
        case SupervisedFeedInvalidReason::DepthFeedGaveUp: return "DepthFeedGaveUp";
        case SupervisedFeedInvalidReason::KlineDisconnected: return "KlineDisconnected";
        case SupervisedFeedInvalidReason::DepthDisconnected: return "DepthDisconnected";
        case SupervisedFeedInvalidReason::KlineNotSynced: return "KlineNotSynced";
        case SupervisedFeedInvalidReason::DepthNotTracking: return "DepthNotTracking";
    }
    return "?";
}

}  // namespace hy
