// SPDX-License-Identifier: proprietary
// barrier_sweep.hpp — private-stream migration M3b (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 5.3): the transport of the sweep between the hot thread
// (which owns the ReconnectBarrier) and the reconcile thread (which owns the OrderTracker).
//
// What a sweep is. When a private-stream generation is (re)connected the barrier cannot let a new order out until every order that is still in flight
// has had an AUTHORITATIVE answer from the exchange that is newer than the reconnect -- an immediate REST query of each of them, not the next scheduled
// poll. The barrier (reconnect_barrier.hpp) asks for it with a numbered request (take_sweep_request) and accepts the answer only for the number that is
// current (on_sweep_report). This header is how the request travels to the reconcile thread and how the answer travels back.
//
// The two directions, and why neither is a new shared-memory protocol.
//
//   hot -> reconcile   SweepRequestRing, an SpscRing<SweepRequest, 4>: the existing single-producer/single-consumer primitive with its
//                      release/acquire pair, carrying one epoch per request. Epochs only grow, so the consumer keeps the NEWEST one it finds and
//                      ignores the rest: a later request supersedes an earlier one whose answer nobody wants any more.
//
//   reconcile -> hot   NOT a second ring: a SweepDone marker rides IN-BAND in the ReconcileEventRing that already carries the reconcile thread's
//                      resolutions to the hot thread (ReconcileEvent::kind == SweepDone). The ring is FIFO, so by the time the hot thread pops the
//                      marker it has already popped -- and drain_reconcile_events() has already made durable and published -- every resolution the
//                      sweep produced before it. A separate report channel would have needed a counter and a rule "do not apply a report before that
//                      many events have been published"; the marker makes the rule true by construction. Without that rule the barrier could declare
//                      "reconciled" while a fill the sweep had found was still sitting in the ring, and the next order would be planned on a position
//                      that is not true -- the very thing the barrier exists to prevent.
//
// What "answered" means (the reconcile thread's side is in order_tracker.hpp, poll_once()). A sweep for epoch E is COMPLETE when every order the tracker
// holds that poll_once() can ask about has been given a DEFINITE answer after the request for E was taken: the exchange identified the order's state
// (changed or unchanged), and -- if it changed -- the resolution was pushed to the event ring. An inconclusive query, a request that never left the
// process (rate limit, stale clock), a resolution the full ring refused: none of these is an answer, the order stays unanswered and is asked again.
// Orders whose fate this process does not know and will not ask about -- escalated to the operator -- are counted in `unresolved` of the marker and
// never go away on their own; so the barrier stays Sweeping while one exists (fail-closed; the operator's way back does not exist yet, see the
// design's appendix D).
//
// THREAD OWNERSHIP. SweepRequestRing: producer = the hot thread (SweepPump), consumer = the reconcile thread (SweepJob). SweepJob is owned by the
// reconcile thread, like the OrderTracker it works with: no atomics of its own.

#pragma once

#include <hengyuan/spsc_ring.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hy {

// hot -> reconcile: "sweep every order you hold, under this epoch".
struct SweepRequest {
    std::uint64_t epoch{0};  // the barrier's epoch; 0 is never a valid request
};

// reconcile -> hot (carried in a ReconcileEvent): "the sweep of this epoch is complete; this many orders still have no definite answer".
struct SweepDone {
    std::uint64_t epoch{0};
    std::uint32_t unresolved{0};
};

static_assert(std::is_trivially_copyable_v<SweepRequest>, "SweepRequest crosses the SpscRing producer/consumer boundary");
static_assert(std::is_trivially_copyable_v<SweepDone>);

// Four slots: the barrier has at most one request outstanding, and a newer one supersedes it, so a handful is more than a backlog can ever need.
inline constexpr std::size_t kSweepRequestRingCapacity = 4;
using SweepRequestRing = SpscRing<SweepRequest, kSweepRequestRingCapacity>;

// The hot thread's consumer of a SweepDone marker, injected into drain_reconcile_events() as a function pointer (the same style as SubmitPort and
// QueryPort), so that order_tracker.hpp does not depend on the barrier. Without a sink a marker is consumed and dropped: the barrier never hears the
// answer, times the request out, and asks again -- fail-closed.
struct SweepSink {
    using Fn = void (*)(void* user_data, const SweepDone& done) noexcept;

