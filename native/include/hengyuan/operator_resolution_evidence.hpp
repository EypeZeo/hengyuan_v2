// SPDX-License-Identifier: proprietary
// operator_resolution_evidence.hpp -- the decisions behind the evidence for an OrderOperatorResolved frame (D5 slice R2).
//
// Governance: L1 (pure logic: no I/O, no clock, no network, no allocation, no JSON, no Boost). docs/OPERATOR_RESOLUTION_DESIGN.md
// 5 and 6 say WHEN the offline operator tool may conclude "the exchange has no such order, and never will" and WHICH reads it
// needs first. This header is that arithmetic and those rules as functions of plain values, so every refusal can be pinned by a
// table test and the tool (slice R3) only has to move bytes: it reads the exchange clock, makes the requests, hands what came
// back to operator_resolution_response.hpp (the one file that parses exchange JSON), and asks conclude_absent() whether the
// evidence is complete. Nothing here is reachable from the trading process: no production header includes this file
// (test_operator_resolution_guard.cpp pins that), and the reconciliation path keeps folding every answer that is not a positive
// match into QueryOutcome::Inconclusive, exactly as before.
//
// Everything fails CLOSED, and the types are built so that forgetting something is a refusal rather than a pass: the value 0
// of every enum that carries evidence is the "no evidence" one (PointQueryVerdict::Unusable, PageVerdict::Malformed,
// AbsentConclusionStatus::NotEvaluated, FenceVerdict::Unset ...), so a verdict nobody filled in can never count; a time outside
// the plausible range (a missing serverTime parsed as 0, a sentinel) can never satisfy a fence; and the only function that
// builds the frame's machine field (detail::make_absent_detail_code) is called by conclude_absent() alone.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include <hengyuan/operator_resolution_frame.hpp>

namespace hy::operator_resolution {

// --- exchange time ------------------------------------------------------------------------------------------------------
//
// Every time here is EXCHANGE time in milliseconds: GET /api/v3/time's serverTime, or the recorded time of a frame written
// with a ServerCorrectedUtc clock. The fence argument (design 5.2) is made on the exchange's clock and nowhere else; this type
// exists so that a local wall-clock reading cannot be passed where an exchange reading is meant without a visible conversion.
class ExchangeMs {
public:
    constexpr ExchangeMs() noexcept = default;
    constexpr explicit ExchangeMs(std::int64_t ms) noexcept : ms_(ms) {}
    constexpr std::int64_t count() const noexcept { return ms_; }
    friend constexpr auto operator<=>(const ExchangeMs&, const ExchangeMs&) noexcept = default;

private:
    std::int64_t ms_{0};
};

// The plausible range of an exchange time: 2020-01-01T00:00:00Z .. 2100-01-01T00:00:00Z. A reading outside it is garbage
// (serverTime missing from a response and parsed as 0, an uninitialised field, a corrupted frame), never a time -- and garbage
// must never satisfy a fence: lock = 0 with read = now would pass the subtraction with decades to spare.
inline constexpr std::int64_t kMinPlausibleExchangeMs = 1'577'836'800'000;
inline constexpr std::int64_t kMaxPlausibleExchangeMs = 4'102'444'800'000;

constexpr bool is_plausible(ExchangeMs t) noexcept {
    return t.count() >= kMinPlausibleExchangeMs && t.count() <= kMaxPlausibleExchangeMs;
}

namespace detail {

// Overflow-checked int64 arithmetic (CLAUDE.md, mandate 5). Unreachable for plausible times and validated parameters, kept so
// that no arithmetic in this file can wrap into a value that looks like a pass.
constexpr std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept {
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) return std::nullopt;
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) return std::nullopt;
    return a + b;
}

constexpr std::optional<std::int64_t> checked_sub(std::int64_t a, std::int64_t b) noexcept {
    if (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) return std::nullopt;
    if (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) return std::nullopt;
    return a - b;
}

}  // namespace detail

