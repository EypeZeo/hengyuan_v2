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
#include <hengyuan/order_lifecycle.hpp>
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
};

// --- QueryPort: same injection pattern and signature style as SubmitPort ---
// (live_submit_orchestrator.hpp) -- const char*, not a value/reference wrapper,
// and a null fn folds into Inconclusive rather than a dedicated error value,
// matching SubmitPort::call()'s own precedent on both counts. A real network
// implementation is separate, not-yet-built work.
struct QueryPort {
    using QueryFn = QueryResult(*)(const char* client_order_id, void* user_data);

    QueryFn fn{nullptr};
    void* user_data{nullptr};

    QueryResult call(const char* coid) const noexcept {
        if (!fn) return {};  // Inconclusive by default-construction
        return fn(coid, user_data);
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
           s == OrderState::Rejected || s == OrderState::Expired;
}

// Never trust an injected QueryFn (mock today, network-backed later) to return
// internally-consistent data -- same "decode-time range/bounds check" discipline
// docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md applies throughout to untrusted
// exchange input.
inline bool is_valid_query_result(const QueryResult& r, const OrderRecord& rec) noexcept {
    if (!is_legal_ambiguous_target(r.confirmed_state)) return false;
    if (r.filled_qty_ticks < 0 || r.filled_qty_ticks > rec.intended_qty_ticks) return false;
    if (r.avg_fill_price_ticks < 0) return false;
    if (r.filled_qty_ticks == 0 && r.avg_fill_price_ticks != 0) return false;  // inconsistent
    if (r.exchange_order_id < 0) return false;
    return true;
}

}  // namespace detail

// Runs on the reconcile thread. Drains `inbound` into `tracker`, walks tracked
// Ambiguous orders applying determine_reconcile_action() (order_lifecycle.hpp,
// unchanged), and for each resolution/escalation pushes one ReconcileEvent onto
// `outbound`. NEVER touches InFlightRegistry or AuditRingSink (see file header).
//
// Backpressure: a slot is only untracked AFTER its ReconcileEvent is
// successfully pushed. If `outbound` is momentarily full, the record stays
// Ambiguous and tracked -- retried on a later call -- rather than silently
// dropping a resolution that already cost a real query.
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
        if (record.state != OrderState::Ambiguous) return;  // defensive; should not happen

        const ReconcileAction action = determine_reconcile_action(record);
        if (action == ReconcileAction::NoAction) return;

        auto coid = record.client_order_id.view();

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

        tracker.note_polled(coid, now_ms);
        ++queries_this_tick;

        char coid_buf[kClientOrderIdLen + 1];
        auto n = coid.size() < kClientOrderIdLen ? coid.size() : kClientOrderIdLen;
        std::memcpy(coid_buf, coid.data(), n);
        coid_buf[n] = '\0';
        QueryResult result = query_port.call(coid_buf);

        // query_attempts is incremented on every genuine attempt (Inconclusive
        // included), matching determine_reconcile_action()'s existing contract
        // (it escalates once query_attempts reaches kMaxQueryAttempts) --
        // this must happen whether or not the result turns out usable below.
        OrderRecord attempted = record;
        ++attempted.query_attempts;

        if (result.outcome != QueryOutcome::Found || !detail::is_valid_query_result(result, attempted)) {
            record = attempted;  // Inconclusive (or rejected as invalid): keep Ambiguous, try again later
            return;
        }

        OrderRecord resolved = attempted;
        if (resolved.transition_to(result.confirmed_state) != TransitionResult::Ok) {
            record = attempted;  // should be unreachable given is_valid_query_result(); fail safe
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

        if (outbound.try_push(ev)) {
            tracker.untrack(coid);
        } else {
            record = attempted;  // publish failed: keep Ambiguous, retry later (backpressure note above)
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
inline void drain_reconcile_events(InFlightRegistry& in_flight,
                                    AuditRingSink* audit,
                                    ReconcileEventRing& events,
                                    std::int64_t now_ms) noexcept {
    ReconcileEvent ev{};
    while (events.try_pop(ev)) {
        if (audit) {
            AuditRecord ar{};
            ar.timestamp_ms = now_ms;
            ar.event_type = (ev.resulting_state == OrderState::EscalatedToOperator)
                                 ? AuditEventType::OrderEscalated
                                 : AuditEventType::OrderReconciled;
            ar.exchange_order_id = ev.exchange_order_id;
            ar.set_client_order_id(ev.coid.view());
            ar.resulting_state = ev.resulting_state;
            ar.filled_qty_ticks = ev.filled_qty_ticks;
            ar.avg_fill_price_ticks = ev.avg_fill_price_ticks;
            audit->append(ar);
        }
        if (is_exchange_final(ev.resulting_state)) {
            in_flight.mark_resolved_handle(ev.handle, ev.coid.view());
        }
    }
}

}  // namespace hy
