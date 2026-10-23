// SPDX-License-Identifier: proprietary
// sweep_pump.hpp — private-stream migration M3b (barrier_sweep.hpp, design 5.3): the hot thread's side of the sweep transport.
//
// The ReconnectBarrier is pure logic and moves nothing itself. The pump is what turns "Sweeping, and a request is due" into a SweepRequest on the
// ring to the reconcile thread, and what turns a SweepDone marker -- popped from the reconcile event ring by drain_reconcile_events() -- back into
// the SweepReport the barrier takes:
//
//   service()   the loop's per-cycle step (orchestrate_submit() runs it at its top when ctx.sweep is set): refresh the barrier from a fresh feed
//               observation, and if it wants a sweep, post the request. A request the ring cannot take is rescinded (the barrier offers the same
//               epoch again at the next call) -- never counted as sent.
//   sink()      the SweepSink to hand to drain_reconcile_events(). A marker means: the reconcile thread asked the exchange about every order it
//               holds and every resolution it found has already been popped -- and made durable and published -- in front of this marker. What the
//               pump adds is the one fact the reconcile thread cannot know, the hot thread's: has the ACCOUNT snapshot been refreshed since the
//               request went out (SweepAccountProbe)? Only a marker for the epoch that is current can complete the barrier; the barrier itself
//               drops the rest.
//
// The pump holds no state the barrier does not need: which epoch it last posted, and when (the hot thread's steady clock, from the same feed
// observation the barrier saw), so the account's age is judged against the right request.
//
// THREAD OWNERSHIP: the hot/submit thread, with the SubmitReadiness it drives. It outlives every call that uses it.

#pragma once

#include <hengyuan/barrier_sweep.hpp>
#include <hengyuan/submit_readiness.hpp>

#include <cstdint>
#include <optional>

namespace hy {

struct SweepPumpStats {
    std::uint64_t requests_sent{0};          // requests the ring took
    std::uint64_t request_push_failures{0};  // the ring was full: rescinded, offered again later
    std::uint64_t markers_received{0};       // SweepDone markers delivered by drain_reconcile_events()
    std::uint64_t reports_account_stale{0};  // a marker of the current epoch with no unresolved order, held back because the account was not refreshed
};

class SweepPump {
public:
    SweepPump(SubmitReadiness& readiness, SweepRequestRing& requests, SweepAccountProbe account = {}) noexcept
        : readiness_(readiness), requests_(requests), account_(account) {}

    SweepPump(const SweepPump&) = delete;
    SweepPump& operator=(const SweepPump&) = delete;

    void service() noexcept {
        const std::optional<std::int64_t> now = readiness_.observe();
        if (!now) return;  // the readiness probe is incomplete: nothing is observed, nothing can be swept
        const std::optional<std::uint64_t> epoch = readiness_.barrier().take_sweep_request(*now);
        if (!epoch) return;
        if (requests_.try_push(SweepRequest{*epoch})) {
            ++stats_.requests_sent;
            requested_epoch_ = *epoch;
            requested_at_ms_ = *now;
        } else {
            ++stats_.request_push_failures;
            readiness_.barrier().on_sweep_request_undelivered(*epoch);
        }
    }

    [[nodiscard]] SweepSink sink() noexcept { return SweepSink{&SweepPump::deliver, this}; }

    [[nodiscard]] const SweepPumpStats& stats() const noexcept { return stats_; }
    // The epoch of the request most recently put on the ring (0 = none yet) and the hot-thread steady time it went out at.
    [[nodiscard]] std::uint64_t requested_epoch() const noexcept { return requested_epoch_; }
    [[nodiscard]] std::int64_t requested_at_ms() const noexcept { return requested_at_ms_; }

private:
    static void deliver(void* self, const SweepDone& done) noexcept { static_cast<SweepPump*>(self)->on_done(done); }

    void on_done(const SweepDone& done) noexcept {
        ++stats_.markers_received;
        // Bring the barrier up to date first, exactly as authorize() does: a generation change or a silent heartbeat that the feed shows NOW makes
        // this marker's epoch obsolete before it is applied.
        const std::optional<std::int64_t> now = readiness_.observe();
        if (!now) return;
        const bool current = done.epoch == requested_epoch_;
        const bool fresh = current && account_.fresh_since(requested_at_ms_);
        if (current && done.unresolved == 0 && !fresh) ++stats_.reports_account_stale;
        readiness_.barrier().on_sweep_report(SweepReport{done.epoch, done.unresolved, fresh}, *now);
    }

    SubmitReadiness& readiness_;
    SweepRequestRing& requests_;
    SweepAccountProbe account_;
    std::uint64_t requested_epoch_{0};
    std::int64_t requested_at_ms_{0};
    SweepPumpStats stats_{};
};

}  // namespace hy
