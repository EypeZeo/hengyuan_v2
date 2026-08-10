// Seal-journal Round B (轨道 C, round 2 of 3) -- the "Started quintet":
// SealExportStartedWire (alias SealExportStarted), SealExportStartedMigrationWire,
// SealStartedCleanupTombstoneWire, SealStartedAbandonWire. Split out from
// test_durable_control_plane_seal_journal_abi.cpp (Round A) for the same
// reason every other round in this family got its own file: a clean,
// self-contained vertical slice maps naturally to its own file.
//
// Round C (future, deferred): CompactionCandidateIntentWire,
// CompactionIntentTransitionWire, CompactionIntentGcAuthorizedWire (confirmed
// by research to not subdivide further).
//
// Round E Slice 1 (docs/SPEC_INVARIANTS.md's "Seal-journal Round E Slice 1"
// entry) promotes SealExportStartedWire from mac[32]-only marker to real
// named fields + a real codec (seal_journal_precondition_codec.hpp). Round E
// Slice 2b (docs/SPEC_INVARIANTS.md's "Seal-journal Round E Slice 2b" entry)
// does the same for the remaining three types in this file --
// SealExportStartedMigrationWire/SealStartedCleanupTombstoneWire/
// SealStartedAbandonWire -- with encode/decode living in
// seal_export_migration_cleanup_abandon_codec.hpp. All four types in this
// file are therefore real named fields as of Slice 2b, none are mac[32]-only
// markers anymore.
//
// See test_durable_control_plane_abi.cpp's own header comment for the full
// caution about what these tests can and cannot prove -- short version: only
// tools/spec_enum_diff.py proves a type was transcribed from the spec, not
// from this file being green. This round adds zero new enums (phase/
// started_kind/abandon_reason are free constexpr std::uint8_t constants, not
// enum class -- see durable_control_plane.hpp's Round B banner comment), so
// that tool has nothing new to check either -- the compensating check for
// everything here is the static_asserts and exhaustive constant-value test
// below.
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/durable_control_plane.hpp>
#include <cstdint>
#include <type_traits>

using hy::kSealExportStartedDraft234Bytes;
using hy::kSealExportStartedFormatVersion;
using hy::kSealExportStartedLegacyV1Bytes;
using hy::kSealExportStartedMigrationDraft136Bytes;
using hy::kSealExportStartedMigrationFormatVersion;
using hy::kSealExportStartedMigrationWireBytes;
using hy::kSealExportStartedWireBytes;
using hy::kSealStartedAbandonFormatVersion;
using hy::kSealStartedAbandonPhaseAbdPending;
using hy::kSealStartedAbandonPhaseAuthorized;
using hy::kSealStartedAbandonPhaseCGone;
using hy::kSealStartedAbandonPhaseGenGone;
using hy::kSealStartedAbandonPhaseLGone;
using hy::kSealStartedAbandonPhaseMGone;
using hy::kSealStartedAbandonPhaseResumeAuthorized;
using hy::kSealStartedAbandonPhaseVGone;
using hy::kSealStartedAbandonReasonNotFound;
using hy::kSealStartedAbandonWireBytes;
using hy::kSealStartedCleanupDraft176Bytes;
using hy::kSealStartedCleanupFormatVersion;
using hy::kSealStartedCleanupPhaseAuthorized;
using hy::kSealStartedCleanupPhaseClrPending;
using hy::kSealStartedCleanupPhaseLGone;
using hy::kSealStartedCleanupPhaseMGone;
using hy::kSealStartedCleanupPhaseVGone;
using hy::kSealStartedCleanupWireBytes;
using hy::kSealStartedKindMigratedV2;
using hy::kSealStartedKindNativeV2;
using hy::SealExportStarted;
using hy::SealExportStartedMigrationWire;
using hy::SealExportStartedWire;
using hy::SealStartedAbandonWire;
using hy::SealStartedCleanupTombstoneWire;

// All four types in this file are real named fields as of Round E Slice 2b
// (SealExportStartedWire since Slice 1). Host-struct sizeof is NOT the wire
// byte count for any of them (u32-then-u64 field ordering means normal C++
// alignment inserts padding) and must never be asserted as such -- only
// is_trivially_copyable_v/is_standard_layout_v and each type's own
// kXxxWireBytes constant (the wire byte count the respective codec pins
// against) are checked below.

// --- SealExportStartedWire (spec L4 §10 / BINANCE:3185) ---

TEST(SealExportStartedWire, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<SealExportStartedWire>);
    static_assert(std::is_standard_layout_v<SealExportStartedWire>);
    SUCCEED();
}

