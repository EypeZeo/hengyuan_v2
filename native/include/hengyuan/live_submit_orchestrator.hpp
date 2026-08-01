// SPDX-License-Identifier: proprietary
// live_submit_orchestrator.hpp — D12-9 minimal live submit path orchestrator.
//
// Governance: L1 (orchestration logic + gate chain, no network).
// Actual HTTP POST is injected via SubmitPort (L5 runtime provides real impl).
//
// ADR-019 D1–D12 gate chain enforced in sequence:
//   1. Audit sink available (D10)
//   2. Kill switch Normal (D3 清单 B-1, D9)
//   3. Dry-run evidence 4/4 + consistent build (D11)
//   4. Signer ready (D3 清单 B-5)
//   5. Depth synced (D3 清单 B-3)
//   6. Pre-trade validation: symbol + price/qty/step + notional + exposure (D7)
//   7. Account truth fresh (D6)
//   8. Rate limit pre-check, non-consuming (D5)
//   9. Generate idempotent clientOrderId (D8)
//  10. Audit: OrderIntentCreated
//  11. Operator CONFIRM with bound summary (D3 M3)
//  12. Submit port valid
//  12b. In-flight / idempotency guard (D8 M6, no blind retry)
//  12c. Rate limit actual charge, right before the network call (D5)
//  13. Audit: OrderSubmitted → SubmitPort::submit() → handle result
//  14. Result routing: Accepted/Rejected/Ambiguous→reconcile/escalate (D8 M6)
//
// Any gate FAIL → fail-closed, no submit, audit the rejection.

#pragma once

#include <hengyuan/account_truth.hpp>
#include <hengyuan/audit_trail.hpp>
#include <hengyuan/dry_run_evidence.hpp>
#include <hengyuan/exit_safety.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/order_lifecycle.hpp>
#include <hengyuan/order_tracker.hpp>
#include <hengyuan/transport_policy.hpp>

#include <cstdint>
#include <cstdio>

namespace hy {

// --- Submit port (dependency injection for L5 runtime) ---

enum class SubmitOutcome : std::uint8_t {
    Accepted = 0,
    Rejected = 1,
    Timeout = 2,    // → Ambiguous (M6)
    NetworkError = 3,
    // SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md §4.1: exists for ABI/logging
    // symmetry with OrchestratorGate::SubmitStaleRulesVersion below. Gate 1 (top
    // of orchestrate_submit()) catches a stale rules_version BEFORE
    // SubmitPort::call() is ever invoked, so no SubmitFn implementation can
    // actually produce this value — see the defensive-only case in
    // orchestrate_submit()'s result-routing switch.
    //
    // = 5, not 4: matches the spec's own numbering exactly (spec line 780/789 —
    // RateLimited = 4 sits between NetworkError and this value there). Value 4
    // is deliberately left unclaimed here rather than "helpfully" renumbered to
    // the next free slot — RateLimited is out of scope for this round
    // (tools/spec_enum_diff.py tracks it as MISSING_IN_CODE, not a conflict).
    StaleRulesVersion = 5,
};

struct SubmitResponse {
    SubmitOutcome outcome{SubmitOutcome::NetworkError};
    std::int64_t exchange_order_id{0};
    std::int32_t error_code{0};
};

struct SubmitPort {
    // side + type are part of the ABI: a Binance POST /order is meaningless without
    // BUY/SELL and an order type. type is frozen to LIMIT for the first path but is
    // passed explicitly so the real runtime constructs the correct request.
    //
    // rules_snapshot (spec §2.1, round-5 P0): the caller's OWN captured
    // SymbolRules copy, not a fresh lookup — this is what actually closes the
    // TOCTOU window Gate 1 below only narrows. A real implementation must format
    // the wire request from THIS parameter alone, never from a second registry
    // read at send time.
    using SubmitFn = SubmitResponse(*)(
        const char* client_order_id,
        std::uint32_t symbol_id,
        OrderSide side,
        OrderType type,
        std::int64_t price_ticks,
        std::int64_t qty_ticks,
        const SymbolRules& rules_snapshot,
        void* user_data);

    // Lightweight, side-effect-free query (spec §2.1), deliberately separate
    // from SubmitFn so Gate 1 can call it before any audit write, CONFIRM
    // check, or in-flight registration. Never used for wire formatting.
    using CurrentRulesVersionFn = std::uint32_t(*)(void* user_data);

