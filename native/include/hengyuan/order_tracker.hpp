// SPDX-License-Identifier: proprietary
// order_tracker.hpp — reconcile/poll loop: drives Ambiguous orders toward a
// discovered terminal (or still-live) state, and is the first real consumer of
// order_lifecycle.hpp's is_exchange_final()/determine_reconcile_action().
//
// Governance: L1 (orchestration logic, no network, no secret). The actual
// GET /api/v3/order call is injected via QueryPort, mirroring
// live_submit_orchestrator.hpp's SubmitPort exactly: mock in tests today, a
// real Binance REST client is separate, not-yet-built work.
//
// WHY THIS FILE EXISTS: orchestrate_submit() returns an OrderRecord BY VALUE.
// Nothing in native/ retained a table of outstanding Ambiguous orders, so even
// though determine_reconcile_action() has existed since early rounds, nothing
// ever had a record to call it on. InFlightRegistry tracks bare COID+active,
// not enough state to reconcile against. This is that missing table, plus the
// loop that walks it.
//
// CLOSES THE InFlightRegistry 64-SLOT EXHAUSTION (docs/SPEC_INVARIANTS.md's
// "容量 / 生命周期" entry): drain_reconcile_events() calls
// InFlightRegistry::mark_resolved_handle() the instant is_exchange_final()
// first becomes true for a tracked order, whether that happens via a direct
// POST response (live_submit_orchestrator.hpp, unchanged) or via this file's
// reconciliation path — the same predicate, the same release call, one
// authoritative decision point.
//
// THREAD OWNERSHIP (load-bearing, not a convention comment):
//   - OrderTracker is owned EXCLUSIVELY by whichever thread calls poll_once().
//     No other thread may touch it.
//   - InFlightRegistry and AuditRingSink remain owned EXCLUSIVELY by whichever
//     thread calls orchestrate_submit() (unchanged from before this file).
//   - The ONLY communication between the two is the pair of SpscRing<T,N>
//     queues below — the same decoupling pattern spsc_ring.hpp's own header
//     comment describes for hot_thread.hpp/binance_ws_session.hpp, applied
//     here instead of atomics so neither structure pays cross-thread
//     synchronization cost on its own (genuinely hot, for InFlightRegistry)
//     access path.
//   - poll_once() therefore NEVER calls InFlightRegistry::mark_resolved*() or
//     AuditRingSink::append() directly — both would be a second writer racing
//     the owning thread's own writes. drain_reconcile_events() is the only
//     place either is called on the reconcile path.

#pragma once

#include <hengyuan/audit_trail.hpp>
#include <hengyuan/order_fill_context.hpp>
#include <hengyuan/order_lifecycle.hpp>
#include <hengyuan/position_truth.hpp>
#include <hengyuan/spsc_ring.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace hy {

// --- Reconciliation query result ---

enum class QueryOutcome : std::uint8_t {
    Found = 0,         // query positively identified the order's current state
    Inconclusive = 1,  // network/schema/timeout/validation uncertainty -- NOT
                       // "not found"; never treated as evidence of anything
};

struct QueryResult {
    QueryOutcome outcome{QueryOutcome::Inconclusive};
    // Valid only when outcome == Found. poll_once() independently validates
    // this against the legal Ambiguous-transition target set and against
    // rec.intended_qty_ticks before trusting it -- never assume an injected
    // QueryFn (mock today, network-backed later) returns something sane.
    OrderState confirmed_state{OrderState::Ambiguous};
    std::int64_t exchange_order_id{0};
    std::int64_t filled_qty_ticks{0};
    std::int64_t avg_fill_price_ticks{0};
    // L4 §6.3/§7.3: NEW fields, appended LAST -- existing 5-positional-arg
    // aggregate inits (test_order_tracker.cpp, test_reconcile_concurrency.cpp)
    // depend on trailing-only placement to keep compiling. binance_private_rest.hpp's
    // query_order() never populates these this batch (no parse_retry_after() yet,
    // §7.3 is a separate, not-yet-built rate-limiter batch) -- ABI reserved now so
    // that batch won't need a second breaking change to this struct.
    bool retry_after_present{false};
    std::int64_t retry_after_deadline_ms{0};
};

