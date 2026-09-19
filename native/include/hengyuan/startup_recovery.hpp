// SPDX-License-Identifier: proprietary
// startup_recovery.hpp — 批次 6 6b-0d: the process may not submit an order until it has recovered
// what the previous run left behind (外部复核 P0-06, verified: repopulate_in_flight_registry()/
// seed_position_truth() were referenced only from durable_audit_sink.hpp and its tests -- no
// process start path ever called them, and the 6b-1 harness ran on a fresh temp audit file every
// time, so it could not have recovered anything).
//
// After a crash the durable log can hold orders whose exchange state this process does not know
// (Submitting/Ambiguous) or knows only as of the crash (Accepted/PartialFill). Accepting a new
// intent before those are reconciled risks a duplicate order, an over-target position, or a wrong
// close. This header provides:
//
//   * StartupRecovery -- a small state machine, Bootstrapping -> Recovering -> Reconciling ->
//     Ready, with a sticky Degraded. It is driven by observations the caller gathers; it owns no
//     I/O. Degraded has NO way out on purpose: only a process restart (which re-runs the whole
//     recovery) clears it, and no confirmation prompt can override it.
//   * apply_recovery() -- registers every recovered order in the InFlightRegistry (keeping the
//     handle), queues it for the existing same-clientOrderId reconciliation (poll_once()), and
//     seeds PositionTruth WITHOUT double counting (see below).
//   * tally_recovered_orders() -- how many recovered orders are still unknown to this process.
//
// Double-count hazard, found by reading drain_reconcile_events(): fill deltas are measured against
// the shared OrderFillContext baseline. seed_position_truth() adds a recovered order's cumulative
// fill X to PositionTruth; if that order's baseline were left at 0, a later reconcile result
// "cumulative Y" would add all of Y on top -- X counted twice. apply_recovery() therefore aligns
// the baseline to X right after seeding (consume_delta(coid, X)), so only fills observed AFTER
// the crash are added.
//
// What "Ready" does and does not mean: every recovered order is either finished or positively
// known to be live. A known-live resting order does NOT block Ready but does still count as in
// flight, so the planner's one-order-at-a-time rule keeps new intents suppressed while it rests.
// An order that stays unknown (or escalates to the operator) never reaches Ready -- it times out
// into Degraded.
//
// NOT here: how the audit HMAC key is provisioned for a persistent store. That is a keying
// decision (no production key ceremony exists in this codebase yet -- every existing tool uses
// fixture key material) and is deliberately left for the owner, see the plan's 6b-2 prerequisites.

#pragma once

#include <hengyuan/account_truth.hpp>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/order_fill_context.hpp>
#include <hengyuan/order_lifecycle.hpp>
#include <hengyuan/order_tracker.hpp>
#include <hengyuan/position_truth.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace hy {

enum class RunState : std::uint8_t {
    Bootstrapping = 0,  // waiting for clock sync + exchangeInfo (needed to scale recovered orders)
    Recovering = 1,     // durable log is healthy; recovered orders not yet applied
    Reconciling = 2,    // recovered orders applied; waiting for every one to be known
    Ready = 3,
    Degraded = 4,       // sticky; only a process restart clears it
};

enum class DegradedReason : std::uint8_t {
    None = 0,
    SinkNotOpen = 1,
    SinkFenced = 2,
    RecoveryScanFailed = 3,
    RepopulateIncomplete = 4,
    ReconcileTimeout = 5,
};

inline constexpr const char* run_state_name(RunState s) noexcept {
    switch (s) {
        case RunState::Bootstrapping: return "Bootstrapping";
        case RunState::Recovering: return "Recovering";
        case RunState::Reconciling: return "Reconciling";
        case RunState::Ready: return "Ready";
        case RunState::Degraded: return "Degraded";
    }
    return "?";
}

