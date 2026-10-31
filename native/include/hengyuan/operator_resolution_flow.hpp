// SPDX-License-Identifier: proprietary
// operator_resolution_flow.hpp -- the offline operator tool's procedure, without its transport (D5 slice R3a).
//
// Governance: offline tool only (it allocates; nothing in the trading process includes it -- test_operator_resolution_guard.cpp
// pins that). docs/OPERATOR_RESOLUTION_DESIGN.md 6 describes what an operator does with an order the log cannot close:
// stop the process, take the store's exclusive lock, wait out the fence, read the exchange, look at everything, type a phrase,
// let the tool append ONE frame. This header is that procedure as code over injected ports (operator_resolution_ports.hpp):
// the stores arrive already open (opening the audit sink in OperatorOpen mode IS taking the lock), the exchange, the console,
// the evidence directory and the sleep are interfaces. Slice R3a proves it against a loopback fake exchange; slice R3b writes
// the real ports and the command line around it.
//
// The order of events, and why:
//   begin_operator_session()  the FIRST serverTime read after the lock is taken is the fence reference (design 5.2). It belongs
//                             to the lock, not to an order, so one session can resolve several orders and only the first
//                             one waits.
//   resolve_order()
//     1. refuse what is knowable without the exchange (an unknown or already-resolved order, a DryRun order, an order the
//        shared rule refuses, a full resolved-table, a symbol the control-plane log cannot name -- or cannot show it had named
//        before the order was sent, symbol_for_order()): no traffic, no wait;
//     2. ask the exchange which account the API key belongs to and refuse unless it is the uid the operator stated (a wrong key or
//        a wrong environment would make every answer below an answer about somebody else's orders; this is not evidence and costs
//        no waiting), then wait until the exchange clock says lock + W + B + margin + L, re-reading it (never trusting the local
//        clock for the fence itself); give up if the exchange clock does not keep up;
//     3. read the evidence -- the point query, every allOrders window of the plan, openOrders -- all of it after that moment;
//     4. put the raw answers into the evidence file and store it BEFORE anything else happens;
//     5. ask conclude_absent() whether the evidence is complete and clean, without the operator's word: if it is not, say why
//        and stop (nobody is asked to confirm a refusal);
//     6. show everything and ask for the phrase (operator_resolution_confirmation.hpp): one line, exactly right, or nothing;
//     7. ask conclude_absent() again, now with the confirmation, build the frame from the CHECKPOINT (never from anything the
//        operator typed) and hand it to DurableAuditSink::append_operator_resolution(), which applies the recovery reader's own
//        rules once more.
// Nothing writes to a log before step 7, and a refusal at any step leaves both logs byte for byte as they were.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <hengyuan/control_plane_log_sink.hpp>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/operator_resolution_bundle.hpp>
#include <hengyuan/operator_resolution_confirmation.hpp>
#include <hengyuan/operator_resolution_evidence.hpp>
#include <hengyuan/operator_resolution_ports.hpp>
#include <hengyuan/operator_resolution_response.hpp>
#include <hengyuan/operator_resolution_rules.hpp>