// --- QueryPort: same injection pattern and signature style as SubmitPort ---
// (live_submit_orchestrator.hpp) -- a null fn folds into Inconclusive rather
// than a dedicated error value, matching SubmitPort::call()'s own precedent.
// A real network implementation lives in binance_private_rest.hpp's
// query_order_adapter() (L4 §6).
//
// QueryFn takes an OrderExpectation, not a bare client_order_id: a real
// query_order() must both build a fully-specified signed GET request (symbol,
// not just the COID) AND validate the response's side/type/price/qty/
// timeInForce against what was actually submitted (L4 §6.1.2) -- a bare COID
// carries neither. expected.rules_snapshot_at_submit.symbol is the sole
// source of truth for which symbol to query, matching L4 §6.1.2's "carry the
// snapshot, don't re-derive" pattern.
struct QueryPort {
    using QueryFn = QueryResult(*)(const OrderExpectation& expected, void* user_data);

    QueryFn fn{nullptr};
    void* user_data{nullptr};

    QueryResult call(const OrderExpectation& expected) const noexcept {
        if (!fn) return {};  // Inconclusive by default-construction
        return fn(expected, user_data);
    }

    bool is_valid() const noexcept { return fn != nullptr; }
};

// --- Poll cadence / backoff policy ---
//
// Numbers are deliberately NOT hardcoded constants: SUBMITPORT spec §5 says
// reconciliation is "a single attempt per scheduled task invocation" but does
// not pin an exact cadence or backoff curve, and L4 §6.2's scheduling section
// has not had the same full-depth read Gate 8/9 got. Revisit the defaults
// below once it has; until then this is a caller-supplied policy, not spec.
struct ReconcilePollPolicy {
    std::int64_t base_interval_ms{200};
    std::uint32_t backoff_multiplier{4};      // attempt N waits base*multiplier^N
    std::int64_t max_interval_ms{5000};       // saturating cap, see backoff_delay_ms()
    std::uint32_t max_queries_per_tick{16};   // throttle: don't fire all 64 at once

    // Cadence for orders that are LIVE on the exchange (Accepted/PartialFill), as
    // opposed to Ambiguous ones. Deliberately a flat interval rather than the
    // exponential backoff above: backoff exists to stop hammering an endpoint over an
    // *uncertainty* that is expected to resolve quickly, while a resting order is a
    // normal steady state that may last hours -- backing off toward max_interval_ms
    // and staying there is the right shape for it, and a flat interval says so
    // directly instead of arriving there by accident.
    std::int64_t live_poll_interval_ms{2000};
};

// base * multiplier^attempt, saturating at max_interval_ms and never
// overflowing/wrapping past it. Config is expected to satisfy base>0,
// multiplier>=1, max>=base; degenerate configs fail closed to max_interval_ms
// (never to 0 / immediate retry -- an unrepresentable backoff must delay, not
// skip, the next attempt).
inline std::int64_t reconcile_backoff_delay_ms(const ReconcilePollPolicy& policy,
                                                std::uint8_t attempt) noexcept {
    if (policy.base_interval_ms <= 0 || policy.max_interval_ms <= 0) {
        return policy.max_interval_ms > 0 ? policy.max_interval_ms : 0;
    }
    std::int64_t interval = policy.base_interval_ms;
    if (interval > policy.max_interval_ms) return policy.max_interval_ms;
    const std::int64_t multiplier =
        policy.backoff_multiplier == 0 ? 1 : static_cast<std::int64_t>(policy.backoff_multiplier);
    for (std::uint8_t i = 0; i < attempt; ++i) {
        if (interval > policy.max_interval_ms / multiplier) {
            // Next multiply would exceed (or overflow toward) max_interval_ms --
            // saturate now instead of computing the product.
            return policy.max_interval_ms;
        }
        interval *= multiplier;
    }
    return interval > policy.max_interval_ms ? policy.max_interval_ms : interval;
}

// --- Tracked order table ---
//
// Owned exclusively by the reconcile thread (see file header). Linear scan
// over a fixed kMaxInFlight-sized array is deliberate, not an oversight: this
// runs on a coarse-interval background loop, not the hot path CLAUDE.md's
// five performance mandates are scoped to -- a few dozen nanoseconds of
// scanning, a handful of times per second, is not worth the complexity of a
// bitmask index kept in sync as a second source of truth.
class OrderTracker {
public:
    // Sentinel for "never polled yet" -- distinct from any real timestamp
    // (including 0, which callers may legitimately pass as now_ms) so a
    // freshly tracked order's first poll_once() call is never backoff-gated
    // against its own tracking time.
    static constexpr std::int64_t kNeverPolled = std::numeric_limits<std::int64_t>::min();

