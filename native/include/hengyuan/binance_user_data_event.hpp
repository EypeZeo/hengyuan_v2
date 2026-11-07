// SPDX-License-Identifier: proprietary
// binance_user_data_event.hpp — TODO 1A.4: the event struct/ring/drain function
// live_submit_orchestrator.hpp needs to consume Binance user-data-stream WS events on the
// hot/submit thread, WITHOUT pulling in Boost.Asio/Beast/OpenSSL/simdjson.
//
// Deliberately split out of the session headers (binance_ws_api_user_data_session.hpp; before M5b-2 also the
// listenKey session's), mirroring this codebase's own
// existing precedent: binance_market_event.hpp (lightweight event struct, zero Boost/simdjson
// dependency) vs. binance_ws_session.hpp (the actual Beast session, depends on it) vs.
// binance_json_parser.hpp (depends on simdjson, not Boost). live_submit_orchestrator.hpp's own
// existing includes (order_lifecycle.hpp, order_tracker.hpp, audit_trail.hpp, ...) are all
// offline/L1 logic with no network-library dependency at all -- pulling the full WS session
// header (and its Boost.Beast/Asio/OpenSSL/simdjson transitive weight) into every consumer of
// orchestrate_submit(), including pure-logic unit tests that never touch a WS session, would be
// a real, avoidable compile-time and layering cost.
//
// Governance: L1 (pure data + orchestration glue, no I/O of its own).
//
// TODO 1A.4 batch 2: ExecutionType (below) lives here rather than in the heavier
// session headers (binance_ws_api_user_data_session.hpp), even though it is WS-event-only vocabulary with no REST
// counterpart -- UserDataWsEvent::exec_type needs the type visible at struct-definition time,
// and this light file cannot include the heavy session header back (that header already
// includes THIS one, for UserDataWsEvent itself; the reverse direction would be circular). Its
// string parser is co-located right here for the same reason account_truth.hpp keeps
// order_side_name()/parse_binance_order_side() together -- one type, one place, no ODR-risk
// split -- and needs nothing from simdjson/Boost to do a handful of string_view comparisons.

#pragma once

#include <hengyuan/account_truth.hpp>       // OrderSide, checked_scaled_mul_div
#include <hengyuan/audit_trail.hpp>
#include <hengyuan/binance_decimal.hpp>     // parse_decimal_to_ticks_with_scale
#include <hengyuan/durable_order_audit_port.hpp>  // DurableOrderAuditPort: the durable append the user-data publisher goes through (M4)
#include <hengyuan/late_fill_alarm.hpp>     // LateFillAlarm (D5 slice R4): the alarm for a report about an order an operator wrote off
#include <hengyuan/order_durable_path.hpp>  // plan_observation(): which frames make an observation replayable (M4)
#include <hengyuan/order_fill_context.hpp>  // OrderFillContext, OrderFillContextEntry
#include <hengyuan/order_lifecycle.hpp>     // ClientOrderId, InFlightRegistry, OrderState
#include <hengyuan/position_truth.hpp>      // PositionTruth
#include <hengyuan/reconnect_barrier.hpp>   // ReconnectBarrier (private-stream migration M3a): the late-event filter of drain_user_data_events()
#include <hengyuan/spsc_ring.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace hy {

enum class UserDataEventKind : std::uint8_t {
    Unknown = 0,
    ExecutionReport = 1,
    OutboundAccountPosition = 2,
    ListenKeyExpired = 3,  // RESERVED: the listenKey stream is retired (M5b-2); nothing produces or consumes this kind, the value stays so the kinds below keep their numbers
    // The WebSocket API user data stream (private-stream migration M1; blueprint 5.7 P-28, P-29):
    BalanceUpdate = 4,           // balanceUpdate: a deposit, withdrawal or transfer between accounts
    ExternalLockUpdate = 5,      // externalLockUpdate: an external system locked or unlocked part of the spot balance
    ListStatus = 6,              // listStatus: an order list changed state (sent besides the executionReport)
    EventStreamTerminated = 7,   // eventStreamTerminated: THIS subscription ended
    ServerShutdown = 8,          // serverShutdown: the connection is about to close -- a connection event, no subscription
};

// listStatus "L" -- the state of an order list as a whole.
enum class ListOrderStatus : std::uint8_t {
    Unknown = 0,
    Executing = 1,  // EXECUTING
    AllDone = 2,    // ALL_DONE
    Reject = 3,     // REJECT
};

