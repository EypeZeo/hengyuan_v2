// Seal-journal Round A (轨道 C, round 1 of 3) -- the id/journal-housekeeping
// cluster of the seal-journal/.xgc family: SealIdWatermark, SealJournalCommit
// Watermark, SealJournalIntakeCloseControl (+SealJournalIntakeCloseProducerSlot),
// SealJournalTombstoneWire, SealJournalOriginKey, is_seal_journal_embeddable_type,
// and the related constants. Split out from test_durable_control_plane_abi.cpp
// / test_durable_control_plane_freeze_abi.cpp / test_durable_control_plane_
// sink_interface_abi.cpp for the same reason those files split: a clean,
// self-contained vertical slice maps naturally to its own file.
//
// Round B (future, deferred): SealExportStartedWire, SealExportStartedMigrationWire,
// SealStartedCleanupTombstoneWire, SealStartedAbandonWire. Round C (future,
// deferred): CompactionCandidateIntentWire, CompactionIntentTransitionWire,
// CompactionIntentGcAuthorizedWire.
//
// SealJournalAppliedView's own shape is exercised in test_durable_control_
// plane_sink_interface_abi.cpp (it's now a real type, constructed and passed
// through DurableControlPlaneSink::append_seal_journal_apply there) -- not
// duplicated here.
//
// See test_durable_control_plane_abi.cpp's own header comment for the full
// caution about what these tests can and cannot prove -- short version: only
// tools/spec_enum_diff.py proves a type was transcribed from the spec, not
// from this file being green. This round adds zero new enums, so that tool
// has nothing new to check either -- the compensating check for everything
// here is the static_asserts and exhaustive-allowlist test below.
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/durable_control_plane.hpp>
#include <cstdint>
#include <type_traits>

using hy::DurableRecordType;
using hy::is_seal_journal_embeddable_type;
using hy::kMaxSealHandoffProducers;
using hy::kSealJournalFixedMetaBytes;
using hy::kSealJournalFormatVersion;
using hy::kSealJournalIntakeCloseDeadlineMs;
using hy::kSealJournalIntakeCloseMaxPollIters;
using hy::kSealJournalMaxEmbeddedBytes;
using hy::kSealJournalMaxEntryBytes;
using hy::kSealJournalTombstoneBytes;
using hy::kSealJournalTombstoneFormatVersion;
using hy::SealIdWatermark;
using hy::SealJournalCommitWatermark;
using hy::SealJournalIntakeCloseControl;
using hy::SealJournalIntakeCloseProducerSlot;
using hy::SealJournalOriginKey;
using hy::SealJournalTombstoneWire;

// --- SealIdWatermark (spec L4 §10 / BINANCE:3009) ---

TEST(SealIdWatermark, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<SealIdWatermark>);
    static_assert(std::is_standard_layout_v<SealIdWatermark>);
    SUCCEED();
}

// Spec: "next_* is the NEXT allocatable value" -- a default-constructed
// watermark must already start at the first legal id, not at 0 (which would
// be an already-consumed-looking id).
TEST(SealIdWatermark, DefaultNextIdsAreOneNotZero) {
    SealIdWatermark w{};
    EXPECT_EQ(w.next_candidate_id, 1u);
    EXPECT_EQ(w.next_request_id, 1u);
}

// --- SealJournalCommitWatermark (spec L4 §10 / BINANCE:3028) ---
// All-zero defaults are legitimate here (candidate_id/highest_committed_
// journal_seq/kek_key_id all start at 0 per spec) -- no extra default-value
// test warranted, noted explicitly rather than silently omitted.

TEST(SealJournalCommitWatermark, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<SealJournalCommitWatermark>);
    static_assert(std::is_standard_layout_v<SealJournalCommitWatermark>);
    SUCCEED();
}

// --- SealJournalTombstoneWire (spec L4 §10 / BINANCE:3116) ---

TEST(SealJournalTombstoneWire, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<SealJournalTombstoneWire>);
    static_assert(std::is_standard_layout_v<SealJournalTombstoneWire>);
    SUCCEED();
}

TEST(SealJournalTombstoneWire, ShapeIsJustTheTrailerMac) {
    // Unlike a full wire-layout struct, this type's only C++ member is the
    // trailing MAC (the packed 108-byte on-disk layout is documented in a
    // comment, not materialized as fields) -- sizeof is meaningful here.
    EXPECT_EQ(sizeof(SealJournalTombstoneWire), 32u);
    EXPECT_EQ(kSealJournalTombstoneFormatVersion, 1u);
    EXPECT_EQ(kSealJournalTombstoneBytes, 108u);
}

// --- SealJournalIntakeCloseProducerSlot / SealJournalIntakeCloseControl
//     (spec L4 §10 / BINANCE:3058, 3065) ---
// RAM control block, NOT durable -- atomic-bearing, so the inverse of the
// usual POD treatment: NOT trivially copyable, non-copyable, non-movable.

TEST(SealJournalIntakeCloseProducerSlot, AtomicBearingShape) {
    // NOT asserting is_trivially_copyable_v/is_standard_layout_v either way:
    // both are real MSVC-vs-GCC divergences for this type, confirmed by WSL2
    // verification. is_standard_layout_v fails on GCC (libstdc++'s
    // std::atomic<T> is not itself standard-layout, unlike MSVC's).
    // is_trivially_copyable_v disagrees the other direction: with all four
    // copy/move special members explicitly deleted, GCC's intrinsic reports
    // trivially-copyable=true, while the standard's "at least one of
    // copy/move ctor/assign must be non-deleted" wording (matched by MSVC)
    // says it should be false. What IS portable and directly checkable is
    // non-copyability/non-movability themselves, asserted below -- matching
    // ExportOutboxRing's own precedent of not asserting copyability traits
    // for atomic-bearing RAM types at all.
    static_assert(std::is_default_constructible_v<SealJournalIntakeCloseProducerSlot>);
    static_assert(!std::is_copy_constructible_v<SealJournalIntakeCloseProducerSlot>);
    static_assert(!std::is_move_constructible_v<SealJournalIntakeCloseProducerSlot>);
    SUCCEED();
}