    // Begin tracking a record for reconciliation. Returns false (fail-closed)
    // if the table is full or this coid is already tracked -- mirrors
    // InFlightRegistry::register_submit_handle()'s own contract deliberately.
    bool track(InFlightHandle handle, const OrderRecord& record, std::int64_t now_ms) noexcept {
        (void)now_ms;
        auto coid = record.client_order_id.view();
        if (coid.empty()) return false;
        if (find_index(coid) != kNotFound) return false;
        for (std::size_t i = 0; i < kMaxInFlight; ++i) {
            if (!slots_[i].active) {
                slots_[i].active = true;
                slots_[i].handle = handle;
                slots_[i].record = record;
                slots_[i].last_poll_ms = kNeverPolled;
                ++count_;
                return true;
            }
        }
        return false;  // capacity exhausted -> fail-closed
    }

    void untrack(std::string_view coid) noexcept {
        auto idx = find_index(coid);
        if (idx != kNotFound) {
            slots_[idx] = Slot{};
            if (count_ > 0) --count_;
        }
    }

    std::size_t count() const noexcept { return count_; }
    static constexpr std::size_t capacity() noexcept { return kMaxInFlight; }

    // Per-slot state poll_once() needs to read/mutate. Deliberately NOT exposed
    // as a public accessor (no at()/raw pointer) -- see file header's thread-
    // ownership note. fn is invoked once per active slot, in slot-index order;
    // fn may call untrack() for the coid it was just given (poll_once() does,
    // on every resolution/escalation path) -- safe because untrack() only
    // clears the slot in place, it never moves or reindexes the others.
    template <typename Fn>
    void for_each_active(Fn&& fn) {
        for (std::size_t i = 0; i < kMaxInFlight; ++i) {
            if (slots_[i].active) {
                fn(slots_[i].handle, slots_[i].record, slots_[i].last_poll_ms);
                // fn may have untracked this exact slot (index i); slots_[i] is
                // now default-constructed/inactive in that case, which is fine
                // -- the loop simply moves on to i+1.
            }
        }
    }

    // Lets poll_once() record that a query was just attempted for a still-
    // tracked (not untracked) order, without exposing a general-purpose mutator.
    void note_polled(std::string_view coid, std::int64_t now_ms) noexcept {
        auto idx = find_index(coid);
        if (idx != kNotFound) slots_[idx].last_poll_ms = now_ms;
    }

private:
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    struct Slot {
        bool active{false};
        InFlightHandle handle{};
        OrderRecord record{};
        std::int64_t last_poll_ms{0};
    };

    std::size_t find_index(std::string_view coid) const noexcept {
        if (coid.empty()) return kNotFound;
        for (std::size_t i = 0; i < kMaxInFlight; ++i) {
            if (slots_[i].active && slots_[i].record.client_order_id.view() == coid) return i;
        }
        return kNotFound;
    }

    std::array<Slot, kMaxInFlight> slots_{};
    std::size_t count_{0};
};

// --- Cross-thread messages ---

struct ReconcileIngress {   // hot thread -> reconcile thread: newly-Ambiguous order
    InFlightHandle handle;
    OrderRecord record;
};

struct ReconcileEvent {     // reconcile thread -> hot thread: a discovered result.
    InFlightHandle handle;
    ClientOrderId coid;
    // May be any legal Ambiguous-transition target, INCLUDING EscalatedToOperator
    // and the still-live Accepted/PartialFill -- drain_reconcile_events() audits
    // all of them but releases the InFlightRegistry slot only when
    // is_exchange_final(resulting_state) is true.
    OrderState resulting_state{OrderState::Ambiguous};
    std::int64_t exchange_order_id{0};
    std::int64_t filled_qty_ticks{0};
    std::int64_t avg_fill_price_ticks{0};

    // TODO 1A.3 follow-up (PositionTruth). Appended at the end, not inserted
    // earlier -- matches this struct's own field-ordering convention.
    // symbol_id/side: fixed at order-intent time, carried through so
    // drain_reconcile_events() can both fix a pre-existing bug (see that
    // function's own comment: OrderReconciled/OrderEscalated AuditRecords
    // never set symbol_id) and fold a fill into PositionTruth.
    // fill_delta_qty_ticks: the INCREMENTAL amount filled since the last
    // observation (resolved.filled_qty_ticks - attempted.filled_qty_ticks at
    // push time in poll_once()), not the cumulative total -- PositionTruth::
    // apply_fill() needs a delta, and InFlightRegistry retains no history to
    // derive one from on the consuming side.
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    std::int64_t fill_delta_qty_ticks{0};
};