inline bool parse_binance_list_order_status(std::string_view s, ListOrderStatus& out) noexcept {
    if (s == "EXECUTING") { out = ListOrderStatus::Executing; return true; }
    if (s == "ALL_DONE") { out = ListOrderStatus::AllDone; return true; }
    if (s == "REJECT") { out = ListOrderStatus::Reject; return true; }
    return false;
}

// Binance executionReport "x" field -- WS-event-only vocabulary, audit/observability purposes
// only (see drain_user_data_events()'s own comment for why this never gates whether a fill gets
// folded into PositionTruth -- the "z" cumulative-quantity delta already does that correctly on
// its own, no exec-type branch needed). Confidence: medium -- this value set is external
// Binance API knowledge, not derived from either accepted spec (see this batch's own plan's
// "实施前建议先做的一件事"); flagged for a final manual doc check before this touches real
// credentials, not a blocking unknown.
enum class ExecutionType : std::uint8_t {
    Unknown = 0,
    New = 1,
    Canceled = 2,
    Replaced = 3,
    Rejected = 4,
    Trade = 5,
    Expired = 6,
    TradePrevention = 7,
};

inline bool parse_binance_execution_type(std::string_view s, ExecutionType& out) noexcept {
    if (s == "NEW") { out = ExecutionType::New; return true; }
    if (s == "CANCELED") { out = ExecutionType::Canceled; return true; }
    if (s == "REPLACED") { out = ExecutionType::Replaced; return true; }
    if (s == "REJECTED") { out = ExecutionType::Rejected; return true; }
    if (s == "TRADE") { out = ExecutionType::Trade; return true; }
    if (s == "EXPIRED") { out = ExecutionType::Expired; return true; }
    if (s == "TRADE_PREVENTION") { out = ExecutionType::TradePrevention; return true; }
    return false;
}

// Field order deliberately groups by alignment size (8-byte fields, then fixed char buffers,
// then 1-byte fields) to minimize compiler-inserted padding -- not a correctness requirement
// (this struct crosses an SPSC ring, consumed once and discarded, not a multi-thread-contended
// coordinator block, so CLAUDE.md's alignas(hardware_destructive_interference_size) rule does
// not apply here), just a zero-risk size reduction.
//
// The six raw-decimal fields are kept as fixed char buffers, not converted to ticks at parse
// time -- conversion needs a per-symbol scale (price_scale/qty_scale/quote_scale) that this
// struct has no way to know (the WS session has no SymbolRegistry access -- see this batch's own
// plan for why one is deliberately not built here); the scale is looked up from OrderFillContext
// on the hot thread instead, in drain_user_data_events() below, the same place the actual
// fold-in decision is made.
struct UserDataWsEvent {
    std::int64_t event_time_ms{0};        // "E"
    std::int64_t exchange_order_id{0};    // "i" -- only meaningful when kind == ExecutionReport
    std::int64_t transaction_time_ms{0};  // "T" (executionReport, balanceUpdate, externalLockUpdate, listStatus);
                                           // "u", the time of the last account update (outboundAccountPosition)
    // --- WebSocket API user data stream (private-stream migration M1). The retired listenKey parser left all of these at their defaults. ---
    std::uint64_t generation{0};          // the connection generation that received the frame: stamped by the session (M2), never by the
                                           // parser; 0 = not stamped
    std::int64_t subscription_id{-1};     // the wrapper's "subscriptionId"; -1 = the frame carried none (serverShutdown never does)
    std::int64_t trade_id{-1};            // "t" -- the exchange's own -1 means "no trade" (an event that is not a fill)
    std::int64_t order_list_id{-1};       // "g" -- listStatus; -1 = none

    ClientOrderId coid{};                 // "c"
    char last_qty_raw[24]{};              // "l" -- last executed quantity, raw decimal string
    char cumulative_filled_qty_raw[24]{}; // "z" -- cumulative filled qty; the dedup-safety key
                                           // OrderFillContext::consume_delta() baselines against
    char cumulative_quote_qty_raw[24]{};  // "Z" -- cumulative quote qty; avg-price numerator
    char last_price_raw[24]{};            // "L" -- last executed price
    char order_qty_raw[24]{};             // "q" -- original order quantity (captured for
                                           // completeness per this batch's plan; not consumed by
                                           // any decision this slice makes)
    char order_price_raw[24]{};           // "p" -- original order price (same as above)
    char commission_raw[24]{};            // "n" -- commission of THIS fill, raw decimal string (the accounting kernel, blueprint 6.1.7,
                                           // is what consumes it; nothing in this slice does)
    char commission_asset[16]{};          // "N" -- commission asset; empty when the exchange sent null (no commission)
    char asset[16]{};                     // "a" -- balanceUpdate / externalLockUpdate
    char delta_raw[24]{};                 // "d" -- balanceUpdate / externalLockUpdate: the balance delta, raw decimal string

