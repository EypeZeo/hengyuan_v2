// SPDX-License-Identifier: proprietary
// synthetic_readiness.hpp — private-stream migration M3a: a SubmitReadiness for environments that have NO private stream -- the verified dry-run
// drills, the evidence and preflight harnesses, and the orchestrator's unit tests -- so that Gate 2b (submit_readiness.hpp) is a real gate in all
// of them instead of a flag they switch off.
//
// What it is: the REAL components, driven through their REAL public transitions with scripted observations. StartupRecovery is updated with a
// clean observation until it is Ready; the ReconnectBarrier sees a connected, active generation, hands out a sweep request, and is given a
// clean report; the probe keeps answering "generation G, connected, active just now, nothing dropped". The kill switch is the caller's own, so a
// tripped switch still refuses, exactly as in production. Nothing is stubbed out of the decision.
//
// What it is NOT: a live process's readiness. It claims a stream that does not exist. A process that sends real orders wires the real probe
// (make_feed_observation() over the supervisor, StartupRecovery over the durable log) -- that wiring is M5. test_submit_readiness_guard.cpp pins
// which production files may include this header; a new use is a review item, and every executable that does is a drill or a harness whose own
// banner says so.

#pragma once

#include <hengyuan/kill_switch.hpp>
#include <hengyuan/reconnect_barrier.hpp>
#include <hengyuan/startup_recovery.hpp>
#include <hengyuan/submit_readiness.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace hy {

class SyntheticReadiness {
public:
    explicit SyntheticReadiness(const KillSwitch& kill_switch, std::uint64_t generation = 1) noexcept
        : recovery_(kReconcileDeadlineMs)
        , readiness_(recovery_, kill_switch, ReadinessProbe{&synthetic_feed, &synthetic_recovery, &synthetic_clock, this})
        , generation_(generation == 0 ? 1 : generation) {
        // Bootstrapping -> Recovering -> Reconciling -> Ready in one update: nothing to recover, nothing unconfirmed.
        (void)recovery_.update(clean_recovery_observation(), 0);
        // Down -> Sweeping, then the sweep that comes back clean: Reconciled(generation).
        readiness_.observe();
        const std::int64_t now = steady_now_ms();
        if (const std::optional<std::uint64_t> epoch = readiness_.barrier().take_sweep_request(now)) {
            readiness_.barrier().on_sweep_report(SweepReport{*epoch, 0U, true}, now);
        }
    }

    SyntheticReadiness(const SyntheticReadiness&) = delete;
    SyntheticReadiness& operator=(const SyntheticReadiness&) = delete;

    [[nodiscard]] SubmitReadiness& readiness() noexcept { return readiness_; }
    [[nodiscard]] const StartupRecovery& recovery() const noexcept { return recovery_; }

private:
    static constexpr std::int64_t kReconcileDeadlineMs = 60'000;

    [[nodiscard]] static std::int64_t steady_now_ms() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    [[nodiscard]] static RecoveryObservation clean_recovery_observation() noexcept {
        RecoveryObservation o{};
        o.clock_synced = true;
        o.exchange_info_loaded = true;
        o.sink_open = true;
        o.sink_fenced = false;
        o.scan_status = RecoveryScanStatus::Clean;
        o.recovery_applied = true;
        o.recovered_orders = 0;
        o.applied_orders = 0;
        o.unconfirmed_orders = 0;
        o.account_fresh = true;
        o.user_data_connected = true;
        return o;
    }

    [[nodiscard]] static FeedObservation synthetic_feed(void* self) noexcept {
        const std::int64_t now = steady_now_ms();
        FeedObservation o{};
        o.generation = static_cast<const SyntheticReadiness*>(self)->generation_;
        o.connected = true;
        o.last_activity_ms = now;
        o.sampled_at_ms = now;
        return o;
    }

    [[nodiscard]] static RecoveryObservation synthetic_recovery(void*) noexcept { return clean_recovery_observation(); }
    [[nodiscard]] static bool synthetic_clock(void*) noexcept { return true; }

    StartupRecovery recovery_;
    SubmitReadiness readiness_;
    std::uint64_t generation_;
};

}  // namespace hy