static_assert(std::is_trivially_copyable_v<OrderRecord>,
              "OrderRecord crosses the SpscRing producer/consumer boundary via ReconcileIngress");
static_assert(std::is_trivially_copyable_v<ReconcileIngress>);
static_assert(std::is_trivially_copyable_v<ReconcileEvent>);

using ToReconcileRing = SpscRing<ReconcileIngress, kMaxInFlight>;
using ReconcileEventRing = SpscRing<ReconcileEvent, kMaxInFlight>;

namespace detail {

// Legal Found targets from Ambiguous -- deliberately re-derived here rather
// than calling validate_transition() speculatively, so an invalid QueryResult
// is rejected as Inconclusive BEFORE any state mutation is attempted, with one
// clear reason, instead of relying on transition_to()'s return value as the
// only signal after the fact.
inline bool is_legal_ambiguous_target(OrderState s) noexcept {
    return s == OrderState::Accepted || s == OrderState::PartialFill ||
           s == OrderState::Filled || s == OrderState::Cancelled ||
           s == OrderState::Rejected || s == OrderState::Expired ||
           s == OrderState::CancelRequested;  // L4 §6.5: confirmed PENDING_CANCEL
}

// True iff a query on an order currently in `from` may legitimately report `to`.
//
// Generalizes is_legal_ambiguous_target() to the live states this loop now also
// polls (audit EXEC-INFLIGHT-003). Two things differ from the Ambiguous case:
//
//   * `to == from` is a legal, expected answer for a LIVE order -- "still resting,
//     nothing changed" is the most common poll result there is, and must be a
//     no-op rather than a rejected result. It is NOT legal for Ambiguous: a Found
//     outcome means the query positively identified the order's state, and
//     "positively identified it as uncertain" is not a thing an exchange reports.
//   * a live order can only move forward into the fill/cancel/expire set; it can
//     never become Ambiguous or Rejected (see validate_transition()'s own note on
//     why Binance's REJECTED is submit-time only).
inline bool is_legal_query_target(OrderState from, OrderState to) noexcept {
    if (to == from) {
        return from == OrderState::Accepted || from == OrderState::PartialFill;
    }
    switch (from) {
        case OrderState::Ambiguous:
            return is_legal_ambiguous_target(to);
        case OrderState::Accepted:
            // CancelRequested (L4 §6.6): the exchange can report PENDING_CANCEL for
            // an already-live order during a normal live-poll, not only via
            // Ambiguous reconciliation -- without this arm, a real cancel-in-
            // progress observation on a resting order would be silently discarded
            // here as an "illegal" result rather than tracked.
            return to == OrderState::PartialFill || to == OrderState::Filled ||
                   to == OrderState::Cancelled || to == OrderState::Expired ||
                   to == OrderState::CancelRequested;
        case OrderState::PartialFill:
            return to == OrderState::Filled || to == OrderState::Cancelled ||
                   to == OrderState::Expired || to == OrderState::CancelRequested;
        default:
            return false;
    }
}

// Never trust an injected QueryFn (mock today, network-backed later) to return
// internally-consistent data -- same "decode-time range/bounds check" discipline
// docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md applies throughout to untrusted
// exchange input. Numeric sanity only; state-transition legality is
// is_legal_query_target()'s job, because it depends on the order's current state.
inline bool is_valid_query_result(const QueryResult& r, const OrderRecord& rec) noexcept {
    if (r.filled_qty_ticks < 0 || r.filled_qty_ticks > rec.intended_qty_ticks) return false;
    if (r.avg_fill_price_ticks < 0) return false;
    if (r.filled_qty_ticks == 0 && r.avg_fill_price_ticks != 0) return false;  // inconsistent
    if (r.exchange_order_id < 0) return false;
    // A Filled order must actually be fully filled -- an exchange (or a buggy
    // adapter) reporting FILLED with a short filled_qty would otherwise release the
    // in-flight slot on an order that still has quantity resting.
    if (r.confirmed_state == OrderState::Filled && r.filled_qty_ticks != rec.intended_qty_ticks) {
        return false;
    }
    return true;
}

}  // namespace detail