TEST(SealJournalIntakeCloseProducerSlot, DefaultConstructionIsZeroed) {
    SealJournalIntakeCloseProducerSlot slot;
    EXPECT_EQ(slot.quiesced_ack_epoch.load(), 0u);
}

TEST(SealJournalIntakeCloseControl, AtomicBearingShape) {
    // See the identical note on SealJournalIntakeCloseProducerSlot's test
    // above -- same is_trivially_copyable_v/is_standard_layout_v divergences
    // apply here (this type embeds two std::atomic members directly plus an
    // array of that slot type).
    static_assert(std::is_default_constructible_v<SealJournalIntakeCloseControl>);
    static_assert(!std::is_copy_constructible_v<SealJournalIntakeCloseControl>);
    static_assert(!std::is_move_constructible_v<SealJournalIntakeCloseControl>);
    SUCCEED();
}

TEST(SealJournalIntakeCloseControl, DefaultConstructionIsZeroed) {
    SealJournalIntakeCloseControl ctrl;
    EXPECT_EQ(ctrl.candidate_id, 0u);
    EXPECT_EQ(ctrl.registered_producer_mask, 0u);
    EXPECT_EQ(ctrl.producer_count, 0u);
    EXPECT_FALSE(ctrl.topology_frozen);
    EXPECT_EQ(ctrl.close_epoch.load(), 0u);
    EXPECT_EQ(ctrl.in_flight_admit_guard.load(), 0u);
    EXPECT_FALSE(ctrl.path_b_prohibited);
}

// --- SealJournalOriginKey (spec L4 §10 / BINANCE:4363) ---

TEST(SealJournalOriginKey, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<SealJournalOriginKey>);
    static_assert(std::is_standard_layout_v<SealJournalOriginKey>);
    SUCCEED();
}

TEST(SealJournalOriginKey, DefaultConstructionIsZeroed) {
    SealJournalOriginKey key{};
    EXPECT_EQ(key.candidate_id, 0u);
    EXPECT_EQ(key.journal_seq, 0u);
}

// --- Constants (spec L4 §10 / BINANCE:3050-3057, 4448-4452) ---
// Zero new enums this round -- spec_enum_diff.py has nothing to check here;
// these constant values are the compensating manual pin.

TEST(SealJournalConstants, MatchSpec) {
    EXPECT_EQ(kMaxSealHandoffProducers, 8u);
    EXPECT_EQ(kSealJournalIntakeCloseDeadlineMs, 5'000u);
    EXPECT_EQ(kSealJournalIntakeCloseMaxPollIters, 1'000'000u);
    EXPECT_EQ(kSealJournalFormatVersion, 1u);
    EXPECT_EQ(kSealJournalFixedMetaBytes, 138u);
    EXPECT_EQ(kSealJournalMaxEmbeddedBytes, 4096u);
    EXPECT_EQ(kSealJournalMaxEntryBytes, 4234u);
}

// --- is_seal_journal_embeddable_type (spec L4 §10 / BINANCE:4373) ---
// Exhaustive over all 17 DurableRecordType values, not a sample -- this is a
// closed allowlist gate and a missed case is exactly the kind of silent
// invented-vs-transcribed drift this test suite exists to catch.

TEST(IsSealJournalEmbeddableType, MatchesSpecAllowlistExhaustively) {
    EXPECT_TRUE(is_seal_journal_embeddable_type(DurableRecordType::OrderEvent));
    EXPECT_FALSE(is_seal_journal_embeddable_type(DurableRecordType::SymbolRegistrySnapshot));
    EXPECT_TRUE(is_seal_journal_embeddable_type(DurableRecordType::RateLimitFreeze));
    EXPECT_FALSE(is_seal_journal_embeddable_type(DurableRecordType::TransportFailover));
    EXPECT_FALSE(is_seal_journal_embeddable_type(DurableRecordType::EndpointWeightConfigSet));
    EXPECT_FALSE(is_seal_journal_embeddable_type(DurableRecordType::RateLimitUsageSnapshot));
    EXPECT_FALSE(is_seal_journal_embeddable_type(DurableRecordType::OperatorOverride));
    EXPECT_FALSE(is_seal_journal_embeddable_type(DurableRecordType::GenerationBridge));
    EXPECT_TRUE(is_seal_journal_embeddable_type(DurableRecordType::OrderCheckpoint));
    EXPECT_TRUE(is_seal_journal_embeddable_type(DurableRecordType::FreezeProbeAttempt));
    EXPECT_TRUE(is_seal_journal_embeddable_type(DurableRecordType::FreezeEpochWatermark));
    EXPECT_TRUE(is_seal_journal_embeddable_type(DurableRecordType::FreezeClear));
    EXPECT_TRUE(is_seal_journal_embeddable_type(DurableRecordType::FreezeWaitSatisfied));
    EXPECT_TRUE(is_seal_journal_embeddable_type(DurableRecordType::FreezeWaitArm));
    EXPECT_FALSE(is_seal_journal_embeddable_type(DurableRecordType::RateLimitFreezeSnapshot));
    EXPECT_FALSE(is_seal_journal_embeddable_type(DurableRecordType::CompactedFreezeWaitEvidence));
    EXPECT_FALSE(is_seal_journal_embeddable_type(DurableRecordType::SealJournalApplied));
}