// --- the fence (design 5.2, O4) -------------------------------------------------------------------------------------------
//
// W  the recvWindow of the dead process's last request (the exchange accepts at most 60 s)
// B  the bound on that process's clock offset from the exchange (SAFE-01 slice 4 refuses a larger one: also <= 60 s)
// M  a margin for the drift between the two time readings and scheduling
// L  how far behind the truth a read may lag (allOrders is served from the database)
//
// Once the tool holds the store's exclusive lock, no request of the dead process can be accepted after W + B of exchange time;
// a read that is to prove absence must happen another L later. The numbers below are the recommended ones and also the FLOORS:
// a parameter may be raised, never lowered (L is NEEDS_BASELINE until the Testnet drill measures it, design slice R6).
inline constexpr std::int64_t kRecvWindowFloorMs = 60'000;
inline constexpr std::int64_t kClockSkewFloorMs = 60'000;
inline constexpr std::int64_t kFenceMarginFloorMs = 30'000;
inline constexpr std::int64_t kReadLagFloorMs = 120'000;
// A sanity ceiling per parameter (one hour): far above anything meaningful, far below anything that could overflow.
inline constexpr std::int64_t kFenceParamCeilingMs = 3'600'000;

struct FenceParams {
    std::int64_t recv_window_ms{kRecvWindowFloorMs};
    std::int64_t clock_skew_ms{kClockSkewFloorMs};
    std::int64_t margin_ms{kFenceMarginFloorMs};
    std::int64_t read_lag_ms{kReadLagFloorMs};
};

// W + B + M + L of the recommended parameters: 4.5 minutes.
inline constexpr std::int64_t kDefaultEvidenceWaitMs =
    kRecvWindowFloorMs + kClockSkewFloorMs + kFenceMarginFloorMs + kReadLagFloorMs;

enum class FenceParamsStatus : std::uint8_t {
    Ok = 0,
    RecvWindowOutOfRange = 1,
    ClockSkewOutOfRange = 2,
    MarginOutOfRange = 3,
    ReadLagOutOfRange = 4,
};

namespace detail {

constexpr bool within_fence_bounds(std::int64_t v, std::int64_t floor) noexcept {
    return v >= floor && v <= kFenceParamCeilingMs;
}

}  // namespace detail

constexpr FenceParamsStatus validate_fence_params(const FenceParams& p) noexcept {
    if (!detail::within_fence_bounds(p.recv_window_ms, kRecvWindowFloorMs)) return FenceParamsStatus::RecvWindowOutOfRange;
    if (!detail::within_fence_bounds(p.clock_skew_ms, kClockSkewFloorMs)) return FenceParamsStatus::ClockSkewOutOfRange;
    if (!detail::within_fence_bounds(p.margin_ms, kFenceMarginFloorMs)) return FenceParamsStatus::MarginOutOfRange;
    if (!detail::within_fence_bounds(p.read_lag_ms, kReadLagFloorMs)) return FenceParamsStatus::ReadLagOutOfRange;
    return FenceParamsStatus::Ok;
}

// lock_ref: the FIRST serverTime read after the tool took the store's exclusive lock (design 5.2: conservative, because the
// reading cannot be earlier than the real moment of locking). nullopt for invalid parameters, an implausible time or overflow.

// The exchange time from which no request of the dead process can still be accepted: lock_ref + W + B + M.
constexpr std::optional<ExchangeMs> fence_deadline(ExchangeMs lock_ref, const FenceParams& p) noexcept {
    if (validate_fence_params(p) != FenceParamsStatus::Ok || !is_plausible(lock_ref)) return std::nullopt;
    auto t = detail::checked_add(lock_ref.count(), p.recv_window_ms);
    if (t) t = detail::checked_add(*t, p.clock_skew_ms);
    if (t) t = detail::checked_add(*t, p.margin_ms);
    if (!t) return std::nullopt;
    return ExchangeMs{*t};
}

// The earliest exchange time at which the evidence reads may start: the fence deadline plus L.
constexpr std::optional<ExchangeMs> earliest_evidence_read(ExchangeMs lock_ref, const FenceParams& p) noexcept {
    const auto deadline = fence_deadline(lock_ref, p);
    if (!deadline) return std::nullopt;
    const auto t = detail::checked_add(deadline->count(), p.read_lag_ms);
    if (!t) return std::nullopt;
    return ExchangeMs{*t};
}