// Runs on the reconcile thread. Drains `inbound` into `tracker`, walks every
// tracked non-exchange-final order, and for each state change pushes one
// ReconcileEvent onto `outbound`. NEVER touches InFlightRegistry or AuditRingSink
// (see file header).
//
// TWO KINDS OF TRACKED ORDER (audit EXEC-INFLIGHT-003):
//   * Ambiguous -- an unresolved uncertainty. Driven by
//     determine_reconcile_action() with exponential backoff, and escalated to the
//     operator after kMaxQueryAttempts. Unchanged from before.
//   * Accepted / PartialFill -- LIVE and resting on the exchange. Polled at a flat
//     live_poll_interval_ms cadence and NEVER escalated: a resting order is a
//     normal steady state, not an anomaly, and its query_attempts must not count
//     toward the Ambiguous escalation cap.
//
// This loop previously handled only the first kind, and untracked an order the
// moment reconciliation discovered it was actually still live. Because
// drain_reconcile_events() correctly refuses to release a non-exchange-final
// InFlightRegistry slot, that left such orders with a held slot and nothing left
// tracking them -- unreleasable for the life of the process, 64 of them enough to
// fail-closed every subsequent submit. Giving live orders a polling path is what
// closes that: every accepted order now has a route to a terminal state.
//
// Backpressure: a slot is only untracked AFTER its ReconcileEvent is
// successfully pushed. If `outbound` is momentarily full, the record keeps its
// pre-transition state and stays tracked -- retried on a later call -- rather than
// silently dropping a resolution that already cost a real query.
inline void poll_once(OrderTracker& tracker,
                       ToReconcileRing& inbound,
                       ReconcileEventRing& outbound,
                       const QueryPort& query_port,
                       const ReconcilePollPolicy& policy,
                       std::int64_t now_ms) noexcept {
    ReconcileIngress in{};
    while (inbound.try_pop(in)) {
        (void)tracker.track(in.handle, in.record, now_ms);
        // A false return (capacity exhausted / duplicate coid) is silently
        // dropped here rather than asserted: ToReconcileRing's capacity already
        // equals kMaxInFlight, matching InFlightRegistry's own cap, so under
        // correct wiring this cannot happen -- see the ToReconcileRing capacity
        // note below. Not treated as fatal because this function is noexcept
        // and has no error-reporting channel back to the hot thread.
    }

    std::uint32_t queries_this_tick = 0;

    tracker.for_each_active([&](InFlightHandle handle, OrderRecord& record, std::int64_t last_poll_ms) {
        // An exchange-final order has nothing left to discover and should already
        // have been untracked; anything else is either Ambiguous or live.
        if (is_exchange_final(record.state)) return;

        const bool ambiguous = (record.state == OrderState::Ambiguous);
        const bool live = (record.state == OrderState::Accepted ||
                            record.state == OrderState::PartialFill);
        if (!ambiguous && !live) return;  // e.g. EscalatedToOperator: operator owns it now

        auto coid = record.client_order_id.view();

        if (ambiguous) {
            const ReconcileAction action = determine_reconcile_action(record);
            if (action == ReconcileAction::NoAction) return;

            if (action == ReconcileAction::EscalateToOperator) {
                OrderRecord escalated = record;
                if (escalated.transition_to(OrderState::EscalatedToOperator) != TransitionResult::Ok) return;
                ReconcileEvent ev{};
                ev.handle = handle;
                ev.coid = escalated.client_order_id;
                ev.resulting_state = OrderState::EscalatedToOperator;
                if (outbound.try_push(ev)) {
                    tracker.untrack(coid);
                }
                // else: leave tracked, retried next call (see backpressure note above)
                return;
            }

            // action == QueryOrder
            if (queries_this_tick >= policy.max_queries_per_tick) return;
            if (last_poll_ms != OrderTracker::kNeverPolled) {
                // last_poll_ms != kNeverPolled implies at least one attempt has
                // already been made, so query_attempts >= 1 here. The delay before
                // the NEXT attempt is indexed by retries already elapsed
                // (query_attempts - 1: 0 after the first attempt, 1 after the
                // second, ...), not by query_attempts itself -- reconcile_backoff_delay_ms's
                // attempt=0 case is the base interval, meant for the first retry.
                const std::uint8_t retries_elapsed =
                    static_cast<std::uint8_t>(record.query_attempts - 1);
                if (now_ms - last_poll_ms < reconcile_backoff_delay_ms(policy, retries_elapsed)) return;
            }
        } else {
            // Live on the exchange: flat cadence, no escalation cap. See
            // ReconcilePollPolicy::live_poll_interval_ms for why not backoff.
            if (queries_this_tick >= policy.max_queries_per_tick) return;
            if (last_poll_ms != OrderTracker::kNeverPolled &&
                now_ms - last_poll_ms < policy.live_poll_interval_ms) {
                return;
            }
        }

        tracker.note_polled(coid, now_ms);
        ++queries_this_tick;

        QueryResult result = query_port.call(OrderExpectation::from(record));

        // query_attempts is incremented on every genuine attempt (Inconclusive
        // included), matching determine_reconcile_action()'s existing contract
        // (it escalates once query_attempts reaches kMaxQueryAttempts) -- this must
        // happen whether or not the result turns out usable below. ONLY for
        // Ambiguous orders: that counter IS the escalation cap, and a live order
        // polled every live_poll_interval_ms would otherwise "escalate" itself after
        // three routine liveness checks.
        OrderRecord attempted = record;
        if (ambiguous) ++attempted.query_attempts;

        if (result.outcome != QueryOutcome::Found || !detail::is_valid_query_result(result, attempted) ||
            !detail::is_legal_query_target(attempted.state, result.confirmed_state)) {
            record = attempted;  // Inconclusive (or rejected as invalid): keep state, try again later
            return;
        }

        // "Still in the state we already knew about" is the ordinary answer for a
        // live resting order. Refresh the fill figures (a partial fill can grow
        // without changing the state) and keep tracking -- no transition, no event,
        // no slot release.
        if (result.confirmed_state == attempted.state) {
            attempted.exchange_order_id = result.exchange_order_id;
            attempted.filled_qty_ticks = result.filled_qty_ticks;
            attempted.avg_fill_price_ticks = result.avg_fill_price_ticks;
            record = attempted;
            return;
        }

        OrderRecord resolved = attempted;
        if (resolved.transition_to(result.confirmed_state) != TransitionResult::Ok) {
            record = attempted;  // should be unreachable given is_legal_query_target(); fail safe
            return;
        }
        resolved.exchange_order_id = result.exchange_order_id;
        resolved.filled_qty_ticks = result.filled_qty_ticks;
        resolved.avg_fill_price_ticks = result.avg_fill_price_ticks;

        ReconcileEvent ev{};
        ev.handle = handle;
        ev.coid = resolved.client_order_id;
        ev.resulting_state = resolved.state;
        ev.exchange_order_id = resolved.exchange_order_id;
        ev.filled_qty_ticks = resolved.filled_qty_ticks;
        ev.avg_fill_price_ticks = resolved.avg_fill_price_ticks;
        ev.symbol_id = resolved.symbol_id;
        ev.side = resolved.side;
        // Incremental amount filled since the last observation, not the
        // cumulative total -- attempted.filled_qty_ticks is the pre-transition
        // baseline this same poll_once() call started from (either the last
        // successfully-polled figure, or 0 for a never-before-resolved order).
        // Never negative: attempted/resolved.filled_qty_ticks only ever grows
        // (is_valid_query_result() already rejects a response reporting less
        // filled than previously observed), but guard with the same
        // non-negative floor apply_fill() itself enforces, defensively.
        ev.fill_delta_qty_ticks = resolved.filled_qty_ticks - attempted.filled_qty_ticks;

        if (outbound.try_push(ev)) {
            if (is_exchange_final(resolved.state)) {
                tracker.untrack(coid);
            } else {
                // Still live (Ambiguous -> Accepted/PartialFill, or
                // Accepted -> PartialFill). Keep tracking so this order still has a
                // route to a terminal state -- untracking here is precisely what
                // stranded its InFlightRegistry slot forever (audit
                // EXEC-INFLIGHT-003). Reset the poll clock so the freshly-adopted
                // live cadence starts from now rather than inheriting the Ambiguous
                // backoff position.
                record = resolved;
                tracker.note_polled(coid, now_ms);
            }
        } else {
            record = attempted;  // publish failed: keep prior state, retry later (backpressure note above)
        }
    });
}

