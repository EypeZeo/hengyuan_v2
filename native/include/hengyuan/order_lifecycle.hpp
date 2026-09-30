// SPDX-License-Identifier: proprietary
// order_lifecycle.hpp — Binance order state machine + idempotency + reconciliation.
//
// Governance: L1 (pure state machine logic, no network, no secret).
// ADR-019 D8 + Architect M6:
//   ✅ Full lifecycle: Intent→Submitting→Accepted/Rejected/Ambiguous→discovered exchange-final state
//   ✅ newClientOrderId idempotency key generation
//   ✅ POST timeout → Ambiguous (not "stop and discard")
//   ✅ Ambiguous → query via same clientOrderId → reconcile
//   ✅ No blind retry from Ambiguous
//   ✅ Valid state transitions enforced
//   ✅ Unknown-escalate → kill switch or operator takeover

#pragma once

#include <hengyuan/account_truth.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace hy {

// --- Order states ---

enum class OrderState : std::uint8_t {
    Intent = 0,           // Created locally, not yet submitted
    Submitting = 1,       // HTTP request in-flight
    Accepted = 2,         // Exchange acknowledged (NEW / PARTIALLY_FILLED / FILLED)
    Rejected = 3,         // Exchange rejected (-1013, -2010, etc.)
    Ambiguous = 4,        // POST timeout — may or may not be accepted (M6)
    PartialFill = 5,      // Partially filled (still open)
    Filled = 6,           // Fully filled
    CancelRequested = 7,  // Cancel in-flight
    Cancelled = 8,        // Confirmed cancelled
    Expired = 9,          // Exchange expired the order
    Reconciled = 10,      // UNREACHABLE by design (docs/SPEC_INVARIANTS.md's "Reconciled"
                          // entry): no validate_transition() case ever targets this value.
                          // Kept only as a historical/documentation marker — reconciliation
                          // resolves Ambiguous directly to whichever real exchange-final
                          // state the query discovered (Filled/Cancelled/Rejected/Expired),
                          // never to this generic bucket. Do not add a transition into it.
    EscalatedToOperator = 11, // Ambiguous could not be resolved → human takeover
};

inline const char* order_state_name(OrderState s) noexcept {
    switch (s) {
        case OrderState::Intent: return "Intent";
        case OrderState::Submitting: return "Submitting";
        case OrderState::Accepted: return "Accepted";
        case OrderState::Rejected: return "Rejected";
        case OrderState::Ambiguous: return "Ambiguous";
        case OrderState::PartialFill: return "PartialFill";
        case OrderState::Filled: return "Filled";
        case OrderState::CancelRequested: return "CancelRequested";
        case OrderState::Cancelled: return "Cancelled";
        case OrderState::Expired: return "Expired";
        case OrderState::Reconciled: return "Reconciled";
        case OrderState::EscalatedToOperator: return "EscalatedToOperator";
    }
    return "Unknown";
}

// --- State transition validation ---

enum class TransitionResult : std::uint8_t {
    Ok = 0,
    InvalidTransition = 1,
    AlreadyTerminal = 2,
};

inline bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Rejected ||
           s == OrderState::Filled ||
           s == OrderState::Cancelled ||
           s == OrderState::Expired ||
           s == OrderState::Reconciled ||
           s == OrderState::EscalatedToOperator;
}

// is_terminal(EscalatedToOperator) is true (no further automatic transition ever
// leaves it), but that does NOT mean the order is done: it may still be live on
// the exchange, awaiting operator resolution. Compaction/COID-release logic that
// uses is_terminal() as its retention filter can silently drop state an escalated
// order still needs — this exact bug is docs/SPEC_INVARIANTS.md's `is_exchange_final`
// entry (SUBMITPORT spec round 9 introduced this split; round 24 separately found
// Reconciled had been wrongly included here — it is not, and must not be, in this
// set). Use is_exchange_final(), never is_terminal(), for any decision about
// whether an order can still be resting on the exchange.
inline bool is_exchange_final(OrderState s) noexcept {
    return s == OrderState::Filled ||
           s == OrderState::Cancelled ||
           s == OrderState::Rejected ||
           s == OrderState::Expired;
}