enum class FenceVerdict : std::uint8_t {
    Unset = 0,            // never returned: a default-initialised verdict is not "satisfied"
    Satisfied = 1,
    TooEarly = 2,         // read_ref is before lock_ref + W + B + M + L
    ReadBeforeLock = 3,   // the exchange clock went backwards between the two readings
    ImplausibleTime = 4,  // either reading is outside the plausible range
    InvalidParams = 5,    // a parameter below its floor or above its ceiling
    Overflow = 6,         // unreachable for plausible times and validated parameters
};

// read_ref: the serverTime read immediately BEFORE the evidence requests are sent (so every evidence read happens at or after it).
constexpr FenceVerdict check_evidence_read(ExchangeMs lock_ref, ExchangeMs read_ref, const FenceParams& p) noexcept {
    if (validate_fence_params(p) != FenceParamsStatus::Ok) return FenceVerdict::InvalidParams;
    if (!is_plausible(lock_ref) || !is_plausible(read_ref)) return FenceVerdict::ImplausibleTime;
    if (read_ref < lock_ref) return FenceVerdict::ReadBeforeLock;
    const auto earliest = earliest_evidence_read(lock_ref, p);
    if (!earliest) return FenceVerdict::Overflow;
    return read_ref < *earliest ? FenceVerdict::TooEarly : FenceVerdict::Satisfied;
}

// The seconds the tool actually waited, for the frame's informational field (design 7.1): floor((read - lock) / 1 s), saturating
// at 65535 (18 h 12 min). 0 when either time is implausible or the clock went backwards.
constexpr std::uint16_t waited_seconds(ExchangeMs lock_ref, ExchangeMs read_ref) noexcept {
    if (!is_plausible(lock_ref) || !is_plausible(read_ref) || read_ref < lock_ref) return 0;
    const std::int64_t s = (read_ref.count() - lock_ref.count()) / 1000;  // both plausible: no overflow
    return s > 65535 ? std::uint16_t{65535} : static_cast<std::uint16_t>(s);
}

// --- the frame's machine field ----------------------------------------------------------------------------------------------

// What an ABSENT conclusion records beyond the six claims every one makes (operator_resolution_frame.hpp).
struct AbsentDetail {
    std::uint16_t waited_seconds{0};
    std::uint16_t tool_version{0};
    friend constexpr bool operator==(const AbsentDetail&, const AbsentDetail&) noexcept = default;
};

namespace detail {

// The one place the six claims are set. conclude_absent() calls it after every condition behind them has been checked; nothing
// else may (test_operator_resolution_guard.cpp pins that), so a detail_code can never claim evidence nobody looked at.
constexpr std::int64_t make_absent_detail_code(const AbsentDetail& d) noexcept {
    const std::uint64_t bits = kKindAbsent | kAbsentRequiredBits |
                               (static_cast<std::uint64_t>(d.waited_seconds) << kWaitedSecondsShift) |
                               (static_cast<std::uint64_t>(d.tool_version) << kToolVersionShift);
    return static_cast<std::int64_t>(bits);
}

}  // namespace detail

// The informational fields of a well-formed ABSENT detail_code; nullopt for anything is_valid_absent_detail_code() refuses.
constexpr std::optional<AbsentDetail> parse_absent_detail_code(std::int64_t code) noexcept {
    if (!is_valid_absent_detail_code(code)) return std::nullopt;
    const auto bits = static_cast<std::uint64_t>(code);
    AbsentDetail d{};
    d.waited_seconds = static_cast<std::uint16_t>((bits & kWaitedSecondsMask) >> kWaitedSecondsShift);
    d.tool_version = static_cast<std::uint16_t>((bits & kToolVersionMask) >> kToolVersionShift);
    return d;
}

// --- what the exchange answered (produced by operator_resolution_response.hpp) ---------------------------------------------

// GET /api/v3/order with origClientOrderId. Only NoSuchOrder is evidence; the 0 value is "nothing usable".
enum class PointQueryVerdict : std::uint8_t {
    Unusable = 0,     // not JSON, wrong shape, a status or an error code that cannot be trusted, oversize: says nothing
    NoSuchOrder = 1,  // HTTP 400 and error code -2013: the only answer that counts toward ABSENT
    OrderExists = 2,  // a success response describing an order: it exists, whoever resolves it, it is not this tool
    Archived = 3,     // -2026 ORDER_ARCHIVED: the order existed (zero fills, cancelled or expired 90+ days ago)
    OtherError = 4,   // any other exchange error: says nothing about this order
};

