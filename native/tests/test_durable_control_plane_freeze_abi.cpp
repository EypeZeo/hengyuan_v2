// Freeze-episode slice of L4 §10's mechanization (轨道 C). Split out from
// test_durable_control_plane_abi.cpp rather than extending it in place --
// same reasoning as that file's own split-out precedent for
// test_durable_frame_codec.cpp: a clean, self-contained vertical slice maps
// naturally to its own file.
//
// See test_durable_control_plane_abi.cpp's own header comment for the full
// caution about what these tests can and cannot prove -- it applies
// identically here and isn't restated to avoid two copies drifting apart.
// Short version: only tools/spec_enum_diff.py proves a type was transcribed
// from the spec, not from this file being green.
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/durable_control_plane.hpp>
#include <cstdint>
#include <type_traits>

using hy::CompactedFreezeWaitEvidencePayload;
using hy::FreezeClearKind;
using hy::FreezeClearPayload;
using hy::FreezeEpochWatermarkPayload;
using hy::FreezeProbeAttemptPayload;
using hy::FreezeProbePurpose;
using hy::FreezeTimeProbeProof;
using hy::FreezeWaitArmPayload;
using hy::FreezeWaitSatisfiedPayload;
using hy::RateLimitFreezePayload;
using hy::SealQueryStatus;

// --- SealQueryStatus (spec L4 §10 / BINANCE:2501) ---

TEST(SealQueryStatusWireValues, MatchSpecNumbering) {
    EXPECT_EQ(static_cast<std::uint8_t>(SealQueryStatus::Found), 0u);
    EXPECT_EQ(static_cast<std::uint8_t>(SealQueryStatus::NotFound), 1u);
    EXPECT_EQ(static_cast<std::uint8_t>(SealQueryStatus::TransportUnavailable), 2u);
    EXPECT_EQ(static_cast<std::uint8_t>(SealQueryStatus::Corrupt), 3u);
}

// --- FreezeClearKind (spec L4 §10 / BINANCE:2570) ---

TEST(FreezeClearKindWireValues, MatchSpecNumbering) {
    EXPECT_EQ(static_cast<std::uint8_t>(FreezeClearKind::ProbeVerified), 0u);
    EXPECT_EQ(static_cast<std::uint8_t>(FreezeClearKind::ConservativeWaitCompleted), 1u);
    EXPECT_EQ(static_cast<std::uint8_t>(FreezeClearKind::OperatorAuthorized), 2u);
}

// --- FreezeProbePurpose (spec L4 §10 / BINANCE:2616) ---

TEST(FreezeProbePurposeWireValues, MatchSpecNumbering) {
    EXPECT_EQ(static_cast<std::uint8_t>(FreezeProbePurpose::DeadlineOrVerify), 0u);
    EXPECT_EQ(static_cast<std::uint8_t>(FreezeProbePurpose::ClockRepublishOrVerify), 1u);
}

// --- RateLimitFreezePayload (spec L4 §10 / BINANCE:2576) ---

TEST(RateLimitFreezePayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<RateLimitFreezePayload>);
    static_assert(std::is_standard_layout_v<RateLimitFreezePayload>);
    SUCCEED();
}

TEST(RateLimitFreezePayload, DefaultConstructionIsZeroed) {
    RateLimitFreezePayload p{};
    EXPECT_EQ(p.recorded_utc_ms, 0);
    EXPECT_EQ(p.deadline_utc_ms, 0);
    EXPECT_EQ(p.conservative_wait_ms, 0);
    EXPECT_EQ(p.source, 0u);
    EXPECT_EQ(p.freeze_epoch, 0u);
    EXPECT_EQ(p.wait_generation, 0u);
}

// --- FreezeProbeAttemptPayload (spec L4 §10 / BINANCE:2623) ---

TEST(FreezeProbeAttemptPayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<FreezeProbeAttemptPayload>);
    static_assert(std::is_standard_layout_v<FreezeProbeAttemptPayload>);
    SUCCEED();
}

// Spec: "MUST be false on append -- sink rejects true." A default-constructed
// payload (the shape an honest append site would start from) must not
// already violate that rule.
TEST(FreezeProbeAttemptPayload, DefaultClearedIsFalse) {
    FreezeProbeAttemptPayload p{};
    EXPECT_FALSE(p.cleared);
    EXPECT_EQ(p.purpose, FreezeProbePurpose::DeadlineOrVerify);
}

