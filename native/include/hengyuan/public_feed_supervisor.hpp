// SPDX-License-Identifier: proprietary
// public_feed_supervisor.hpp — 批次 6 6b-0f-2: the reconnect state machine for the PUBLIC market-data
// sessions (kline WS, depth WS). BinanceKlineWsSession / BinanceWsSession are deliberately fail-stop
// with zero reconnect logic inside the class, which left a single dropped connection as the end of
// the feed (外部复核 P0-05); this is the layer that was missing.
//
// SHAPE: the same as UserDataWsSessionSupervisor (binance_user_data_ws_supervisor.hpp), which this
// follows on purpose -- a brand-new session per attempt, no per-attempt join (a dropped session
// object stays alive through its own shared_from_this() until its posted handlers finish), and
// everything owned by ONE thread, the hot/submit thread that calls poll(). The only cross-thread
// reads are the session's own stopped()/is_connected() atomics, which already exist. What is new:
//
//   * generic over the session type (it only needs start/stop/stopped/is_connected), so the state
//     machine is Boost-free and tested against fakes; the real sessions plug in through the factory;
//   * exponential backoff WITH jitter, and flap protection: a connection only resets the failure
//     streak once it has stayed up for stable_after_ms -- a peer that accepts the handshake and then
//     drops within seconds must keep escalating, not be retried every initial_backoff_ms forever;
//   * a connect deadline: a session that neither connects nor stops is stopped and counted failed;
//   * GENERATIONS: each session instance gets a strictly increasing number, and generation N+1 only
//     starts once generation N has stopped() AND the consumer has acknowledged the drain (below);
//   * planned rollover: Binance closes a single connection after 24h, so a healthy session older than
//     max_connection_age_ms is replaced at the next moment the consumer approves, with no failure
//     counted and no backoff;
//   * an optional terminal state after N consecutive failures. UserDataWsSessionSupervisor never gives
//     up (a silent user-data stream is worse than a retrying one); a market-data feed that stays dead
//     means the strategy is blind, and the operator must be told -- the harness turns Terminal into
//     a non-zero exit instead of trading on nothing.
//
// SHARING ONE RING ACROSS GENERATIONS: consecutive sessions push into the same SpscRing, and
// SpscRing keeps a PLAIN producer-private cached_tail_, so two producers may only alternate if every
// push comes from the same thread. This repo's topology gives that: one io_context, one dedicated
// I/O thread running ioc.run() for the process's lifetime, so every session's handlers -- old and
// new -- run on it. If a multi-threaded io_context is ever introduced, each generation needs its own
// ring (or the sessions' stopped()/stop flags must become release/acquire and the ring reset).
// test_public_feed_supervisor_concurrency.cpp reproduces exactly this arrangement under TSan.
//
// THE DRAIN ACK (DrainFn) IS HYGIENE, NOT THE SAFETY NET. Called on the hot thread once per poll
// while the old generation is gone and the next has not started, until it returns true or
// drain_timeout_ms elapses (the timeout proceeds anyway and is counted). Its job is to let the
// consumer (a) consume bars already received, in order, rather than leave them to be mistaken for
// the next generation's, and (b) reset per-generation state -- DepthManager::start_buffering() -- at
// the one moment no producer is running. Correctness never depends on the drain being complete: the
// stop flags are relaxed, so on weak-memory hardware the last push of a dying session can become
// visible after the drain reported empty, and the consumer-side validators (a second
// KlineBarGapGuard, DepthManager's update-id continuity) are what actually reject anything
// out of sequence. FIFO order in the ring is preserved regardless.
//
// THREAD OWNERSHIP: poll()/shutdown()/every accessor from the same single thread for the whole
// lifetime. The factory, drain and boundary callbacks run on that thread and MUST NOT throw
// (poll() is noexcept: a throwing callback terminates, the same fail-stop stance as an allocation
// failure elsewhere on this path). Not copyable/movable; one long-lived instance per feed.

#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

