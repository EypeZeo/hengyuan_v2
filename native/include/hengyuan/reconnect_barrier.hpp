// SPDX-License-Identifier: proprietary
// reconnect_barrier.hpp — private-stream migration M3a (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 5.2, 5.3): the state machine that answers
// "given what the private user-data stream has done, may a NEW order leave this process?".
//
// Why it exists. An order's fate reaches this process by two independent routes: the user-data stream (fast, but it can drop, lag, be cut at a
// connection's 24-hour end or when the exchange says so) and the REST reconcile (authoritative, slower). Between "the stream broke" and "the REST
// sweep caught up" a fill may have happened that this process does not know about: a new order planned on a position that is no longer true.
// The barrier closes that window. From the moment the stream is anything less than a connected, verified subscription, new orders are refused;
// when a stream (re)connects they stay refused until a SWEEP -- an immediate REST query of every order in flight, design 5.3 -- has come back
// with a definite answer for each of them.
//
//     Down        no verified subscription (start, a dropped or ended connection, a heartbeat that stopped, backoff)      -> refuses
//     Sweeping    a verified subscription exists; its sweep is not answered yet, or an event was lost after it started    -> refuses
//     Reconciled  generation G is subscribed AND swept                                                                     -> allows (the
//                 other prerequisites of SubmitReadiness still apply)
//
// What moves it. All of it is observed on the hot thread; none of it is I/O.
//   * observe(FeedObservation): the supervisor's generation and the current session's is_connected(), last_activity_steady_ms(), ring_drops()
//     and lost_events(). A different generation, or a session that is not connected -> Down. A connected session while Down -> Sweeping.
//     Any change in the ring-drop or unreadable-executionReport counters since the barrier last looked (they only grow within one session, so
//     a change means an event was lost; a decrease is not the reading the barrier saw before either) -> a "soft barrier": back to Sweeping
//     WITHOUT a reconnect (what an earlier sweep reported may be out of date). The first connected sample of a generation sets the baseline.
//     No frame at all -- data OR the server's 20 s ping control frame, blueprint 5.7 P-30 -- for stale_after_ms -> the connection is dead
//     whatever TCP says: Down, and take_stop_request() tells the loop to stop the session so the supervisor starts a new generation.
//   * on_generation_ended(g): the consumer drained a serverShutdown / eventStreamTerminated event of generation g. g is over NOW, before the
//     supervisor has noticed the session stopping; observations of g (or older) can no longer bring the barrier up.
//   * take_sweep_request(now) / on_sweep_report(report, now): the sweep protocol (the transport is barrier_sweep.hpp / sweep_pump.hpp, M3b). A
//     request carries an EPOCH; a report is honoured only for the epoch that is current, so the answer to a sweep that a later trouble event made
//     obsolete changes nothing, and a sweep nobody answers is asked again under a new epoch after sweep_timeout_ms. A request that could not be put
//     on the ring to the reconcile thread is rescinded with on_sweep_request_undelivered(epoch).
//   * admit_event(g): an event stamped with a generation OLDER than the Reconciled one is dropped (and counted): the sweep already reflects
//     whatever it said. Before Reconciled -- Down or Sweeping -- old events are merged: OrderFillContext::consume_delta() only ever applies a
//     positive delta, so a late duplicate is harmless there. Generation 0 means "not stamped" (the retired listenKey session) and is always merged.
//
// THREAD OWNERSHIP: the hot/submit thread, together with the rest of the order state (order_lifecycle.hpp's contract). No atomics, no heap, no
// I/O, no clocks of its own (every time comes in as an argument, all in the same steady-clock millisecond domain as the session's
// last_activity_steady_ms()). The class is constexpr so the state machine's tests can also run at compile time.

#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace hy {

enum class BarrierState : std::uint8_t {
    Down = 0,
    Sweeping = 1,
    Reconciled = 2,
};

