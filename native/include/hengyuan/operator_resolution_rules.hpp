// SPDX-License-Identifier: proprietary
// operator_resolution_rules.hpp -- may THIS OrderOperatorResolved frame close THAT order? (D5 slice R3-0)
//
// Governance: L1 (pure logic: no I/O, no allocation, no clock). docs/OPERATOR_RESOLUTION_DESIGN.md 7.2 lists what the
// recovery reader requires of an OrderOperatorResolved frame before it lets the frame close an order. Slice R1 wrote those
// rules into DurableAuditSink's recovery scan; slice R3-0 needs the very same rules a second time, on the WRITING side
// (DurableAuditSink::append_operator_resolution(), and later the operator tool, which wants to say WHY an order cannot be
// resolved before it asks anybody to confirm anything). There is one function and two callers on purpose: the writer's
// pre-check cannot accept a frame the reader would call Corrupt, because it IS the reader's check. A frame the writer lets
// through but the reader refuses would not just be refused later -- it would make the whole store Corrupt at the next start.
//
// The caller has already established the three facts this function does not look at: that the frame IS an
// OrderOperatorResolved frame, that it is the last frame of its client order id, and that it is written at frame format
// v6 (the reader checks those where it has the whole log in front of it; the writer checks them in
// append_operator_resolution()).

#pragma once

#include <cstdint>

#include <hengyuan/audit_trail.hpp>
#include <hengyuan/operator_resolution_frame.hpp>
#include <hengyuan/order_lifecycle.hpp>

namespace hy::operator_resolution {

// The order as the log shows it: the replay state of one client order id. `state` is the order's TRUE last state -- an order
// whose last frame is Submitting is Submitting here, not the Ambiguous that recovery hands out for it (the two take the
// same edge to Rejected, so for this function the difference does not matter, but a caller that has the true state should
// pass it).
struct ResolutionTarget {
    OrderState state{OrderState::Intent};
    std::int64_t exchange_order_id{0};
    std::int64_t filled_qty_ticks{0};
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    ExecutionMode mode{ExecutionMode::DryRun};
    std::int64_t price_ticks{0};  // the intended price of the Intent frame
    std::int64_t qty_ticks{0};    // the intended quantity of the Intent frame
};

// The first rule a frame breaks, in the order the design lists them. 0 is "nothing was checked": a result nobody filled in
// is never a pass.
enum class ResolutionCheck : std::uint8_t {
    NotChecked = 0,
    Ok = 1,
    NotInAResolvableState = 2,  // 1. only Submitting (after Prepared), Ambiguous and EscalatedToOperator can be closed this way
    OrderShowsExistence = 3,    // 6. the order showed an exchange id or a fill at some point: it existed, it is not "absent"
    FrameNotRejected = 4,       // 2. the frame must say Rejected
    FrameClaimsExistence = 5,   // 2. ...with no fill and no exchange id of its own
    NotTheSameOrder = 6,        // 2. symbol, side, mode, price and quantity must be those of the Intent frame
    BadDetailCode = 7,          // 3. the machine field must have the shape of an ABSENT conclusion
};

// (The numbers in the comments are the rule numbers of design 7.2.)
inline ResolutionCheck check_resolution(const ResolutionTarget& target, const AuditRecord& rec) noexcept {
    // 1. The order is in a state nothing automatic can finish. Submitting and Ambiguous already reach Rejected through the
    //    shared state machine; EscalatedToOperator only through the operator edge, which validate_transition() deliberately
    //    does not have.
    switch (target.state) {
        case OrderState::Submitting:
        case OrderState::Ambiguous:
            if (validate_transition(target.state, OrderState::Rejected) != TransitionResult::Ok) {
                return ResolutionCheck::NotInAResolvableState;
            }
            break;
        case OrderState::EscalatedToOperator:
            if (validate_operator_resolution(target.state, OrderState::Rejected) != TransitionResult::Ok) {
                return ResolutionCheck::NotInAResolvableState;
            }
            break;
        default:
            return ResolutionCheck::NotInAResolvableState;
    }
    // 6. The order must never have shown a sign of existing on the exchange: no exchange id, no fill (the average price is
    //    only ever recorded together with a fill, so it needs no check of its own).
    if (target.exchange_order_id != 0 || target.filled_qty_ticks != 0) return ResolutionCheck::OrderShowsExistence;
    // 2. What the frame says: Rejected, nothing filled, no exchange id, and the same order the Intent frame described.
    if (rec.resulting_state != OrderState::Rejected) return ResolutionCheck::FrameNotRejected;
    if (rec.exchange_order_id != 0 || rec.filled_qty_ticks != 0 || rec.avg_fill_price_ticks != 0) {
        return ResolutionCheck::FrameClaimsExistence;
    }
    if (rec.symbol_id != target.symbol_id || rec.side != target.side || rec.mode != target.mode) {
        return ResolutionCheck::NotTheSameOrder;
    }
    if (rec.price_ticks != target.price_ticks || rec.qty_ticks != target.qty_ticks) return ResolutionCheck::NotTheSameOrder;
    // 3. The machine field has the shape of an ABSENT conclusion (operator_resolution_frame.hpp).
    if (!is_valid_absent_detail_code(rec.detail_code)) return ResolutionCheck::BadDetailCode;
    return ResolutionCheck::Ok;
}

}  // namespace hy::operator_resolution
