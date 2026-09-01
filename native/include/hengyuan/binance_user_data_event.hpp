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

#pragma once

#include <hengyuan/audit_trail.hpp>
#include <hengyuan/order_lifecycle.hpp>  // ClientOrderId, InFlightRegistry
#include <hengyuan/spsc_ring.hpp>

#include <cstdint>
#include <type_traits>

namespace hy {

enum class UserDataEventKind : std::uint8_t {
    Unknown = 0,
    ExecutionReport = 1,
    OutboundAccountPosition = 2,
    ListenKeyExpired = 3,
};

struct UserDataWsEvent {
    UserDataEventKind kind{UserDataEventKind::Unknown};
    std::int64_t event_time_ms{0};      // Binance "E" field
    ClientOrderId coid{};                // "c" -- only meaningful when kind == ExecutionReport
    std::int64_t exchange_order_id{0};   // "i" -- only meaningful when kind == ExecutionReport
};
static_assert(std::is_trivially_copyable_v<UserDataWsEvent>,
              "UserDataWsEvent crosses the SpscRing producer/consumer boundary");

using UserDataWsEventRing = SpscRing<UserDataWsEvent, 64>;

// Runs on the hot/submit thread -- the same thread that exclusively owns InFlightRegistry/
// AuditRingSink (order_lifecycle.hpp's/order_tracker.hpp's own THREAD OWNERSHIP contract).
// Drains `events`; for each, looks the event's coid up in `in_flight` (this is the ONLY way a
// WS-pushed event can be attributed to a tracked order -- unlike drain_reconcile_events(),
// there is no prior InFlightHandle handshake, the WS thread learns about an order purely from
// Binance's own push, keyed by "c"). A hit writes a single UserDataStreamEventObserved
// AuditRecord (the only place that event type is ever emitted); a miss is silently ignored,
// not a crash/UB -- an event for an order this process isn't tracking (already resolved, or
// belongs to a different session entirely) is expected, not exceptional.
//
// Deliberately does NOT call InFlightRegistry::mark_resolved()/PositionTruth::apply_fill()
// here -- this slice only proves the event reached the hot thread and was correctly attributed
// to a tracked order; folding it into position/state belongs to a future slice (see
// binance_user_data_ws_session.hpp's own header comment on JSON parsing scope).
inline void drain_user_data_events(const InFlightRegistry& in_flight, AuditRingSink* audit,
                                    UserDataWsEventRing& events, std::int64_t now_ms) noexcept {
    UserDataWsEvent ev{};
    while (events.try_pop(ev)) {
        if (ev.kind != UserDataEventKind::ExecutionReport) continue;
        if (!in_flight.is_in_flight(ev.coid.view())) continue;
        if (!audit) continue;

        AuditRecord ar{};
        ar.timestamp_ms = now_ms;
        ar.event_type = AuditEventType::UserDataStreamEventObserved;
        ar.exchange_order_id = ev.exchange_order_id;
        ar.set_client_order_id(ev.coid.view());
        audit->append(ar);
    }
}

}  // namespace hy