    SubmitFn fn{nullptr};
    void* user_data{nullptr};
    // Placed after the two original fields (not between them) so existing
    // 2-value aggregate-inits (`{fn, user_data}`) keep compiling unchanged and
    // keep defaulting to nullptr here -- a real implementation must supply this
    // explicitly to get real version-fencing instead of the fail-closed default.
    CurrentRulesVersionFn current_rules_version_fn{nullptr};

    SubmitResponse call(const char* coid, std::uint32_t sym,
                        OrderSide side, OrderType type,
                        std::int64_t price, std::int64_t qty,
                        const SymbolRules& rules_snapshot) const noexcept {
        if (!fn) return {SubmitOutcome::NetworkError, 0, -1};
        return fn(coid, sym, side, type, price, qty, rules_snapshot, user_data);
    }

    // 0 is reserved as "unknown/unavailable" and never matches a real
    // rules_version (account_truth.hpp's SymbolRules comment) -- a missing
    // function pointer therefore fails closed by construction: Gate 1 below
    // will see a mismatch against any real (non-zero) snapshot version.
    std::uint32_t current_rules_version() const noexcept {
        if (!current_rules_version_fn) return 0;
        return current_rules_version_fn(user_data);
    }

    bool is_valid() const noexcept { return fn != nullptr; }
};

// --- Operator confirmation bound to an immutable order summary (ADR-019 D3 M3) ---
//
// A bare "confirmed=true" flag binds nothing: it cannot prove the operator approved
// THIS order. This structure binds the confirmation to the exact order parameters,
// a max-notional ceiling, and an expiry. The orchestrator refuses to submit unless
// every bound field matches the concrete order and the confirmation is unexpired.
struct OrderConfirmation {
    bool confirmed{false};
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Limit};
    std::int64_t price_ticks{0};
    std::int64_t qty_ticks{0};
    std::int64_t max_notional{0};    // operator-approved ceiling (must be > 0 to authorize)
    std::int64_t valid_until_ms{0};  // confirmation expiry (inclusive)

    // Does this confirmation authorize the given concrete order at time now_ms?
    bool authorizes(std::uint32_t sym, OrderSide s, OrderType t,
                    std::int64_t price, std::int64_t qty,
                    std::int64_t notional, std::int64_t now_ms) const noexcept {
        return confirmed
            && symbol_id == sym
            && side == s
            && type == t
            && price_ticks == price
            && qty_ticks == qty
            && max_notional > 0
            && notional <= max_notional
            && now_ms <= valid_until_ms;
    }
};

// --- Orchestrator gate results ---

enum class OrchestratorGate : std::uint8_t {
    Passed = 0,
    AuditUnavailable = 1,
    KillSwitchNotNormal = 2,
    DryRunEvidenceIncomplete = 3,
    SignerNotReady = 4,
    DepthNotSynced = 5,
    PreTradeFailed = 6,
    AccountTruthStale = 7,
    RateLimitExhausted = 8,
    OperatorNotConfirmed = 9,
    SubmitPortInvalid = 10,
    SubmitRejected = 11,
    SubmitAmbiguous = 12,
    SubmitNetworkError = 13,
    SubmitAccepted = 14,  // success — not a failure gate
    ConfirmationMismatch = 15,       // confirmed=true but bound summary/expiry mismatch (F3)
    DuplicateInFlight = 16,          // client_order_id already submitted, no blind retry (F4)
    InFlightRegistryUnavailable = 17,  // no registry wired → fail-closed (F4)
    // SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md §2.2/§3 Gate 1: symbol registry
    // version fast-reject. Pure local comparison, no network, no audit write, no
    // state mutation. Deliberately positioned where the existing pre-trade
    // block already dereferences ctx.symbol_rules, NOT moved to literally the
    // top of orchestrate_submit() ahead of Audit/KillSwitch/DryRun/Signer/Depth
    // -- the spec's full gate-chain renumbering (transport-policy/
    // clock-freshness gates, audit-intent-before-CONFIRM reordering) is
    // separate, larger, not-yet-scoped follow-up work; this round is deliberately
    // limited to the fast-reject + snapshot-carrying mechanism only, so every
    // gate that runs before the pre-trade block is completely unaffected.
    //
    // = 21, not 18: matches the spec's own numbering exactly (spec line
    // 835-842 — SubmitPartialFill=18, SubmitFilled=19, SubmitRateLimited=20 sit
    // between this file's existing values and this one there). 18-20 are
    // deliberately left unclaimed rather than "helpfully" renumbered to the
    // next free slot — those three gates are out of scope for this round.
    SubmitStaleRulesVersion = 21,
};

