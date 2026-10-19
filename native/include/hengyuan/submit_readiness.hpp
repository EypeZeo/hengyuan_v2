// SPDX-License-Identifier: proprietary
// submit_readiness.hpp — private-stream migration M3a (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 5.4): the typed answer to "may this process send an
// order right now?". orchestrate_submit() asks for a SubmitReadyToken (Gate 2b) and refuses without one; unwired is refused too.
//
// The token is issued only when ALL of these hold at the moment of the call:
//   * the operator kill switch is Normal (the in-memory breaker; SAFE-04 will replace it with a persistent one -- design R10);
//   * StartupRecovery is Ready AND its live prerequisites hold right now (StartupRecovery::submit_enabled): the durable log is open and not
//     fenced, the clock is synced, the account snapshot is fresh, the user-data stream is connected;
//   * the ReconnectBarrier is Reconciled(G), and the live feed is on that same generation G and connected: a subscription that was swept is the
//     only kind that may carry orders;
//   * a signing timestamp can be issued (the clock is fresh -- the same predicate as issue_clock_ready_token()).
// The public-market-data validity gate (feed_validity_gate) is deliberately NOT here: it answers "may a new order INTENT be formed from this
// data", which the planner consumes; this token answers "may the execution side send one" (design 5.4). Two gates, two questions.
//
// Where the facts come from. The readiness does not hold references to the supervisor, the session, the durable sink or the account: it reads
// them through a ReadinessProbe (function pointers, like SubmitPort and QueryPort), so every input is read AT THE MOMENT OF THE DECISION rather
// than cached by a loop that may have stopped running, and so tests drive it with scripted facts. authorize() first brings the barrier up to
// date from a fresh FeedObservation -- whether or not the loop observed it a moment ago -- and only then decides.
//
// The token is move-only and has exactly one issuer (SubmitReadiness::authorize): the type is the proof that Gate 2b ran. It is not a lease --
// nothing re-checks it after it was issued -- so it is used inside the call that obtained it and let go.
//
// SYNTHETIC SOURCES. Drills, harnesses and tests that have no private stream use synthetic_readiness.hpp, which drives the REAL components
// (barrier, StartupRecovery) through their public transitions with scripted observations. It is not a bypass, and a guard test
// (test_submit_readiness_guard.cpp) pins which production files may use it.
//
// THREAD OWNERSHIP: the hot/submit thread, like the order state it guards; the barrier it owns is single-threaded.

#pragma once

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/reconnect_barrier.hpp>
#include <hengyuan/startup_recovery.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hy {

enum class NotReadyReason : std::uint8_t {
    None = 0,
    Unwired = 1,              // OrchestratorContext::readiness is null
    NoProbe = 2,              // wired, but its probe is incomplete
    KillSwitchNotNormal = 3,
    StartupNotReady = 4,      // StartupRecovery is not Ready, or a live prerequisite of submitting does not hold now
    FeedDown = 5,             // barrier Down: no connected, verified subscription (never connected, dropped, ended, or silent)
    BarrierSweeping = 6,      // barrier Sweeping: the subscription is up but the sweep has not been answered
    GenerationMismatch = 7,   // the barrier's Reconciled generation is not the live feed's (structurally excluded after observe(); kept as an
                              // independent check so a future change of the barrier cannot silently weaken the token)
    ClockNotReady = 8,        // no fresh signing timestamp can be issued
};
inline constexpr std::size_t kNotReadyReasonCount = 9;

[[nodiscard]] constexpr const char* not_ready_reason_name(NotReadyReason r) noexcept {
    switch (r) {
        case NotReadyReason::None: return "None";
        case NotReadyReason::Unwired: return "Unwired";
        case NotReadyReason::NoProbe: return "NoProbe";
        case NotReadyReason::KillSwitchNotNormal: return "KillSwitchNotNormal";
        case NotReadyReason::StartupNotReady: return "StartupNotReady";
        case NotReadyReason::FeedDown: return "FeedDown";
        case NotReadyReason::BarrierSweeping: return "BarrierSweeping";
        case NotReadyReason::GenerationMismatch: return "GenerationMismatch";
        case NotReadyReason::ClockNotReady: return "ClockNotReady";
    }
    return "?";
}

// The facts the decision needs, read when it is made. Every function must be wired; a probe with a missing function refuses everything.
struct ReadinessProbe {
    using FeedFn = FeedObservation (*)(void* user_data) noexcept;
    using RecoveryFn = RecoveryObservation (*)(void* user_data) noexcept;
    using ClockFn = bool (*)(void* user_data) noexcept;

    FeedFn feed{nullptr};          // the supervisor + the current session, see make_feed_observation()
    RecoveryFn recovery{nullptr};  // the durable log, clock, account and stream as StartupRecovery wants them
    ClockFn clock_ready{nullptr};  // a fresh signing timestamp can be issued, see clock_ready_now()
    void* user_data{nullptr};