// One page of GET /api/v3/allOrders, or the whole answer of GET /api/v3/openOrders.
enum class PageVerdict : std::uint8_t {
    Malformed = 0,          // not a usable page: wrong status, not JSON, not an array of order objects, wrong symbol ...
    Clear = 1,              // a well-formed page that does not list the client order id (and is not at the page limit)
    Listed = 2,             // the client order id is in the page
    PossiblyTruncated = 3,  // a full page: the exchange may have cut it off, so absence is not shown
};

// --- GET /api/v3/allOrders windows (design 6 step 5b) ---------------------------------------------------------------------
//
// allOrders is served from the database, takes a symbol and a time window of at most 24 hours, and returns at most `limit`
// orders (default 500, maximum 1000; the answer says nothing when it truncated). The tool therefore walks the whole interval
// from before the order could have been created to the evidence read, one window at a time, and treats a full page as
// "possibly truncated".
inline constexpr std::int64_t kAllOrdersMaxSpanMs = 86'400'000 - 1'000;  // one second under the documented 24 h
inline constexpr std::size_t kAllOrdersMaxPages = 14;
inline constexpr std::size_t kAllOrdersPageLimit = 1000;

struct AllOrdersWindow {
    ExchangeMs start{};
    ExchangeMs end{};
};

enum class PlanStatus : std::uint8_t {
    Unset = 0,
    Ok = 1,
    ImplausibleTime = 2,
    EmptyRange = 3,    // start after end
    TooManyPages = 4,  // more than kAllOrdersMaxPages windows would be needed: the tool refuses, the operator resolves by hand
    Overflow = 5,
};

struct AllOrdersPlan {
    PlanStatus status{PlanStatus::Unset};
    std::array<AllOrdersWindow, kAllOrdersMaxPages> windows{};
    std::size_t count{0};
};

// Windows covering [start, end] with no gap: the first starts at `start`, the last ends at `end`, every window is at most
// kAllOrdersMaxSpanMs long, and each window starts where the previous one ended. (The shared millisecond is deliberate: it is
// covered whether the exchange treats endTime as inclusive or exclusive, so no order created at a boundary is ever missed.)
constexpr AllOrdersPlan plan_all_orders_windows(ExchangeMs start, ExchangeMs end) noexcept {
    AllOrdersPlan plan{};
    if (!is_plausible(start) || !is_plausible(end)) {
        plan.status = PlanStatus::ImplausibleTime;
        return plan;
    }
    if (start > end) {
        plan.status = PlanStatus::EmptyRange;
        return plan;
    }
    std::int64_t cursor = start.count();
    for (;;) {
        if (plan.count == kAllOrdersMaxPages) {
            plan = AllOrdersPlan{};
            plan.status = PlanStatus::TooManyPages;
            return plan;
        }
        const auto limit = detail::checked_add(cursor, kAllOrdersMaxSpanMs);
        if (!limit) {
            plan = AllOrdersPlan{};
            plan.status = PlanStatus::Overflow;
            return plan;
        }
        const std::int64_t window_end = *limit < end.count() ? *limit : end.count();
        plan.windows[plan.count] = AllOrdersWindow{ExchangeMs{cursor}, ExchangeMs{window_end}};
        ++plan.count;
        if (window_end == end.count()) break;
        cursor = window_end;
    }
    plan.status = PlanStatus::Ok;
    return plan;
}