struct OrchestratorResult {
    OrchestratorGate gate{OrchestratorGate::Passed};
    OrderRecord order{};
    PreTradeCheck pre_trade_detail{PreTradeCheck::Ok};
};

// --- Orchestrator context (all dependencies injected) ---

struct OrchestratorContext {
    // Gate inputs
    AuditRingSink* audit{nullptr};
    const KillSwitch* kill_switch{nullptr};
    const DryRunEvidenceChain* evidence{nullptr};
    bool signer_ready{false};
    bool depth_synced{false};
    RequestWeightTracker* rate_tracker{nullptr};
    OrderConfirmation confirmation{};       // F3: bound operator CONFIRM
    InFlightRegistry* in_flight{nullptr};   // F4: idempotency / no-blind-retry guard

    // Reconcile/poll loop wiring (order_tracker.hpp). Both nullable and default
    // to nullptr for backward compatibility with existing callers/tests that
    // don't yet run a reconcile thread -- orchestrate_submit() behaves exactly
    // as before when either is unset. reconcile_events is drained at the top of
    // every call (piggy-backing on the hot thread's own cadence, not a separate
    // timer); to_reconcile receives {handle, order} for every order that leaves
    // this function Ambiguous, so the reconcile thread has something to poll.
    ToReconcileRing* to_reconcile{nullptr};
    ReconcileEventRing* reconcile_events{nullptr};

    // Order parameters
    const SymbolRules* symbol_rules{nullptr};
    // Captured by value from *symbol_rules at the top of orchestrate_submit(),
    // before Gate 1's version fast-reject even runs -- see spec §2.2. This is
    // the ONE copy that travels all the way to SubmitPort::call(); nothing after
    // capture re-reads *symbol_rules. Caller-visible so tests/callers can
    // inspect what was actually captured, but callers should treat this as
    // orchestrate_submit()'s own working copy, not an input to set themselves.
    SymbolRules pre_trade_rules_snapshot{};
    const AccountSnapshot* account{nullptr};
    OrderSide side{OrderSide::Buy};
    OrderType order_type{OrderType::Limit};  // frozen (spec 6.8)
    std::string_view base_asset{};           // for sell-side balance check
    std::string_view quote_asset{};
    std::int64_t now_ms{0};
    ExposureLimits exposure_limits{};

    std::int64_t price_ticks{0};
    std::int64_t qty_ticks{0};
    std::uint32_t symbol_id{0};
    std::uint32_t sequence{0};

    // Runtime
    SubmitPort submit_port{};
    ExecutionMode mode{ExecutionMode::Live};

    // Binance request weight for POST /order
    std::uint32_t order_weight{1};
};

// --- Core orchestration function ---