namespace hy {

template <typename S>
concept SupervisedFeedSession = requires(S& s, const S& cs) {
    s.start();
    s.stop();
    { cs.stopped() } -> std::convertible_to<bool>;
    { cs.is_connected() } -> std::convertible_to<bool>;
};

// Like UserDataWsReconnectPolicy, these numbers have no spec/doc basis and are a starting point to
// revisit against real operational experience -- but a public WS handshake against a rate-limit-
// sensitive exchange wants the same slow, gentle curve, so the same base values.
struct FeedSupervisorPolicy {
    std::int64_t initial_backoff_ms{1000};
    std::uint32_t backoff_multiplier{2};
    std::int64_t max_backoff_ms{60'000};
    std::uint32_t jitter_percent{20};  // +/- this share of each delay, uniform; clamped to 0..100
    std::uint64_t jitter_seed{0x9E3779B97F4A7C15ULL};  // production seeds this from real entropy
    std::int64_t connect_deadline_ms{30'000};  // neither connected nor stopped by then -> stop() + failed
    std::int64_t drain_timeout_ms{5'000};      // longest wait for the consumer's drain ack
    std::int64_t stable_after_ms{60'000};      // connected this long before a drop = streak restarts
    std::uint32_t max_consecutive_failures{0}; // 0 = never give up; N = Terminal at the Nth in a row
    std::int64_t max_connection_age_ms{0};     // 0 = no planned rollover
};

enum class FeedState : std::uint8_t {
    Idle = 0,        // nothing started yet
    Connecting = 1,  // a session is running but has not been observed connected
    Connected = 2,   // a session is running and connected
    Draining = 3,    // the old generation is gone; waiting for the consumer's drain ack
    Backoff = 4,     // no session; waiting for the next attempt
    Terminal = 5,    // will never start another session (see FeedTerminalReason)
};

enum class FeedTerminalReason : std::uint8_t {
    None = 0,
    Shutdown = 1,
    TooManyFailures = 2,
};

inline constexpr const char* feed_state_name(FeedState s) noexcept {
    switch (s) {
        case FeedState::Idle: return "Idle";
        case FeedState::Connecting: return "Connecting";
        case FeedState::Connected: return "Connected";
        case FeedState::Draining: return "Draining";
        case FeedState::Backoff: return "Backoff";
        case FeedState::Terminal: return "Terminal";
    }
    return "?";
}

struct FeedSupervisorStats {
    FeedState state{FeedState::Idle};
    FeedTerminalReason terminal_reason{FeedTerminalReason::None};
    std::uint64_t generation{0};            // 0 = no session ever started
    std::uint32_t consecutive_failures{0};  // the current streak (see stable_after_ms)
    std::uint64_t total_attempts{0};
    std::uint64_t total_failures{0};
    std::uint64_t rollovers{0};
    std::uint64_t connect_timeouts{0};
    std::uint64_t drain_timeouts{0};
    std::int64_t last_connected_ms{0};  // 0 = never observed connected
    std::int64_t next_attempt_ms{0};
};

namespace detail {

// Delays above a day are a misconfiguration; capping them here also keeps every jitter/time sum
// below far from int64 overflow (2 * cap << INT64_MAX).
inline constexpr std::int64_t kFeedMaxBackoffCapMs = 24LL * 60 * 60 * 1000;
// Used only when the configured curve is unusable (initial or max <= 0): a supervisor must never
// degenerate into retrying on every poll.
inline constexpr std::int64_t kFeedFallbackBackoffMs = 1000;

// a + b saturating at the int64 range (time + delay).
inline constexpr std::int64_t feed_sat_add_ms(std::int64_t a, std::int64_t b) noexcept {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (b > 0 && a > kMax - b) return kMax;
    if (b < 0 && a < kMin - b) return kMin;
    return a + b;
}

// later - earlier clamped to [0, INT64_MAX]. A clock that appears to go backwards yields 0 (nothing
// has elapsed), so a `>= deadline` never fires and a `< timeout` keeps waiting; and the subtraction
// itself cannot overflow when `earlier` is very negative.
inline constexpr std::int64_t feed_elapsed_ms(std::int64_t later, std::int64_t earlier) noexcept {
    if (later <= earlier) return 0;
    if (earlier < 0 && later > std::numeric_limits<std::int64_t>::max() + earlier) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return later - earlier;
}

// initial * multiplier^attempt, saturating at max_ms -- the same check-before-multiply loop shape as
// reconcile_backoff_delay_ms() (order_tracker.hpp) and ws_reconnect_backoff_delay_ms(); never
// `initial << attempt`, which is undefined for a large enough shift. `attempt` 0 = the first retry.
// multiplier 1 (no growth) returns before the loop so a huge attempt count cannot spin.
inline constexpr std::int64_t saturating_backoff_ms(std::int64_t initial_ms, std::uint32_t multiplier,
                                                    std::int64_t max_ms, std::uint32_t attempt) noexcept {
    if (initial_ms <= 0 || max_ms <= 0) return max_ms > 0 ? max_ms : kFeedFallbackBackoffMs;
    std::int64_t interval = initial_ms;
    if (interval > max_ms) return max_ms;
    const std::int64_t m = multiplier == 0 ? 1 : static_cast<std::int64_t>(multiplier);
    if (m == 1) return interval;
    for (std::uint32_t i = 0; i < attempt; ++i) {
        if (interval > max_ms / m) return max_ms;
        interval *= m;
    }
    return interval > max_ms ? max_ms : interval;
}

// xorshift64*: tiny, allocation-free, deterministic for a given seed (tests), and good enough for
// spreading reconnects -- this is not cryptography.
class FeedJitter {
public:
    explicit constexpr FeedJitter(std::uint64_t seed) noexcept
        : state_(seed != 0 ? seed : 0x9E3779B97F4A7C15ULL) {}

    constexpr std::uint64_t next() noexcept {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

private:
    std::uint64_t state_;
};

// base +/- percent%, uniform over the closed interval, never below 1ms for a positive base.
// spread = base * pct / 100 is computed as (base/100)*pct + ((base%100)*pct)/100, which is exact and
// cannot overflow (spread <= base), unlike base * pct.
inline constexpr std::int64_t jittered_ms(std::int64_t base_ms, std::uint32_t percent,
                                          std::uint64_t random) noexcept {
    if (base_ms <= 0) return base_ms;
    const std::int64_t pct = percent > 100U ? 100 : static_cast<std::int64_t>(percent);
    if (pct == 0) return base_ms;
    const std::int64_t spread = (base_ms / 100) * pct + ((base_ms % 100) * pct) / 100;
    const std::uint64_t range = static_cast<std::uint64_t>(spread) * 2U + 1U;
    const std::int64_t delay = (base_ms - spread) + static_cast<std::int64_t>(random % range);
    return delay < 1 ? 1 : delay;
}

}  // namespace detail

template <SupervisedFeedSession Session>
class PublicFeedSupervisor {
public:
    // Builds generation `generation`'s session (constructed, NOT started -- the supervisor calls
    // start()). May return null for an unusable config; that counts as a failed attempt.
    using Factory = std::function<std::shared_ptr<Session>(std::uint64_t generation)>;
    // The consumer's drain ack: true = the old generation is drained and per-generation state reset.
    // Empty = nothing to wait for. See the header comment for why this is not the safety net.
    using DrainFn = std::function<bool()>;
    // Consulted only when a planned rollover is due: true = replacing the connection now is fine
    // (e.g. a bar has just closed). Empty = always fine.
    using BoundaryFn = std::function<bool(std::int64_t now_ms)>;

    explicit PublicFeedSupervisor(Factory factory, FeedSupervisorPolicy policy = {}, DrainFn drain = {},
                                  BoundaryFn boundary = {})
        : factory_(std::move(factory))
        , policy_(sanitize(policy))
        , drain_(std::move(drain))
        , boundary_(std::move(boundary))
        , jitter_(policy_.jitter_seed) {}

    PublicFeedSupervisor(const PublicFeedSupervisor&) = delete;
    PublicFeedSupervisor& operator=(const PublicFeedSupervisor&) = delete;

    // Non-blocking; call once per hot-loop tick with a monotonic millisecond clock.
    void poll(std::int64_t now_ms) noexcept {
        switch (state_) {
            case FeedState::Terminal:
                return;
            case FeedState::Idle:
                start_generation(now_ms);  // the very first attempt is immediate, no backoff
                return;
            case FeedState::Connecting:
            case FeedState::Connected:
                poll_running(now_ms);
                return;
            case FeedState::Draining:
                poll_draining(now_ms);
                return;
            case FeedState::Backoff:
                if (now_ms >= next_attempt_ms_) start_generation(now_ms);
                return;
        }
    }

    // Idempotent; safe before the first poll(). Stops the current session and drops this object's
    // reference (the session outlives the drop through its own shared_from_this()). Does not block:
    // a caller that needs the I/O quiesced still stops + joins the io_context's thread itself.
    void shutdown() noexcept {
        if (state_ == FeedState::Terminal) return;  // keeps the first reason
        if (session_) {
            session_->stop();
            session_.reset();
        }
        state_ = FeedState::Terminal;
        terminal_reason_ = FeedTerminalReason::Shutdown;
    }

    FeedState state() const noexcept { return state_; }
    // A session is up and connected. NOT sufficient on its own for "the feed is trustworthy": the
    // consumer-side validators (gap guard, DepthManager state) still have their say.
    bool healthy() const noexcept { return state_ == FeedState::Connected; }
    bool terminal() const noexcept { return state_ == FeedState::Terminal; }
    FeedTerminalReason terminal_reason() const noexcept { return terminal_reason_; }
    std::uint64_t generation() const noexcept { return generation_; }
    // The live session, or null between generations. For diagnostics and for the recovery protocol's
    // per-session calls (e.g. resume_after_gap()); a copy, so it stays valid even if replaced.
    std::shared_ptr<Session> current() const noexcept { return session_; }

    FeedSupervisorStats stats() const noexcept {
        FeedSupervisorStats s{};
        s.state = state_;
        s.terminal_reason = terminal_reason_;
        s.generation = generation_;
        s.consecutive_failures = consecutive_failures_;
        s.total_attempts = total_attempts_;
        s.total_failures = total_failures_;
        s.rollovers = rollovers_;
        s.connect_timeouts = connect_timeouts_;
        s.drain_timeouts = drain_timeouts_;
        s.last_connected_ms = last_connected_ms_;
        s.next_attempt_ms = next_attempt_ms_;
        return s;
    }

private:
    static FeedSupervisorPolicy sanitize(FeedSupervisorPolicy p) noexcept {
        if (p.jitter_percent > 100U) p.jitter_percent = 100U;
        if (p.max_backoff_ms > detail::kFeedMaxBackoffCapMs) p.max_backoff_ms = detail::kFeedMaxBackoffCapMs;
        if (p.initial_backoff_ms > detail::kFeedMaxBackoffCapMs) p.initial_backoff_ms = detail::kFeedMaxBackoffCapMs;
        return p;
    }

    void start_generation(std::int64_t now_ms) noexcept {
        ++generation_;
        ++total_attempts_;
        started_at_ms_ = now_ms;
        stop_requested_ = false;
        planned_rollover_ = false;
        was_connected_ = false;
        session_ = factory_ ? factory_(generation_) : nullptr;
        if (!session_) {
            // An unusable config is a failure like any other: back off, never spin.
            end_generation(now_ms);
            return;
        }
        session_->start();
        state_ = FeedState::Connecting;
    }

    void poll_running(std::int64_t now_ms) noexcept {
        if (session_->stopped()) {
            end_generation(now_ms);
            return;
        }
        if (state_ == FeedState::Connecting) {
            if (session_->is_connected()) {
                state_ = FeedState::Connected;
                was_connected_ = true;
                connected_at_ms_ = now_ms;
                last_connected_ms_ = now_ms;
            } else if (!stop_requested_ &&
                       detail::feed_elapsed_ms(now_ms, started_at_ms_) >= policy_.connect_deadline_ms) {
                ++connect_timeouts_;
                stop_requested_ = true;  // once: stop() posts a cancellation, repeating it adds nothing
                session_->stop();
            }
            return;
        }
        // Connected: only a planned rollover can end it from here (a failure shows up as stopped()).
        if (!stop_requested_ && policy_.max_connection_age_ms > 0 &&
            detail::feed_elapsed_ms(now_ms, connected_at_ms_) >= policy_.max_connection_age_ms &&
            (!boundary_ || boundary_(now_ms))) {
            planned_rollover_ = true;
            stop_requested_ = true;
            session_->stop();
        }
    }

    // The current generation is over (stopped, or never produced). Drop it and decide what follows.
    void end_generation(std::int64_t now_ms) noexcept {
        session_.reset();
        last_end_planned_ = planned_rollover_;
        if (planned_rollover_) {
            ++rollovers_;
            consecutive_failures_ = 0;  // it lived a full max_connection_age_ms
        } else {
            if (was_connected_ &&
                detail::feed_elapsed_ms(now_ms, connected_at_ms_) >= policy_.stable_after_ms) {
                consecutive_failures_ = 0;  // a long healthy run ended: this is a fresh streak
            }
            ++consecutive_failures_;
            ++total_failures_;
            if (policy_.max_consecutive_failures > 0 &&
                consecutive_failures_ >= policy_.max_consecutive_failures) {
                state_ = FeedState::Terminal;
                terminal_reason_ = FeedTerminalReason::TooManyFailures;
                return;
            }
        }
        planned_rollover_ = false;
        stop_requested_ = false;
        was_connected_ = false;
        state_ = FeedState::Draining;
        drain_started_ms_ = now_ms;
        poll_draining(now_ms);
    }

    void poll_draining(std::int64_t now_ms) noexcept {
        if (drain_ && !drain_()) {
            if (detail::feed_elapsed_ms(now_ms, drain_started_ms_) < policy_.drain_timeout_ms) return;
            ++drain_timeouts_;  // proceed anyway: see the header comment on why this is safe
        }
        const std::int64_t delay = last_end_planned_ ? 0 : next_backoff_ms();
        next_attempt_ms_ = detail::feed_sat_add_ms(now_ms, delay);
        state_ = FeedState::Backoff;
        // Only a planned rollover starts in the same tick. A failure always waits for the NEXT poll
        // even if the delay rounds to nothing, which also rules out start -> fail -> start recursion
        // for a factory that keeps returning null.
        if (last_end_planned_) start_generation(now_ms);
    }

    std::int64_t next_backoff_ms() noexcept {
        // consecutive_failures_ >= 1 here: this is only reached after an unplanned end.
        const std::int64_t base = detail::saturating_backoff_ms(
            policy_.initial_backoff_ms, policy_.backoff_multiplier, policy_.max_backoff_ms,
            consecutive_failures_ - 1U);
        return detail::jittered_ms(base, policy_.jitter_percent, jitter_.next());
    }

    Factory factory_;
    FeedSupervisorPolicy policy_;
    DrainFn drain_;
    BoundaryFn boundary_;
    detail::FeedJitter jitter_;

    std::shared_ptr<Session> session_;
    FeedState state_{FeedState::Idle};
    FeedTerminalReason terminal_reason_{FeedTerminalReason::None};
    std::uint64_t generation_{0};
    std::uint32_t consecutive_failures_{0};
    std::uint64_t total_attempts_{0};
    std::uint64_t total_failures_{0};
    std::uint64_t rollovers_{0};
    std::uint64_t connect_timeouts_{0};
    std::uint64_t drain_timeouts_{0};
    std::int64_t last_connected_ms_{0};
    std::int64_t next_attempt_ms_{0};

    // Per-generation bookkeeping.
    std::int64_t started_at_ms_{0};
    std::int64_t connected_at_ms_{0};
    std::int64_t drain_started_ms_{0};
    bool was_connected_{false};
    bool stop_requested_{false};
    bool planned_rollover_{false};
    bool last_end_planned_{false};
};

}  // namespace hy