// The timestamp inside a client order id, from the grammar of make_client_order_id() (order_lifecycle.hpp):
//   "HY-" <timestamp_ms> "-" <sequence> "-" <four UPPERCASE hex digits>
// decimal numbers without sign or leading zeros, the sequence a uint32. Anything else is not one of ours, and so is a timestamp
// outside the plausible range: nullopt. (No length check of its own: the grammar already bounds the id at 32 characters, and
// anything longer fails it somewhere. The timestamp is the LOCAL clock at the moment the id was made, before the order's
// intent was written, so it is never later than the frame times and at most B off the exchange's clock.)
constexpr std::optional<ExchangeMs> client_order_id_timestamp(std::string_view coid) noexcept {
    if (!coid.starts_with("HY-")) return std::nullopt;
    std::size_t i = 3;

    const auto read_decimal = [&](std::uint64_t max, std::uint64_t& out) constexpr noexcept -> bool {
        const std::size_t begin = i;
        std::uint64_t v = 0;
        while (i < coid.size() && coid[i] >= '0' && coid[i] <= '9') {
            const auto digit = static_cast<std::uint64_t>(coid[i] - '0');
            if (v > (max - digit) / 10) return false;  // would exceed `max`
            v = v * 10 + digit;
            ++i;
        }
        if (i == begin) return false;                       // no digits
        if (coid[begin] == '0' && i - begin > 1) return false;  // leading zero
        out = v;
        return true;
    };

    std::uint64_t ts = 0;
    if (!read_decimal(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), ts)) return std::nullopt;
    if (i >= coid.size() || coid[i] != '-') return std::nullopt;
    ++i;
    std::uint64_t seq = 0;
    if (!read_decimal(std::numeric_limits<std::uint32_t>::max(), seq)) return std::nullopt;
    if (i >= coid.size() || coid[i] != '-') return std::nullopt;
    ++i;
    if (coid.size() - i != 4) return std::nullopt;
    for (; i < coid.size(); ++i) {
        const char c = coid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return std::nullopt;
    }
    const ExchangeMs t{static_cast<std::int64_t>(ts)};
    if (!is_plausible(t)) return std::nullopt;
    return t;
}

enum class RangeStatus : std::uint8_t {
    Unset = 0,
    Ok = 1,
    NoUsableAnchor = 2,  // neither a ServerCorrectedUtc frame time nor a parseable client order id timestamp
    ImplausibleTime = 3,
    InvalidParams = 4,
    AnchorAfterRead = 5,  // the order's own timestamps are later than the evidence read: the inputs contradict each other
    Overflow = 6,
};

struct AllOrdersRange {
    RangeStatus status{RangeStatus::Unset};
    ExchangeMs start{};
    ExchangeMs end{};
};

// The interval allOrders must cover: from 2W before the earliest moment the order could have been sent to the evidence read.
// `frame_anchor` is the recorded time of the frame at which the send began (the Prepared / Submitting frame), only when that
// frame carries a ServerCorrectedUtc time (frame_wall_time_ms(), durable_control_plane.hpp, is 0 for any other); the client
// order id's timestamp is the second anchor. The EARLIER of the anchors that exist is used: the design prefers the frame time
// and falls back to the id, and taking the minimum is never later than either, which only widens the window (more pages, never
// a gap). A frame anchor that is present but implausible is refused, not ignored: the log is MAC-protected, so garbage there
// means something is wrong.
constexpr AllOrdersRange all_orders_range(std::optional<ExchangeMs> frame_anchor, std::string_view client_order_id,
                                          ExchangeMs read_ref, const FenceParams& p) noexcept {
    AllOrdersRange r{};
    if (validate_fence_params(p) != FenceParamsStatus::Ok) {
        r.status = RangeStatus::InvalidParams;
        return r;
    }
    if (!is_plausible(read_ref) || (frame_anchor && !is_plausible(*frame_anchor))) {
        r.status = RangeStatus::ImplausibleTime;
        return r;
    }
    std::optional<ExchangeMs> base = frame_anchor;
    if (const auto from_id = client_order_id_timestamp(client_order_id); from_id && (!base || *from_id < *base)) {
        base = from_id;
    }
    if (!base) {
        r.status = RangeStatus::NoUsableAnchor;
        return r;
    }
    if (*base > read_ref) {
        r.status = RangeStatus::AnchorAfterRead;
        return r;
    }
    const auto two_w = detail::checked_add(p.recv_window_ms, p.recv_window_ms);
    const auto start = two_w ? detail::checked_sub(base->count(), *two_w) : std::nullopt;
    if (!start) {
        r.status = RangeStatus::Overflow;
        return r;
    }
    if (!is_plausible(ExchangeMs{*start})) {
        r.status = RangeStatus::ImplausibleTime;
        return r;
    }
    r.status = RangeStatus::Ok;
    r.start = ExchangeMs{*start};
    r.end = read_ref;
    return r;
}

// --- the conclusion -----------------------------------------------------------------------------------------------------------

struct AbsentConclusionInput {
    // the order, as the log has it
    std::string_view client_order_id{};
    std::optional<ExchangeMs> frame_anchor{};