// Runs on the hot/submit thread. Drains `events`; for each, constructs and
// appends the corresponding AuditRecord (the ONLY place OrderReconciled/
// OrderEscalated are ever emitted, matching orchestrate_submit()'s own
// per-transition audit style), then releases the InFlightRegistry slot via
// mark_resolved_handle() IF AND ONLY IF is_exchange_final(resulting_state) --
// EscalatedToOperator and the still-live Accepted/PartialFill are audited but
// do not release (see docs/SPEC_INVARIANTS.md's InFlightRegistry entry: an
// order that may still be live must keep its slot).
//
// `position_truth` is a trailing-default parameter (nullptr = no-op, same
// backward-compatible-extension pattern as RequestWeightTracker::reset()'s
// window_seconds and fetch_signed_body_coro's verb) -- every existing call
// site's behavior is unchanged.
//
// `fill_context` (TODO 1A.4 batch 2, same trailing-default-nullptr convention):
// when non-null, this is the SAME OrderFillContext table
// live_submit_orchestrator.hpp's direct-fill branch and
// binance_user_data_event.hpp's drain_user_data_events() also route through --
// the fix for the cross-mechanism double-count hazard those files' own
// comments describe (a WS executionReport crediting part of a fill via its
// own baseline, then this function separately crediting the full cumulative
// amount via ev.fill_delta_qty_ticks -- computed from OrderTracker's own
// private, WS-unaware baseline -- would double-apply the WS-observed
// portion). When configured, the safe delta is recomputed here via
// consume_delta(ev.coid.view(), ev.filled_qty_ticks) -- ev.filled_qty_ticks
// is the CUMULATIVE observed fill (poll_once()'s resolved.filled_qty_ticks),
// not ev.fill_delta_qty_ticks, precisely because the correct increment must
// be measured against the SHARED baseline, not OrderTracker::Slot's own
// private one. When fill_context is null (existing callers/tests), behavior
// is byte-identical to before: ev.fill_delta_qty_ticks is used directly.
// OrderFillContext::remove() is called at exactly the same point
// mark_resolved_handle() is -- one of this batch's own verified-exhaustive 3
// real call sites (see order_fill_context.hpp's own header comment).
inline void drain_reconcile_events(InFlightRegistry& in_flight,
                                    AuditRingSink* audit,
                                    ReconcileEventRing& events,
                                    std::int64_t now_ms,
                                    PositionTruth* position_truth = nullptr,
                                    OrderFillContext* fill_context = nullptr) noexcept {
    ReconcileEvent ev{};
    while (events.try_pop(ev)) {
        if (audit) {
            AuditRecord ar{};
            ar.timestamp_ms = now_ms;
            ar.event_type = (ev.resulting_state == OrderState::EscalatedToOperator)
                                 ? AuditEventType::OrderEscalated
                                 : AuditEventType::OrderReconciled;
            // AUDIT ORDER-RECONCILED-SYMBOL-014: symbol_id was never assigned
            // here since OrderReconciled/OrderEscalated were first introduced
            // (this is the only place either is ever emitted) -- every such
            // AuditRecord's symbol_id has been the struct's default (0) since
            // this function landed, unlike orchestrate_submit()'s own
            // AuditRecord constructions, which do assign ctx.symbol_id. Found
            // and fixed as part of adding ReconcileEvent::symbol_id/side for
            // PositionTruth below -- not a design change, a historical bug fix.
            ar.symbol_id = ev.symbol_id;
            ar.exchange_order_id = ev.exchange_order_id;
            ar.set_client_order_id(ev.coid.view());
            ar.resulting_state = ev.resulting_state;
            ar.filled_qty_ticks = ev.filled_qty_ticks;
            ar.avg_fill_price_ticks = ev.avg_fill_price_ticks;
            ar.side = ev.side;
            audit->append(ar);
        }

        const std::int64_t safe_delta_qty_ticks =
            fill_context ? fill_context->consume_delta(ev.coid.view(), ev.filled_qty_ticks)
                         : ev.fill_delta_qty_ticks;
        if (position_truth && safe_delta_qty_ticks > 0) {
            position_truth->apply_fill(ev.symbol_id, ev.side, safe_delta_qty_ticks);
        }
        if (is_exchange_final(ev.resulting_state)) {
            in_flight.mark_resolved_handle(ev.handle, ev.coid.view());
            if (fill_context) fill_context->remove(ev.coid.view());
        }
    }
}

}  // namespace hy