inline TransitionResult validate_transition(OrderState from, OrderState to) noexcept {
    if (is_terminal(from)) return TransitionResult::AlreadyTerminal;

    switch (from) {
        case OrderState::Intent:
            if (to == OrderState::Submitting) return TransitionResult::Ok;
            break;
        case OrderState::Submitting:
            if (to == OrderState::Accepted ||
                to == OrderState::Rejected ||
                to == OrderState::Ambiguous ||
                to == OrderState::Filled ||       // immediate fill
                to == OrderState::PartialFill)    // immediate partial fill (TODO 1A.3) --
                                                    // a real POST /api/v3/order response
                                                    // can report PARTIALLY_FILLED status
                                                    // right at submit time if the resting
                                                    // order matched some, but not all, of
                                                    // available opposing liquidity.
                return TransitionResult::Ok;
            break;
        case OrderState::Accepted:
            // Cancelled is reachable WITHOUT a local CancelRequested (audit
            // STATE-TRANS-011): the operator can cancel from the Binance app during a
            // manual takeover -- that is the documented procedure in
            // docs/NATIVE_EXIT_SAFETY_RUNBOOK.md, not a hypothetical -- and the
            // exchange itself cancels for self-trade prevention. Reconciliation then
            // discovers CANCELED for an order this process last knew as Accepted.
            // Without this edge that discovery was an InvalidTransition and the local
            // state diverged from the exchange permanently. Expired was already
            // allowed here; the asymmetry with Cancelled was the oversight.
            //
            // Rejected is deliberately NOT added: Binance's REJECTED is a
            // submit-time status, never produced for an order that already reached
            // NEW. Ambiguous -> Rejected (below) covers "the POST timed out and the
            // query says it was rejected at submit time", which is the real case.
            if (to == OrderState::PartialFill ||
                to == OrderState::Filled ||
                to == OrderState::CancelRequested ||
                to == OrderState::Cancelled ||
                to == OrderState::Expired)
                return TransitionResult::Ok;
            break;
        case OrderState::Ambiguous:
            // From Ambiguous, reconciliation resolves to whichever state the query
            // actually discovered, or escalation if it couldn't be resolved.
            // `Reconciled` is deliberately NOT a target here (see
            // docs/SPEC_INVARIANTS.md's "Reconciled" entry): SUBMITPORT spec round 24
            // is authoritative that no live transition ever targets it, and
            // apply_confirmed_state()'s design maps a reconciliation query's
            // confirmed_state directly onto the matching OrderState, never onto a
            // generic "reconciled" bucket that would discard which outcome it was.
            // Accepted/PartialFill included (SUBMITPORT spec line 961's full target
            // set) now that order_tracker.hpp's poll_once() is the first real caller
            // that can discover "actually still live" via reconciliation — an earlier
            // revision of this file deliberately left these two out pending that
            // work; this is that work.
            //
            // CancelRequested (L4 §6.5): confirmed PENDING_CANCEL — still live, a
            // cancel is in progress on Binance's side. NOTE: order_tracker.hpp's
            // detail::is_legal_ambiguous_target() and detail::is_legal_query_target()
            // independently gate the same set of targets before this function is
            // even reached — both were updated alongside this edge; see their own
            // comments for why is_legal_query_target() in particular is not optional.
            if (to == OrderState::Accepted ||
                to == OrderState::PartialFill ||
                to == OrderState::Filled ||
                to == OrderState::Cancelled ||
                to == OrderState::Rejected ||
                to == OrderState::Expired ||
                to == OrderState::CancelRequested ||
                to == OrderState::EscalatedToOperator)
                return TransitionResult::Ok;
            break;
        case OrderState::PartialFill:
            // Cancelled without a local CancelRequested, same reasoning as Accepted
            // above (audit STATE-TRANS-011) -- a partially-filled order is exactly as
            // cancellable by the operator or the exchange as a resting one.
            if (to == OrderState::Filled ||
                to == OrderState::CancelRequested ||
                to == OrderState::Cancelled ||
                to == OrderState::Expired)
                return TransitionResult::Ok;
            break;
        case OrderState::CancelRequested:
            if (to == OrderState::Cancelled ||
                to == OrderState::Filled ||   // filled before cancel arrived
                to == OrderState::Expired)
                return TransitionResult::Ok;
            break;
        default:
            break;
    }

    return TransitionResult::InvalidTransition;
}