    Fn fn{nullptr};
    void* user_data{nullptr};

    [[nodiscard]] bool is_valid() const noexcept { return fn != nullptr; }
    void deliver(const SweepDone& done) const noexcept {
        if (fn != nullptr) fn(user_data, done);
    }
};

// Has the account snapshot been refreshed at or after `since_ms` (the hot thread's steady clock)? The barrier's Reconciled condition includes it
// (design 5.2): balances must not predate the fills the sweep found. NOTHING in the tree refreshes the account yet (that is the live wiring, M5), so an
// unwired probe answers "no" and the barrier cannot complete -- fail-closed, like an unwired SubmitReadiness.
struct SweepAccountProbe {
    using Fn = bool (*)(void* user_data, std::int64_t since_ms) noexcept;

    Fn fn{nullptr};
    void* user_data{nullptr};

    [[nodiscard]] bool fresh_since(std::int64_t since_ms) const noexcept { return fn != nullptr && fn(user_data, since_ms); }
};

struct SweepJobStats {
    std::uint64_t requests_taken{0};        // requests popped from the ring
    std::uint64_t sweeps_started{0};        // epochs that began (or restarted) a sweep: strictly above the previous one
    std::uint64_t requests_stale{0};        // popped requests at or below the current epoch (superseded or repeated): ignored
    std::uint64_t sweeps_completed{0};      // SweepDone markers pushed
    std::uint64_t marker_push_failures{0};  // the event ring was full when the marker was due: retried on the next tick
};

// The reconcile thread's bookkeeping of the sweep in progress. It knows nothing about orders: poll_once() tells it when a tick found the sweep
// complete and when the marker went out.
class SweepJob {
public:
    explicit SweepJob(SweepRequestRing& requests) noexcept : requests_(requests) {}

    SweepJob(const SweepJob&) = delete;
    SweepJob& operator=(const SweepJob&) = delete;

    // First thing in a tick, BEFORE the ingress ring is drained: pops every pending request and starts a sweep for the newest epoch that is above
    // the current one (an older or repeated epoch is dropped). Order matters: the hot thread pushed the orders it sent before it posted the
    // request, so a request seen here guarantees (release/acquire on the ring that carried it, then the ingress ring's own acquire) that those
    // orders are in the ingress ring by the time poll_once() drains it next -- the sweep cannot miss an order that preceded its request.
    // `now_ms` is the reconcile thread's own clock; it marks the start, which separates "answered before the sweep" from "answered by it".
    // Returns true when a new sweep began.
    bool take_requests(std::int64_t now_ms) noexcept {
        bool began = false;
        SweepRequest request{};
        while (requests_.try_pop(request)) {
            ++stats_.requests_taken;
            if (request.epoch == 0 || request.epoch <= epoch_) {
                ++stats_.requests_stale;
                continue;
            }
            epoch_ = request.epoch;
            started_ms_ = now_ms;
            running_ = true;
            began = true;
            ++stats_.sweeps_started;
        }
        return began;
    }

    // A sweep was begun and its marker has not gone out yet.
    [[nodiscard]] bool running() const noexcept { return running_; }
    // The epoch of the sweep in progress (or of the last one, once it finished); 0 before any.
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    // The reconcile clock reading at which the sweep in progress began.
    [[nodiscard]] std::int64_t started_ms() const noexcept { return started_ms_; }

    // poll_once() pushed the marker: this sweep is over.
    void finished() noexcept {
        running_ = false;
        ++stats_.sweeps_completed;
    }
    // poll_once() could not push the marker (the event ring was full): the sweep stays running and the marker is due again next tick.
    void marker_push_failed() noexcept { ++stats_.marker_push_failures; }

    [[nodiscard]] const SweepJobStats& stats() const noexcept { return stats_; }

private:
    SweepRequestRing& requests_;
    std::uint64_t epoch_{0};
    std::int64_t started_ms_{0};
    bool running_{false};
    SweepJobStats stats_{};
};

}  // namespace hy