inline constexpr const char* degraded_reason_name(DegradedReason r) noexcept {
    switch (r) {
        case DegradedReason::None: return "None";
        case DegradedReason::SinkNotOpen: return "SinkNotOpen";
        case DegradedReason::SinkFenced: return "SinkFenced";
        case DegradedReason::RecoveryScanFailed: return "RecoveryScanFailed";
        case DegradedReason::RepopulateIncomplete: return "RepopulateIncomplete";
        case DegradedReason::ReconcileTimeout: return "ReconcileTimeout";
    }
    return "?";
}

// Everything the state machine looks at. Every default is fail-closed.
struct RecoveryObservation {
    // Environment (needed before recovered orders can be scaled / queried).
    bool clock_synced{false};
    bool exchange_info_loaded{false};
    // Durable log, as reported by the DurableAuditSink the caller constructed (construction runs
    // the recovery scan).
    bool sink_open{false};
    bool sink_fenced{true};
    RecoveryScanStatus scan_status{RecoveryScanStatus::IoError};
    // Result of apply_recovery().
    bool recovery_applied{false};
    std::size_t recovered_orders{0};
    std::size_t applied_orders{0};
    // From tally_recovered_orders(): recovered orders still unknown to this process.
    std::size_t unconfirmed_orders{0};
    // Live prerequisites for accepting intents.
    bool account_fresh{false};
    bool user_data_connected{false};
};

class StartupRecovery {
public:
    explicit StartupRecovery(std::int64_t reconcile_deadline_ms) noexcept
        : reconcile_deadline_ms_(reconcile_deadline_ms) {}

    RunState state() const noexcept { return state_; }
    DegradedReason degraded_reason() const noexcept { return reason_; }

    RunState update(const RecoveryObservation& obs, std::int64_t now_ms) noexcept {
        if (state_ == RunState::Degraded) return state_;  // sticky, by design

        // The durable log's health is checked in EVERY state, before anything else: nothing below
        // means anything if the log cannot be trusted or written.
        if (!obs.sink_open) return degrade(DegradedReason::SinkNotOpen);
        if (obs.sink_fenced) return degrade(DegradedReason::SinkFenced);
        if (obs.scan_status != RecoveryScanStatus::Clean &&
            obs.scan_status != RecoveryScanStatus::Recovered) {
            return degrade(DegradedReason::RecoveryScanFailed);
        }

        if (state_ == RunState::Bootstrapping) {
            if (!obs.clock_synced || !obs.exchange_info_loaded) return state_;
            state_ = RunState::Recovering;
        }
        if (state_ == RunState::Recovering) {
            if (!obs.recovery_applied) return state_;
            if (obs.applied_orders < obs.recovered_orders) {
                return degrade(DegradedReason::RepopulateIncomplete);
            }
            state_ = RunState::Reconciling;
            reconciling_since_ms_ = now_ms;
        }
        if (state_ == RunState::Reconciling) {
            if (obs.unconfirmed_orders > 0) {
                if (now_ms - reconciling_since_ms_ > reconcile_deadline_ms_) {
                    return degrade(DegradedReason::ReconcileTimeout);
                }
                return state_;
            }
            if (!obs.account_fresh || !obs.user_data_connected) return state_;
            state_ = RunState::Ready;
        }
        return state_;
    }

    // Ready is necessary, not sufficient: the live prerequisites must also hold RIGHT NOW, so a
    // dropped user-data stream or a stale account snapshot switches submission off again without
    // any state transition.
    bool submit_enabled(const RecoveryObservation& latest) const noexcept {
        return state_ == RunState::Ready && latest.sink_open && !latest.sink_fenced &&
               latest.clock_synced && latest.account_fresh && latest.user_data_connected;
    }

private:
    RunState degrade(DegradedReason r) noexcept {
        state_ = RunState::Degraded;
        reason_ = r;
        return state_;
    }

    RunState state_{RunState::Bootstrapping};
    DegradedReason reason_{DegradedReason::None};
    std::int64_t reconcile_deadline_ms_;
    std::int64_t reconciling_since_ms_{0};
};