    UserDataEventKind kind{UserDataEventKind::Unknown};
    OrderSide side{OrderSide::Buy};        // "S" -- defensive cross-check only; the operational
                                            // side for apply_fill() always comes from
                                            // OrderFillContext (the value recorded locally at
                                            // submit time), never from this field -- same
                                            // "never trust the response, only the ask" discipline
                                            // OrderExpectation already applies to the REST path.
    bool side_known{false};
    OrderState order_status{OrderState::Intent};  // "X", via map_binance_order_status()
    bool order_status_known{false};
    ExecutionType exec_type{ExecutionType::Unknown};  // "x" -- audit-only, see enum's own comment
    bool tif_is_gtc{false};                // "f" -- equality-checked against "GTC" only, no
                                            // enum: this codebase only ever sends GTC (L5 §1),
                                            // same discipline binance_private_rest.hpp's REST
                                            // response parsing already applies to this field.
    ListOrderStatus list_order_status{ListOrderStatus::Unknown};  // "L" -- listStatus
};
static_assert(std::is_trivially_copyable_v<UserDataWsEvent>,
              "UserDataWsEvent crosses the SpscRing producer/consumer boundary");

using UserDataWsEventRing = SpscRing<UserDataWsEvent, 64>;

// --- Private-stream migration M4 (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 6): the durable-first publication of execution reports ---------------------

// What the stream said about one order, held until it is durable. One per order: a newer word about the same order replaces it, so a burst of reports for
// one order is one frame, not one per fill (design 6.2, "merge publisher").
struct HeldUserDataObservation {
    UserDataWsEvent ev{};
    std::int64_t cumulative_filled_qty_ticks{0};  // "z" in the order's own quantity scale
    std::int64_t avg_fill_price_ticks{0};         // derived; 0 when there is no fill yet or the quotient does not fit
};
static_assert(std::is_trivially_copyable_v<HeldUserDataObservation>);

namespace detail {

// The average fill price from "Z" and "z" at the order's scales -- the REST path's own formula (parse_order_query_response()). A derived value only: it
// never gates anything, and 0 means "none" (no fill yet, or it does not fit).
inline std::int64_t derive_avg_fill_price_ticks(const UserDataWsEvent& ev, const OrderFillContextEntry& e, std::int64_t cumulative_filled_ticks) noexcept {
    std::int64_t cumulative_quote_ticks = 0;
    if (!parse_decimal_to_ticks_with_scale(std::string_view(ev.cumulative_quote_qty_raw), e.quote_scale, cumulative_quote_ticks)) return 0;
    const int exponent = static_cast<int>(e.price_scale) + static_cast<int>(e.qty_scale) - static_cast<int>(e.quote_scale);
    std::int64_t computed_avg_price = 0;
    if (!checked_scaled_mul_div(cumulative_quote_ticks, exponent, cumulative_filled_ticks, computed_avg_price)) return 0;
    return computed_avg_price;
}

// The record a drain without a durable publisher puts in the audit ring: the DELTA this event added (what consume_delta() just credited), not the
// cumulative fill. Also what an event the durable drain refuses leaves in the ring -- a trace of what the stream said, with no fill credited.
inline AuditRecord make_user_data_memory_record(const UserDataWsEvent& ev, const OrderFillContextEntry* e, std::int64_t delta_qty_ticks,
                                                std::int64_t avg_fill_price_ticks, std::int64_t now_ms) noexcept {
    AuditRecord ar{};
    ar.timestamp_ms = now_ms;
    ar.event_type = AuditEventType::UserDataStreamEventObserved;
    ar.exchange_order_id = ev.exchange_order_id;
    ar.set_client_order_id(ev.coid.view());
    ar.filled_qty_ticks = delta_qty_ticks;
    ar.avg_fill_price_ticks = avg_fill_price_ticks;
    if (e != nullptr) {
        ar.symbol_id = e->symbol_id;
        ar.side = e->side;
    }
    if (ev.order_status_known) ar.resulting_state = ev.order_status;
    return ar;
}

// The frame a held observation becomes -- and, in durable mode, the record the audit ring gets as well (SAFE-03's rule: built in one place, so the two always
// carry the same content). It carries the CUMULATIVE fill, which is what the recovery scan reads and holds monotone, unlike the delta of the record above.
inline AuditRecord make_user_data_frame(const HeldUserDataObservation& h, const OrderFillContextEntry& e, std::int64_t now_ms) noexcept {
    AuditRecord ar{};
    ar.timestamp_ms = now_ms;
    ar.event_type = AuditEventType::UserDataStreamEventObserved;
    ar.mode = ExecutionMode::Live;
    ar.symbol_id = e.symbol_id;
    ar.exchange_order_id = h.ev.exchange_order_id;
    ar.set_client_order_id(h.ev.coid.view());
    ar.qty_ticks = e.intended_qty_ticks;
    ar.resulting_state = h.ev.order_status;
    ar.filled_qty_ticks = h.cumulative_filled_qty_ticks;
    ar.avg_fill_price_ticks = h.avg_fill_price_ticks;
    ar.side = e.side;
    return ar;
}

// The frame a plan may put in front of it: "this order is Ambiguous", nothing observed (it carries the shadow's own fill, so it is a legal same-state-fill
// frame whatever the log holds).
inline AuditRecord make_user_data_ambiguous_record(const HeldUserDataObservation& h, const OrderFillContextEntry& e, std::int64_t now_ms) noexcept {
    AuditRecord ar{};
    ar.timestamp_ms = now_ms;
    ar.event_type = AuditEventType::OrderAmbiguous;
    ar.mode = ExecutionMode::Live;
    ar.symbol_id = e.symbol_id;
    ar.set_client_order_id(h.ev.coid.view());
    ar.resulting_state = OrderState::Ambiguous;
    ar.filled_qty_ticks = e.shadow.filled_qty_ticks;
    ar.side = e.side;
    ar.set_detail("user-data: Ambiguous re-recorded before its observation");
    return ar;
}

}  // namespace detail

