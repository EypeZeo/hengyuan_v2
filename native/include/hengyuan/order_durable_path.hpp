// SPDX-License-Identifier: proprietary
// order_durable_path.hpp -- private-stream migration M4 (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 6): which frames make a WebSocket observation of
// an order replayable, and which observations may not be written at all.
//
// WHY. The durable log has to replay. DurableAuditSink::run_recovery_scan_impl() judges every frame of an order against what it already holds for
// that order: the SAME state again (the cumulative fill must not go down and must not pass the intended quantity), or a DIFFERENT state that
// validate_transition() allows -- nothing else; one frame that is neither makes the WHOLE log Corrupt at the next start. (The scan applies its fill
// checks to same-state frames only; a state-changing frame is judged by validate_transition() alone. This header checks the fill of EVERY frame it plans,
// more strictly than the scan does: an overfill or a lower fill is an exchange report that contradicts itself, and is never written.) The reconcile
// thread knows each order's state (ReconcileEvent::prior_state), so its publisher can pick the frames. A WebSocket event reaches the hot thread with no
// such history, so the hot thread keeps its own record of what the log holds for each order -- the DurableShadow, one per order, in OrderFillContext --
// and asks this header what to write.
//
// THE SHADOW IS WHAT THE LOG HOLDS, NOT WHAT THE PROCESS BELIEVES. It is advanced (after_ack()) only AFTER a frame's durable ACK -- a shadow that runs
// ahead of the log makes the next frame illegal -- at every place a frame is made durable: the orchestrator's Prepared and result frames, the reconcile
// publisher, the WebSocket publisher, and the seeding of a recovered order (shadow_of_recovered()). One place that forgets is a shadow that runs
// behind, and a frame written from it is illegal just the same -- test_order_durable_path_guard.cpp pins every such place.
//
// A RECOVERED ORDER is the tricky one: the scan hands back an Ambiguous checkpoint for an order whose last frame is Submitting OR Ambiguous
// (OrderRecoveryCheckpoint is an ABI-pinned layout and does not say which; the scan remaps Submitting to Ambiguous). The shadow is then
// {Ambiguous, maybe_submitting}: a target that replays from both (Accepted, PartialFill, Filled, Rejected) is written as it stands, any other starts
// with an Ambiguous frame -- legal after Submitting and, as a same-state repeat, after Ambiguous. After its first ACK the shadow is exact.
//
// WHAT IS WRITTEN is the exchange's own word: only the seven states a WebSocket execution report can name (map_binance_order_status()) are ever a target.
// The one frame a plan may put in front of the observation is an Ambiguous frame, and only where it is TRUE -- the process did not know the outcome of
// the submit -- never for an order the exchange has already acknowledged (Accepted / PartialFill -> Ambiguous is not an edge, and neither is it true).
//
// KNOWN LIMIT: a PARTIALLY_FILLED report while the log says CancelRequested is Unreachable (CancelRequested -> PartialFill is not an edge of
// validate_transition(), which is shared with the live state machine and is not extended here). Nothing in the live process sends a cancel yet; the day one
// does, that edge needs its own decision.
//
// Everything is derived from validate_transition() itself, never from a list of states, so a change of the state machine cannot leave this behind. Pure
// functions over values: no I/O, no allocation, constexpr.

#pragma once

#include <hengyuan/order_lifecycle.hpp>

#include <cstdint>