struct RecoveryApplyResult {
    std::size_t recovered{0};  // checkpoints found in the durable log
    std::size_t applied{0};    // registered + baseline-aligned + queued for reconciliation
    bool ok() const noexcept { return applied == recovered; }
};

// Applies every recovered checkpoint. `rules_for(symbol_id)` must return that symbol's current
// SymbolRules (SymbolRegistry::current_rules()); rules_version == 0 means "unknown symbol" and
// stops the application (fail-closed: the order cannot be scaled). Stops at the first failure;
// the caller's StartupRecovery then degrades on applied < recovered.
template <typename RulesLookup>
inline RecoveryApplyResult apply_recovery(std::span<const OrderRecoveryCheckpoint> checkpoints,
                                           InFlightRegistry& in_flight, ToReconcileRing& to_reconcile,
                                           PositionTruth& position_truth, OrderFillContext& fill_context,
                                           RulesLookup&& rules_for) noexcept {
    RecoveryApplyResult r;
    r.recovered = checkpoints.size();
    for (const OrderRecoveryCheckpoint& cp : checkpoints) {
        const std::string_view coid = cp.client_order_id.view();

        const SymbolRules rules = rules_for(cp.symbol_id);
        if (rules.rules_version == 0) break;

        // register_submit_handle(), not repopulate_in_flight_registry(): the reconcile ring needs
        // the handle so drain_reconcile_events() can release the slot when the order finishes.
        const InFlightHandle handle = in_flight.register_submit_handle(coid);
        if (!handle.valid()) break;

        if (!fill_context.track(coid, cp.symbol_id, cp.side, rules.price_scale, rules.qty_scale,
                                 rules.quote_scale)) {
            break;
        }

        // Seed, then align the shared baseline to what was just seeded (see this file's header
        // comment on the double-count hazard). consume_delta()'s return value is the amount just
        // seeded -- deliberately discarded, it must not be applied a second time.
        seed_position_truth(position_truth, std::span<const OrderRecoveryCheckpoint>(&cp, 1));
        if (cp.filled_qty_ticks > 0) (void)fill_context.consume_delta(coid, cp.filled_qty_ticks);

        if (!to_reconcile.try_push(ReconcileIngress{handle, checkpoint_to_order_record(cp)})) break;
        ++r.applied;
    }
    return r;
}

struct RecoveredOrderTally {
    std::size_t total{0};
    std::size_t resolved{0};    // no longer in flight: reached an exchange-final state
    std::size_t known_live{0};  // in flight, positively known live (Accepted / PartialFill)
    std::size_t unconfirmed{0}; // everything else -- still unknown to this process
};

// Classifies each recovered order from what this process can see now. "Unconfirmed" deliberately
// includes an order that is in flight but no longer tracked by `tracker`: that is either a
// not-yet-ingested reconcile request or an order poll_once() escalated to the operator, and
// neither may count as known. (The tracker's own records are reconcile-thread-owned; call this
// from the thread that runs poll_once(), as the harness main loop does.)
inline RecoveredOrderTally tally_recovered_orders(const InFlightRegistry& in_flight, OrderTracker& tracker,
                                                    std::span<const OrderRecoveryCheckpoint> checkpoints) noexcept {
    RecoveredOrderTally t;
    t.total = checkpoints.size();
    for (const OrderRecoveryCheckpoint& cp : checkpoints) {
        const std::string_view coid = cp.client_order_id.view();
        if (!in_flight.is_in_flight(coid)) {
            ++t.resolved;
            continue;
        }
        bool tracked = false;
        OrderState state = OrderState::Ambiguous;
        tracker.for_each_active([&](InFlightHandle, OrderRecord& record, std::int64_t) {
            if (record.client_order_id.view() == coid) {
                tracked = true;
                state = record.state;
            }
        });
        if (tracked && (state == OrderState::Accepted || state == OrderState::PartialFill)) {
            ++t.known_live;
        } else {
            ++t.unconfirmed;
        }
    }
    return t;
}

}  // namespace hy
