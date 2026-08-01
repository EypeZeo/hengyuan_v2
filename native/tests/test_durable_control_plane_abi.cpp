// Layer 2 of docs/SPEC_INVARIANTS.md's mechanization plan.
//
// durable_control_plane.hpp is an ABI TRANSCRIPTION of the spec, so most of what
// matters about it is checked by tools/spec_enum_diff.py (enumerator names and
// values, diffed against the spec text) rather than here. These tests cover what
// a text differ cannot: that the types are constructible and trivially copyable,
// that the two-phase ExportOutboxRing contract actually behaves as specified, and
// that is_exchange_final() matches the authoritative set in SPEC_INVARIANTS.md.
//
// A caution learned the hard way: an earlier version of this file passed
// completely while testing three INVENTED types that did not match the spec at
// all — including a bounds test for a payload buffer the spec has no field for.
// Unit tests prove a type behaves as written; only spec_enum_diff.py proves it
// was written from the spec. Do not read green here as "the port is faithful".
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/order_lifecycle.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

using hy::AuditAppendResult;
using hy::DurableRecordType;
using hy::ExportOutboxRing;
using hy::ExportTuple;
using hy::FrameTimeKind;
using hy::OrderRecoveryCheckpoint;
using hy::OrderState;
using hy::RecoveryScanStatus;

// --- AuditAppendResult (spec L4 §10 / BINANCE:2473) ---

TEST(AuditAppendResult, DefaultIsNotAcked) {
    AuditAppendResult r{};
    EXPECT_EQ(r.status, AuditAppendResult::Status::Failed);
    EXPECT_FALSE(r.acked());
    EXPECT_EQ(r.sequence, 0u);
}

// The whole reason spec round 14 turned this from a bare enum into a struct:
// callers must be able to learn the sequence their own write was assigned.
TEST(AuditAppendResult, AckedCarriesAssignedSequence) {
    AuditAppendResult r{AuditAppendResult::Status::Acked, 42};
    EXPECT_TRUE(r.acked());
    EXPECT_EQ(r.sequence, 42u);
}

// Status is a NESTED enum per the spec, not a top-level one beside the struct.
TEST(AuditAppendResult, StatusIsNestedWithSpecWireValues) {
    EXPECT_EQ(static_cast<std::uint8_t>(AuditAppendResult::Status::Acked), 0u);
    EXPECT_EQ(static_cast<std::uint8_t>(AuditAppendResult::Status::Failed), 1u);
}

// --- Wire-discriminator values (spec L4 §10 / BINANCE:2486, :2511, :2565) ---
//
// spec_enum_diff.py checks the full enumerator lists against the spec text.
// These pin the handful whose exact numbers are load-bearing for stored frames,
// so a renumbering fails a test even if someone edits the spec and the header
// together without thinking about frames already on disk.

TEST(DurableRecordTypeWireValues, OrderEventIsZeroPerRound11P0) {
    // Round-11 P0 states this explicitly: "OrderEvent MUST be a real enumerator
    // (value 0)". An earlier invented version had four separate Order* values
    // starting at 0 and no OrderEvent at all.
    EXPECT_EQ(static_cast<std::uint8_t>(DurableRecordType::OrderEvent), 0u);
}

TEST(DurableRecordTypeWireValues, MatchSpecNumbering) {
    EXPECT_EQ(static_cast<std::uint8_t>(DurableRecordType::SymbolRegistrySnapshot), 1u);
    EXPECT_EQ(static_cast<std::uint8_t>(DurableRecordType::RateLimitFreeze), 2u);
    EXPECT_EQ(static_cast<std::uint8_t>(DurableRecordType::OrderCheckpoint), 8u);
    EXPECT_EQ(static_cast<std::uint8_t>(DurableRecordType::FreezeEpochWatermark), 10u);
    EXPECT_EQ(static_cast<std::uint8_t>(DurableRecordType::SealJournalApplied), 16u);
}

TEST(RecoveryScanStatusWireValues, MatchSpecNumbering) {
    EXPECT_EQ(static_cast<std::uint8_t>(RecoveryScanStatus::Clean), 0u);
    EXPECT_EQ(static_cast<std::uint8_t>(RecoveryScanStatus::Recovered), 1u);
    EXPECT_EQ(static_cast<std::uint8_t>(RecoveryScanStatus::Corrupt), 2u);
    EXPECT_EQ(static_cast<std::uint8_t>(RecoveryScanStatus::CapacityExceeded), 3u);
    EXPECT_EQ(static_cast<std::uint8_t>(RecoveryScanStatus::IoError), 4u);
    // Round-11 P0: must be a real enumerator, not an out-of-band side channel.
    EXPECT_EQ(static_cast<std::uint8_t>(RecoveryScanStatus::ExternalAnchorUnavailable), 5u);
}