// L4 §6.5's status-string -> OrderState table. Returns false (out untouched) for anything
// unrecognized -- "not a guess": an unrecognized `status` value is not evidence of any
// particular outcome and must fail schema validation in the caller, exactly like a
// missing/malformed field, never be silently mapped to some default state.
// EXPIRED_IN_MATCH (self-trade-prevention-triggered expiry) is deliberately folded into the
// same terminal bucket as EXPIRED -- OrderState has no dedicated STP-expiry state; §6.5's own
// text documents this as a deliberate simplification, not an oversight.
//
// TODO 1A.4 batch 2: relocated here (verbatim, still inline) from binance_private_rest.hpp so
// binance_user_data_ws_session.hpp's executionReport "X" field parsing can reuse it without
// pulling in that file's Boost.Beast/Asio/OpenSSL/simdjson weight -- same "no real reason for
// a light consumer to drag in the heavy L4 REST client" reasoning binance_user_data_event.hpp's
// own header comment already gives for keeping the event/ring types Boost-free.
// binance_private_rest.hpp already includes this header, so its own call sites see this
// transparently -- zero behavior change, pure move.
inline bool map_binance_order_status(std::string_view status, OrderState& out) noexcept {
    if (status == "NEW") { out = OrderState::Accepted; return true; }
    if (status == "PARTIALLY_FILLED") { out = OrderState::PartialFill; return true; }
    if (status == "FILLED") { out = OrderState::Filled; return true; }
    if (status == "PENDING_CANCEL") { out = OrderState::CancelRequested; return true; }
    if (status == "CANCELED") { out = OrderState::Cancelled; return true; }
    if (status == "REJECTED") { out = OrderState::Rejected; return true; }
    if (status == "EXPIRED") { out = OrderState::Expired; return true; }
    if (status == "EXPIRED_IN_MATCH") { out = OrderState::Expired; return true; }
    return false;
}

// --- Client order ID (idempotency key) ---

static constexpr std::size_t kClientOrderIdLen = 36;

struct ClientOrderId {
    char id[kClientOrderIdLen + 1]{};

    std::string_view view() const noexcept {
        return {id, std::strlen(id)};
    }

    bool empty() const noexcept { return id[0] == '\0'; }
};

// Generate a deterministic client order ID from components.
// Format: HY-{timestamp_ms}-{seq}-{symbol_hash4}
// This is NOT a UUID — it's deterministic so the same intent always
// produces the same ID (idempotency: retry with same ID = same order).
inline ClientOrderId make_client_order_id(
    std::int64_t timestamp_ms,
    std::uint32_t sequence,
    std::uint32_t symbol_id) noexcept {

    ClientOrderId coid{};
    std::snprintf(coid.id, sizeof(coid.id),
                  "HY-%lld-%u-%04X",
                  static_cast<long long>(timestamp_ms),
                  sequence,
                  symbol_id & 0xFFFF);
    return coid;
}

// --- Order record ---

struct OrderRecord {
    ClientOrderId client_order_id{};
    std::int64_t exchange_order_id{0};  // Binance orderId, 0 if unknown
    std::uint32_t symbol_id{0};
    OrderState state{OrderState::Intent};

    std::int64_t submit_timestamp_ms{0};
    std::int64_t last_update_ms{0};