// What a durable drain publishes into (all of it hot-thread state; `fill_context` is never null for a flush).
struct UserDataFlushEnv {
    AuditRingSink* audit{nullptr};
    PositionTruth* position_truth{nullptr};
    OrderFillContext* fill_context{nullptr};
    ReconnectBarrier* barrier{nullptr};
};

struct UserDataDurableStats {
    std::uint64_t acked_frames{0};          // frames the sink acknowledged (Ambiguous prefixes included)
    std::uint64_t failed_appends{0};        // appends it did not (each retry of a blocked observation counts again)
    std::uint64_t written{0};               // observations whose frame is durable
    std::uint64_t stale{0};                 // observations the log already said, or something later: nothing to write
    std::uint64_t coalesced{0};             // reports folded into a newer one of the same order held in the same drain (or older than it)
    std::uint64_t refused_unreadable{0};    // a status no mapping names, or a cumulative fill that does not parse: nothing to record
    std::uint64_t refused_untracked{0};     // a report for an in-flight order the fill context does not hold: no shadow to plan against
    std::uint64_t refused_contradiction{0}; // a frame the log cannot take, or a report that contradicts what the log holds (or itself)
    std::uint64_t dropped_gone{0};          // held observations whose order finished before they were written
};

// The durable-first publisher of execution reports (design 6.3). A report reaches the audit ring and PositionTruth only after its frame -- planned against
// the order's DurableShadow (order_durable_path.hpp), acknowledged by the sink -- is durable; a report the log cannot take is REFUSED, counted, and handed to
// the soft barrier (ReconnectBarrier::on_event_refused()): a sweep reads the truth from REST instead of this process guessing.
//
// BLOCKING. Like ReconcileDurablePublisher, a failed append is a property of the sink (DurableAuditSink fences itself for good on the first I/O failure),
// not of one record: the observation stays held, the shadow moves only for the frames that WERE acknowledged (so the retry plans from what the log holds),
// and the drain pops nothing from the ring while anything is held -- the ring fills, the session counts drops, the barrier blocks new orders.
//
// Owned by the thread that owns InFlightRegistry, OrderFillContext and the durable sink (the hot/submit thread); nothing here is shared, so -- unlike the
// SPSC rings -- it is deliberately not cacheline-aligned. It must outlive every drain call that uses it (it holds observations between calls) and so must
// whatever its port points at.
class UserDataDurablePublisher {
public:
    static constexpr std::size_t kCapacity = kMaxInFlight;  // at most one held observation per order that can be in flight

    UserDataDurablePublisher() = default;
    explicit UserDataDurablePublisher(const DurableOrderAuditPort& port) noexcept : port_(port) {}