TEST(FrameTimeKindWireValues, MatchSpecNumbering) {
    EXPECT_EQ(static_cast<std::uint8_t>(FrameTimeKind::ServerCorrectedUtc), 0u);
    EXPECT_EQ(static_cast<std::uint8_t>(FrameTimeKind::UnknownBootstrap), 1u);
}

// --- OrderRecoveryCheckpoint (spec SUBMITPORT:1289) ---

TEST(OrderRecoveryCheckpoint, ConstructibleAndTriviallyCopyable) {
    static_assert(std::is_trivially_copyable_v<OrderRecoveryCheckpoint>);
    OrderRecoveryCheckpoint cp{};
    cp.resulting_state = OrderState::PartialFill;
    cp.filled_qty_ticks = 100;
    OrderRecoveryCheckpoint copy = cp;  // plain memberwise copy, no allocation
    EXPECT_EQ(copy.resulting_state, OrderState::PartialFill);
    EXPECT_EQ(copy.filled_qty_ticks, 100);
}

// --- ExportTuple (spec L4 §10.2.1 / BINANCE:4611) ---

TEST(ExportTupleAbi, IsATipAnchorRecord) {
    ExportTuple t{};
    t.store_uuid_lo = 0xAAAA'BBBB'CCCC'DDDDull;
    t.store_uuid_hi = 0x1111'2222'3333'4444ull;
    t.generation = 7;
    t.sequence = 12345;
    t.tip_mac[0] = 0xABu;
    t.tip_mac[31] = 0xCDu;
    t.key_id = 3;

    ExportTuple copy = t;
    EXPECT_EQ(copy.store_uuid_lo, 0xAAAA'BBBB'CCCC'DDDDull);
    EXPECT_EQ(copy.store_uuid_hi, 0x1111'2222'3333'4444ull);
    EXPECT_EQ(copy.generation, 7u);
    EXPECT_EQ(copy.sequence, 12345u);
    EXPECT_EQ(copy.tip_mac.size(), 32u);
    EXPECT_EQ(copy.tip_mac[0], 0xABu);
    EXPECT_EQ(copy.tip_mac[31], 0xCDu);
    EXPECT_EQ(copy.key_id, 3u);
}

// A tuple stamped UnknownBootstrap carries enqueued_utc_ms == 0 and must be
// excluded from hard-lag age arithmetic — never treated as "epoch, therefore
// ancient".
TEST(ExportTupleAbi, UnknownBootstrapIsExcludedFromAgeComputation) {
    ExportTuple unknown{};
    unknown.time_kind = static_cast<std::uint8_t>(FrameTimeKind::UnknownBootstrap);
    unknown.enqueued_utc_ms = 0;
    EXPECT_FALSE(unknown.participates_in_age_computation());

    ExportTuple corrected{};
    corrected.time_kind = static_cast<std::uint8_t>(FrameTimeKind::ServerCorrectedUtc);
    corrected.enqueued_utc_ms = 1'700'000'000'000;
    EXPECT_TRUE(corrected.participates_in_age_computation());
}

// --- ExportOutboxRing two-phase contract (spec L4 §10.2.1 / BINANCE:4626) ---
//
// This is the part of the port most worth testing, because the contract is
// unusual: peek does NOT remove. An earlier version aliased this to a pop-on-read
// SPSC ring, which drops the tuple before the external anchor confirms it — the
// exact data loss the two-phase design exists to prevent.

namespace {
ExportTuple make_tuple(std::uint64_t seq) {
    ExportTuple t{};
    t.sequence = seq;
    t.generation = 1;
    t.time_kind = static_cast<std::uint8_t>(FrameTimeKind::ServerCorrectedUtc);
    return t;
}
}  // namespace

TEST(ExportOutboxRing, PeekDoesNotRemove) {
    ExportOutboxRing ring;
    ASSERT_TRUE(ring.try_push(make_tuple(1)));
    ASSERT_EQ(ring.size(), 1u);

    ExportTuple a{};
    ASSERT_TRUE(ring.peek_oldest(a));
    EXPECT_EQ(a.sequence, 1u);
    EXPECT_EQ(ring.size(), 1u) << "peek_oldest must NOT consume — the remote has not acked yet";

    // Peeking repeatedly keeps returning the same tuple.
    ExportTuple b{};
    ASSERT_TRUE(ring.peek_oldest(b));
    EXPECT_EQ(b.sequence, 1u);
    EXPECT_EQ(ring.size(), 1u);

    ring.pop_after_remote_ack();
    EXPECT_EQ(ring.size(), 0u);
    EXPECT_TRUE(ring.empty());
}

TEST(ExportOutboxRing, PeekOnEmptyReturnsFalse) {
    ExportOutboxRing ring;
    ExportTuple out{};
    EXPECT_FALSE(ring.peek_oldest(out));
    EXPECT_TRUE(ring.empty());
}

// Never advance past the producer: a stray pop on an empty ring must be a no-op,
// not an index corruption that makes size() underflow.
TEST(ExportOutboxRing, PopOnEmptyIsNoOp) {
    ExportOutboxRing ring;
    ring.pop_after_remote_ack();
    EXPECT_EQ(ring.size(), 0u);

    ASSERT_TRUE(ring.try_push(make_tuple(1)));
    ring.pop_after_remote_ack();
    EXPECT_EQ(ring.size(), 0u);
    ring.pop_after_remote_ack();  // extra pop, still empty
    EXPECT_EQ(ring.size(), 0u);
    EXPECT_TRUE(ring.empty());
}

TEST(ExportOutboxRing, FifoOrderAcrossPeekPopCycles) {
    ExportOutboxRing ring;
    for (std::uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(ring.try_push(make_tuple(i)));
    }
    for (std::uint64_t i = 0; i < 8; ++i) {
        ExportTuple out{};
        ASSERT_TRUE(ring.peek_oldest(out));
        EXPECT_EQ(out.sequence, i) << "outbox must drain strictly in order";
        ring.pop_after_remote_ack();
    }
    EXPECT_TRUE(ring.empty());
}

// Full ring returns false — the caller treats this as the hard-lag fence, NOT as
// permission to drop the tuple.
TEST(ExportOutboxRing, FullIsHardLagFenceNotOverwrite) {
    ExportOutboxRing ring;
    for (std::size_t i = 0; i < ExportOutboxRing::kCapacity; ++i) {
        ASSERT_TRUE(ring.try_push(make_tuple(static_cast<std::uint64_t>(i))));
    }
    EXPECT_EQ(ring.size(), ExportOutboxRing::kCapacity);
    EXPECT_FALSE(ring.try_push(make_tuple(9999)))
        << "at capacity the ring must refuse, never evict the oldest un-acked tuple";

    // The oldest entry is still intact — nothing was overwritten.
    ExportTuple out{};
    ASSERT_TRUE(ring.peek_oldest(out));
    EXPECT_EQ(out.sequence, 0u);
}

TEST(ExportOutboxRing, CapacityMatchesSpecHardLagFrames) {
    EXPECT_EQ(ExportOutboxRing::kCapacity, 256u);
}

TEST(ExportOutboxRing, WrapsAroundWithoutLosingOrder) {
    ExportOutboxRing ring;
    // Push/drain enough to wrap the modular index several times over.
    std::uint64_t next_push = 0;
    std::uint64_t next_expect = 0;
    for (int cycle = 0; cycle < 5; ++cycle) {
        for (int i = 0; i < 200; ++i) {
            ASSERT_TRUE(ring.try_push(make_tuple(next_push++)));
        }
        for (int i = 0; i < 200; ++i) {
            ExportTuple out{};
            ASSERT_TRUE(ring.peek_oldest(out));
            ASSERT_EQ(out.sequence, next_expect++);
            ring.pop_after_remote_ack();
        }
    }
    EXPECT_TRUE(ring.empty());
}

// --- is_exchange_final (docs/SPEC_INVARIANTS.md) ---

// Authoritative set is exactly {Filled, Cancelled, Rejected, Expired} — must
// exclude EscalatedToOperator (the whole reason the predicate exists, SUBMITPORT
// round 9) and must exclude Reconciled (round 24's self-inflicted bug: an earlier
// revision wrongly included it, contradicting spec §6.5).
TEST(IsExchangeFinal, MatchesAuthoritativeSetFromInvariantLedger) {
    EXPECT_TRUE(hy::is_exchange_final(OrderState::Filled));
    EXPECT_TRUE(hy::is_exchange_final(OrderState::Cancelled));
    EXPECT_TRUE(hy::is_exchange_final(OrderState::Rejected));
    EXPECT_TRUE(hy::is_exchange_final(OrderState::Expired));

    EXPECT_FALSE(hy::is_exchange_final(OrderState::EscalatedToOperator));
    EXPECT_FALSE(hy::is_exchange_final(OrderState::Reconciled));
    EXPECT_FALSE(hy::is_exchange_final(OrderState::Intent));
    EXPECT_FALSE(hy::is_exchange_final(OrderState::Submitting));
    EXPECT_FALSE(hy::is_exchange_final(OrderState::Accepted));
    EXPECT_FALSE(hy::is_exchange_final(OrderState::Ambiguous));
    EXPECT_FALSE(hy::is_exchange_final(OrderState::PartialFill));
    EXPECT_FALSE(hy::is_exchange_final(OrderState::CancelRequested));
}

// is_terminal(EscalatedToOperator) is deliberately true while
// is_exchange_final(EscalatedToOperator) is false — that gap IS the bug fix
// (SUBMITPORT round 9). Pin both sides so a future "simplification" that merges
// the two predicates back together is caught here, not in review round N+1.
TEST(IsExchangeFinal, DivergesFromIsTerminalOnEscalated) {
    EXPECT_TRUE(hy::is_terminal(OrderState::EscalatedToOperator));
    EXPECT_FALSE(hy::is_exchange_final(OrderState::EscalatedToOperator));
}

// --- InFlightRegistry capacity semantics (docs/SPEC_INVARIANTS.md) ---
//
// These tests PIN CURRENT BEHAVIOUR AS CORRECT-BUT-INCOMPLETE. They are not a
// bug report against InFlightRegistry, and the fix is emphatically NOT "release
// the slot sooner".
//
// live_submit_orchestrator.hpp:402-408 calls mark_resolved() on exactly one of
// four submit outcomes — Rejected — and that is right: Rejected is the only one
// of the four in is_exchange_final()'s set, i.e. the only one that proves the
// order never rested on the exchange. Accepted / Timeout / NetworkError orders
// keep their slot precisely so a blind resubmit stays blocked while the order
// may still be live. Releasing those early would hand the same client-order-id
// back out for an order the exchange still holds — a duplicate-order bug far
// worse than the exhaustion pinned below.
//
// The real gap is a missing component, not wrong logic: nothing in native/ ever
// drives an Accepted/Ambiguous order forward to a terminal state (there is no
// reconcile/poll loop — determine_reconcile_action() has no production caller),
// so no second mark_resolved() call site exists yet.
TEST(InFlightRegistryCapacity, ExhaustsAtKMaxInFlightAndThenFailsClosed) {
    hy::InFlightRegistry reg;
    char id[32];

    for (std::size_t i = 0; i < hy::kMaxInFlight; ++i) {
        std::snprintf(id, sizeof(id), "HY-COID-%zu", i);
        EXPECT_TRUE(reg.register_submit(id)) << "slot " << i << " should still be available";
    }
    EXPECT_EQ(reg.count(), hy::kMaxInFlight);

    std::snprintf(id, sizeof(id), "HY-COID-%zu", hy::kMaxInFlight);
    EXPECT_FALSE(reg.register_submit(id))
        << "capacity exhaustion must fail closed rather than evict a tracked order";
}

TEST(InFlightRegistryCapacity, OnlyExplicitResolutionReturnsCapacity) {
    hy::InFlightRegistry reg;
    ASSERT_TRUE(reg.register_submit("HY-A"));
    ASSERT_TRUE(reg.register_submit("HY-B"));
    EXPECT_EQ(reg.count(), 2u);

    // Stands in for the orchestrator's Rejected branch — the only outcome of the
    // four that calls mark_resolved() today.
    reg.mark_resolved("HY-A");
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_FALSE(reg.is_in_flight("HY-A"));

    // Stands in for Accepted / Timeout / NetworkError: correctly still tracked,
    // and with no driver to resolve it, tracked forever.
    EXPECT_TRUE(reg.is_in_flight("HY-B"));

    EXPECT_TRUE(reg.register_submit("HY-A"));
    EXPECT_EQ(reg.count(), 2u);
}

// --- InFlightHandle / generation (order_tracker.hpp's cross-thread release path) ---
//
// register_submit()/mark_resolved() (COID-string-only) remain correct and
// sufficient for Gate 12b's synchronous call site, which has no cross-thread
// staleness window. The handle-returning pair exists specifically for
// order_tracker.hpp's reconcile thread, which learns about a resolution at some
// later, independently-scheduled time -- generation is what lets a stale handle
// be detected instead of silently matching whatever now occupies that slot index.

TEST(InFlightHandle, RegisterSubmitHandleReturnsValidHandle) {
    hy::InFlightRegistry reg;
    auto h = reg.register_submit_handle("HY-A");
    EXPECT_TRUE(h.valid());
    EXPECT_LT(h.slot_index, hy::kMaxInFlight);
}

TEST(InFlightHandle, InvalidOnEmptyCoidOrDuplicateOrCapacityExhausted) {
    hy::InFlightRegistry reg;
    EXPECT_FALSE(reg.register_submit_handle("").valid());

    auto first = reg.register_submit_handle("HY-DUP");
    ASSERT_TRUE(first.valid());
    EXPECT_FALSE(reg.register_submit_handle("HY-DUP").valid()) << "already in-flight";

    char id[32];
    for (std::size_t i = 1; i < hy::kMaxInFlight; ++i) {
        std::snprintf(id, sizeof(id), "HY-FILL-%zu", i);
        ASSERT_TRUE(reg.register_submit_handle(id).valid());
    }
    EXPECT_EQ(reg.count(), hy::kMaxInFlight);
    EXPECT_FALSE(reg.register_submit_handle("HY-ONE-TOO-MANY").valid());
}

TEST(InFlightHandle, MarkResolvedHandleReleasesOnExactMatch) {
    hy::InFlightRegistry reg;
    auto h = reg.register_submit_handle("HY-A");
    ASSERT_TRUE(h.valid());
    EXPECT_TRUE(reg.mark_resolved_handle(h, "HY-A"));
    EXPECT_FALSE(reg.is_in_flight("HY-A"));
    EXPECT_EQ(reg.count(), 0u);
}

TEST(InFlightHandle, MismatchedCoidDoesNotRelease) {
    hy::InFlightRegistry reg;
    auto h = reg.register_submit_handle("HY-A");
    ASSERT_TRUE(h.valid());
    // Same slot_index/generation, wrong coid -- must not release someone else's
    // (hypothetical) tracked order under a mismatched string.
    EXPECT_FALSE(reg.mark_resolved_handle(h, "HY-NOT-A"));
    EXPECT_TRUE(reg.is_in_flight("HY-A"));
}

// The scenario the generation field exists to prevent: a handle issued for an
// order that has ALREADY been resolved and whose slot has since been reused for
// a different order must not release the new occupant, even if (hypothetically)
// the new occupant happened to reuse the exact same coid string.
TEST(InFlightHandle, StaleGenerationDoesNotReleaseReusedSlot) {
    hy::InFlightRegistry reg;
    auto stale = reg.register_submit_handle("HY-A");
    ASSERT_TRUE(stale.valid());
    reg.mark_resolved("HY-A");  // released via the ordinary COID path
    ASSERT_FALSE(reg.is_in_flight("HY-A"));

    // Slot reused (likely the same slot_index, since it's now the only free one
    // in a small registry) for a genuinely different order under the same coid
    // string -- a caller bug (non-unique coid), but exactly the case generation
    // exists to defend against.
    auto fresh = reg.register_submit_handle("HY-A");
    ASSERT_TRUE(fresh.valid());

    // The stale handle must fail even though the coid string matches again.
    EXPECT_FALSE(reg.mark_resolved_handle(stale, "HY-A"))
        << "a handle from a previous occupant of this slot must never release "
           "the current occupant, even under coid string reuse";
    EXPECT_TRUE(reg.is_in_flight("HY-A")) << "the fresh order must still be tracked";

    // The fresh handle, correctly, still works.
    EXPECT_TRUE(reg.mark_resolved_handle(fresh, "HY-A"));
    EXPECT_FALSE(reg.is_in_flight("HY-A"));
}

TEST(InFlightHandle, InvalidHandleNeverReleasesAnything) {
    hy::InFlightRegistry reg;
    ASSERT_TRUE(reg.register_submit("HY-A"));
    hy::InFlightHandle invalid{};
    EXPECT_FALSE(invalid.valid());
    EXPECT_FALSE(reg.mark_resolved_handle(invalid, "HY-A"));
    EXPECT_TRUE(reg.is_in_flight("HY-A"));
}
