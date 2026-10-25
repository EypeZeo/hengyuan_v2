// SPDX-License-Identifier: proprietary
// operator_resolution_frame.hpp -- the shape of an OrderOperatorResolved frame's machine field (D5 slice R1).
//
// Governance: L1 (pure constants and one predicate, no I/O). docs/OPERATOR_RESOLUTION_DESIGN.md 7.1 proposed this
// layout of AuditRecord::detail_code and left it to the implementation PR to freeze; R1 freezes it here, so the
// reader (durable_audit_sink.hpp) and the tool that will write the frame (slice R3) share ONE definition.
//
//   bits  0..3   conclusion kind: 1 = ABSENT ("the exchange has no such order"); every other value is refused
//   bit   4      the fence was satisfied (the tool waited W + B on the exchange clock, counted from its own lock)
//   bit   5      the point query for the COID answered "unknown order"
//   bit   6      allOrders over the window did not list the COID
//   bit   7      openOrders did not list the COID
//   bit   8      the operator typed the confirmation phrase
//   bit   9      the tool held the exclusive lock on the store
//   bits 10..15  reserved, MUST be 0
//   bits 16..31  seconds the tool actually waited (informational, any value)
//   bits 32..47  tool version (informational, any value)
//   bits 48..63  reserved, MUST be 0 (bit 63 is the sign bit: a negative detail_code is never valid)
//
// "The bits are set" is a DECLARATION: the reader cannot check that the fence really passed or that the exchange
// really said so. The guarantee is the protocol and the tool (the design's 5 and 6); what this predicate gives is
// that a writer which does not even claim the evidence cannot slip into the log, and that a field with bits no
// version of the tool defines is refused rather than guessed at (fail closed: a future kind is a new design, not a
// value the current reader may wave through).

#pragma once

#include <cstdint>

namespace hy::operator_resolution {

inline constexpr std::uint64_t kKindMask = 0xF;
inline constexpr std::uint64_t kKindAbsent = 1;

inline constexpr std::uint64_t kFenceSatisfied = std::uint64_t{1} << 4;
inline constexpr std::uint64_t kPointQueryUnknownOrder = std::uint64_t{1} << 5;
inline constexpr std::uint64_t kAllOrdersAbsent = std::uint64_t{1} << 6;
inline constexpr std::uint64_t kOpenOrdersAbsent = std::uint64_t{1} << 7;
inline constexpr std::uint64_t kOperatorConfirmed = std::uint64_t{1} << 8;
inline constexpr std::uint64_t kLockHeld = std::uint64_t{1} << 9;

// Every claim an ABSENT conclusion has to make (bits 4..9).
inline constexpr std::uint64_t kAbsentRequiredBits = kFenceSatisfied | kPointQueryUnknownOrder | kAllOrdersAbsent |
                                                      kOpenOrdersAbsent | kOperatorConfirmed | kLockHeld;

// Informational fields (any value): the seconds actually waited and the tool version.
inline constexpr unsigned kWaitedSecondsShift = 16;
inline constexpr std::uint64_t kWaitedSecondsMask = std::uint64_t{0xFFFF} << kWaitedSecondsShift;
inline constexpr unsigned kToolVersionShift = 32;
inline constexpr std::uint64_t kToolVersionMask = std::uint64_t{0xFFFF} << kToolVersionShift;

// Bits no version defines: 10..15 and 48..63.
inline constexpr std::uint64_t kReservedMask = (std::uint64_t{0x3F} << 10) | (std::uint64_t{0xFFFF} << 48);

static_assert((kKindMask & kAbsentRequiredBits) == 0);
static_assert((kKindMask & kReservedMask) == 0);
static_assert((kAbsentRequiredBits & kReservedMask) == 0);
static_assert((kWaitedSecondsMask & kReservedMask) == 0);
static_assert((kToolVersionMask & kReservedMask) == 0);
static_assert((kKindMask | kAbsentRequiredBits | kReservedMask | kWaitedSecondsMask | kToolVersionMask) ==
              ~std::uint64_t{0});

// Is `code` a well-formed detail_code of an ABSENT conclusion? (Shape only -- see the header comment.)
constexpr bool is_valid_absent_detail_code(std::int64_t code) noexcept {
    // A negative code has bit 63 set, which kReservedMask refuses -- no separate sign check.
    const auto bits = static_cast<std::uint64_t>(code);
    if ((bits & kKindMask) != kKindAbsent) return false;
    if ((bits & kAbsentRequiredBits) != kAbsentRequiredBits) return false;
    if ((bits & kReservedMask) != 0) return false;
    return true;
}

}  // namespace hy::operator_resolution