[[nodiscard]] constexpr const char* barrier_state_name(BarrierState s) noexcept {
    switch (s) {
        case BarrierState::Down: return "Down";
        case BarrierState::Sweeping: return "Sweeping";
        case BarrierState::Reconciled: return "Reconciled";
    }
    return "?";
}

struct BarrierPolicy {
    // No frame -- data or the server's ping -- for this long = the connection is dead. The server pings every 20 s and drops a connection that does
    // not pong within a minute (blueprint 5.7 P-30): three silent ping intervals. A multiple of the PING interval, not of the event interval -- a
    // quiet account has no events at all.
    std::int64_t stale_after_ms{60'000};
    // After a sweep that left orders unresolved: how long before the next request is handed out.
    std::int64_t resweep_after_ms{1'000};
    // A request nobody answered within this long is asked again, under a new epoch.
    std::int64_t sweep_timeout_ms{30'000};
};

// What the hot thread reads off the supervisor and the current session, once per decision. Every default is fail-closed.
struct FeedObservation {
    std::uint64_t generation{0};       // PublicFeedSupervisor::generation(); 0 = no session has ever started
    bool connected{false};             // the current session is_connected() (subscription verified, M2b), is not stopped, and the supervisor is
                                       // in Connected -- see make_feed_observation() in feed_observation.hpp
    std::int64_t last_activity_ms{0};  // session.last_activity_steady_ms(); 0 = nothing yet
    std::uint64_t ring_drops{0};       // session.ring_drops(), monotone within one session
    std::uint64_t lost_events{0};      // session.lost_events(), monotone within one session
    std::int64_t sampled_at_ms{0};     // steady-clock ms at which the fields above were read
};

// The reconcile thread's answer to a sweep request (carried by barrier_sweep.hpp's SweepDone marker; the hot thread adds account_fresh).
struct SweepReport {
    std::uint64_t epoch{0};        // the epoch of the request being answered
    std::uint32_t unresolved{0};   // orders in flight that still have no definite answer newer than the sweep's start
    bool account_fresh{false};     // the account snapshot was refreshed by this sweep
};

struct BarrierStats {
    std::uint64_t generations_seen{0};
    std::uint64_t downs{0};                // entries into Down from another state
    std::uint64_t sweeps_begun{0};         // entries into Sweeping (including restarts)
    std::uint64_t sweeps_requested{0};     // requests handed out by take_sweep_request() (offers; an undelivered one is offered again)
    std::uint64_t sweeps_undelivered{0};   // requests rescinded by on_sweep_request_undelivered(): they never left the hot thread
    std::uint64_t sweeps_completed{0};     // reports that made the barrier Reconciled
    std::uint64_t sweeps_unresolved{0};    // reports that left orders without a definite answer
    std::uint64_t sweep_timeouts{0};       // requests nobody answered in time
    std::uint64_t reports_ignored{0};      // reports of another epoch, or while not waiting for one
    std::uint64_t soft_barriers{0};        // Reconciled/Sweeping restarted by a dropped or unreadable event, without a reconnect
    std::uint64_t stale_heartbeats{0};     // sessions declared dead for silence
    std::uint64_t generation_ends_seen{0}; // on_generation_ended() calls that ended the tracked (or a newer) generation
    std::uint64_t late_events_dropped{0};  // admit_event() == false
};

namespace detail {

// later - earlier, clamped to [0, INT64_MAX]: a clock that appears to run backwards, or a sample taken a moment before the activity it is compared
// with, yields 0 (nothing has elapsed); and the subtraction cannot overflow when `earlier` is very negative.
[[nodiscard]] constexpr std::int64_t barrier_elapsed_ms(std::int64_t later, std::int64_t earlier) noexcept {
    if (later <= earlier) return 0;
    if (earlier < 0 && later > std::numeric_limits<std::int64_t>::max() + earlier) return std::numeric_limits<std::int64_t>::max();
    return later - earlier;
}

// a + b saturating at the int64 range (time + delay).
[[nodiscard]] constexpr std::int64_t barrier_sat_add_ms(std::int64_t a, std::int64_t b) noexcept {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (b > 0 && a > kMax - b) return kMax;
    if (b < 0 && a < kMin - b) return kMin;
    return a + b;
}

}  // namespace detail

class ReconnectBarrier {
public:
    constexpr explicit ReconnectBarrier(BarrierPolicy policy = {}) noexcept : policy_(policy) {}

    // --- the feed ------------------------------------------------------------------------------------------------

    constexpr void observe(const FeedObservation& o) noexcept {
        if (o.generation != generation_) {
            // Another session (or none): everything known about the previous one ends here. Its counters are no baseline for the new session's
            // (they are its own, not a continuation): the new session's first CONNECTED sample sets that, below, and a barrier that is Down compares
            // nothing.
            generation_ = o.generation;
            stop_pending_ = false;
            if (generation_ != 0) ++stats_.generations_seen;
            enter_down();
        }
        if (generation_ == 0 || generation_ <= ended_generation_ || !o.connected) {
            enter_down();
            return;
        }
        if (is_stale(o)) {
            ++stats_.stale_heartbeats;
            ended_generation_ = generation_;  // the loop is about to stop this session; it must not come back in the meantime
            stop_pending_ = true;
            enter_down();
            return;
        }
        // `!=`, not `>`: the counters are monotone within a session, so any difference means the reading is not what this barrier thought it was.
        const bool trouble = o.ring_drops != drops_seen_ || o.lost_events != lost_seen_;
        drops_seen_ = o.ring_drops;
        lost_seen_ = o.lost_events;
        if (state_ == BarrierState::Down) {
            begin_sweep();  // a verified subscription exists; whatever happened before it is what the sweep is for
        } else if (trouble) {
            ++stats_.soft_barriers;
            begin_sweep();
        }
    }

    // A serverShutdown / eventStreamTerminated event of `generation` was drained: that generation is over, whatever the supervisor still says.
    constexpr void on_generation_ended(std::uint64_t generation) noexcept {
        if (generation == 0) return;              // not stamped (the retired listenKey session): says nothing about this stream
        if (generation < generation_) return;      // an old generation's end arriving late; the barrier has moved on
        if (generation > ended_generation_) ended_generation_ = generation;
        ++stats_.generation_ends_seen;
        enter_down();
    }

    // True ONCE after a heartbeat went stale: the loop must stop the session, so the supervisor starts a new generation.
    [[nodiscard]] constexpr bool take_stop_request() noexcept {
        const bool pending = stop_pending_;
        stop_pending_ = false;
        return pending;
    }

    // --- the sweep (design 5.3; the transport is barrier_sweep.hpp / sweep_pump.hpp, M3b) -------------------------

    // The epoch to request a sweep under, or nullopt when none is due: not Sweeping, a request is already outstanding, or the wait after an
    // unresolved answer has not passed. An outstanding request that nobody answered within sweep_timeout_ms is replaced by a new epoch.
    [[nodiscard]] constexpr std::optional<std::uint64_t> take_sweep_request(std::int64_t now_ms) noexcept {
        if (state_ != BarrierState::Sweeping) return std::nullopt;
        if (sweep_outstanding_) {
            if (detail::barrier_elapsed_ms(now_ms, requested_at_ms_) <= policy_.sweep_timeout_ms) return std::nullopt;
            ++stats_.sweep_timeouts;
            ++epoch_;  // a late answer to the old request must change nothing
            sweep_outstanding_ = false;
            next_sweep_not_before_ms_ = kNoPause;
        }
        if (now_ms < next_sweep_not_before_ms_) return std::nullopt;
        sweep_outstanding_ = true;
        requested_at_ms_ = now_ms;
        ++stats_.sweeps_requested;
        return epoch_;
    }

    // The request take_sweep_request() handed out for `epoch` never left the hot thread (the ring to the reconcile thread was full). Nothing was
    // sent, so there is nothing to wait for: the next take_sweep_request() offers the same epoch again, without the timeout and without a pause.
    // Ignored unless `epoch` is the request that is outstanding.
    constexpr void on_sweep_request_undelivered(std::uint64_t epoch) noexcept {
        if (state_ != BarrierState::Sweeping || !sweep_outstanding_ || epoch != epoch_) return;
        sweep_outstanding_ = false;
        ++stats_.sweeps_undelivered;
    }

    constexpr void on_sweep_report(const SweepReport& report, std::int64_t now_ms) noexcept {
        if (state_ != BarrierState::Sweeping || !sweep_outstanding_ || report.epoch != epoch_) {
            ++stats_.reports_ignored;
            return;
        }
        sweep_outstanding_ = false;
        if (report.unresolved == 0 && report.account_fresh) {
            state_ = BarrierState::Reconciled;
            reconciled_generation_ = generation_;
            ++stats_.sweeps_completed;
            return;
        }
        // Not enough: stay Sweeping, and ask again (under a new epoch) after a pause.
        ++stats_.sweeps_unresolved;
        ++epoch_;
        next_sweep_not_before_ms_ = detail::barrier_sat_add_ms(now_ms, policy_.resweep_after_ms);
    }

    // --- events ---------------------------------------------------------------------------------------------------

    // false = drop this event (and it has been counted): it belongs to a generation older than the Reconciled one.
    [[nodiscard]] constexpr bool admit_event(std::uint64_t event_generation) noexcept {
        if (event_generation == 0) return true;
        if (state_ == BarrierState::Reconciled && event_generation < reconciled_generation_) {
            ++stats_.late_events_dropped;
            return false;
        }
        return true;
    }

    // --- what the barrier thinks ------------------------------------------------------------------------------------

    [[nodiscard]] constexpr BarrierState state() const noexcept { return state_; }
    [[nodiscard]] constexpr std::uint64_t generation() const noexcept { return generation_; }
    // The generation that was swept, or 0 while not Reconciled.
    [[nodiscard]] constexpr std::uint64_t reconciled_generation() const noexcept { return reconciled_generation_; }
    [[nodiscard]] constexpr std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] constexpr bool sweep_outstanding() const noexcept { return sweep_outstanding_; }
    [[nodiscard]] constexpr const BarrierStats& stats() const noexcept { return stats_; }
    [[nodiscard]] constexpr const BarrierPolicy& policy() const noexcept { return policy_; }

private:
    [[nodiscard]] constexpr bool is_stale(const FeedObservation& o) const noexcept {
        return o.last_activity_ms <= 0 || detail::barrier_elapsed_ms(o.sampled_at_ms, o.last_activity_ms) > policy_.stale_after_ms;
    }

    constexpr void enter_down() noexcept {
        if (state_ == BarrierState::Down) return;
        state_ = BarrierState::Down;
        reconciled_generation_ = 0;
        sweep_outstanding_ = false;
        ++epoch_;  // an outstanding request's answer is for a sweep that no longer matters
        ++stats_.downs;
    }

    constexpr void begin_sweep() noexcept {
        state_ = BarrierState::Sweeping;
        reconciled_generation_ = 0;
        sweep_outstanding_ = false;
        next_sweep_not_before_ms_ = kNoPause;
        ++epoch_;
        ++stats_.sweeps_begun;
    }

    // "No pause pending": no time is before it (a plain 0 would make a negative clock reading look paused).
    static constexpr std::int64_t kNoPause = std::numeric_limits<std::int64_t>::min();

    BarrierPolicy policy_;
    BarrierState state_{BarrierState::Down};
    std::uint64_t generation_{0};             // the generation being tracked; 0 = none
    std::uint64_t ended_generation_{0};       // generations up to this one are over
    std::uint64_t reconciled_generation_{0};  // != 0 only while Reconciled
    std::uint64_t epoch_{0};
    bool sweep_outstanding_{false};
    bool stop_pending_{false};
    std::int64_t requested_at_ms_{0};
    std::int64_t next_sweep_not_before_ms_{kNoPause};
    std::uint64_t drops_seen_{0};
    std::uint64_t lost_seen_{0};
    BarrierStats stats_{};
};

}  // namespace hy