// --- FreezeTimeProbeProof (spec L4 §10 / BINANCE:2694) ---

TEST(FreezeTimeProbeProof, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<FreezeTimeProbeProof>);
    static_assert(std::is_standard_layout_v<FreezeTimeProbeProof>);
    SUCCEED();
}

TEST(FreezeTimeProbeProof, TlsVerifiedHostBufferSizeMatchesSpec) {
    // "max 63 chars + NUL" per spec:2689 -- pin the exact buffer size since a
    // silently-shrunk buffer would truncate a real hostname and fail an
    // otherwise-legitimate allowlist check.
    EXPECT_EQ(FreezeTimeProbeProof::kTlsVerifiedHostMax, 64u);
    FreezeTimeProbeProof proof{};
    EXPECT_EQ(sizeof(proof.tls_verified_host), 64u);
}

// --- FreezeClearPayload (spec L4 §10 / BINANCE:2715) ---

TEST(FreezeClearPayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<FreezeClearPayload>);
    static_assert(std::is_standard_layout_v<FreezeClearPayload>);
    SUCCEED();
}

TEST(FreezeClearPayload, DefaultClearKindIsProbeVerified) {
    FreezeClearPayload p{};
    EXPECT_EQ(p.clear_kind, FreezeClearKind::ProbeVerified);
}

// --- FreezeWaitArmPayload (spec L4 §10 / BINANCE:2747) ---

TEST(FreezeWaitArmPayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<FreezeWaitArmPayload>);
    static_assert(std::is_standard_layout_v<FreezeWaitArmPayload>);
    SUCCEED();
}

TEST(FreezeWaitArmPayload, DefaultArmOrdinalIsOneNotZero) {
    // Spec: "1-based within this generation" -- a default-constructed Arm
    // must already reflect that, not silently start at an illegal 0.
    FreezeWaitArmPayload p{};
    EXPECT_EQ(p.arm_ordinal, 1u);
}

// --- FreezeWaitSatisfiedPayload (spec L4 §10 / BINANCE:2760) ---

TEST(FreezeWaitSatisfiedPayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<FreezeWaitSatisfiedPayload>);
    static_assert(std::is_standard_layout_v<FreezeWaitSatisfiedPayload>);
    SUCCEED();
}

TEST(FreezeWaitSatisfiedPayload, DefaultSatisfactionOrdinalIsOneArmOrdinalIsZero) {
    // satisfaction_ordinal is "1-based within this generation" (like Arm's own
    // ordinal); arm_ordinal defaults to 0 because it MUST be explicitly set to
    // match a real, already-Acked Arm -- 0 is not a legal Arm ordinal (see
    // FreezeWaitArmPayload's own 1-based default above), so a satisfied
    // record left at its default arm_ordinal is trivially detectable as
    // "never actually bound to an Arm."
    FreezeWaitSatisfiedPayload p{};
    EXPECT_EQ(p.satisfaction_ordinal, 1u);
    EXPECT_EQ(p.arm_ordinal, 0u);
}

// --- CompactedFreezeWaitEvidencePayload (spec L4 §10 / BINANCE:2779) ---

TEST(CompactedFreezeWaitEvidencePayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<CompactedFreezeWaitEvidencePayload>);
    static_assert(std::is_standard_layout_v<CompactedFreezeWaitEvidencePayload>);
    SUCCEED();
}

// --- FreezeEpochWatermarkPayload (spec L4 §10 / BINANCE:2810) ---

TEST(FreezeEpochWatermarkPayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<FreezeEpochWatermarkPayload>);
    static_assert(std::is_standard_layout_v<FreezeEpochWatermarkPayload>);
    SUCCEED();
}

// Round-21 P0's exact wording: "the field names the next unused value, not
// the epoch being consumed." A default-constructed watermark that starts at
// 0 instead of 1 would be exactly the kind of invented-vs-transcribed drift
// this file exists to catch -- 0 is spec-reserved as "never used."
TEST(FreezeEpochWatermarkPayload, DefaultNextFreezeEpochIsOneNotZero) {
    FreezeEpochWatermarkPayload p{};
    EXPECT_EQ(p.next_freeze_epoch, 1u);
}