    // Held observations have exactly one owner: a copy would be a second way to publish the same unacknowledged report.
    UserDataDurablePublisher(const UserDataDurablePublisher&) = delete;
    UserDataDurablePublisher& operator=(const UserDataDurablePublisher&) = delete;
    UserDataDurablePublisher(UserDataDurablePublisher&&) = delete;
    UserDataDurablePublisher& operator=(UserDataDurablePublisher&&) = delete;

    // Points an idle publisher at a port, for an owner that has to construct it before its sink exists. Refused (false, nothing changed) while an
    // observation is held: part of it may already be in the log behind the old port.
    bool set_port(const DurableOrderAuditPort& port) noexcept {
        if (held_count_ != 0) return false;
        port_ = port;
        return true;
    }

    enum class Stage : std::uint8_t { Held, Replaced, DroppedOlder, Contradiction, Full };

    // Intake (the drain's): `h` is what the stream just said about an order the fill context tracks, whose quantity is `intended_qty_ticks`. The latest word
    // about an order wins as far as the planner can tell: a newer one replaces the held one (Replaced), an older or equal one is dropped (DroppedOlder), and two
    // words that cannot both be true leave the held one in place and refuse the new one (Contradiction -- the caller counts it and tells the barrier).
    // Full cannot happen while every held order is tracked in a context of kMaxInFlight slots; it is refused rather than assumed.
    Stage stage(const HeldUserDataObservation& h, std::int64_t intended_qty_ticks) noexcept {
        for (std::size_t i = 0; i < held_count_; ++i) {
            HeldUserDataObservation& cur = held_[i];
            if (cur.ev.coid.view() != h.ev.coid.view()) continue;
            const DurableShadow as_held{cur.ev.order_status, false, cur.cumulative_filled_qty_ticks};
            switch (plan_observation(as_held, h.ev.order_status, h.cumulative_filled_qty_ticks, intended_qty_ticks).status) {
                case PathStatus::Write:
                    cur = h;
                    ++stats_.coalesced;
                    return Stage::Replaced;
                case PathStatus::Stale:
                    ++stats_.coalesced;
                    return Stage::DroppedOlder;
                case PathStatus::Unreachable:
                    return Stage::Contradiction;
            }
        }
        if (held_count_ >= held_.size()) [[unlikely]] return Stage::Full;
        held_[held_count_++] = h;
        return Stage::Held;
    }

    // Writes the held observations in the order they were first held, each as: plan against the shadow, its frames, the ACK, the shadow, and only then the
    // fold into PositionTruth and the audit-ring record. Stops at the first append that is not acknowledged; what is left stays held (blocked()).
    void flush(const UserDataFlushEnv& env, std::int64_t now_ms) noexcept {
        std::size_t published = 0;
        while (published < held_count_ && publish_one(held_[published], env, now_ms)) ++published;
        if (published == 0) return;
        for (std::size_t i = published; i < held_count_; ++i) held_[i - published] = held_[i];
        held_count_ -= published;
    }

    // True while an observation has been taken from the ring but is not durable yet (between drain calls: a flush that stopped).
    bool blocked() const noexcept { return held_count_ != 0; }
    std::size_t held_count() const noexcept { return held_count_; }

    // Why the drain refused a report before it could even be held (the planner's refusals are counted by flush() itself).
    enum class Refusal : std::uint8_t { Unreadable, Untracked, Contradiction };
    void note_refusal(Refusal why) noexcept {
        switch (why) {
            case Refusal::Unreadable: ++stats_.refused_unreadable; break;
            case Refusal::Untracked: ++stats_.refused_untracked; break;
            case Refusal::Contradiction: ++stats_.refused_contradiction; break;
        }
    }

    const UserDataDurableStats& stats() const noexcept { return stats_; }
    const DurableOrderAuditPort& port() const noexcept { return port_; }

private:
    bool append(const AuditRecord& record, std::int64_t now_ms) noexcept {
        const AuditAppendResult result = port_.append_durable(record, now_ms);
        if (result.acked()) [[likely]] {
            ++stats_.acked_frames;
            return true;
        }
        ++stats_.failed_appends;
        return false;
    }

