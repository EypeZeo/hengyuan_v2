// SPDX-License-Identifier: proprietary
// binance_user_data_event.hpp — TODO 1A.4: the event struct/ring/drain function
// live_submit_orchestrator.hpp needs to consume Binance user-data-stream WS events on the
// hot/submit thread, WITHOUT pulling in Boost.Asio/Beast/OpenSSL/simdjson.
//
// Deliberately split out of binance_user_data_ws_session.hpp, mirroring this codebase's own
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
// binance_user_data_ws_session.hpp, even though it is WS-event-only vocabulary with no REST
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
#include <hengyuan/order_fill_context.hpp>  // OrderFillContext, OrderFillContextEntry
#include <hengyuan/order_lifecycle.hpp>     // ClientOrderId, InFlightRegistry, OrderState
#include <hengyuan/position_truth.hpp>      // PositionTruth
#include <hengyuan/spsc_ring.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace hy {

enum class UserDataEventKind : std::uint8_t {
    Unknown = 0,
    ExecutionReport = 1,
    OutboundAccountPosition = 2,
    ListenKeyExpired = 3,
};

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
    std::int64_t transaction_time_ms{0};  // "T"

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
};
static_assert(std::is_trivially_copyable_v<UserDataWsEvent>,
              "UserDataWsEvent crosses the SpscRing producer/consumer boundary");

using UserDataWsEventRing = SpscRing<UserDataWsEvent, 64>;

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
inline void drain_user_data_events(const InFlightRegistry& in_flight, AuditRingSink* audit,
                                    UserDataWsEventRing& events, std::int64_t now_ms,
                                    PositionTruth* position_truth = nullptr,
                                    OrderFillContext* fill_context = nullptr) noexcept {
    UserDataWsEvent ev{};
    while (events.try_pop(ev)) {
        if (ev.kind != UserDataEventKind::ExecutionReport) continue;
        if (!in_flight.is_in_flight(ev.coid.view())) continue;

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
                // zero-check is needed here; just check the bool return, same discipline
                // parse_order_query_response() (binance_private_rest.hpp) already applies to
                // the identical formula on the REST path.
                std::int64_t cumulative_quote_ticks = 0;
                if (parse_decimal_to_ticks_with_scale(
                        std::string_view(ev.cumulative_quote_qty_raw), fctx_entry->quote_scale,
                        cumulative_quote_ticks)) {
                    const int exponent = static_cast<int>(fctx_entry->price_scale) +
                                          static_cast<int>(fctx_entry->qty_scale) -
                                          static_cast<int>(fctx_entry->quote_scale);
                    std::int64_t computed_avg_price = 0;
                    if (checked_scaled_mul_div(cumulative_quote_ticks, exponent,
                                                cumulative_filled_ticks, computed_avg_price)) {
                        avg_fill_price_ticks = computed_avg_price;
                    }
                }
            }
        }

        if (!audit) continue;

        AuditRecord ar{};
        ar.timestamp_ms = now_ms;
        ar.event_type = AuditEventType::UserDataStreamEventObserved;
        ar.exchange_order_id = ev.exchange_order_id;
        ar.set_client_order_id(ev.coid.view());
        ar.filled_qty_ticks = safe_delta_qty_ticks;
        ar.avg_fill_price_ticks = avg_fill_price_ticks;
        if (fctx_entry != nullptr) {
            ar.symbol_id = fctx_entry->symbol_id;
            ar.side = fctx_entry->side;
        }
        if (ev.order_status_known) ar.resulting_state = ev.order_status;
        audit->append(ar);
    }
}

}  // namespace hy
