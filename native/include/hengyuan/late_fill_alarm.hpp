// SPDX-License-Identifier: proprietary
// late_fill_alarm.hpp — D5 slice R4 (docs/OPERATOR_RESOLUTION_DESIGN.md 9 "R-late"; blueprint L-47): what happens when the exchange speaks about
// an order an operator wrote off.
//
// An order the offline tool closed (OrderOperatorResolved) is FINAL: "the exchange has no such order and, after the fence, never will". If that
// proof was wrong -- a read that lagged further behind than the tool assumed, an account or an environment the tool should not have been looking at
// -- the order is live after all, it is nobody's business in this process (recovery tracks nothing for it), and drain_user_data_events() used to
// throw its execution reports away without a trace: a miss in the in-flight table is "expected, not exceptional". The position drifts and nothing
// says so.
//
// This is the alarm. It watches exactly the client order ids that recovered_operator_resolved() (durable_audit_sink.hpp) lists -- not "every own
// order id that is not in flight", which would flag the legitimate late duplicate of an ordinary final order (the design says so in as many words) --
// and when the drain sees an execution report for one of them it
//   1. trips the breaker (KillSwitch::trigger()): Gate 2 and Gate 2b refuse every order from then on, the very call that drained the report
//      included, because the drain runs ahead of the gates; and
//   2. writes the audit records: UserDataStreamEventObserved for the FIRST report of each watched order, KillSwitchTriggered whenever the report is
//      what moved the breaker.
// The breaker first, the audit second: a missing or full audit sink can lose a record, never the trip.
//
// WHAT COUNTS. Any execution report for a watched order, with a fill or without, whatever its execution type. The design text says "executedQty >
// 0"; the alarm does not read the quantity at all, and that is the point. A report is evidence that the order exists (or existed), which is the one
// thing the operator's proof said could not be, and a quantity that was misread would be a missed alarm -- the one failure this slice is for. The
// price is a halt on a report that carries no fill (a cancel, an expiry): it contradicts the proof too, and a restart clears the halt.
//
// WHAT IT DOES NOT DO. It does not fold the fill into PositionTruth (the symbol and the side are known, the quantity scale is not; "resolution with a
// fill" is K3 and needs the accounting recovery path first), and it repairs nothing: after the trip the position this process believes in is wrong and
// an operator has to look at the account. It is memory-only: the breaker (SAFE-04 will make it persistent) and the audit ring both forget on a
// restart. A durable record cannot be an order frame keyed by the resolved id -- the recovery scan reads any frame after an operator frame as
// Corrupt (design 7.2 rule 4) -- so it needs a record type of its own: M4 / SAFE-04.
//
// THE TABLE is a view of the recovered list, not a copy: the sink that owns the list must outlive the alarm. The list is complete or the trading
// open failed (kMaxOperatorResolved, durable_control_plane.hpp). A caller that hands over the list of an OperatorOpen sink -- the tool's mode, which
// counts what did not fit -- says so through `resolved_total` and gets an alarm that trips the breaker at once: a watch list with holes is no watch list.
//
// A watched id that is also in flight is NOT alarmed (the drain asks the registry before it calls the alarm): it can only be a re-used id, and the
// registry's order is the one this process is tracking.
//
// THREAD OWNERSHIP: the hot/submit thread, like the KillSwitch it trips and the registry the drain reads. No atomics, no heap, no I/O, no clock of
// its own (the caller's now_ms stamps the records). The alarm and the OrchestratorContext it is wired into must name the SAME KillSwitch; the alarm
// holds it by reference, the context by pointer, and nothing can check that they agree.

#pragma once