    // the two exchange-clock readings and the fence
    ExchangeMs lock_ref{};
    ExchangeMs read_ref{};
    FenceParams fence{};

    // the verdicts, one per evidence read; all_orders_pages has one entry per window of the plan
    // plan_all_orders_windows(all_orders_range(...)) yields, in order
    PointQueryVerdict point_query{PointQueryVerdict::Unusable};
    std::span<const PageVerdict> all_orders_pages{};
    PageVerdict open_orders{PageVerdict::Malformed};

    // facts only the tool knows
    bool lock_held{false};
    bool operator_confirmed{false};
    std::uint16_t tool_version{0};
};

enum class AbsentConclusionStatus : std::uint8_t {
    NotEvaluated = 0,
    Absent = 1,  // every piece of evidence is in and clean: detail_code may be written
    LockNotHeld = 2,
    FenceNotSatisfied = 3,  // see AbsentConclusion::fence
    InvalidFenceParams = 4,
    PointQueryNotAbsent = 5,
    NoUsableRange = 6,
    RangeNotPlannable = 7,
    AllOrdersIncomplete = 8,  // the pages do not match the plan one for one
    AllOrdersNotClear = 9,
    OpenOrdersNotClear = 10,
    NotConfirmed = 11,
};

struct AbsentConclusion {
    AbsentConclusionStatus status{AbsentConclusionStatus::NotEvaluated};
    FenceVerdict fence{FenceVerdict::Unset};
    std::int64_t detail_code{0};  // meaningful only when status == Absent
};

// May the tool write an OrderOperatorResolved frame? Only when ALL of: it holds the lock, the fence and the read lag have
// passed on the exchange clock, the point query said "no such order", every allOrders window of the interval the order could
// have been created in came back clear (the interval and the windows are recomputed here from the same inputs, so the tool
// cannot hand in a shorter one), openOrders came back clear, and the operator typed the confirmation. The first failure in
// that order is reported; the detail_code of an Absent result is the frame's machine field (operator_resolution_frame.hpp).
constexpr AbsentConclusion conclude_absent(const AbsentConclusionInput& in) noexcept {
    AbsentConclusion out{};
    if (!in.lock_held) {
        out.status = AbsentConclusionStatus::LockNotHeld;
        return out;
    }
    out.fence = check_evidence_read(in.lock_ref, in.read_ref, in.fence);
    if (out.fence == FenceVerdict::InvalidParams) {
        out.status = AbsentConclusionStatus::InvalidFenceParams;
        return out;
    }
    if (out.fence != FenceVerdict::Satisfied) {
        out.status = AbsentConclusionStatus::FenceNotSatisfied;
        return out;
    }
    if (in.point_query != PointQueryVerdict::NoSuchOrder) {
        out.status = AbsentConclusionStatus::PointQueryNotAbsent;
        return out;
    }
    const AllOrdersRange range = all_orders_range(in.frame_anchor, in.client_order_id, in.read_ref, in.fence);
    if (range.status != RangeStatus::Ok) {
        out.status = AbsentConclusionStatus::NoUsableRange;
        return out;
    }
    const AllOrdersPlan plan = plan_all_orders_windows(range.start, range.end);
    if (plan.status != PlanStatus::Ok) {
        out.status = AbsentConclusionStatus::RangeNotPlannable;
        return out;
    }
    // (An Ok plan always has a window; the first clause only guarantees that an empty plan could never read as "nothing to check".)
    if (plan.count == 0 || in.all_orders_pages.size() != plan.count) {
        out.status = AbsentConclusionStatus::AllOrdersIncomplete;
        return out;
    }
    for (const PageVerdict page : in.all_orders_pages) {
        if (page != PageVerdict::Clear) {
            out.status = AbsentConclusionStatus::AllOrdersNotClear;
            return out;
        }
    }
    if (in.open_orders != PageVerdict::Clear) {
        out.status = AbsentConclusionStatus::OpenOrdersNotClear;
        return out;
    }
    if (!in.operator_confirmed) {
        out.status = AbsentConclusionStatus::NotConfirmed;
        return out;
    }
    out.status = AbsentConclusionStatus::Absent;
    out.detail_code = detail::make_absent_detail_code(
        AbsentDetail{waited_seconds(in.lock_ref, in.read_ref), in.tool_version});
    return out;
}

}  // namespace hy::operator_resolution