    std::int64_t intended_price_ticks{0};
    std::int64_t intended_qty_ticks{0};
    std::int64_t filled_qty_ticks{0};
    std::int64_t avg_fill_price_ticks{0};

    // Reconciliation queries actually SENT while this order was Ambiguous and answered without a
    // usable result (R-10: a query that never left the process -- QueryOutcome::NotSent -- is not
    // counted here). Saturates at 255 rather than wrapping. It feeds the quarantine criterion
    // (unknown_quarantine_due() below); there is no fixed attempt cap any more.
    std::uint8_t query_attempts{0};

    // L4 §6.1.2: captured once at submit time, never mutated afterward — the
    // basis for OrderExpectation::from(), which a reconciliation query
    // validates a Binance response against. Without these, a query response
    // could only be checked by clientOrderId, which is not enough to trust a
    // Found result (see OrderExpectation below).
    OrderSide side{OrderSide::Buy};
    OrderType order_type{OrderType::Limit};
    SymbolRules rules_snapshot_at_submit{};

    // R-10 (Owner decision 2026-09-29): when this order FIRST became Ambiguous ("UNKNOWN"), read
    // off the two clocks the reconcile loop has -- the monotonic one it already uses for backoff,
    // and the wall clock -- both in milliseconds. kClockUnset until stamp_unknown_since() sets
    // them, and stamped exactly once (never moved afterwards), so the unresolved time only ever
    // accumulates. Appended LAST, like every other additive field here.
    //
    // Limitation L-30, and how far it is closed: the MONOTONIC anchor cannot survive a restart (that
    // clock starts over), so a recovered order gets a fresh one. The WALL anchor can: recovery seeds
    // it from the MAC-verified header time of the durable frame that first made the order uncertain
    // (recovered_wall_anchor(), below), so the unresolved time keeps accumulating across restarts
    // and crash loops -- the wall clock is what carries it, which is exactly the job R-10 gave it.
    static constexpr std::int64_t kClockUnset = std::numeric_limits<std::int64_t>::min();
    std::int64_t unknown_since_mono_ms{kClockUnset};
    std::int64_t unknown_since_wall_ms{kClockUnset};
    // True when unknown_since_wall_ms came out of the durable log rather than from this process's
    // own first sight of the order. Such an anchor also counts the time the process was DOWN, so
    // unknown_quarantine_due() lets it trigger only after this process has sent the order at least
    // one query (see there).
    bool wall_anchor_recovered{false};

    TransitionResult transition_to(OrderState next) noexcept {
        auto result = validate_transition(state, next);
        if (result == TransitionResult::Ok) {
            state = next;
        }
        return result;
    }
};