#include <hengyuan/audit_trail.hpp>
#include <hengyuan/durable_control_plane.hpp>  // OperatorResolvedOrder, kMaxOperatorResolved
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/order_lifecycle.hpp>        // OrderState

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace hy {

// What the alarm needs to know about one execution report. exec_type is the numeric ExecutionType of binance_user_data_event.hpp, which includes this
// header and so cannot be named here; it goes into the audit record's detail_code as it is.
struct LateFillReport {
    std::int64_t exchange_order_id{0};
    std::uint8_t exec_type{0};
    OrderState order_status{OrderState::Intent};
    bool order_status_known{false};
};

struct LateFillStats {
    std::uint64_t reports{0};         // execution reports for a watched order that is not in flight -- every one, repeats included
    std::uint64_t alarmed_orders{0};  // distinct watched orders that reported at least once
    std::uint64_t trips{0};           // reports that moved the breaker (Normal or Armed -> Triggered)
    std::uint64_t audit_failed{0};    // alarm records the audit sink did not take (no sink, or a full ring)
};

class LateFillAlarm {
public:
    // detail_msg of the two records (each fits AuditRecord::detail_msg, 64 bytes with the terminator).
    static constexpr const char* kObservedMsg = "late report: operator-resolved order";
    static constexpr const char* kTripMsg = "breaker: report on operator-resolved order";

    // `resolved` is the recovered list (recovered_operator_resolved()), `resolved_total` what the scan found (recovered_operator_resolved_total()).
    // They differ only for an OperatorOpen scan that overflowed -- or for a list longer than this alarm can hold: either way the table is incomplete,
    // complete() says so, and the breaker is tripped here and now.
    LateFillAlarm(std::span<const OperatorResolvedOrder> resolved, std::size_t resolved_total, KillSwitch& kill_switch) noexcept
        : resolved_(resolved.first(resolved.size() < kMaxOperatorResolved ? resolved.size() : kMaxOperatorResolved)),
          complete_(resolved_total == resolved.size() && resolved.size() <= kMaxOperatorResolved),
          kill_switch_(kill_switch) {
        if (!complete_) (void)trip();
    }

    LateFillAlarm(const LateFillAlarm&) = delete;
    LateFillAlarm& operator=(const LateFillAlarm&) = delete;

    // Is `coid` one of the written-off orders? An empty id matches nothing.
    [[nodiscard]] bool watches(std::string_view coid) const noexcept { return find(coid) != kNotFound; }

    // The hot thread, for an execution report whose client order id is NOT in flight. Returns true when `coid` is a watched order (the report was
    // dealt with here, whatever came of the audit), false when it is none of the alarm's business.
    bool on_execution_report(std::string_view coid, const LateFillReport& report, AuditRingSink* audit, std::int64_t now_ms) noexcept {
        const std::size_t index = find(coid);
        if (index == kNotFound) return false;
        const OperatorResolvedOrder& order = resolved_[index];

        ++stats_.reports;
        const bool first = !alarmed_[index];
        if (first) {
            alarmed_[index] = true;
            ++stats_.alarmed_orders;
        }
        // The breaker moves on EVERY report that finds it Normal or Armed, not only on the first report of an order: an operator reset (the
        // restart equivalent) followed by one more report for the same order must stop the process again.
        const bool tripped = trip();
        if (tripped) ++stats_.trips;

        if (first) record(audit, AuditEventType::UserDataStreamEventObserved, kObservedMsg, order, report, now_ms);
        if (tripped) record(audit, AuditEventType::KillSwitchTriggered, kTripMsg, order, report, now_ms);
        return true;
    }

    // False when the table was handed over with holes (see the constructor). An alarm that is not complete has already tripped the breaker.
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    [[nodiscard]] std::size_t watched() const noexcept { return resolved_.size(); }
    [[nodiscard]] const LateFillStats& stats() const noexcept { return stats_; }

private:
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    [[nodiscard]] std::size_t find(std::string_view coid) const noexcept {
        if (resolved_.empty()) [[likely]] return kNotFound;  // no operator has ever resolved an order here: the common case, one branch
        if (coid.empty()) [[unlikely]] return kNotFound;
        for (std::size_t i = 0; i < resolved_.size(); ++i) {
            if (resolved_[i].client_order_id.view() == coid) return i;
        }
        return kNotFound;
    }

    // Triggered from Normal or Armed; Triggered stays as it is (trigger() would only count again) and Latched is never touched. True when the
    // state moved.
    bool trip() noexcept {
        const KillState state = kill_switch_.state();
        if (state != KillState::Normal && state != KillState::Armed) return false;
        kill_switch_.trigger();
        return true;
    }

    void record(AuditRingSink* audit, AuditEventType type, const char* message, const OperatorResolvedOrder& order, const LateFillReport& report,
                std::int64_t now_ms) noexcept {
        if (audit == nullptr) {
            ++stats_.audit_failed;
            return;
        }
        AuditRecord ar{};
        ar.timestamp_ms = now_ms;
        ar.event_type = type;
        ar.mode = ExecutionMode::Live;  // the tool resolves Live orders only
        ar.symbol_id = order.symbol_id;
        ar.set_client_order_id(order.client_order_id.view());
        ar.exchange_order_id = report.exchange_order_id;
        ar.qty_ticks = order.qty_ticks;  // the intended quantity; what the report says was filled is not parsed (see WHAT COUNTS)
        ar.side = order.side;
        ar.detail_code = static_cast<std::int64_t>(report.exec_type);
        ar.set_detail(message);
        if (report.order_status_known) ar.resulting_state = report.order_status;
        if (!audit->append(ar)) ++stats_.audit_failed;
    }

    std::span<const OperatorResolvedOrder> resolved_;
    bool complete_;
    KillSwitch& kill_switch_;
    LateFillStats stats_{};
    std::array<bool, kMaxOperatorResolved> alarmed_{};  // by position in resolved_: has this order reported before?
};

}  // namespace hy