namespace hy::operator_resolution {

inline constexpr std::uint16_t kOperatorToolVersion = 1;
// One sleep never exceeds this, so the exchange clock is read again at least this often while the fence runs.
inline constexpr std::int64_t kFenceWaitStepMs = 30'000;
// A sleep is the time still missing PLUS this much (up to the step above). Without it a clock that runs a little slow leaves a
// remainder that shrinks by a constant factor with every sleep and, in whole milliseconds, ends in a sleep that moves it by
// nothing: the wait would spin on the last millisecond until it timed out. Overshooting the fence by this much is harmless.
inline constexpr std::int64_t kFenceWaitPadMs = 250;
// The wait gives up after the time the fence needs plus this much of local sleeping: the exchange clock has to keep up.
inline constexpr std::int64_t kFenceWaitSlackMs = 120'000;
inline constexpr std::chrono::milliseconds kDefaultConfirmTimeout{std::chrono::minutes{5}};

// --- the session -----------------------------------------------------------------------------------------------------------------

enum class SessionStatus : std::uint8_t {
    NotStarted = 0,  // the zero value is not a session
    Ready = 1,
    BadFenceParams = 2,
    AuditNotUsable = 3,  // not open, fenced, or not opened in OperatorOpen mode
    ServerTimeUnavailable = 4,
};

struct OperatorSession {
    SessionStatus status{SessionStatus::NotStarted};  // only Ready may be used
    DurableAuditSink* audit{nullptr};
    const ControlPlaneLogSink* names{nullptr};
    const OperatorPorts* ports{nullptr};
    FenceParams fence{};
    ExchangeMs lock_ref{};  // the first serverTime read after the lock was taken
};

// Call it right after the stores are open (the audit sink, opened in OperatorOpen mode, holds the exclusive lock for as long as
// it lives; the session is valid only while it does). The reading it takes is the fence reference: the later it is taken, the
// longer the fence, never the shorter.
inline OperatorSession begin_operator_session(DurableAuditSink& audit, const ControlPlaneLogSink& names, const OperatorPorts& ports,
                                              const FenceParams& fence) {
    OperatorSession s{};
    if (validate_fence_params(fence) != FenceParamsStatus::Ok) {
        s.status = SessionStatus::BadFenceParams;
        return s;
    }
    if (!audit.is_open() || audit.fenced() || audit.open_mode() != AuditSinkOpenMode::OperatorOpen) {
        s.status = SessionStatus::AuditNotUsable;
        return s;
    }
    const std::optional<ExchangeMs> t = ports.exchange.server_time();
    if (!t || !is_plausible(*t)) {
        s.status = SessionStatus::ServerTimeUnavailable;
        return s;
    }
    s.audit = &audit;
    s.names = &names;
    s.ports = &ports;
    s.fence = fence;
    s.lock_ref = *t;
    s.status = SessionStatus::Ready;
    return s;
}

// --- the symbol of an order -----------------------------------------------------------------------------------------------------------

// The send-start time the audit log recorded for the order, as an anchor: the Prepared / Submitting frame's time, when that frame
// carried a wall time (the checkpoint's send_started_utc_ms is 0 otherwise).
inline std::optional<ExchangeMs> frame_anchor_of(const OrderRecoveryCheckpoint& cp) noexcept {
    return cp.send_started_utc_ms != 0 ? std::optional<ExchangeMs>(ExchangeMs{cp.send_started_utc_ms}) : std::nullopt;
}

// D5 / R3b-3 (blueprint L-50): what the control-plane log says the order's symbol_id stands for, as far as it can be trusted for
// THIS order. ControlPlaneLogSink::recovered_symbol_name() answers from the log alone, and cannot know whether it is the log that
// was in use when the order was placed: had it been deleted and recreated since, an old id may have been given another symbol and
// still be Known, and the tool would ask the exchange about the wrong one -- "no such order" is true for a wrong symbol. The cheap
// extra defence: a name only counts for an order sent AT OR AFTER the first snapshot that gave it (the log's own record of when
// it began to know the name). The order's send time is the EARLIEST anchor there is (earliest_send_anchor(): the frame's time and
// the client order id's timestamp, whichever is earlier); the snapshot's time is a wall time on the exchange's clock, the id's is
// the local clock, at most B off it, so an order sent within B of the first snapshot may be refused wrongly -- the accepted side of
// the error, since a refusal sends the operator to the manual path and the other error resolves an order against the wrong symbol.
// No margin is added in the permissive direction. A name with no usable time (a frame without a wall time, or an implausible one)
// is refused too, for any order that has an anchor. An order with no anchor at all is not judged here: the range check (step 2 of
// resolve_order()) refuses it before any request, and the refusal precedence stays what it was.
// Unknown, Conflict and every other answer pass through unchanged; NotNamedAtOrderTime hands out no name. THE one place that asks
// the sink for a name: resolve_order() and list_unresolved_orders() both come here (test_operator_resolution_guard.cpp pins that),
// so the listing can never say "can be resolved" for an order the resolution would refuse for its symbol.
inline SymbolNameLookup symbol_for_order(const ControlPlaneLogSink& names, const OrderRecoveryCheckpoint& cp) noexcept {
    SymbolNameLookup lookup = names.recovered_symbol_name(cp.symbol_id);
    if (lookup.status != SymbolNameStatus::Known) return lookup;
    const std::optional<ExchangeMs> sent = earliest_send_anchor(frame_anchor_of(cp), cp.client_order_id.view());
    if (!sent) return lookup;
    const ExchangeMs named{lookup.named_since_ms};
    if (!is_plausible(named) || *sent < named) {
        lookup.status = SymbolNameStatus::NotNamedAtOrderTime;
        lookup.name = {};
    }
    return lookup;
}

// --- listing ------------------------------------------------------------------------------------------------------------------------

// One unresolved order as the tool lists it (design 6 step 3), with whether this tool could resolve it and why not.
struct UnresolvedOrderSummary {
    std::size_t checkpoint_index{0};
    std::string client_order_id;
    OrderState state{OrderState::Intent};
    ExecutionMode mode{ExecutionMode::DryRun};
    std::uint32_t symbol_id{0};
    SymbolNameStatus symbol_status{SymbolNameStatus::Unknown};
    std::int64_t symbol_named_since_ms{0};  // when the log first named the id; set when symbol_status is Known or NotNamedAtOrderTime
    std::string symbol;  // empty unless symbol_status == Known
    OrderSide side{OrderSide::Buy};
    std::int64_t price_ticks{0};
    std::int64_t qty_ticks{0};
    std::int64_t send_started_utc_ms{0};
    ResolutionCheck order_check{ResolutionCheck::NotChecked};  // Ok = the order itself may be closed this way
    bool resolved_this_session{false};
};

inline std::vector<UnresolvedOrderSummary> list_unresolved_orders(const DurableAuditSink& audit, const ControlPlaneLogSink& names) {
    std::vector<UnresolvedOrderSummary> out;
    const auto checkpoints = audit.recovered_checkpoints();
    out.reserve(checkpoints.size());
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
        const OrderRecoveryCheckpoint& cp = checkpoints[i];
        UnresolvedOrderSummary s{};
        s.checkpoint_index = i;
        s.client_order_id = std::string(cp.client_order_id.view());
        s.state = cp.resulting_state;
        s.mode = cp.mode;
        s.symbol_id = cp.symbol_id;
        const SymbolNameLookup lookup = symbol_for_order(names, cp);
        s.symbol_status = lookup.status;
        s.symbol_named_since_ms = lookup.named_since_ms;
        if (lookup.status == SymbolNameStatus::Known) s.symbol = std::string(lookup.name);
        s.side = cp.side;
        s.price_ticks = cp.intended_price_ticks;
        s.qty_ticks = cp.intended_qty_ticks;
        s.send_started_utc_ms = cp.send_started_utc_ms;
        s.order_check = check_order_resolvable(target_of(cp));
        s.resolved_this_session = audit.operator_resolved_this_session(i);
        out.push_back(std::move(s));
    }
    return out;
}

