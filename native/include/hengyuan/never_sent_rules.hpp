// SPDX-License-Identifier: proprietary
// never_sent_rules.hpp -- may THIS OrderNeverSent frame close THAT order? (Q23 slices S2/S3)
//
// Governance: L1 (pure logic: no I/O, no allocation, no clock). docs/NOTSENT_CLOSURE_DESIGN.md 4.3 says what an
// OrderNeverSent frame contains and 4.6 what the recovery reader does with one; this header is the "does it close THIS order"
// half of that, in one place. Two callers, both in DurableAuditSink: the recovery scan, and the writer, append_never_sent() (slice
// S4), which calls the very same function before it writes -- for the reason operator_resolution_rules.hpp gives for its own: a
// frame the writer lets through but the reader refuses would not just be refused later, it would make the whole store Corrupt at
// the next start. One rule, two callers, on purpose. (The writer's own callers are the later slices: S5, the final gate's
// refusal, class B, and S6, a refusal before the Prepared frame, class A.)
//
// WHAT AN OrderNeverSent FRAME CLAIMS, AND WHAT THE READER CAN AND CANNOT CHECK. The frame says "this order never crossed": no
// byte of its request ever left the process. The reader cannot verify that -- only the writer knew whether it had claimed the
// send (docs/NOTSENT_CLOSURE_DESIGN.md 4.2) -- so the guarantee that it is TRUE lives in the protocol (modeled in
// formal/notsent_closure.tla: NotSentImpliesNeverCrossed) and in the writer. What the reader CAN do is refuse every frame that
// contradicts what the log already shows of the order, and that is all this file does:
//   * the order is in a state from which it never crossed is even possible: Intent (nothing prepared, class A) or Submitting
//     (Prepared and nothing since, class B) -- through the shared state machine, validate_transition(), so there is no second
//     list to drift. NOT Ambiguous: an order that may have crossed is closed by the exchange or by the operator tool (D5),
//     never by the process's own say-so after the fact;
//   * the order showed no sign of existing on the exchange (no exchange id, no fill) -- an order the exchange has spoken for
//     did cross, whatever a later frame says;
//   * the frame is the NeverSent pair (the event type and the resulting state are one fact said twice: a frame that says only
//     half of it is refused, wherever it stands in the log), carries no exchange id, no fill and no average fill price of its
//     own, and describes the order the Intent frame described (symbol, side, mode, price, quantity).
// The machine field `detail_code` carries the REASON (design 4.3); it is informational -- a value this build does not know is
// not corruption -- and is deliberately not checked. Whether the frame is the LAST frame of its client order id, is written at
// frame format v6, and follows an Intent frame are facts the caller establishes where it has the whole log in front of it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <hengyuan/audit_trail.hpp>
#include <hengyuan/order_lifecycle.hpp>

