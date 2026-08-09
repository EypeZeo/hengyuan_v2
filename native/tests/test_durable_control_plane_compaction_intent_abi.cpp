// Seal-journal Round C (轨道 C, round 3 of 3 -- LAST round) -- the
// CompactionCandidateIntent GC family: CompactionCandidateIntentWire,
// CompactionIntentTransitionWire, CompactionIntentGcAuthorizedWire. Split out
// from test_durable_control_plane_seal_journal_started_abi.cpp (Round B) for
// the same reason every other round in this family got its own file.
//
// This completes the 3-round seal-journal split -- every type L4 §10 names
// is now transcribed (the only thing left out is the crash-window table and
// §10.1/10.2/10.3 procedural prose, which contain zero named types and stay
// permanently excluded).
//
// SCOPE (updated for Round D, docs/SPEC_INVARIANTS.md's "Seal-journal Round
// D" entry): as of Round C these three types were mac[32]-only markers and
// every test below was purely a compile-time shape/constant check. Round D
// promotes them to real, readable fields and adds a real codec
// (compaction_intent_codec.hpp) -- see test_compaction_intent_codec.cpp for
// the round-trip/tamper/semantic-validation coverage that file now owns.
// This file keeps only what it originally covered well: is_trivially_
// copyable/is_standard_layout, and the free constexpr constants (format
// versions, phase/disposition/flag values) that were never struct members
// to begin with. The `sizeof(...)==32u` assertions are gone -- they only
// ever measured the marker-only shape (a lone trailing mac[32]), and now
// that these structs carry real fields, sizeof is no longer pinned to wire
// byte count (this file's own established rule for every other Wire type:
// in-memory layout is never asserted equal to on-disk wire size -- the wire
// byte count is proven by the codec's encode() return value and round-trip
// tests, not by sizeof(struct)).
//
// Combination-validity tests (e.g. "does disposition X require flag
// combination Y"), fault-injection crash-matrix tests, candidate-ownership/
// concurrency tests, and Windows/POSIX no-replace-publish tests all live in
// test_compaction_intent_codec.cpp / test_compaction_lease.cpp /
// test_compaction_intent_store.cpp, not here.
//
// See test_durable_control_plane_abi.cpp's own header comment for the full
// caution about what these tests can and cannot prove -- short version: only
// tools/spec_enum_diff.py proves a type was transcribed from the spec, not
// from this file being green. This round adds zero new enums (disposition/
// flags/phase are free constexpr constants, not enum class), so that tool
// has nothing new to check either.
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/durable_control_plane.hpp>
#include <cstdint>
#include <type_traits>

using hy::kCompactionCandidateIntentFormatVersion;
using hy::kCompactionCandidateIntentPhaseAbandonFinalizing;
using hy::kCompactionCandidateIntentPhaseBuilding;
using hy::kCompactionCandidateIntentPhasePostSealFinalizing;
using hy::kCompactionCandidateIntentPhaseReserved;
using hy::kCompactionCandidateIntentPhaseStartedPublished;
using hy::kCompactionCandidateIntentWireBytes;
using hy::kCompactionIntentGcAuthFlagGateAbsentAtCreate;
using hy::kCompactionIntentGcAuthFlagGenGone;
using hy::kCompactionIntentGcAuthFlagJournalDrain;
using hy::kCompactionIntentGcAuthFlagPostSealBound;
using hy::kCompactionIntentGcAuthFlagResumeAuthorized;
using hy::kCompactionIntentGcAuthorizedFormatVersion;
using hy::kCompactionIntentGcAuthorizedLegacyV1Bytes;
using hy::kCompactionIntentGcAuthorizedWireBytes;
using hy::kCompactionIntentGcDispositionAbandonFinalizingClear;
using hy::kCompactionIntentGcDispositionPostSealFinalizingClear;
using hy::kCompactionIntentGcDispositionPreSealAbandonClear;
using hy::kCompactionIntentTransitionFormatVersion;
using hy::kCompactionIntentTransitionWireBytes;
using hy::CompactionCandidateIntentWire;
using hy::CompactionIntentGcAuthorizedWire;
using hy::CompactionIntentTransitionWire;

// All three types below now carry real fields (Round D) -- the shape/layout
// checks here are is_trivially_copyable/is_standard_layout plus the free
// constexpr constants; see this file's header comment for why sizeof is not
// asserted against wire byte count.

// --- CompactionCandidateIntentWire (spec L4 §10 / BINANCE:3375) ---

TEST(CompactionCandidateIntentWire, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<CompactionCandidateIntentWire>);
    static_assert(std::is_standard_layout_v<CompactionCandidateIntentWire>);
    SUCCEED();
}

TEST(CompactionCandidateIntentWire, FormatVersionAndWireByteConstantsMatchSpec) {
    EXPECT_EQ(kCompactionCandidateIntentFormatVersion, 1u);
    EXPECT_EQ(kCompactionCandidateIntentWireBytes, 140u);
}