// --- one order -------------------------------------------------------------------------------------------------------------------------

struct OperatorResolutionRequest {
    std::string_view client_order_id;
    OperatorEnvironment environment{static_cast<OperatorEnvironment>(255)};  // what the operator says; the log cannot confirm it (L-48)
    std::uint64_t account_uid{0};  // what the operator says; resolve_order() checks it against what the exchange reports for the key
    std::string_view operator_label;  // goes into the frame's detail_msg as "op=<label>"
    std::uint16_t tool_version{kOperatorToolVersion};
    std::chrono::milliseconds confirm_timeout{kDefaultConfirmTimeout};
};

enum class ResolveStatus : std::uint8_t {
    NotRun = 0,  // the zero value is not a success
    Resolved = 1,  // the OrderOperatorResolved frame is durable and acknowledged
    BadRequest = 2,
    SessionNotReady = 3,
    AuditNotUsable = 4,
    NoSuchUnresolvedOrder = 5,
    AlreadyResolvedThisSession = 6,
    DryRunOrder = 7,
    NotResolvable = 8,       // the shared rule refuses the order itself: see OperatorResolutionOutcome::order_check
    TableFull = 9,           // the store already holds the most operator-resolved orders a trading start can load
    SymbolNameNotKnown = 10,  // the control-plane log does not name the order's symbol_id, names it inconsistently, or not before the order was sent (symbol_status says which)
    ServerTimeUnavailable = 11,
    FenceWaitTimedOut = 12,
    NoUsableRange = 13,  // no anchor, an implausible time, or more than 14 allOrders windows: resolve by hand
    EvidenceReadFailed = 14,
    EvidenceNotAbsent = 15,  // the evidence is incomplete or says the order exists: see OperatorResolutionOutcome::conclusion
    EvidenceStoreFailed = 16,
    NotConfirmed = 17,
    AppendRefused = 18,
    AppendFailed = 19,
    AccountUnverified = 20,   // the exchange did not report the uid of the account the key belongs to (request failed, no single uid)
    AccountUidMismatch = 21,  // it did, and it is not the uid the operator stated: see OperatorResolutionOutcome::exchange_account_uid
};