    // false = blocked: an append was not acknowledged, and this observation (and everything behind it) stays held.
    bool publish_one(const HeldUserDataObservation& h, const UserDataFlushEnv& env, std::int64_t now_ms) noexcept {
        const std::string_view coid = h.ev.coid.view();
        const OrderFillContextEntry* e = env.fill_context->find(coid);
        if (e == nullptr) {  // the order finished (or was never tracked) since the report was held: nothing of it is left to record
            ++stats_.dropped_gone;
            return true;
        }
        const DurablePath plan = plan_observation(e->shadow, h.ev.order_status, h.cumulative_filled_qty_ticks, e->intended_qty_ticks);
        if (plan.status == PathStatus::Unreachable) [[unlikely]] {
            ++stats_.refused_contradiction;
            if (env.barrier != nullptr) env.barrier->on_event_refused();
            if (env.audit != nullptr) env.audit->append(detail::make_user_data_memory_record(h.ev, e, 0, 0, now_ms));
            return true;
        }
        if (plan.status == PathStatus::Stale) {
            ++stats_.stale;  // the log already holds this or something later: no frame; the fold below still brings memory up to the log
        } else {
            if (plan.ambiguous_first) {
                if (!append(detail::make_user_data_ambiguous_record(h, *e, now_ms), now_ms)) return false;
                env.fill_context->durable_acked(coid, OrderState::Ambiguous, e->shadow.filled_qty_ticks);
            }
            const AuditRecord frame = detail::make_user_data_frame(h, *e, now_ms);
            if (!append(frame, now_ms)) return false;
            env.fill_context->durable_acked(coid, h.ev.order_status, h.cumulative_filled_qty_ticks);
            ++stats_.written;
        }
        // The frame (or the proof that the log already holds this) is in hand: only now does the report reach memory.
        const std::int64_t delta = env.fill_context->consume_delta(coid, h.cumulative_filled_qty_ticks);
        if (delta > 0 && env.position_truth != nullptr) env.position_truth->apply_fill(e->symbol_id, e->side, delta);
        if (env.audit != nullptr) env.audit->append(detail::make_user_data_frame(h, *e, now_ms));
        return true;
    }

    DurableOrderAuditPort port_{};
    std::array<HeldUserDataObservation, kCapacity> held_{};
    std::size_t held_count_{0};
    UserDataDurableStats stats_{};
};