TEST(SealExportStartedWire, ShapeIsJustTheTrailerMac) {
    EXPECT_EQ(kSealExportStartedFormatVersion, 2u);
    EXPECT_EQ(kSealExportStartedWireBytes, 238u);
    EXPECT_EQ(kSealExportStartedLegacyV1Bytes, 192u);
    EXPECT_EQ(kSealExportStartedDraft234Bytes, 234u);
}

// Alias used in prose: SealExportStarted == SealExportStartedWire. Not a
// second type -- confirm the compiler agrees.
TEST(SealExportStarted, IsSameTypeAsSealExportStartedWire) {
    static_assert(std::is_same_v<SealExportStarted, SealExportStartedWire>);
    SUCCEED();
}

// --- SealExportStartedMigrationWire (spec L4 §10 / BINANCE:3224) ---

TEST(SealExportStartedMigrationWire, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<SealExportStartedMigrationWire>);
    static_assert(std::is_standard_layout_v<SealExportStartedMigrationWire>);
    SUCCEED();
}

TEST(SealExportStartedMigrationWire, WireByteConstantMatchesSpec) {
    EXPECT_EQ(kSealExportStartedMigrationFormatVersion, 2u);
    EXPECT_EQ(kSealExportStartedMigrationWireBytes, 208u);
    EXPECT_EQ(kSealExportStartedMigrationDraft136Bytes, 136u);
}

// --- SealStartedCleanupTombstoneWire (spec L4 §10 / BINANCE:3278) ---

TEST(SealStartedCleanupTombstoneWire, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<SealStartedCleanupTombstoneWire>);
    static_assert(std::is_standard_layout_v<SealStartedCleanupTombstoneWire>);
    SUCCEED();
}

TEST(SealStartedCleanupTombstoneWire, WireByteConstantMatchesSpec) {
    EXPECT_EQ(kSealStartedCleanupFormatVersion, 2u);
    EXPECT_EQ(kSealStartedCleanupWireBytes, 304u);
    EXPECT_EQ(kSealStartedCleanupDraft176Bytes, 176u);
}

// phase is monotonic (Authorized -> MGone -> VGone -> LGone -> ClrPending) --
// exhaustive over all 5 values, not a sample.
TEST(SealStartedCleanupPhase, MatchesSpecOrderingExhaustively) {
    EXPECT_EQ(kSealStartedCleanupPhaseAuthorized, 0u);
    EXPECT_EQ(kSealStartedCleanupPhaseMGone, 1u);
    EXPECT_EQ(kSealStartedCleanupPhaseVGone, 2u);
    EXPECT_EQ(kSealStartedCleanupPhaseLGone, 3u);
    EXPECT_EQ(kSealStartedCleanupPhaseClrPending, 4u);
}

// --- SealStartedAbandonWire (spec L4 §10 / BINANCE:3326) ---

TEST(SealStartedAbandonWire, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<SealStartedAbandonWire>);
    static_assert(std::is_standard_layout_v<SealStartedAbandonWire>);
    SUCCEED();
}

TEST(SealStartedAbandonWire, WireByteConstantMatchesSpec) {
    EXPECT_EQ(kSealStartedAbandonFormatVersion, 1u);
    EXPECT_EQ(kSealStartedAbandonWireBytes, 192u);
    EXPECT_EQ(kSealStartedAbandonReasonNotFound, 1u);
}

// phase is monotonic (Authorized -> CGone -> MGone -> VGone -> LGone ->
// GenGone -> ResumeAuthorized -> AbdPending) -- exhaustive over all 8 values.
TEST(SealStartedAbandonPhase, MatchesSpecOrderingExhaustively) {
    EXPECT_EQ(kSealStartedAbandonPhaseAuthorized, 0u);
    EXPECT_EQ(kSealStartedAbandonPhaseCGone, 1u);
    EXPECT_EQ(kSealStartedAbandonPhaseMGone, 2u);
    EXPECT_EQ(kSealStartedAbandonPhaseVGone, 3u);
    EXPECT_EQ(kSealStartedAbandonPhaseLGone, 4u);
    EXPECT_EQ(kSealStartedAbandonPhaseGenGone, 5u);
    EXPECT_EQ(kSealStartedAbandonPhaseResumeAuthorized, 6u);
    EXPECT_EQ(kSealStartedAbandonPhaseAbdPending, 7u);
}

// --- started_kind (shared by Cleanup and Abandon, spec L4 §10 / BINANCE:3267-3268) ---

TEST(SealStartedKind, MatchesSpec) {
    EXPECT_EQ(kSealStartedKindNativeV2, 1u);
    EXPECT_EQ(kSealStartedKindMigratedV2, 2u);
}