namespace hy::never_sent {

// The order as the log shows it: the replay state of one client order id (the same shape operator_resolution::ResolutionTarget
// has, kept separate so neither feature has to include the other). `state` is the order's TRUE last state -- an order whose
// last frame is Submitting is Submitting here.
struct ClosureTarget {
    OrderState state{OrderState::Intent};
    std::int64_t exchange_order_id{0};
    std::int64_t filled_qty_ticks{0};
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    ExecutionMode mode{ExecutionMode::DryRun};
    std::int64_t price_ticks{0};  // the intended price of the Intent frame
    std::int64_t qty_ticks{0};    // the intended quantity of the Intent frame
};

// The first rule a frame breaks, in the order the checks run. 0 is "nothing was checked": a result nobody filled in is never a
// pass.
enum class ClosureCheck : std::uint8_t {
    NotChecked = 0,
    Ok = 1,
    NotInAClosableState = 2,  // only Intent (class A) and Submitting (class B) can be closed this way
    OrderShowsExistence = 3,  // the order showed an exchange id or a fill at some point: it existed, it did not "never cross"
    FramePairingBroken = 4,   // the event type and the resulting state are not the NeverSent pair (both, or neither)
    FrameClaimsExistence = 5, // the frame carries an exchange id, a fill or an average fill price of its own
    NotTheSameOrder = 6,      // symbol, side, mode, price and quantity must be those of the Intent frame
};

// Does `rec` say NeverSent in BOTH places (the event type and the resulting state), or in neither? false -- exactly one of the
// two -- is a frame the reader refuses wherever it stands in the log, the first frame of a client order id included.
inline bool pairing_holds(const AuditRecord& rec) noexcept {
    return (rec.event_type == AuditEventType::OrderNeverSent) == (rec.resulting_state == OrderState::NeverSent);
}

// May this ORDER be closed as never sent at all? A property of the order alone.
inline ClosureCheck check_order_closable(const ClosureTarget& target) noexcept {
    if (validate_transition(target.state, OrderState::NeverSent) != TransitionResult::Ok) {
        return ClosureCheck::NotInAClosableState;
    }
    // The order must never have shown a sign of existing on the exchange: no exchange id, no fill (the average price is only
    // ever recorded together with a fill, so it needs no check of its own).
    if (target.exchange_order_id != 0 || target.filled_qty_ticks != 0) return ClosureCheck::OrderShowsExistence;
    return ClosureCheck::Ok;
}

// Is this FRAME a legitimate closing of that order? (Meaningful only for an order check_order_closable() accepts;
// check_closure() asks them in that order.)
inline ClosureCheck check_closing_frame(const ClosureTarget& target, const AuditRecord& rec) noexcept {
    if (rec.event_type != AuditEventType::OrderNeverSent || rec.resulting_state != OrderState::NeverSent) {
        return ClosureCheck::FramePairingBroken;
    }
    if (rec.exchange_order_id != 0 || rec.filled_qty_ticks != 0 || rec.avg_fill_price_ticks != 0) {
        return ClosureCheck::FrameClaimsExistence;
    }
    if (rec.symbol_id != target.symbol_id || rec.side != target.side || rec.mode != target.mode) {
        return ClosureCheck::NotTheSameOrder;
    }
    if (rec.price_ticks != target.price_ticks || rec.qty_ticks != target.qty_ticks) return ClosureCheck::NotTheSameOrder;
    return ClosureCheck::Ok;
}

// All of it: the rule the recovery reader applies to a frame it reads and the writers will apply to a frame they are about to
// write.
inline ClosureCheck check_closure(const ClosureTarget& target, const AuditRecord& rec) noexcept {
    const ClosureCheck order = check_order_closable(target);
    return order != ClosureCheck::Ok ? order : check_closing_frame(target, rec);
}

// --- Class A: an order refused after its Intent frame and before its Prepared frame (Q23 S6, docs/NOTSENT_CLOSURE_DESIGN.md 4.3) ---

// Stable closure reasons carried in detail_code. Readers accept unknown values.
// Class A local refusals use 1..6; class B final authorization refusals start at 32.
enum class Reason : std::int64_t {
    ConfirmationMissing = 1,         // the operator's CONFIRM is not there
    ConfirmationMismatch = 2,        // ...or does not authorise this order
    SubmitPortInvalid = 3,           // the injected submit port is not usable
    InFlightRegistryUnavailable = 4, // no in-flight registry: a live submit never runs unguarded
    RateLimitExhausted = 5,          // the actual rate-limit charge was refused
    DuplicateInFlight = 6,           // the in-flight registry already holds the client order id
    ReadinessRevoked = 32,
    AuthorizationExpired = 33,
    ConfirmationExpired = 34,
    SlotOwnershipLost = 35,
    ReservationLost = 36,
    AccountChanged = 37,
    PreconditionsChanged = 38,
};

// What the owner of a class A order knows of it: its Intent frame was acknowledged and NOTHING else happened -- no Prepared frame, no
// claim, no byte of a request. Built from that Intent frame, so the target and the closing frame cannot disagree about the order.
inline ClosureTarget class_a_target_of(const AuditRecord& intent) noexcept {
    ClosureTarget t{};
    t.state = OrderState::Intent;
    t.exchange_order_id = 0;
    t.filled_qty_ticks = 0;
    t.symbol_id = intent.symbol_id;
    t.side = intent.side;
    t.mode = intent.mode;
    t.price_ticks = intent.price_ticks;
    t.qty_ticks = intent.qty_ticks;
    return t;
}

// The closing frame of such an order: the NeverSent pair, the Intent's order unchanged, nothing filled, no exchange id, the reason in
// detail_code and nothing in detail_msg. check_closure(class_a_target_of(intent), class_a_closure_of(intent, r, t)) is Ok for every
// reason (test_never_sent_rules.cpp pins it).
inline AuditRecord class_a_closure_of(const AuditRecord& intent, Reason reason, std::int64_t now_ms) noexcept {
    AuditRecord r = intent;
    r.timestamp_ms = now_ms;
    r.event_type = AuditEventType::OrderNeverSent;
    r.resulting_state = OrderState::NeverSent;
    r.exchange_order_id = 0;
    r.filled_qty_ticks = 0;
    r.avg_fill_price_ticks = 0;
    r.detail_code = static_cast<std::int64_t>(reason);
    r.detail_msg[0] = '\0';
    return r;
}

// The client order ids a context closed as never sent -- the most recent kCapacity of them. A client order id is ONE attempt: a frame
// after the closure makes the next start Corrupt (design 4.2, the model's SingleAttemptCoid), and a class A order has no in-flight slot
// to keep that true, so whoever is about to write an Intent frame asks here first. Fixed storage, no allocation. An id is remembered
// when its closure was acknowledged AND when the append failed (the frame may be on the disk anyway: design 4.5). Older ids are
// forgotten in order. DurableAuditSink independently enforces the generated attempt watermark across contexts and restarts.
class RecentlyClosedIds {
public:
    static constexpr std::size_t kCapacity = 64;

    void remember(std::string_view coid) noexcept {
        if (coid.empty() || coid.size() > kClientOrderIdLen) return;
        ClientOrderId& slot = ids_[next_];
        std::memcpy(slot.id, coid.data(), coid.size());
        slot.id[coid.size()] = '\0';
        next_ = (next_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
    }

    bool contains(std::string_view coid) const noexcept {
        if (coid.empty()) return false;
        for (std::size_t i = 0; i < count_; ++i) {
            if (ids_[i].view() == coid) return true;
        }
        return false;
    }

    std::size_t size() const noexcept { return count_; }

private:
    std::array<ClientOrderId, kCapacity> ids_{};
    std::size_t next_{0};
    std::size_t count_{0};
};

}  // namespace hy::never_sent