// Runs on the hot/submit thread -- the same thread that exclusively owns InFlightRegistry/
// AuditRingSink/PositionTruth/OrderFillContext (order_lifecycle.hpp's/order_tracker.hpp's/
// position_truth.hpp's/order_fill_context.hpp's own THREAD OWNERSHIP contracts). Drains
// `events`; for each, looks the event's coid up in `in_flight` (this is the ONLY way a
// WS-pushed event can be attributed to a tracked order -- unlike drain_reconcile_events(), there
// is no prior InFlightHandle handshake, the WS thread learns about an order purely from
// Binance's own push, keyed by "c"). A miss is silently ignored, not a crash/UB -- an event for
// an order this process isn't tracking (already resolved, or belongs to a different session
// entirely) is expected, not exceptional.
//
// `position_truth`/`fill_context` are trailing-default nullptr, matching this codebase's
// established additive-extension convention (position_truth.hpp's own OrchestratorContext
// field, order_tracker.hpp's drain_reconcile_events() fill_context parameter) -- omitted, this
// function's observable behavior is byte-identical to the narrower slice that only wrote the
// audit record.
//
// Fold-in is gated on fill_context->find(coid) succeeding, NOT on in_flight.is_in_flight()
// alone -- a coid can still be "in flight" per InFlightRegistry (that slot's own lifecycle is
// governed by mark_resolved()/mark_resolved_handle(), a separate table) while its
// OrderFillContext entry has already been removed by the SAME call site
// (live_submit_orchestrator.hpp's Accepted/Rejected branches, order_tracker.hpp's
// drain_reconcile_events() is_exchange_final branch all pair track()/remove() 1:1 with
// InFlightRegistry's own resolve calls, so in practice they move together) -- but relying on two
// independently-queried tables to stay in lockstep on every call is a needless hazard when
// find() returning nullptr is already the correct, self-contained signal for "nothing to fold
// in, most likely a late/reordered network delivery for an order this process already finished
// with". That miss is NOT an error: silently skip the fold-in (still write the audit record,
// still using in_flight for gating whether an event is worth recording at all -- unchanged from
// the original narrower slice).
//
// Cross-mechanism dedup: OrderFillContext::consume_delta() is the SAME table
// live_submit_orchestrator.hpp's direct-fill branch and order_tracker.hpp's
// drain_reconcile_events() now also route through (see those files' own comments) -- so a fill
// this function observes here can never be double-applied by either of the other two paths, and
// vice versa, regardless of which of the three fires first for a given order.
//
// `barrier` (private-stream migration M3a, trailing default nullptr -- omitted, nothing below changes) is the
// ReconnectBarrier of the stream that produced `events`. Two things happen before an event is looked at:
//   * a serverShutdown / eventStreamTerminated event tells the barrier its generation is OVER (on_generation_ended),
//     before the supervisor has even noticed the session stopping -- the one thing a pending event can say that the
//     session's atomics have not said yet;
//   * an event stamped with a generation OLDER than the one the barrier has Reconciled is dropped and counted
//     (admit_event): the sweep that made that generation Reconciled already reflects whatever it said. Until then
//     (Down, Sweeping) old events are merged as usual -- consume_delta() applies positive deltas only, so a late
//     duplicate is harmless.
//
// `late_fill` (D5 slice R4, trailing default nullptr -- omitted, nothing below changes) is the alarm for an order an
// operator wrote off (late_fill_alarm.hpp). An execution report for one of ITS client order ids that is not in flight
// is the "miss" this function otherwise ignores -- and the one miss that is not expected: it means the exchange has an
// order the operator's proof said it never would. The alarm sees such a report FIRST, before the barrier's stale-
// generation filter: that filter's reason ("the sweep already reflects whatever it said") does not hold for an order
// nothing sweeps, so a report of an older generation must still raise it. What the alarm does with the report is its
// own business (it trips the breaker and writes its own audit records); the report then takes the same path as any
// untracked one -- no fold-in, no ordinary UserDataStreamEventObserved record (the alarm wrote its own), no position
// effect.
//
// `durable` (private-stream migration M4, trailing default nullptr -- omitted, everything above is byte-identical to before) switches the drain to
// DURABLE-FIRST publication of execution reports (design 6.3): a report is parsed, merged with any other report about the same order in this drain, planned
// against the order's DurableShadow (order_durable_path.hpp) and written through the publisher's port, and only after the frame's ACK does it fold into
// PositionTruth (consume_delta()) and reach the audit ring -- as the very record that went to the log, cumulative fill and all, not the delta record of the
// memory-first path. What the log cannot take is REFUSED, never guessed: a status no mapping names, a fill that does not parse, an in-flight order the fill
// context does not hold, a side that disagrees with the one recorded at submit time, a report that contradicts the log or the other report held for its
// order. Each refusal is counted, leaves a no-fill trace in the ring, and tells the barrier (on_event_refused(): a soft barrier, so a REST sweep reads the
// truth). Control events (serverShutdown / eventStreamTerminated) and the late-fill alarm work exactly as above, in either mode. While the publisher holds
// an observation that is not durable the drain pops nothing (head of line, as ReconcileDurablePublisher does): the ring fills, the session counts drops,
// the barrier blocks new orders. `fill_context` is where the shadows live; without it nothing can be planned and every report about an in-flight order is
// refused as untracked.
inline void drain_user_data_events(const InFlightRegistry& in_flight, AuditRingSink* audit,
                                    UserDataWsEventRing& events, std::int64_t now_ms,
                                    PositionTruth* position_truth = nullptr,
                                    OrderFillContext* fill_context = nullptr,
                                    ReconnectBarrier* barrier = nullptr,
                                    LateFillAlarm* late_fill = nullptr,
                                    UserDataDurablePublisher* durable = nullptr) noexcept {
    const UserDataFlushEnv flush_env{audit, position_truth, fill_context, barrier};
    if (durable != nullptr && durable->blocked()) {
        // An observation was taken from the ring in an earlier call and is not durable yet: it goes first, and nothing is popped behind it.
        if (fill_context != nullptr) durable->flush(flush_env, now_ms);
        if (durable->blocked()) return;
    }
    UserDataWsEvent ev{};
    while (events.try_pop(ev)) {
        if (late_fill != nullptr && ev.kind == UserDataEventKind::ExecutionReport && late_fill->watches(ev.coid.view()) &&
            !in_flight.is_in_flight(ev.coid.view())) {
            late_fill->on_execution_report(
                ev.coid.view(),
                LateFillReport{ev.exchange_order_id, static_cast<std::uint8_t>(ev.exec_type), ev.order_status, ev.order_status_known}, audit,
                now_ms);
        }
        if (barrier != nullptr) {
            if (ev.kind == UserDataEventKind::ServerShutdown || ev.kind == UserDataEventKind::EventStreamTerminated) {
                barrier->on_generation_ended(ev.generation);
            }
            if (!barrier->admit_event(ev.generation)) continue;
        }
        if (ev.kind != UserDataEventKind::ExecutionReport) continue;
        if (!in_flight.is_in_flight(ev.coid.view())) continue;

        if (durable != nullptr) {
            // DURABLE-FIRST intake (M4): nothing below this block runs for the report; it is parsed and held, and flush() at the end of the call makes it
            // durable before it reaches memory. Every way of not holding it is a refusal: counted, traced in the ring with no fill credited, and told to the
            // barrier.
            const OrderFillContextEntry* tracked = fill_context != nullptr ? fill_context->find(ev.coid.view()) : nullptr;
            const auto refuse = [&](UserDataDurablePublisher::Refusal why) noexcept {
                durable->note_refusal(why);
                if (barrier != nullptr) barrier->on_event_refused();
                if (audit != nullptr) audit->append(detail::make_user_data_memory_record(ev, tracked, 0, 0, now_ms));
            };
            if (tracked == nullptr) {
                refuse(UserDataDurablePublisher::Refusal::Untracked);
                continue;
            }
            if (ev.side_known && ev.side != tracked->side) {
                refuse(UserDataDurablePublisher::Refusal::Contradiction);  // never act on a side we did not record at submit time
                continue;
            }
            HeldUserDataObservation held{};
            held.ev = ev;
            if (!ev.order_status_known ||
                !parse_decimal_to_ticks_with_scale(std::string_view(ev.cumulative_filled_qty_raw), tracked->qty_scale, held.cumulative_filled_qty_ticks)) {
                refuse(UserDataDurablePublisher::Refusal::Unreadable);
                continue;
            }
            held.avg_fill_price_ticks = detail::derive_avg_fill_price_ticks(ev, *tracked, held.cumulative_filled_qty_ticks);
            switch (durable->stage(held, tracked->intended_qty_ticks)) {
                case UserDataDurablePublisher::Stage::Held:
                case UserDataDurablePublisher::Stage::Replaced:
                case UserDataDurablePublisher::Stage::DroppedOlder:
                    break;
                case UserDataDurablePublisher::Stage::Contradiction:
                case UserDataDurablePublisher::Stage::Full:
                    refuse(UserDataDurablePublisher::Refusal::Contradiction);
                    break;
            }
            continue;
        }

        std::int64_t safe_delta_qty_ticks = 0;
        std::int64_t avg_fill_price_ticks = 0;
        const OrderFillContextEntry* fctx_entry =
            fill_context ? fill_context->find(ev.coid.view()) : nullptr;

        // Suspicious-but-not-fatal: a side reported by the exchange that disagrees with what
        // this process recorded locally at submit time. Skip the fold-in (never apply_fill()
        // with a side we can't trust) but still write the audit record below -- the mismatch
        // itself is evidence worth keeping, not a reason to stop observing the stream.
        const bool side_mismatch =
            fctx_entry != nullptr && ev.side_known && ev.side != fctx_entry->side;

        if (fctx_entry != nullptr && !side_mismatch) {
            std::int64_t cumulative_filled_ticks = 0;
            const bool have_filled = parse_decimal_to_ticks_with_scale(
                std::string_view(ev.cumulative_filled_qty_raw), fctx_entry->qty_scale,
                cumulative_filled_ticks);
            if (have_filled) {
                const std::int64_t delta =
                    fill_context->consume_delta(ev.coid.view(), cumulative_filled_ticks);
                if (delta > 0) {
                    safe_delta_qty_ticks = delta;
                    if (position_truth != nullptr) {
                        position_truth->apply_fill(fctx_entry->symbol_id, fctx_entry->side,
                                                    safe_delta_qty_ticks);
                    }
                }

                // avg_fill_price_ticks: a derived value only, never itself gates the fold-in
                // above. checked_scaled_mul_div() already returns false (out left untouched,
                // stays 0) for a non-positive divisor -- exactly what cumulative_filled_ticks
                // is on a NEW/CANCELED/REJECTED event ("z"="0.00000000") -- so no separate
                // zero-check is needed there; derive_avg_fill_price_ticks() just checks the bool
                // return, same discipline parse_order_query_response() (binance_private_rest.hpp)
                // already applies to the identical formula on the REST path.
                avg_fill_price_ticks = detail::derive_avg_fill_price_ticks(ev, *fctx_entry, cumulative_filled_ticks);
            }
        }

        if (!audit) continue;
        audit->append(detail::make_user_data_memory_record(ev, fctx_entry, safe_delta_qty_ticks, avg_fill_price_ticks, now_ms));
    }
    // M4: what the loop staged is made durable now, in the order it was first held (and stops at the first append that is not acknowledged).
    if (durable != nullptr && fill_context != nullptr) durable->flush(flush_env, now_ms);
}

}  // namespace hy