// phase is a third, independent state machine from Round B's two
// SealStartedCleanupTombstoneWire/SealStartedAbandonWire phase enumerations
// -- exhaustive over all 5 values, not a sample.
TEST(CompactionCandidateIntentPhase, MatchesSpecOrderingExhaustively) {
    EXPECT_EQ(kCompactionCandidateIntentPhaseBuilding, 0u);
    EXPECT_EQ(kCompactionCandidateIntentPhaseReserved, 1u);
    EXPECT_EQ(kCompactionCandidateIntentPhaseStartedPublished, 2u);
    EXPECT_EQ(kCompactionCandidateIntentPhasePostSealFinalizing, 3u);
    EXPECT_EQ(kCompactionCandidateIntentPhaseAbandonFinalizing, 4u);
}

// --- CompactionIntentTransitionWire (.x1, spec L4 §10 / BINANCE:3494) ---

TEST(CompactionIntentTransitionWire, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<CompactionIntentTransitionWire>);
    static_assert(std::is_standard_layout_v<CompactionIntentTransitionWire>);
    SUCCEED();
}

TEST(CompactionIntentTransitionWire, FormatVersionAndWireByteConstantsMatchSpec) {
    EXPECT_EQ(kCompactionIntentTransitionFormatVersion, 1u);
    EXPECT_EQ(kCompactionIntentTransitionWireBytes, 176u);
}

// --- CompactionIntentGcAuthorizedWire (.xgc, spec L4 §10 / BINANCE:3602) ---

TEST(CompactionIntentGcAuthorizedWire, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<CompactionIntentGcAuthorizedWire>);
    static_assert(std::is_standard_layout_v<CompactionIntentGcAuthorizedWire>);
    SUCCEED();
}

TEST(CompactionIntentGcAuthorizedWire, FormatVersionAndWireByteConstantsMatchSpec) {
    EXPECT_EQ(kCompactionIntentGcAuthorizedFormatVersion, 2u);
    EXPECT_EQ(kCompactionIntentGcAuthorizedWireBytes, 316u);
    // Legacy v1/204B is fail-closed only, never an active write format --
    // pinning the number here is what catches a future accidental "restore
    // Mode B for legacy" regression.
    EXPECT_EQ(kCompactionIntentGcAuthorizedLegacyV1Bytes, 204u);
}

TEST(CompactionIntentGcDisposition, MatchesSpecExhaustively) {
    EXPECT_EQ(kCompactionIntentGcDispositionPreSealAbandonClear, 0u);
    EXPECT_EQ(kCompactionIntentGcDispositionPostSealFinalizingClear, 1u);
    EXPECT_EQ(kCompactionIntentGcDispositionAbandonFinalizingClear, 2u);
}

// cleanup_auth_flags is a BITFLAG set, not a sequential enum -- pin the exact
// bit position of each flag (not just its decimal value) and confirm the 5
// bits are pairwise disjoint. This is the one place a transcription slip
// (wrong shift amount) would be easy to make and easy to miss by eye.
//
// NOT tested here (needs a real codec, see file header): whether a given
// disposition's *actual* encoded flags obey the spec's required combination
// (e.g. PostSealFinalizingClear must set JournalDrain|PostSealBound) -- that
// requires a real, readable cleanup_auth_flags field, which this
// mac[32]-only struct does not have.
TEST(CompactionIntentGcAuthFlags, BitPositionsMatchSpecAndArePairwiseDisjoint) {
    EXPECT_EQ(kCompactionIntentGcAuthFlagJournalDrain, 0x01u);
    EXPECT_EQ(kCompactionIntentGcAuthFlagPostSealBound, 0x02u);
    EXPECT_EQ(kCompactionIntentGcAuthFlagGenGone, 0x04u);
    EXPECT_EQ(kCompactionIntentGcAuthFlagResumeAuthorized, 0x08u);
    EXPECT_EQ(kCompactionIntentGcAuthFlagGateAbsentAtCreate, 0x10u);

    const std::uint8_t all_flags[] = {
        kCompactionIntentGcAuthFlagJournalDrain,
        kCompactionIntentGcAuthFlagPostSealBound,
        kCompactionIntentGcAuthFlagGenGone,
        kCompactionIntentGcAuthFlagResumeAuthorized,
        kCompactionIntentGcAuthFlagGateAbsentAtCreate,
    };
    std::uint8_t seen = 0;
    for (const std::uint8_t flag : all_flags) {
        EXPECT_EQ(seen & flag, 0u) << "flag " << static_cast<int>(flag)
                                    << " overlaps an earlier flag";
        seen |= flag;
    }
    EXPECT_EQ(seen, 0x1Fu);  // exactly the low 5 bits, bits 5..7 unused
}