    [[nodiscard]] bool is_valid() const noexcept { return feed != nullptr && recovery != nullptr && clock_ready != nullptr; }
};

// The production predicate behind ReadinessProbe::clock_ready: exactly the freshness rule issue_clock_ready_token() applies.
[[nodiscard]] inline bool clock_ready_now(const ClockOffsetPublisher& publisher) noexcept {
    return issue_clock_ready_token(publisher, fetch_clock_pair()).has_value();
}

class SubmitReadyToken {
public:
    SubmitReadyToken(const SubmitReadyToken&) = delete;
    SubmitReadyToken& operator=(const SubmitReadyToken&) = delete;
    SubmitReadyToken(SubmitReadyToken&&) noexcept = default;
    SubmitReadyToken& operator=(SubmitReadyToken&&) noexcept = default;
    ~SubmitReadyToken() = default;

    // The stream generation this authorization rests on.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    // The caller's clock (the orchestrator's now_ms) at issue; informational.
    [[nodiscard]] std::int64_t issued_at_ms() const noexcept { return issued_at_ms_; }

private:
    friend class SubmitReadiness;
    constexpr SubmitReadyToken(std::uint64_t generation, std::int64_t issued_at_ms) noexcept
        : generation_(generation), issued_at_ms_(issued_at_ms) {}

    std::uint64_t generation_;
    std::int64_t issued_at_ms_;
};

struct ReadinessResult {
    std::optional<SubmitReadyToken> token{};
    NotReadyReason reason{NotReadyReason::Unwired};  // None exactly when `token` holds one

    [[nodiscard]] bool ready() const noexcept { return token.has_value(); }
};

struct ReadinessStats {
    std::uint64_t authorized{0};
    std::array<std::uint64_t, kNotReadyReasonCount> refused{};  // indexed by NotReadyReason
};

class SubmitReadiness {
public:
    SubmitReadiness(const StartupRecovery& recovery, const KillSwitch& kill_switch, ReadinessProbe probe, BarrierPolicy policy = {}) noexcept
        : recovery_(recovery), kill_switch_(kill_switch), probe_(probe), barrier_(policy) {}

    SubmitReadiness(const SubmitReadiness&) = delete;
    SubmitReadiness& operator=(const SubmitReadiness&) = delete;

    // The loop's per-cycle refresh: pulls a fresh feed observation into the barrier (heartbeat watch, soft barriers, generation changes) even when
    // no order is being sent. authorize() does not depend on it having run.
    void observe() noexcept {
        if (!probe_.is_valid()) return;
        barrier_.observe(probe_.feed(probe_.user_data));
    }

    [[nodiscard]] ReadinessResult authorize(std::int64_t now_ms) noexcept {
        if (!probe_.is_valid()) return refuse(NotReadyReason::NoProbe);

        // The barrier is brought up to date FIRST and unconditionally, so that what it says below is what the feed says now.
        const FeedObservation feed = probe_.feed(probe_.user_data);
        barrier_.observe(feed);

        if (kill_switch_.state() != KillState::Normal) return refuse(NotReadyReason::KillSwitchNotNormal);
        if (!recovery_.submit_enabled(probe_.recovery(probe_.user_data))) return refuse(NotReadyReason::StartupNotReady);
        switch (barrier_.state()) {
            case BarrierState::Down: return refuse(NotReadyReason::FeedDown);
            case BarrierState::Sweeping: return refuse(NotReadyReason::BarrierSweeping);
            case BarrierState::Reconciled: break;
        }
        if (!feed.connected || feed.generation == 0 || barrier_.reconciled_generation() != feed.generation) {
            return refuse(NotReadyReason::GenerationMismatch);
        }
        if (!probe_.clock_ready(probe_.user_data)) return refuse(NotReadyReason::ClockNotReady);

        ++stats_.authorized;
        ReadinessResult result{};
        result.reason = NotReadyReason::None;
        result.token = std::optional<SubmitReadyToken>(SubmitReadyToken(feed.generation, now_ms));
        return result;
    }

    [[nodiscard]] ReconnectBarrier& barrier() noexcept { return barrier_; }
    [[nodiscard]] const ReconnectBarrier& barrier() const noexcept { return barrier_; }
    [[nodiscard]] const ReadinessStats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] ReadinessResult refuse(NotReadyReason reason) noexcept {
        ++stats_.refused[static_cast<std::size_t>(reason)];
        ReadinessResult result{};
        result.reason = reason;
        return result;
    }

    const StartupRecovery& recovery_;
    const KillSwitch& kill_switch_;
    ReadinessProbe probe_;
    ReconnectBarrier barrier_;
    ReadinessStats stats_{};
};

}  // namespace hy