namespace hy {

// What the durable log holds for one order, as far as this process has seen it acknowledged.
struct DurableShadow {
    OrderState state{OrderState::Intent};  // the state of the last acknowledged frame
    // true: that frame is Submitting OR Ambiguous and the process cannot tell which (a recovered order that the scan remapped); `state` is Ambiguous.
    bool maybe_submitting{false};
    std::int64_t filled_qty_ticks{0};  // the highest cumulative fill any acknowledged frame carried (the scan's own high-water mark)
};

// The shadow of an order that recovery handed back: `checkpoint_state` and `filled_qty_ticks` are OrderRecoveryCheckpoint::resulting_state and
// ::filled_qty_ticks. Only Ambiguous is uncertain (see above); every other state the scan reports is the exact state of the last frame.
constexpr DurableShadow shadow_of_recovered(OrderState checkpoint_state, std::int64_t filled_qty_ticks) noexcept {
    return DurableShadow{checkpoint_state, checkpoint_state == OrderState::Ambiguous, filled_qty_ticks};
}

// The shadow after a frame `(state, filled_qty_ticks)` has been acknowledged by the durable log. Exact from then on, and the fill only ever goes up -- the
// scan keeps the same high-water mark, so shadow and scan stay equal frame for frame.
constexpr DurableShadow after_ack(const DurableShadow& log, OrderState state, std::int64_t filled_qty_ticks) noexcept {
    return DurableShadow{state, false, filled_qty_ticks > log.filled_qty_ticks ? filled_qty_ticks : log.filled_qty_ticks};
}

enum class PathStatus : std::uint8_t {
    Write = 0,        // write the observation's frame, an Ambiguous frame first if `ambiguous_first`
    Stale = 1,        // nothing new: the log already says this, or something later
    Unreachable = 2,  // cannot be written without breaking replay or contradicting itself: refused and counted (fail closed); the reconcile sweep catches up
};

struct DurablePath {
    PathStatus status{PathStatus::Unreachable};
    // Write an Ambiguous frame (carrying the shadow's own fill) before the observation's frame. Never true unless status is Write.
    bool ambiguous_first{false};
};

namespace detail {

// A step the replay accepts for a frame that carries no new fill: the same state again, or a legal transition.
constexpr bool replay_accepts(OrderState from, OrderState to) noexcept {
    return from == to || validate_transition(from, to) == TransitionResult::Ok;
}

// The states a WebSocket execution report can name: the image of map_binance_order_status().
constexpr bool is_observable(OrderState s) noexcept {
    return s == OrderState::Accepted || s == OrderState::PartialFill || s == OrderState::Filled || s == OrderState::CancelRequested ||
           s == OrderState::Cancelled || s == OrderState::Rejected || s == OrderState::Expired;
}

// The states a reconcile resolution can name: the observable ones, plus the local decision to hand an order nothing can resolve to the operator.
constexpr bool is_resolvable(OrderState s) noexcept { return is_observable(s) || s == OrderState::EscalatedToOperator; }

// `target` is nothing later than what the log holds: it IS that state, or a state from which the log's state is reachable (an event delivered late).
constexpr bool is_not_later(const DurableShadow& log, OrderState target) noexcept {
    if (log.maybe_submitting) return false;  // the log may hold Submitting: nothing an observation says is known to be old
    return replay_accepts(target, log.state);
}

// state_path() once the caller has decided which targets are its business.
constexpr DurablePath state_path_unchecked(const DurableShadow& from, OrderState target) noexcept {
    DurablePath p{};
    if (from.maybe_submitting) {
        // The last frame is Submitting or Ambiguous. A target that replays from BOTH (Accepted, PartialFill, Filled, Rejected) needs nothing in front of
        // it. Any other target needs an Ambiguous frame first -- legal after Submitting and, as a same-state repeat, after Ambiguous -- and must then be
        // legal from Ambiguous.
        if (replay_accepts(OrderState::Submitting, target) && replay_accepts(OrderState::Ambiguous, target)) {
            p.status = PathStatus::Write;
            return p;
        }
        if (validate_transition(OrderState::Ambiguous, target) != TransitionResult::Ok) return p;
        p.status = PathStatus::Write;
        p.ambiguous_first = true;
        return p;
    }
    if (replay_accepts(from.state, target)) {
        p.status = PathStatus::Write;
        return p;
    }
    // One re-recorded Ambiguous bridges Submitting to the states only Ambiguous reaches (Cancelled, Expired, CancelRequested, EscalatedToOperator). It is
    // never offered to an order the exchange has already acknowledged: Accepted / PartialFill -> Ambiguous is not a transition, and neither is it true.
    if (validate_transition(from.state, OrderState::Ambiguous) == TransitionResult::Ok &&
        validate_transition(OrderState::Ambiguous, target) == TransitionResult::Ok) {
        p.status = PathStatus::Write;
        p.ambiguous_first = true;
    }
    return p;
}

// plan_observation() / plan_resolution() once the caller has decided which targets are its business.
constexpr DurablePath plan_unchecked(const DurableShadow& log, OrderState target, std::int64_t cumulative_filled_qty_ticks,
                                     std::int64_t intended_qty_ticks) noexcept {
    DurablePath p{};
    if (cumulative_filled_qty_ticks < 0) return p;
    const bool not_later = is_not_later(log, target);
    if (cumulative_filled_qty_ticks < log.filled_qty_ticks) {
        if (not_later) p.status = PathStatus::Stale;
        return p;  // else Unreachable: a lower fill with a later state contradicts the log
    }
    if (cumulative_filled_qty_ticks == log.filled_qty_ticks && not_later) {
        p.status = PathStatus::Stale;
        return p;
    }
    if (cumulative_filled_qty_ticks > 0 && (intended_qty_ticks <= 0 || cumulative_filled_qty_ticks > intended_qty_ticks)) return p;
    return state_path_unchecked(log, target);
}

}  // namespace detail

// The state path alone: whether a single frame for `target` replays from the shadow, whether an Ambiguous frame has to go first, or whether no legal
// sequence gets there. `target` outside the seven observable states is Unreachable (nothing the exchange said).
constexpr DurablePath state_path(const DurableShadow& from, OrderState target) noexcept {
    return detail::is_observable(target) ? detail::state_path_unchecked(from, target) : DurablePath{};
}

// What to do with one observation: `target` is the state the exchange reported ("X"), `cumulative_filled_qty_ticks` its cumulative fill ("z"),
// `intended_qty_ticks` the order's own quantity.
//   * a negative cumulative fill is Unreachable;
//   * a cumulative fill that goes DOWN is an older observation: Stale when the state is nothing later than the log's, otherwise a contradiction
//     (Unreachable);
//   * the same cumulative fill and a state that is nothing later: Stale -- there is nothing to record;
//   * a fill past the intended quantity, or a fill with no intended quantity known (<= 0): Unreachable -- the scan refuses it for a same-state frame
//     and silently accepts it for a transition, and neither is a thing to write;
//   * otherwise state_path() decides.
constexpr DurablePath plan_observation(const DurableShadow& log, OrderState target, std::int64_t cumulative_filled_qty_ticks,
                                       std::int64_t intended_qty_ticks) noexcept {
    return detail::is_observable(target) ? detail::plan_unchecked(log, target, cumulative_filled_qty_ticks, intended_qty_ticks) : DurablePath{};
}

// plan_observation()'s counterpart for the reconcile path: a resolution the reconcile thread reached for an order, whose target may also be
// EscalatedToOperator (a local decision, not an exchange report; the state machine itself lets only an Ambiguous order -- or one that may still be
// Submitting, through an Ambiguous frame -- reach it). The fill bound is the caller's call: the reconcile thread has already held the fill against its own
// record's intended quantity, so a caller that does not know the quantity passes std::numeric_limits<std::int64_t>::max() ("unbounded"), never 0, which
// here as there means "refuse every fill".
constexpr DurablePath plan_resolution(const DurableShadow& log, OrderState target, std::int64_t cumulative_filled_qty_ticks,
                                      std::int64_t intended_qty_ticks) noexcept {
    return detail::is_resolvable(target) ? detail::plan_unchecked(log, target, cumulative_filled_qty_ticks, intended_qty_ticks) : DurablePath{};
}

}  // namespace hy