constexpr std::string_view resolve_status_name(ResolveStatus s) noexcept {
    switch (s) {
        case ResolveStatus::NotRun: return "NotRun";
        case ResolveStatus::Resolved: return "Resolved";
        case ResolveStatus::BadRequest: return "BadRequest";
        case ResolveStatus::SessionNotReady: return "SessionNotReady";
        case ResolveStatus::AuditNotUsable: return "AuditNotUsable";
        case ResolveStatus::NoSuchUnresolvedOrder: return "NoSuchUnresolvedOrder";
        case ResolveStatus::AlreadyResolvedThisSession: return "AlreadyResolvedThisSession";
        case ResolveStatus::DryRunOrder: return "DryRunOrder";
        case ResolveStatus::NotResolvable: return "NotResolvable";
        case ResolveStatus::TableFull: return "TableFull";
        case ResolveStatus::SymbolNameNotKnown: return "SymbolNameNotKnown";
        case ResolveStatus::ServerTimeUnavailable: return "ServerTimeUnavailable";
        case ResolveStatus::FenceWaitTimedOut: return "FenceWaitTimedOut";
        case ResolveStatus::NoUsableRange: return "NoUsableRange";
        case ResolveStatus::EvidenceReadFailed: return "EvidenceReadFailed";
        case ResolveStatus::EvidenceNotAbsent: return "EvidenceNotAbsent";
        case ResolveStatus::EvidenceStoreFailed: return "EvidenceStoreFailed";
        case ResolveStatus::NotConfirmed: return "NotConfirmed";
        case ResolveStatus::AppendRefused: return "AppendRefused";
        case ResolveStatus::AppendFailed: return "AppendFailed";
        case ResolveStatus::AccountUnverified: return "AccountUnverified";
        case ResolveStatus::AccountUidMismatch: return "AccountUidMismatch";
    }
    return "Unknown";
}

constexpr std::string_view conclusion_name(AbsentConclusionStatus s) noexcept {
    switch (s) {
        case AbsentConclusionStatus::NotEvaluated: return "NotEvaluated";
        case AbsentConclusionStatus::Absent: return "Absent";
        case AbsentConclusionStatus::LockNotHeld: return "LockNotHeld";
        case AbsentConclusionStatus::FenceNotSatisfied: return "FenceNotSatisfied";
        case AbsentConclusionStatus::InvalidFenceParams: return "InvalidFenceParams";
        case AbsentConclusionStatus::PointQueryNotAbsent: return "PointQueryNotAbsent";
        case AbsentConclusionStatus::NoUsableRange: return "NoUsableRange";
        case AbsentConclusionStatus::RangeNotPlannable: return "RangeNotPlannable";
        case AbsentConclusionStatus::AllOrdersIncomplete: return "AllOrdersIncomplete";
        case AbsentConclusionStatus::AllOrdersNotClear: return "AllOrdersNotClear";
        case AbsentConclusionStatus::OpenOrdersNotClear: return "OpenOrdersNotClear";
        case AbsentConclusionStatus::NotConfirmed: return "NotConfirmed";
    }
    return "Unknown";
}

struct OperatorResolutionOutcome {
    ResolveStatus status{ResolveStatus::NotRun};
    ResolutionCheck order_check{ResolutionCheck::NotChecked};
    SymbolNameStatus symbol_status{SymbolNameStatus::Unknown};
    std::int64_t symbol_named_since_ms{0};  // when the log first named the id; set when symbol_status is Known or NotNamedAtOrderTime
    FenceVerdict fence{FenceVerdict::Unset};
    RangeStatus range{RangeStatus::Unset};
    PlanStatus plan{PlanStatus::Unset};
    AbsentConclusionStatus conclusion{AbsentConclusionStatus::NotEvaluated};
    ConfirmationResult confirmation{};
    OperatorResolutionAppend append{};
    ExchangeMs lock_ref{};
    ExchangeMs read_ref{};
    std::optional<std::uint64_t> exchange_account_uid;  // what the exchange reported for the key's account; unset if it was not asked or did not say
    std::string evidence_digest_hex;  // empty unless an evidence file was built
    std::string evidence_stem;        // the name handed to the evidence store
};