// --- R-10: the UNKNOWN quarantine criterion ---
//
// An Ambiguous order is escalated to the operator (EscalatedToOperator, which carries
// UNKNOWN_QUARANTINE) when EITHER
//   (1) at least `min_sent_queries` reconciliation queries were actually sent and came back
//       without an answer AND at least `min_elapsed_ms` have passed since it first became
//       Ambiguous, OR
//   (2) at least `hard_cap_ms` have passed since then, however many queries went out (the
//       absolute cap: it is what still bounds an order whose queries cannot even be sent).
// Replaces the old "third inconclusive query" rule, which quarantined within about a second.
//
// Elapsed time is read on two clocks and the LARGER reading counts. The monotonic clock is the
// one deadlines and backoff live on (blueprint V-07); the wall clock is the single documented
// exception and may only bring escalation FORWARD -- a suspended host stops the monotonic clock
// but not the wall clock, and a wall clock that steps backwards reads as "no time has passed" on
// that clock while the other one keeps counting. Either clock alone is enough to trigger.
//
// Every value is configurable. A degenerate policy fails closed to "escalate": a non-positive
// hard_cap_ms means the cap is already reached (an order that cannot be resolved goes to a human
// rather than being retried forever).
struct UnknownQuarantinePolicy {
    std::uint8_t min_sent_queries{5};
    std::int64_t min_elapsed_ms{5'000};
    std::int64_t hard_cap_ms{15'000};
};

// Records that `rec` first became Ambiguous, on whichever clocks the caller has. Idempotent: an
// anchor that is already set is never moved, and a clock the caller does not have
// (OrderRecord::kClockUnset) leaves its anchor unset.
inline void stamp_unknown_since(OrderRecord& rec, std::int64_t now_mono_ms,
                                std::int64_t now_wall_ms) noexcept {
    if (rec.unknown_since_mono_ms == OrderRecord::kClockUnset) rec.unknown_since_mono_ms = now_mono_ms;
    if (rec.unknown_since_wall_ms == OrderRecord::kClockUnset) rec.unknown_since_wall_ms = now_wall_ms;
}

// Milliseconds from `since_ms` to `now_ms`: never negative, saturating instead of overflowing, and
// 0 whenever either reading is unset or `now_ms` is not later than `since_ms` -- a clock that stood
// still or stepped backwards is no evidence that time has passed.
inline std::int64_t elapsed_since_ms(std::int64_t now_ms, std::int64_t since_ms) noexcept {
    if (now_ms == OrderRecord::kClockUnset || since_ms == OrderRecord::kClockUnset) return 0;
    if (now_ms <= since_ms) return 0;
    // now > since, so the difference is positive; it can only overflow when `since` is negative.
    if (since_ms < 0 && now_ms > std::numeric_limits<std::int64_t>::max() + since_ms) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return now_ms - since_ms;
}

// The wall anchor of an order recovered from the durable log (limitation L-30). `utc_ms` is the
// MAC-verified header time of the frame that first made the order uncertain
// (OrderRecoveryCheckpoint::unknown_since_utc_ms; 0 = none). Returns OrderRecord::kClockUnset -- "no
// anchor: stamp at first sight, as before" -- unless the value can be a wall time at all. A reading
// before 2001-09-09 is a relative or monotonic clock (the demo harnesses stamp their frames with
// one); one more than a minute ahead of `now_wall_ms` is a clock that does not agree with ours.
// Either would otherwise sit in the anchor for good (stamp_unknown_since() never moves a set
// anchor) and silence the wall clock for that order.
inline std::int64_t recovered_wall_anchor(std::int64_t utc_ms, std::int64_t now_wall_ms) noexcept {
    constexpr std::int64_t kMinPlausibleWallMs = 1'000'000'000'000;
    constexpr std::int64_t kFutureSlackMs = 60'000;
    if (utc_ms < kMinPlausibleWallMs) return OrderRecord::kClockUnset;
    // utc_ms >= kMinPlausibleWallMs, so the subtraction cannot underflow.
    if (now_wall_ms != OrderRecord::kClockUnset && utc_ms - kFutureSlackMs > now_wall_ms) {
        return OrderRecord::kClockUnset;
    }
    return utc_ms;
}

// True iff the R-10 criterion holds for `rec` at the given readings of the two clocks
// (`now_wall_ms` may be OrderRecord::kClockUnset when the caller has no wall clock).
//
// A wall anchor recovered from the durable log counts the time this process was down. Held against
// an order this process has not asked about yet, that would quarantine every order left unresolved
// by a planned restart before its first query -- and put the run into Degraded for want of one
// GET. So such a reading may only trigger once the order has been sent at least one query in this
// process; until then only the (fresh) monotonic reading counts, which still bounds an order whose
// queries cannot be sent at all. A crash loop that never gets as far as one query is not
// quarantined either, but a process in that state cannot trade (startup recovery gates on it), so
// it opens no new exposure.
inline bool unknown_quarantine_due(const OrderRecord& rec, std::int64_t now_mono_ms,
                                   std::int64_t now_wall_ms,
                                   const UnknownQuarantinePolicy& policy) noexcept {
    const std::int64_t mono = elapsed_since_ms(now_mono_ms, rec.unknown_since_mono_ms);
    std::int64_t wall = elapsed_since_ms(now_wall_ms, rec.unknown_since_wall_ms);
    if (rec.wall_anchor_recovered && rec.query_attempts == 0) wall = 0;
    const std::int64_t elapsed = mono > wall ? mono : wall;
    // (2) The hard cap. `elapsed` is never negative, so a non-positive cap is reached by
    // definition -- that is how a degenerate policy fails closed to "escalate" with no special case.
    if (elapsed >= policy.hard_cap_ms) return true;
    return rec.query_attempts >= policy.min_sent_queries && elapsed >= policy.min_elapsed_ms;  // (1)
}

// L4 §6.1.2: everything a reconciliation query needs to validate a GET
// /api/v3/order response against — captured once at submit time (via
// from()), never re-derived from a live lookup that could have moved on
// since. A response that identifies itself by clientOrderId but disagrees on
// symbol/side/price/qty/timeInForce is not trustworthy evidence about THIS
// order and must never be treated as Found (see binance_private_rest.hpp's
// parse_order_query_response()).
struct OrderExpectation {
    ClientOrderId client_order_id{};
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    OrderType order_type{OrderType::Limit};
    std::int64_t intended_price_ticks{0};
    std::int64_t intended_qty_ticks{0};
    SymbolRules rules_snapshot_at_submit{};

    static OrderExpectation from(const OrderRecord& rec) noexcept {
        OrderExpectation exp{};
        exp.client_order_id = rec.client_order_id;
        exp.symbol_id = rec.symbol_id;
        exp.side = rec.side;
        exp.order_type = rec.order_type;
        exp.intended_price_ticks = rec.intended_price_ticks;
        exp.intended_qty_ticks = rec.intended_qty_ticks;
        exp.rules_snapshot_at_submit = rec.rules_snapshot_at_submit;
        return exp;
    }
};
static_assert(std::is_trivially_copyable_v<OrderExpectation>);
static_assert(std::is_standard_layout_v<OrderExpectation>);

// --- Reconciliation action ---

enum class ReconcileAction : std::uint8_t {
    QueryOrder = 0,       // GET /api/v3/order with same clientOrderId
    EscalateToOperator = 1, // R-10 quarantine criterion met → human takeover
    NoAction = 2,         // Not in ambiguous state
};

// `now_wall_ms` may be OrderRecord::kClockUnset when the caller has no wall clock; the monotonic
// reading alone then decides (see UnknownQuarantinePolicy). The caller must have stamped
// rec.unknown_since_* (stamp_unknown_since()) -- an unstamped record reads as "no time has passed".
inline ReconcileAction determine_reconcile_action(const OrderRecord& rec, std::int64_t now_mono_ms,
                                                  std::int64_t now_wall_ms,
                                                  const UnknownQuarantinePolicy& policy) noexcept {
    if (rec.state != OrderState::Ambiguous) {
        return ReconcileAction::NoAction;
    }
    if (unknown_quarantine_due(rec, now_mono_ms, now_wall_ms, policy)) {
        return ReconcileAction::EscalateToOperator;
    }
    return ReconcileAction::QueryOrder;
}

// --- In-flight registry (idempotency / no-blind-retry guard, ADR-019 D8 M6) ---
//
// A client_order_id that has been submitted and is not yet resolved to a terminal
// state must NEVER be submitted a second time. On timeout/network error the order
// is Ambiguous, and recovery is query-by-same-id + reconciliation — not a fresh
// POST. This registry is the enforcement point: register_submit() fails if the id
// is already tracked, so the orchestrator can fail-closed instead of double-sending.
static constexpr std::size_t kMaxInFlight = 64;

// A slot reference that stays valid across the reconcile/poll loop's cross-thread
// handoff (order_tracker.hpp). `slot_index` alone is an ABA hazard: if a stale
// handle from an already-resolved order arrives after that slot has been reused
// for a genuinely different order, matching on index alone would release the
// WRONG (new) order's slot. `generation` (bumped every time a slot transitions
// inactive -> active) makes a stale handle detectable and rejected instead of
// silently matching. In ordinary operation client_order_id's own
// timestamp+sequence+symbol construction should never collide across two live
// orders, so this is defense-in-depth against a misbehaving caller (e.g. a
// non-monotonic sequence), not a fix for an observed collision.
struct InFlightHandle {
    std::size_t slot_index{kMaxInFlight};  // == kMaxInFlight means invalid
    std::uint32_t generation{0};

    bool valid() const noexcept { return slot_index < kMaxInFlight; }
};

class InFlightRegistry {
public:
    // Attempt to record a submit for `coid`. Returns false if this id is already
    // tracked (in-flight or unresolved) or if capacity is exhausted (fail-closed).
    bool register_submit(std::string_view coid) noexcept {
        return register_submit_handle(coid).valid();
    }

    // Handle-returning variant of register_submit(), for callers that need to hand
    // the reservation across a thread boundary (order_tracker.hpp) and release it
    // later via mark_resolved_handle() rather than by COID string alone. Behaves
    // identically to register_submit() otherwise -- same fail-closed rules, same
    // slot selection -- this is purely an additive overload, not a replacement;
    // Gate 12b's existing synchronous call site has no cross-thread staleness
    // hazard and can keep using the bool-returning form unchanged.
    InFlightHandle register_submit_handle(std::string_view coid) noexcept {
        if (coid.empty()) return {};
        if (find_index(coid) != kNotFound) return {};  // already in-flight
        for (std::size_t i = 0; i < kMaxInFlight; ++i) {
            if (!slots_[i].active) {
                auto len = coid.size() < kClientOrderIdLen ? coid.size() : kClientOrderIdLen;
                std::memcpy(slots_[i].id, coid.data(), len);
                slots_[i].id[len] = '\0';
                slots_[i].active = true;
                ++slots_[i].generation;  // bump on every inactive -> active transition
                ++count_;
                return InFlightHandle{i, slots_[i].generation};
            }
        }
        return {};  // capacity exhausted → fail-closed
    }

    bool is_in_flight(std::string_view coid) const noexcept {
        return find_index(coid) != kNotFound;
    }

    // Release a resolved (terminal) order so its slot can be reused.
    void mark_resolved(std::string_view coid) noexcept {
        auto idx = find_index(coid);
        if (idx != kNotFound) {
            release_slot(idx);
        }
    }

    // Handle-checked release: only succeeds if `h` still refers to a slot that is
    // (a) active, (b) on the same generation as when the handle was issued, and
    // (c) still holds the same coid -- all three must agree, or this is a no-op.
    // This is the release path order_tracker.hpp's drain_reconcile_events() uses;
    // mark_resolved() (COID-only) remains for Gate 12b's synchronous Rejected path.
    bool mark_resolved_handle(InFlightHandle h, std::string_view coid) noexcept {
        if (!h.valid() || h.slot_index >= kMaxInFlight) return false;
        Slot& slot = slots_[h.slot_index];
        if (!slot.active || slot.generation != h.generation) return false;
        std::string_view sv(slot.id, std::strlen(slot.id));
        if (sv != coid) return false;
        release_slot(h.slot_index);
        return true;
    }

    std::size_t count() const noexcept { return count_; }

private:
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    struct Slot {
        bool active{false};
        std::uint32_t generation{0};
        char id[kClientOrderIdLen + 1]{};
    };

    std::size_t find_index(std::string_view coid) const noexcept {
        if (coid.empty()) return kNotFound;
        for (std::size_t i = 0; i < kMaxInFlight; ++i) {
            if (slots_[i].active) {
                std::string_view sv(slots_[i].id, std::strlen(slots_[i].id));
                if (sv == coid) return i;
            }
        }
        return kNotFound;
    }

    void release_slot(std::size_t idx) noexcept {
        slots_[idx].active = false;
        slots_[idx].id[0] = '\0';
        if (count_ > 0) --count_;
    }

    std::array<Slot, kMaxInFlight> slots_{};
    std::size_t count_{0};
};

}  // namespace hy