inline OrchestratorResult orchestrate_submit(OrchestratorContext& ctx) noexcept {
    OrchestratorResult result{};

    // Drain any reconcile-thread resolutions before processing this new order.
    // Unconditional on gates below (even a KillSwitch-halted or audit-unavailable
    // call still owns the only writer access to ctx.in_flight/ctx.audit on this
    // thread, per order_tracker.hpp's thread-ownership contract) -- deferring
    // this would let resolved slots sit unreleased for an arbitrarily long time
    // if this thread's gates keep failing closed. Both ctx.in_flight and
    // ctx.reconcile_events must be wired for there to be anything to drain into.
    if (ctx.in_flight && ctx.reconcile_events) {
        drain_reconcile_events(*ctx.in_flight, ctx.audit, *ctx.reconcile_events, ctx.now_ms);
    }

    // Gate 1: Audit available
    if (!ctx.audit || !can_submit_order(*ctx.audit)) {
        result.gate = OrchestratorGate::AuditUnavailable;
        return result;
    }

    // Gate 2: Kill switch Normal
    if (!ctx.kill_switch || ctx.kill_switch->state() != KillState::Normal) {
        result.gate = OrchestratorGate::KillSwitchNotNormal;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::PreflightFailed;
        ar.mode = ctx.mode;
        ar.set_detail("kill switch not Normal");
        ctx.audit->append(ar);
        return result;
    }

    // Gate 3: Dry-run evidence 4/4 + consistent build
    if (!ctx.evidence || !ctx.evidence->live_ready()) {
        result.gate = OrchestratorGate::DryRunEvidenceIncomplete;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::PreflightFailed;
        ar.mode = ctx.mode;
        ar.set_detail("dry-run evidence incomplete");
        ctx.audit->append(ar);
        return result;
    }

    // Gate 4: Signer ready
    if (!ctx.signer_ready) {
        result.gate = OrchestratorGate::SignerNotReady;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::PreflightFailed;
        ar.mode = ctx.mode;
        ar.set_detail("signer not ready");
        ctx.audit->append(ar);
        return result;
    }

    // Gate 5: Depth synced
    if (!ctx.depth_synced) {
        result.gate = OrchestratorGate::DepthNotSynced;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::PreflightFailed;
        ar.mode = ctx.mode;
        ar.set_detail("depth not synced");
        ctx.audit->append(ar);
        return result;
    }

    // Gate 6+7: Pre-trade validation (includes account freshness, symbol, limits)
    if (!ctx.symbol_rules || !ctx.account) {
        result.gate = OrchestratorGate::PreTradeFailed;
        return result;
    }

    // Capture the snapshot BEFORE the version fast-reject and before validation
    // reads any of its fields (spec §2.2): this is the ONE copy that travels
    // unchanged all the way to SubmitPort::call() below. validate_pre_trade()
    // and every gate after this point read ctx.pre_trade_rules_snapshot, never
    // *ctx.symbol_rules again -- that is what actually closes the TOCTOU window;
    // the version comparison right below only narrows it (see SubmitPort::
    // current_rules_version()'s doc comment for the fail-closed default).
    ctx.pre_trade_rules_snapshot = *ctx.symbol_rules;

    // Gate "1" (spec §2.1/§2.2/§3) -- named SubmitStaleRulesVersion, not
    // renumbered into this function's own gate sequence; see that enumerator's
    // doc comment for why it sits here rather than ahead of Audit/KillSwitch/
    // DryRun/Signer/Depth. Pure local comparison: no network, no audit write, no
    // state mutation has happened yet at this point.
    if (ctx.submit_port.current_rules_version() != ctx.pre_trade_rules_snapshot.rules_version) {
        result.gate = OrchestratorGate::SubmitStaleRulesVersion;
        return result;
    }

    auto ptc = validate_pre_trade(
        ctx.pre_trade_rules_snapshot, ctx.side, ctx.price_ticks, ctx.qty_ticks,
        *ctx.account, ctx.base_asset, ctx.quote_asset, ctx.now_ms, ctx.exposure_limits);
    if (ptc != PreTradeCheck::Ok) {
        result.gate = OrchestratorGate::PreTradeFailed;
        result.pre_trade_detail = ptc;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::PreTradeRejected;
        ar.mode = ctx.mode;
        ar.symbol_id = ctx.symbol_id;
        ar.detail_code = static_cast<std::int64_t>(ptc);
        ctx.audit->append(ar);
        return result;
    }

    // Gate 8: Rate limit — non-consuming pre-check (F11). Fails fast if we're
    // already out of budget, without charging weight for an attempt that may
    // still be rejected by CONFIRM/port-validity below. The real charge
    // happens at Gate 12c, right before the network call is actually made.
    if (!ctx.rate_tracker || !ctx.rate_tracker->can_send(ctx.order_weight)) {
        result.gate = OrchestratorGate::RateLimitExhausted;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::RateLimitApproaching;
        ar.mode = ctx.mode;
        ctx.audit->append(ar);
        return result;
    }

    // Gate 9: Generate idempotent clientOrderId
    auto coid = make_client_order_id(ctx.now_ms, ctx.sequence, ctx.symbol_id);
    result.order.client_order_id = coid;
    result.order.symbol_id = ctx.symbol_id;
    result.order.intended_price_ticks = ctx.price_ticks;
    result.order.intended_qty_ticks = ctx.qty_ticks;
    result.order.submit_timestamp_ms = ctx.now_ms;

    // Gate 10: Audit intent
    {
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::OrderIntentCreated;
        ar.mode = ctx.mode;
        ar.symbol_id = ctx.symbol_id;
        ar.price_ticks = ctx.price_ticks;
        ar.qty_ticks = ctx.qty_ticks;
        ar.set_client_order_id(coid.view());
        ar.resulting_state = result.order.state;
        ctx.audit->append(ar);
    }

    // Gate 11: Operator CONFIRM, bound to the immutable order summary (D3 M3).
    // notional is recomputed here for the max-notional binding; pre-trade already
    // proved it does not overflow.
    std::int64_t confirm_notional = 0;
    (void)checked_notional(ctx.price_ticks, ctx.qty_ticks, confirm_notional);
    if (!ctx.confirmation.confirmed) {
        result.gate = OrchestratorGate::OperatorNotConfirmed;
        return result;
    }
    if (!ctx.confirmation.authorizes(ctx.symbol_id, ctx.side, ctx.order_type,
                                     ctx.price_ticks, ctx.qty_ticks,
                                     confirm_notional, ctx.now_ms)) {
        result.gate = OrchestratorGate::ConfirmationMismatch;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::PreflightFailed;
        ar.mode = ctx.mode;
        ar.symbol_id = ctx.symbol_id;
        ar.set_client_order_id(coid.view());
        ar.resulting_state = result.order.state;
        ar.set_detail("confirmation does not authorize this order");
        ctx.audit->append(ar);
        return result;
    }

    // Gate 12: Submit port valid
    if (!ctx.submit_port.is_valid()) {
        result.gate = OrchestratorGate::SubmitPortInvalid;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::PreflightFailed;
        ar.mode = ctx.mode;
        ar.set_client_order_id(coid.view());
        ar.resulting_state = result.order.state;
        ar.set_detail("submit port invalid");
        ctx.audit->append(ar);
        return result;
    }

    // Gate 12b: Idempotency / no-blind-retry guard (F4, D8 M6).
    // A missing registry fails closed — live submit must never run un-guarded.
    if (!ctx.in_flight) {
        result.gate = OrchestratorGate::InFlightRegistryUnavailable;
        return result;
    }

    // Gate 12c: Rate limit — actual charge (F11), right before the network
    // call. Re-checked here (not just at Gate 8) because time may have passed
    // since the pre-check and the window may have rotated unfavorably.
    if (!ctx.rate_tracker->try_consume(ctx.order_weight)) {
        result.gate = OrchestratorGate::RateLimitExhausted;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::RateLimitApproaching;
        ar.mode = ctx.mode;
        ar.set_client_order_id(coid.view());
        ar.resulting_state = result.order.state;
        ctx.audit->append(ar);
        return result;
    }

    // Handle-returning form (not the bool-only register_submit()) so a
    // reconcile-bound handle is on hand below if this submit ends up Ambiguous
    // -- see order_lifecycle.hpp's InFlightHandle doc comment for why the
    // reconcile path needs {slot_index, generation} rather than just the coid.
    InFlightHandle in_flight_handle = ctx.in_flight->register_submit_handle(coid.view());
    if (!in_flight_handle.valid()) {
        result.gate = OrchestratorGate::DuplicateInFlight;
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::PreflightFailed;
        ar.mode = ctx.mode;
        ar.set_client_order_id(coid.view());
        ar.resulting_state = result.order.state;
        ar.set_detail("duplicate in-flight - reconcile, do NOT resubmit");
        ctx.audit->append(ar);
        return result;
    }

    // Transition to Submitting
    result.order.transition_to(OrderState::Submitting);
    {
        AuditRecord ar{};
        ar.timestamp_ms = ctx.now_ms;
        ar.event_type = AuditEventType::OrderSubmitted;
        ar.mode = ctx.mode;
        ar.symbol_id = ctx.symbol_id;
        ar.price_ticks = ctx.price_ticks;
        ar.qty_ticks = ctx.qty_ticks;
        ar.set_client_order_id(coid.view());
        ar.resulting_state = result.order.state;
        ctx.audit->append(ar);
    }

    // Execute submit. Passes the captured snapshot, not *ctx.symbol_rules --
    // see the capture site above for why (TOCTOU).
    auto resp = ctx.submit_port.call(
        coid.id, ctx.symbol_id, ctx.side, ctx.order_type,
        ctx.price_ticks, ctx.qty_ticks, ctx.pre_trade_rules_snapshot);

    // Gate 13: Result routing
    switch (resp.outcome) {
        case SubmitOutcome::Accepted:
            result.order.exchange_order_id = resp.exchange_order_id;
            result.order.transition_to(OrderState::Accepted);
            result.gate = OrchestratorGate::SubmitAccepted;
            {
                AuditRecord ar{};
                ar.timestamp_ms = ctx.now_ms;
                ar.event_type = AuditEventType::OrderAccepted;
                ar.mode = ctx.mode;
                ar.symbol_id = ctx.symbol_id;
                ar.exchange_order_id = resp.exchange_order_id;
                ar.set_client_order_id(coid.view());
                ar.resulting_state = result.order.state;
                ctx.audit->append(ar);
            }
            break;

        case SubmitOutcome::Rejected:
            result.order.transition_to(OrderState::Rejected);
            result.gate = OrchestratorGate::SubmitRejected;
            // Rejected is terminal — release the in-flight slot so the id can be
            // retired. Accepted/Ambiguous stay registered (order may exist on the
            // exchange; blind resubmit must remain blocked until reconciled).
            ctx.in_flight->mark_resolved(coid.view());
            {
                AuditRecord ar{};
                ar.timestamp_ms = ctx.now_ms;
                ar.event_type = AuditEventType::OrderRejected;
                ar.mode = ctx.mode;
                ar.symbol_id = ctx.symbol_id;
                ar.detail_code = resp.error_code;
                ar.set_client_order_id(coid.view());
                ar.resulting_state = result.order.state;
                ar.set_detail("exchange rejected");
                ctx.audit->append(ar);
            }
            break;

        case SubmitOutcome::Timeout:
            // M6: timeout → Ambiguous, NOT "stop and discard"
            result.order.transition_to(OrderState::Ambiguous);
            result.gate = OrchestratorGate::SubmitAmbiguous;
            {
                AuditRecord ar{};
                ar.timestamp_ms = ctx.now_ms;
                ar.event_type = AuditEventType::OrderAmbiguous;
                ar.mode = ctx.mode;
                ar.symbol_id = ctx.symbol_id;
                ar.set_client_order_id(coid.view());
                ar.resulting_state = result.order.state;
                ar.set_detail("POST timeout - ambiguous, do NOT retry");
                ctx.audit->append(ar);
            }
            if (ctx.to_reconcile) {
                // Cannot genuinely fail under correct wiring: ToReconcileRing's
                // capacity equals kMaxInFlight (order_tracker.hpp), and this
                // push only ever happens for an order that just consumed one of
                // ctx.in_flight's <= kMaxInFlight slots, so unconsumed pushes
                // can never outnumber ring capacity. The check is kept anyway
                // (not asserted) because this function is noexcept and has no
                // error-reporting channel back to the caller.
                (void)ctx.to_reconcile->try_push(ReconcileIngress{in_flight_handle, result.order});
            }
            break;

        case SubmitOutcome::NetworkError:
            result.order.transition_to(OrderState::Ambiguous);
            result.gate = OrchestratorGate::SubmitNetworkError;
            {
                AuditRecord ar{};
                ar.timestamp_ms = ctx.now_ms;
                ar.event_type = AuditEventType::OrderAmbiguous;
                ar.mode = ctx.mode;
                ar.symbol_id = ctx.symbol_id;
                ar.set_client_order_id(coid.view());
                ar.resulting_state = result.order.state;
                ar.set_detail("network error - ambiguous");
                ctx.audit->append(ar);
            }
            if (ctx.to_reconcile) {
                (void)ctx.to_reconcile->try_push(ReconcileIngress{in_flight_handle, result.order});
            }
            break;

        case SubmitOutcome::StaleRulesVersion:
            // UNREACHABLE by construction under this file's own gate ordering:
            // the top-of-block check above catches a stale version before
            // SubmitPort::call() is ever invoked (see SubmitOutcome::
            // StaleRulesVersion's doc comment -- this value exists only for ABI/
            // logging symmetry with OrchestratorGate::SubmitStaleRulesVersion).
            // Defensive fallback, not dead code with undefined behaviour: if a
            // SubmitFn implementation ever returns this anyway, treat it the
            // same as every other "uncertain, already sent" outcome in this
            // switch rather than silently miscounting it as success -- matching
            // spec §4's "default to Ambiguous unless positively proven otherwise"
            // principle.
            result.order.transition_to(OrderState::Ambiguous);
            result.gate = OrchestratorGate::SubmitAmbiguous;
            {
                AuditRecord ar{};
                ar.timestamp_ms = ctx.now_ms;
                ar.event_type = AuditEventType::OrderAmbiguous;
                ar.mode = ctx.mode;
                ar.symbol_id = ctx.symbol_id;
                ar.set_client_order_id(coid.view());
                ar.resulting_state = result.order.state;
                ar.set_detail("StaleRulesVersion returned by SubmitFn post-send (should be unreachable) - ambiguous");
                ctx.audit->append(ar);
            }
            if (ctx.to_reconcile) {
                (void)ctx.to_reconcile->try_push(ReconcileIngress{in_flight_handle, result.order});
            }
            break;
    }

    return result;
}

}  // namespace hy