namespace detail {

// A file name stem from a client order id and a time: only [A-Za-z0-9._-] (the exchange allows '/' and ':' in an id; neither
// belongs in a file name).
inline std::string evidence_stem(std::string_view coid, std::int64_t read_ref_ms) {
    std::string stem;
    stem.reserve(coid.size() + 24);
    for (const char c : coid) stem.push_back((is_ascii_alnum(c) || c == '.' || c == '_' || c == '-') ? c : '_');
    stem += '-';
    stem += std::to_string(read_ref_ms);
    return stem;
}

inline BundleCall make_call(std::string name, std::string params, const HttpRead& r) {
    BundleCall c;
    c.name = std::move(name);
    c.params = std::move(params);
    c.transport_ok = r.transport_ok;
    c.http_status = r.http_status;
    c.body = r.body;
    return c;
}

}  // namespace detail

inline OperatorResolutionOutcome resolve_order(OperatorSession& session, const OperatorResolutionRequest& req) {
    OperatorResolutionOutcome out{};
    out.lock_ref = session.lock_ref;
    const auto stop = [&out](ResolveStatus s) -> OperatorResolutionOutcome {
        out.status = s;
        return out;
    };

    // 0. the request and the session
    if (session.status != SessionStatus::Ready || session.audit == nullptr || session.names == nullptr || session.ports == nullptr) {
        return stop(ResolveStatus::SessionNotReady);
    }
    if (environment_name(req.environment).empty() || req.account_uid == 0 || !is_exchange_client_order_id(req.client_order_id) ||
        !is_operator_label(req.operator_label) || req.confirm_timeout.count() <= 0) {
        return stop(ResolveStatus::BadRequest);
    }
    DurableAuditSink& audit = *session.audit;
    const ControlPlaneLogSink& names = *session.names;
    ExchangeReadPort& exchange = session.ports->exchange;
    SleepPort& sleeper = session.ports->sleeper;
    ConsolePort& console = session.ports->console;
    EvidenceStorePort& store = session.ports->evidence;

    // 1. what is knowable without the exchange
    if (!audit.is_open() || audit.fenced() || audit.open_mode() != AuditSinkOpenMode::OperatorOpen) {
        return stop(ResolveStatus::AuditNotUsable);
    }
    const auto checkpoints = audit.recovered_checkpoints();
    std::size_t index = checkpoints.size();
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
        if (checkpoints[i].client_order_id.view() == req.client_order_id) {
            index = i;
            break;
        }
    }
    if (index == checkpoints.size()) return stop(ResolveStatus::NoSuchUnresolvedOrder);
    const OrderRecoveryCheckpoint& cp = checkpoints[index];
    if (audit.operator_resolved_this_session(index)) return stop(ResolveStatus::AlreadyResolvedThisSession);
    if (cp.mode != ExecutionMode::Live) return stop(ResolveStatus::DryRunOrder);
    out.order_check = check_order_resolvable(target_of(cp));
    if (out.order_check != ResolutionCheck::Ok) return stop(ResolveStatus::NotResolvable);
    if (!audit.operator_resolved_table_has_room()) return stop(ResolveStatus::TableFull);
    const SymbolNameLookup lookup = symbol_for_order(names, cp);
    out.symbol_status = lookup.status;
    out.symbol_named_since_ms = lookup.named_since_ms;
    if (lookup.status != SymbolNameStatus::Known || !is_exchange_symbol(lookup.name)) {
        return stop(ResolveStatus::SymbolNameNotKnown);
    }
    const std::string symbol(lookup.name);
    const std::string_view coid = req.client_order_id;
    const std::optional<ExchangeMs> frame_anchor = frame_anchor_of(cp);

    // 2. the fence, counted on the exchange clock
    const std::optional<ExchangeMs> earliest = earliest_evidence_read(session.lock_ref, session.fence);
    if (!earliest) return stop(ResolveStatus::SessionNotReady);
    // An interval that cannot be covered even by the EARLIEST possible read (no anchor at all, or more than 14 windows) can only
    // get longer while we wait: say so now instead of after four and a half minutes. (The real interval is computed again from
    // the real read_ref below.)
    {
        const AllOrdersRange earliest_range = all_orders_range(frame_anchor, coid, *earliest, session.fence);
        out.range = earliest_range.status;
        if (earliest_range.status != RangeStatus::Ok) return stop(ResolveStatus::NoUsableRange);
        out.plan = plan_all_orders_windows(earliest_range.start, earliest_range.end).status;
        if (out.plan != PlanStatus::Ok) return stop(ResolveStatus::NoUsableRange);
    }
    // Which account does this key belong to? The operator typed a uid; the exchange says which one the key reads. They must agree before
    // anything is waited for or read as evidence: with a wrong key or a wrong environment, every answer below would be an answer about
    // somebody else's orders. It is a single request, it is not evidence (conclude_absent() never sees it) and it comes after every
    // refusal that needs no traffic. The summary it brings is what the operator is shown later; the uid is also written into the evidence.
    const AccountView account = exchange.account();
    out.exchange_account_uid = account.uid;
    if (!account.uid) {
        console.write_line("the exchange did not report the uid of the account this API key belongs to: nothing was written");
        return stop(ResolveStatus::AccountUnverified);
    }
    if (*account.uid != req.account_uid) {
        console.write_line("this API key belongs to account uid " + std::to_string(*account.uid) + ", not to the uid " +
                           std::to_string(req.account_uid) + " you stated: nothing was written");
        return stop(ResolveStatus::AccountUidMismatch);
    }
    const std::int64_t wait_cap_ms = (earliest->count() - session.lock_ref.count()) + kFenceWaitSlackMs;  // both plausible: no overflow
    std::vector<BundleTimeReading> readings;
    readings.push_back({"lock", session.lock_ref.count()});
    std::int64_t slept_ms = 0;
    ExchangeMs read_ref{};
    for (;;) {
        const std::optional<ExchangeMs> now = exchange.server_time();
        if (!now) return stop(ResolveStatus::ServerTimeUnavailable);
        // (an implausible reading is refused by the verdict below, FenceVerdict::ImplausibleTime, and reported as such)
        const FenceVerdict verdict = check_evidence_read(session.lock_ref, *now, session.fence);
        out.fence = verdict;
        if (verdict == FenceVerdict::Satisfied) {
            read_ref = *now;
            readings.push_back({"read", now->count()});
            break;
        }
        if (verdict != FenceVerdict::TooEarly) return stop(ResolveStatus::ServerTimeUnavailable);  // the clock went backwards, ...
        readings.push_back({"wait", now->count()});
        const std::int64_t remaining_ms = earliest->count() - now->count();  // > 0: the reading is too early
        const std::int64_t step_ms = std::min(remaining_ms + kFenceWaitPadMs, kFenceWaitStepMs);
        if (slept_ms + step_ms > wait_cap_ms) return stop(ResolveStatus::FenceWaitTimedOut);
        console.write_line("waiting for the fence: " + std::to_string((remaining_ms + 999) / 1000) + " s of exchange time to go");
        sleeper.sleep_for(std::chrono::milliseconds{step_ms});
        slept_ms += step_ms;
    }
    out.read_ref = read_ref;

    // 3. the evidence, every request after read_ref
    // The same inputs as the early check above, except that the read time is the real one -- later, so the interval is longer. Only
    // the plan can now fail (more windows than the tool will walk); a range that was Ok at the earliest read is Ok at a later one.
    // (An unusable range has start = end = 0, which no plan accepts, so this one check would refuse that too.)
    const AllOrdersRange range = all_orders_range(frame_anchor, coid, read_ref, session.fence);
    out.range = range.status;
    const AllOrdersPlan plan = plan_all_orders_windows(range.start, range.end);
    out.plan = plan.status;
    if (plan.status != PlanStatus::Ok) return stop(ResolveStatus::NoUsableRange);

    std::vector<BundleCall> calls;
    BundleVerdicts verdicts;
    {
        const HttpRead r = exchange.query_order(symbol, coid);
        calls.push_back(detail::make_call("order", "symbol=" + symbol + "&origClientOrderId=" + std::string(coid), r));
        if (!r.transport_ok) return stop(ResolveStatus::EvidenceReadFailed);
        verdicts.point_query = classify_point_query(r.http_status, r.body);
    }
    for (std::size_t i = 0; i < plan.count; ++i) {
        const AllOrdersWindow& w = plan.windows[i];
        const HttpRead r = exchange.all_orders(symbol, w.start, w.end, kAllOrdersPageLimit);
        calls.push_back(detail::make_call("allOrders",
                                          "symbol=" + symbol + "&startTime=" + std::to_string(w.start.count()) +
                                              "&endTime=" + std::to_string(w.end.count()) +
                                              "&limit=" + std::to_string(kAllOrdersPageLimit),
                                          r));
        if (!r.transport_ok) return stop(ResolveStatus::EvidenceReadFailed);
        verdicts.all_orders.push_back(classify_all_orders_page(r.http_status, r.body, coid, symbol));
    }
    {
        const HttpRead r = exchange.open_orders();
        calls.push_back(detail::make_call("openOrders", "", r));
        if (!r.transport_ok) return stop(ResolveStatus::EvidenceReadFailed);
        verdicts.open_orders = classify_open_orders_page(r.http_status, r.body, coid);
    }

    // 4. the conclusion without the operator's word, and the evidence file
    AbsentConclusionInput in{};
    in.client_order_id = coid;
    in.frame_anchor = frame_anchor;
    in.lock_ref = session.lock_ref;
    in.read_ref = read_ref;
    in.fence = session.fence;
    in.point_query = verdicts.point_query;
    in.all_orders_pages = std::span<const PageVerdict>(verdicts.all_orders.data(), verdicts.all_orders.size());
    in.open_orders = verdicts.open_orders;
    in.lock_held = true;  // the sink is open in OperatorOpen mode and not fenced: this process holds the store's exclusive lock
    in.operator_confirmed = false;
    in.tool_version = req.tool_version;
    const AbsentConclusion pre = conclude_absent(in);
    out.conclusion = pre.status;

    BundleHeader header{};
    header.tool_version = req.tool_version;
    header.client_order_id = coid;
    header.symbol = symbol;
    header.environment = req.environment;
    header.account_uid_claimed = req.account_uid;
    header.account_uid_reported = account.uid;
    header.fence = session.fence;
    header.lock_ref = session.lock_ref;
    header.read_ref = read_ref;
    header.frame_anchor = frame_anchor;
    header.range_start = range.start;
    header.range_end = range.end;
    header.time_readings = std::move(readings);
    header.account_summary = account.summary;
    const std::string bundle = build_evidence_bundle(header, calls, verdicts);
    out.evidence_digest_hex = sha256_hex(bundle);
    out.evidence_stem = detail::evidence_stem(coid, read_ref.count());
    const bool stored = store.store(out.evidence_stem, bundle);

    // 5. a refusal is reported with its evidence and nobody is asked to confirm it
    if (pre.status != AbsentConclusionStatus::NotConfirmed) {
        console.write_line(std::string("the evidence does not show that the exchange has no such order: ") +
                           std::string(conclusion_name(pre.status)));
        console.write_line(std::string("point query: ") + std::string(verdict_name(verdicts.point_query)));
        for (std::size_t i = 0; i < verdicts.all_orders.size(); ++i) {
            console.write_line("allOrders page " + std::to_string(i + 1) + ": " + std::string(verdict_name(verdicts.all_orders[i])));
        }
        console.write_line(std::string("openOrders: ") + std::string(verdict_name(verdicts.open_orders)));
        console.write_line(stored ? "evidence file: " + out.evidence_stem : std::string("the evidence file could not be stored"));
        return stop(ResolveStatus::EvidenceNotAbsent);
    }
    if (!stored) return stop(ResolveStatus::EvidenceStoreFailed);

    // 6. show everything, ask for the phrase
    console.write_line("== operator resolution: order " + std::string(coid) + " ==");
    console.write_line("symbol: " + symbol + " (id " + std::to_string(cp.symbol_id) + ")  side: " +
                       (cp.side == OrderSide::Buy ? "Buy" : "Sell") + "  quantity: " + std::to_string(cp.intended_qty_ticks) +
                       " ticks  price: " + std::to_string(cp.intended_price_ticks) + " ticks  mode: Live");
    console.write_line("state in the log: " + std::string(order_state_name(cp.resulting_state)) + "  send started (log time, ms): " +
                       (cp.send_started_utc_ms != 0 ? std::to_string(cp.send_started_utc_ms) : std::string("unknown")));
    console.write_line("environment (stated by you): " + std::string(environment_name(req.environment)) +
                       "  account uid (stated by you; the exchange reports the same for this API key): " + std::to_string(req.account_uid));
    console.write_line("exchange clock: lock " + std::to_string(session.lock_ref.count()) + "  read " + std::to_string(read_ref.count()) +
                       "  waited " + std::to_string(waited_seconds(session.lock_ref, read_ref)) + " s  (W " +
                       std::to_string(session.fence.recv_window_ms / 1000) + " s, B " + std::to_string(session.fence.clock_skew_ms / 1000) +
                       " s, margin " + std::to_string(session.fence.margin_ms / 1000) + " s, read lag L " +
                       std::to_string(session.fence.read_lag_ms / 1000) + " s)");
    console.write_line("point query: " + std::string(verdict_name(verdicts.point_query)));
    console.write_line("allOrders: " + std::to_string(verdicts.all_orders.size()) + " window(s) from " +
                       std::to_string(range.start.count()) + " to " + std::to_string(range.end.count()) + ", all Clear");
    console.write_line("openOrders: " + std::string(verdict_name(verdicts.open_orders)));
    if (account.summary.empty()) {
        console.write_line("account summary (display only): not available");
    } else {
        console.write_line("account summary (display only): " + account.summary);
    }
    console.write_line("If you confirm, the order is closed as Rejected: the exchange has no such order and never will.");
    console.write_line("This tool cannot check, so YOU must have:");
    console.write_line("  A-D5-1 stopped the old process for good (its log handle is closed, it can send nothing more);");
    console.write_line("  A-D5-2 made sure no other process or host sends orders with the same API credentials;");
    console.write_line("  A-D5-3 pointed this tool at the same exchange environment and account the order was sent to;");
    console.write_line("  A-D5-4 checked that the symbol above is the one this order was for;");
    console.write_line("  and looked in the exchange's own order and trade history for that window: nothing for this order.");
    console.write_line("evidence file: " + out.evidence_stem + "  sha256: " + out.evidence_digest_hex);

    ConfirmationChallenge challenge{};
    challenge.client_order_id = coid;
    challenge.symbol = symbol;
    challenge.environment = req.environment;
    challenge.account_uid = req.account_uid;
    challenge.evidence_digest_hex = out.evidence_digest_hex;
    const ConfirmationPhrase phrase = make_confirmation_phrase(challenge);
    if (!phrase.valid) {
        out.confirmation.verdict = ConfirmationVerdict::InvalidPhrase;
        return stop(ResolveStatus::NotConfirmed);
    }
    console.write_line("To resolve this order as ABSENT type exactly this line and press Enter:");
    console.write_line(std::string(phrase.view()));
    const OperatorLineResult line = console.read_line(req.confirm_timeout);
    out.confirmation = judge_confirmation(phrase, line);
    if (out.confirmation.verdict != ConfirmationVerdict::Confirmed) {
        console.write_line("not confirmed: nothing was written");
        return stop(ResolveStatus::NotConfirmed);
    }

    // 7. the conclusion with the operator's word, and the frame
    in.operator_confirmed = true;
    const AbsentConclusion fin = conclude_absent(in);
    out.conclusion = fin.status;
    if (fin.status != AbsentConclusionStatus::Absent) return stop(ResolveStatus::EvidenceNotAbsent);
    const std::optional<ExchangeMs> append_ref = exchange.server_time();
    if (!append_ref || !is_plausible(*append_ref)) return stop(ResolveStatus::ServerTimeUnavailable);

    AuditRecord rec{};
    rec.timestamp_ms = append_ref->count();
    rec.event_type = AuditEventType::OrderOperatorResolved;
    rec.mode = cp.mode;
    rec.symbol_id = cp.symbol_id;
    rec.set_client_order_id(coid);
    rec.price_ticks = cp.intended_price_ticks;
    rec.qty_ticks = cp.intended_qty_ticks;
    rec.resulting_state = OrderState::Rejected;
    rec.side = cp.side;
    rec.detail_code = fin.detail_code;
    const std::string detail_msg = "op=" + std::string(req.operator_label) + ";ev=" + out.evidence_digest_hex.substr(0, 32);
    rec.set_detail(detail_msg.c_str());
    out.append = audit.append_operator_resolution(rec, append_ref->count());
    if (out.append.status == OperatorAppendStatus::Appended) {
        console.write_line("resolved: the frame is durable. Restart the trading process.");
        out.status = ResolveStatus::Resolved;
        return out;
    }
    console.write_line("the frame was not written: " + std::to_string(static_cast<int>(out.append.status)));
    return stop(out.append.status == OperatorAppendStatus::AppendFailed ? ResolveStatus::AppendFailed : ResolveStatus::AppendRefused);
}

}  // namespace hy::operator_resolution
