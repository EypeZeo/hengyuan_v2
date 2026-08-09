// Pure in-memory round-trip, tamper-detection, and semantic-validation tests
// for compaction_intent_codec.hpp (Round D). No file I/O anywhere in this
// file -- that's compaction_lease.hpp's job (test_compaction_lease.cpp).
#include <gtest/gtest.h>
#include <hengyuan/compaction_intent_codec.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace hy;

namespace {

std::array<std::byte, 32> make_key(std::uint8_t seed) {
    std::array<std::byte, 32> k{};
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::byte>(seed + i);
    return k;
}

void fill_bytes(std::uint8_t (&arr)[32], std::uint8_t seed) {
    for (int i = 0; i < 32; ++i) arr[i] = static_cast<std::uint8_t>(seed + i);
}

CompactionCandidateIntentWire make_sample_intent(std::uint8_t phase = kCompactionCandidateIntentPhaseBuilding) {
    CompactionCandidateIntentWire v{};
    v.format_version = kCompactionCandidateIntentFormatVersion;
    v.total_bytes = kCompactionCandidateIntentWireBytes;
    v.store_uuid_lo = 0x1111111111111111ull;
    v.store_uuid_hi = 0x2222222222222222ull;
    v.kek_key_id = 7;
    v.phase = phase;
    v.source_generation = 5;
    v.target_generation = 6;
    v.baseline_tip_seq = 42;
    fill_bytes(v.baseline_tip_mac, 0x10);
    v.baseline_key_id = 3;
    v.build_nonce = 0xABCDEF0123456789ull;
    if (phase != kCompactionCandidateIntentPhaseBuilding) {
        v.candidate_id = 100;
        v.request_id = 200;
    }
    return v;
}

CompactionIntentTransitionWire make_sample_transition(std::uint32_t seq, std::uint8_t from_phase,
                                                        std::uint8_t to_phase,
                                                        const CompactionCandidateIntentWire& intent) {
    CompactionIntentTransitionWire t{};
    t.format_version = kCompactionIntentTransitionFormatVersion;
    t.total_bytes = kCompactionIntentTransitionWireBytes;
    t.store_uuid_lo = intent.store_uuid_lo;
    t.store_uuid_hi = intent.store_uuid_hi;
    t.kek_key_id = intent.kek_key_id;
    t.from_phase = from_phase;
    t.to_phase = to_phase;
    t.transition_seq = seq;
    t.source_generation = intent.source_generation;
    t.target_generation = intent.target_generation;
    t.baseline_tip_seq = intent.baseline_tip_seq;
    std::memcpy(t.baseline_tip_mac, intent.baseline_tip_mac, sizeof(t.baseline_tip_mac));
    t.baseline_key_id = intent.baseline_key_id;
    t.build_nonce = intent.build_nonce;
    t.candidate_id = 100;
    t.request_id = 200;
    return t;
}

}  // namespace

// ===========================================================================
// CompactionCandidateIntentWire codec
// ===========================================================================

TEST(CompactionCandidateIntentCodec, RoundTripsAllFieldsExactly) {
    const auto key = make_key(1);
    const CompactionCandidateIntentWire v = make_sample_intent();
    std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
    EXPECT_EQ(encode_compaction_candidate_intent_wire(buf, v, key), kCompactionCandidateIntentWireBytes);

    std::optional<VerifiedCompactionCandidateIntent> out;
    ASSERT_EQ(decode_compaction_candidate_intent_wire(buf, key, out), CompactionWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    const auto& d = out->value();
    EXPECT_EQ(d.format_version, v.format_version);
    EXPECT_EQ(d.total_bytes, v.total_bytes);
    EXPECT_EQ(d.store_uuid_lo, v.store_uuid_lo);
    EXPECT_EQ(d.store_uuid_hi, v.store_uuid_hi);
    EXPECT_EQ(d.kek_key_id, v.kek_key_id);
    EXPECT_EQ(d.phase, v.phase);
    EXPECT_EQ(d.source_generation, v.source_generation);
    EXPECT_EQ(d.target_generation, v.target_generation);
    EXPECT_EQ(d.baseline_tip_seq, v.baseline_tip_seq);
    EXPECT_EQ(0, std::memcmp(d.baseline_tip_mac, v.baseline_tip_mac, 32));
    EXPECT_EQ(d.baseline_key_id, v.baseline_key_id);
    EXPECT_EQ(d.build_nonce, v.build_nonce);
    EXPECT_EQ(d.candidate_id, v.candidate_id);
    EXPECT_EQ(d.request_id, v.request_id);
}

TEST(CompactionCandidateIntentCodec, RejectsUnknownFormatVersion) {
    const auto key = make_key(1);
    CompactionCandidateIntentWire v = make_sample_intent();
    v.format_version = 99;
    std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
    encode_compaction_candidate_intent_wire(buf, v, key);
    std::optional<VerifiedCompactionCandidateIntent> out;
    EXPECT_EQ(decode_compaction_candidate_intent_wire(buf, key, out), CompactionWireDecodeStatus::UnknownVersion);
    EXPECT_FALSE(out.has_value());
}

TEST(CompactionCandidateIntentCodec, RejectsWrongTotalBytes) {
    const auto key = make_key(1);
    CompactionCandidateIntentWire v = make_sample_intent();
    v.total_bytes = 999;
    std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
    encode_compaction_candidate_intent_wire(buf, v, key);
    std::optional<VerifiedCompactionCandidateIntent> out;
    EXPECT_EQ(decode_compaction_candidate_intent_wire(buf, key, out), CompactionWireDecodeStatus::TotalBytesInvalid);
}

TEST(CompactionCandidateIntentCodec, RejectsNonzeroReservedBits) {
    const auto key = make_key(1);
    {
        CompactionCandidateIntentWire v = make_sample_intent();
        v.reserved0 = 1;
        std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
        encode_compaction_candidate_intent_wire(buf, v, key);
        std::optional<VerifiedCompactionCandidateIntent> out;
        EXPECT_EQ(decode_compaction_candidate_intent_wire(buf, key, out), CompactionWireDecodeStatus::ReservedNonzero);
    }
    {
        CompactionCandidateIntentWire v = make_sample_intent();
        v.reserved1 = 1;
        std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
        encode_compaction_candidate_intent_wire(buf, v, key);
        std::optional<VerifiedCompactionCandidateIntent> out;
        EXPECT_EQ(decode_compaction_candidate_intent_wire(buf, key, out), CompactionWireDecodeStatus::ReservedNonzero);
    }
}

TEST(CompactionCandidateIntentCodec, RejectsPhaseOutOfRange) {
    const auto key = make_key(1);
    CompactionCandidateIntentWire v = make_sample_intent();
    v.phase = kCompactionCandidateIntentPhaseAbandonFinalizing + 1;
    std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
    encode_compaction_candidate_intent_wire(buf, v, key);
    std::optional<VerifiedCompactionCandidateIntent> out;
    EXPECT_EQ(decode_compaction_candidate_intent_wire(buf, key, out), CompactionWireDecodeStatus::MalformedField);
}

TEST(CompactionCandidateIntentCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(1);
    const CompactionCandidateIntentWire v = make_sample_intent();
    std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
    encode_compaction_candidate_intent_wire(buf, v, key);
    buf[20] ^= std::byte{0x01};  // flip a bit inside store_uuid_hi
    std::optional<VerifiedCompactionCandidateIntent> out;
    EXPECT_EQ(decode_compaction_candidate_intent_wire(buf, key, out), CompactionWireDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());
}

TEST(CompactionCandidateIntentCodec, RejectsWrongKey) {
    const auto key = make_key(1);
    const auto wrong_key = make_key(2);
    const CompactionCandidateIntentWire v = make_sample_intent();
    std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
    encode_compaction_candidate_intent_wire(buf, v, key);
    std::optional<VerifiedCompactionCandidateIntent> out;
    EXPECT_EQ(decode_compaction_candidate_intent_wire(buf, wrong_key, out), CompactionWireDecodeStatus::ChecksumMismatch);
}

TEST(CompactionCandidateIntentCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(1);
    const CompactionCandidateIntentWire v = make_sample_intent();
    std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
    encode_compaction_candidate_intent_wire(buf, v, key);
    std::optional<VerifiedCompactionCandidateIntent> out;
    EXPECT_EQ(decode_compaction_candidate_intent_wire(
                  std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              CompactionWireDecodeStatus::Truncated);
}

TEST(PeekCompactionCandidateIntentKekKeyId, ReadsBeforeMacVerification) {
    const auto key = make_key(1);
    CompactionCandidateIntentWire v = make_sample_intent();
    v.kek_key_id = 0xDEAD;
    std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
    encode_compaction_candidate_intent_wire(buf, v, key);
    buf.back() ^= std::byte{0xFF};  // corrupt the mac -- peek must still work
    std::uint32_t out_key_id = 0;
    EXPECT_TRUE(peek_compaction_candidate_intent_kek_key_id(buf, out_key_id));
    EXPECT_EQ(out_key_id, 0xDEADu);
}

TEST(PeekCompactionCandidateIntentKekKeyId, RejectsTooShortBuffer) {
    std::array<std::byte, 4> short_buf{};
    std::uint32_t out_key_id = 0;
    EXPECT_FALSE(peek_compaction_candidate_intent_kek_key_id(short_buf, out_key_id));
}

// ===========================================================================
// CompactionIntentTransitionWire codec
// ===========================================================================

TEST(CompactionIntentTransitionCodec, RoundTripsAllFieldsExactly) {
    const auto key = make_key(3);
    const CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    const CompactionIntentTransitionWire t = make_sample_transition(
        1, kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved, intent);
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
    EXPECT_EQ(encode_compaction_intent_transition_wire(buf, t, key), kCompactionIntentTransitionWireBytes);

    std::optional<VerifiedTransition> out;
    ASSERT_EQ(decode_compaction_intent_transition_wire(buf, key, out), CompactionWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->value().transition_seq, 1u);
    EXPECT_EQ(out->value().from_phase, kCompactionCandidateIntentPhaseBuilding);
    EXPECT_EQ(out->value().to_phase, kCompactionCandidateIntentPhaseReserved);
    EXPECT_EQ(out->value().candidate_id, t.candidate_id);
    EXPECT_EQ(out->value().request_id, t.request_id);
}

TEST(CompactionIntentTransitionCodec, RejectsUnknownFormatVersion) {
    const auto key = make_key(3);
    const CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    CompactionIntentTransitionWire t = make_sample_transition(
        1, kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved, intent);
    t.format_version = 7;
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
    encode_compaction_intent_transition_wire(buf, t, key);
    std::optional<VerifiedTransition> out;
    EXPECT_EQ(decode_compaction_intent_transition_wire(buf, key, out), CompactionWireDecodeStatus::UnknownVersion);
}

TEST(CompactionIntentTransitionCodec, RejectsWrongTotalBytes) {
    const auto key = make_key(3);
    const CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    CompactionIntentTransitionWire t = make_sample_transition(
        1, kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved, intent);
    t.total_bytes = 1;
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
    encode_compaction_intent_transition_wire(buf, t, key);
    std::optional<VerifiedTransition> out;
    EXPECT_EQ(decode_compaction_intent_transition_wire(buf, key, out), CompactionWireDecodeStatus::TotalBytesInvalid);
}

TEST(CompactionIntentTransitionCodec, RejectsNonzeroReserved) {
    const auto key = make_key(3);
    const CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    CompactionIntentTransitionWire t = make_sample_transition(
        1, kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved, intent);
    t.reserved0 = 5;
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
    encode_compaction_intent_transition_wire(buf, t, key);
    std::optional<VerifiedTransition> out;
    EXPECT_EQ(decode_compaction_intent_transition_wire(buf, key, out), CompactionWireDecodeStatus::ReservedNonzero);
}

TEST(CompactionIntentTransitionCodec, RejectsPhaseOutOfRange) {
    const auto key = make_key(3);
    const CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    CompactionIntentTransitionWire t = make_sample_transition(
        1, kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved, intent);
    t.to_phase = kCompactionCandidateIntentPhaseAbandonFinalizing + 10;
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
    encode_compaction_intent_transition_wire(buf, t, key);
    std::optional<VerifiedTransition> out;
    EXPECT_EQ(decode_compaction_intent_transition_wire(buf, key, out), CompactionWireDecodeStatus::MalformedField);
}

TEST(CompactionIntentTransitionCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(3);
    const CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    const CompactionIntentTransitionWire t = make_sample_transition(
        1, kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved, intent);
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
    encode_compaction_intent_transition_wire(buf, t, key);
    buf[50] ^= std::byte{0x01};
    std::optional<VerifiedTransition> out;
    EXPECT_EQ(decode_compaction_intent_transition_wire(buf, key, out), CompactionWireDecodeStatus::ChecksumMismatch);
}

TEST(CompactionIntentTransitionCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(3);
    const CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    const CompactionIntentTransitionWire t = make_sample_transition(
        1, kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved, intent);
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
    encode_compaction_intent_transition_wire(buf, t, key);
    std::optional<VerifiedTransition> out;
    EXPECT_EQ(decode_compaction_intent_transition_wire(
                  std::span<const std::byte>(buf.data(), 10), key, out),
              CompactionWireDecodeStatus::Truncated);
}

// ===========================================================================
// CompactionIntentGcAuthorizedWire codec
// ===========================================================================

namespace {

CompactionIntentGcAuthorizedWire make_sample_gc(const CompactionCandidateIntentWire& intent) {
    CompactionIntentGcAuthorizedWire g{};
    g.format_version = kCompactionIntentGcAuthorizedFormatVersion;
    g.total_bytes = kCompactionIntentGcAuthorizedWireBytes;
    g.store_uuid_lo = intent.store_uuid_lo;
    g.store_uuid_hi = intent.store_uuid_hi;
    g.kek_key_id = intent.kek_key_id;
    g.terminal_disposition = kCompactionIntentGcDispositionPreSealAbandonClear;
    g.intent_phase_at_auth = kCompactionCandidateIntentPhaseBuilding;
    g.source_generation = intent.source_generation;
    g.target_generation = intent.target_generation;
    g.baseline_tip_seq = intent.baseline_tip_seq;
    std::memcpy(g.baseline_tip_mac, intent.baseline_tip_mac, sizeof(g.baseline_tip_mac));
    g.baseline_key_id = intent.baseline_key_id;
    g.build_nonce = intent.build_nonce;
    g.cleanup_auth_flags = 0;  // PreSealAbandonClear, Building-only: all evidence zero
    return g;
}

}  // namespace

TEST(CompactionIntentGcAuthorizedCodec, RoundTripsAllFieldsExactly) {
    const auto key = make_key(5);
    const CompactionCandidateIntentWire intent = make_sample_intent();
    const CompactionIntentGcAuthorizedWire g = make_sample_gc(intent);
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
    EXPECT_EQ(encode_compaction_intent_gc_authorized_wire(buf, g, key), kCompactionIntentGcAuthorizedWireBytes);

    std::optional<VerifiedGcAuthorized> out;
    ASSERT_EQ(decode_compaction_intent_gc_authorized_wire(buf, key, out), CompactionWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->value().terminal_disposition, g.terminal_disposition);
    EXPECT_EQ(out->value().cleanup_auth_flags, g.cleanup_auth_flags);
}

TEST(CompactionIntentGcAuthorizedCodec, LegacyV1FormatVersionRejectedBeforeAnythingElse) {
    // format_version=1 (legacy) in an otherwise full-size (316B) buffer --
    // isolates "decode rejects on UnknownVersion before inspecting anything
    // else" from the separate, already-covered Truncated case. A genuinely
    // short (204B) legacy-sized buffer is rejected even earlier, as
    // Truncated, by the same fail-closed length check every other type
    // uses -- see RejectsTruncatedBuffer above; that's an equally valid
    // rejection, just a different status code, so it's not re-asserted here.
    const auto key = make_key(5);
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> legacy_shaped{};
    std::byte* p = legacy_shaped.data();
    detail::write_u32_le(p, 1u);  // format_version = 1 (legacy)
    detail::write_u32_le(p, static_cast<std::uint32_t>(kCompactionIntentGcAuthorizedLegacyV1Bytes));
    std::optional<VerifiedGcAuthorized> out;
    EXPECT_EQ(decode_compaction_intent_gc_authorized_wire(legacy_shaped, key, out),
              CompactionWireDecodeStatus::UnknownVersion);
    EXPECT_FALSE(out.has_value());
}

TEST(CompactionIntentGcAuthorizedCodec, LegacyV1_204ByteBuffer_RejectedAsTruncated) {
    // The realistic case: a real legacy 204B file read into a decode buffer
    // -- shorter than the current 316B wire size, so it's rejected via the
    // length check before format_version is even read. Still fail-closed
    // (Ok is never returned), just a different status than the isolated
    // format-version test above.
    const auto key = make_key(5);
    std::array<std::byte, kCompactionIntentGcAuthorizedLegacyV1Bytes> legacy{};
    std::byte* p = legacy.data();
    detail::write_u32_le(p, 1u);
    detail::write_u32_le(p, static_cast<std::uint32_t>(kCompactionIntentGcAuthorizedLegacyV1Bytes));
    std::optional<VerifiedGcAuthorized> out;
    EXPECT_EQ(decode_compaction_intent_gc_authorized_wire(legacy, key, out), CompactionWireDecodeStatus::Truncated);
    EXPECT_FALSE(out.has_value());
}

TEST(CompactionIntentGcAuthorizedCodec, RejectsTotalBytesMismatch) {
    const auto key = make_key(5);
    const CompactionCandidateIntentWire intent = make_sample_intent();
    CompactionIntentGcAuthorizedWire g = make_sample_gc(intent);
    g.total_bytes = 1;
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
    encode_compaction_intent_gc_authorized_wire(buf, g, key);
    std::optional<VerifiedGcAuthorized> out;
    EXPECT_EQ(decode_compaction_intent_gc_authorized_wire(buf, key, out), CompactionWireDecodeStatus::TotalBytesInvalid);
}

TEST(CompactionIntentGcAuthorizedCodec, RejectsUndefinedHighFlagBits) {
    const auto key = make_key(5);
    const CompactionCandidateIntentWire intent = make_sample_intent();
    CompactionIntentGcAuthorizedWire g = make_sample_gc(intent);
    g.cleanup_auth_flags = 0b1110'0000;  // bits 5..7, all undefined
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
    encode_compaction_intent_gc_authorized_wire(buf, g, key);
    std::optional<VerifiedGcAuthorized> out;
    EXPECT_EQ(decode_compaction_intent_gc_authorized_wire(buf, key, out), CompactionWireDecodeStatus::MalformedField);
}

TEST(CompactionIntentGcAuthorizedCodec, RejectsDispositionOutOfRange) {
    const auto key = make_key(5);
    const CompactionCandidateIntentWire intent = make_sample_intent();
    CompactionIntentGcAuthorizedWire g = make_sample_gc(intent);
    g.terminal_disposition = kCompactionIntentGcDispositionAbandonFinalizingClear + 1;
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
    encode_compaction_intent_gc_authorized_wire(buf, g, key);
    std::optional<VerifiedGcAuthorized> out;
    EXPECT_EQ(decode_compaction_intent_gc_authorized_wire(buf, key, out), CompactionWireDecodeStatus::MalformedField);
}

TEST(CompactionIntentGcAuthorizedCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(5);
    const CompactionCandidateIntentWire intent = make_sample_intent();
    const CompactionIntentGcAuthorizedWire g = make_sample_gc(intent);
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
    encode_compaction_intent_gc_authorized_wire(buf, g, key);
    buf[100] ^= std::byte{0x01};
    std::optional<VerifiedGcAuthorized> out;
    EXPECT_EQ(decode_compaction_intent_gc_authorized_wire(buf, key, out), CompactionWireDecodeStatus::ChecksumMismatch);
}

TEST(CompactionIntentGcAuthorizedCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(5);
    const CompactionCandidateIntentWire intent = make_sample_intent();
    const CompactionIntentGcAuthorizedWire g = make_sample_gc(intent);
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
    encode_compaction_intent_gc_authorized_wire(buf, g, key);
    std::optional<VerifiedGcAuthorized> out;
    EXPECT_EQ(decode_compaction_intent_gc_authorized_wire(
                  std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              CompactionWireDecodeStatus::Truncated);
}

// ===========================================================================
// Semantic validation
// ===========================================================================

TEST(IsLegalCandidateIdsForPhase, BuildingRequiresBothIdsZero) {
    EXPECT_TRUE(is_legal_candidate_ids_for_phase(kCompactionCandidateIntentPhaseBuilding, 0, 0));
    EXPECT_FALSE(is_legal_candidate_ids_for_phase(kCompactionCandidateIntentPhaseBuilding, 1, 0));
    EXPECT_FALSE(is_legal_candidate_ids_for_phase(kCompactionCandidateIntentPhaseBuilding, 0, 1));
}

TEST(IsLegalCandidateIdsForPhase, ReservedAndBeyondRequireBothIdsNonzero) {
    for (std::uint8_t phase : {kCompactionCandidateIntentPhaseReserved,
                                kCompactionCandidateIntentPhaseStartedPublished,
                                kCompactionCandidateIntentPhasePostSealFinalizing,
                                kCompactionCandidateIntentPhaseAbandonFinalizing}) {
        EXPECT_TRUE(is_legal_candidate_ids_for_phase(phase, 1, 1));
        EXPECT_FALSE(is_legal_candidate_ids_for_phase(phase, 0, 1));
        EXPECT_FALSE(is_legal_candidate_ids_for_phase(phase, 1, 0));
        EXPECT_FALSE(is_legal_candidate_ids_for_phase(phase, 0, 0));
    }
}

TEST(IsLegalGenerationTransition, RequiresExactlyPlusOne) {
    EXPECT_TRUE(is_legal_generation_transition(5, 6));
    EXPECT_FALSE(is_legal_generation_transition(5, 5));
    EXPECT_FALSE(is_legal_generation_transition(5, 7));
    EXPECT_FALSE(is_legal_generation_transition(5, 4));
}

TEST(IsLegalGenerationTransition, RejectsSourceMaxOverflow) {
    EXPECT_FALSE(is_legal_generation_transition(std::numeric_limits<std::uint32_t>::max(), 0));
}

TEST(IntentPermanentlyImmutableFieldsMatch, DetectsEveryFieldMismatch) {
    const CompactionCandidateIntentWire base = make_sample_intent();
    EXPECT_TRUE(intent_permanently_immutable_fields_match(base, base));

    auto tweak_and_check = [&](auto mutator) {
        CompactionCandidateIntentWire other = base;
        mutator(other);
        EXPECT_FALSE(intent_permanently_immutable_fields_match(base, other));
    };
    tweak_and_check([](auto& v) { v.store_uuid_lo++; });
    tweak_and_check([](auto& v) { v.store_uuid_hi++; });
    tweak_and_check([](auto& v) { v.kek_key_id++; });
    tweak_and_check([](auto& v) { v.source_generation++; });
    tweak_and_check([](auto& v) { v.target_generation++; });
    tweak_and_check([](auto& v) { v.baseline_tip_seq++; });
    tweak_and_check([](auto& v) { v.baseline_tip_mac[0] ^= 0x01; });
    tweak_and_check([](auto& v) { v.baseline_key_id++; });
    tweak_and_check([](auto& v) { v.build_nonce++; });
}

TEST(IsLegalCandidateIdTransition, BuildingToReservedAllowsExactlyOneZeroToNonzero) {
    EXPECT_TRUE(is_legal_candidate_id_transition(
        kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved,
        /*before*/ 0, 0, /*after*/ 100, 200, /*x1 seq1*/ 100, 200));
}

TEST(IsLegalCandidateIdTransition, RejectsBuildingToReservedWithMismatchedX1Binding) {
    EXPECT_FALSE(is_legal_candidate_id_transition(
        kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved,
        0, 0, 100, 200, /*x1 seq1*/ 999, 200));
}

TEST(IsLegalCandidateIdTransition, RejectsBuildingToReservedWithNonzeroBefore) {
    EXPECT_FALSE(is_legal_candidate_id_transition(
        kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved,
        5, 0, 100, 200, 100, 200));
}

TEST(IsLegalCandidateIdTransition, RejectsBuildingToReservedWithZeroAfter) {
    EXPECT_FALSE(is_legal_candidate_id_transition(
        kCompactionCandidateIntentPhaseBuilding, kCompactionCandidateIntentPhaseReserved,
        0, 0, 0, 200, 0, 200));
}

TEST(IsLegalCandidateIdTransition, OtherEdgesRequireIdsUnchangedAndAlreadyBound) {
    EXPECT_TRUE(is_legal_candidate_id_transition(
        kCompactionCandidateIntentPhaseReserved, kCompactionCandidateIntentPhaseStartedPublished,
        100, 200, 100, 200, 0, 0));
    EXPECT_FALSE(is_legal_candidate_id_transition(
        kCompactionCandidateIntentPhaseReserved, kCompactionCandidateIntentPhaseStartedPublished,
        100, 200, 101, 200, 0, 0));  // ids changed on a non-Building->Reserved edge
    EXPECT_FALSE(is_legal_candidate_id_transition(
        kCompactionCandidateIntentPhaseReserved, kCompactionCandidateIntentPhaseStartedPublished,
        0, 0, 0, 0, 0, 0));  // unbound ids on a non-genesis edge
}

TEST(IsLegalCandidateIdTransition, RejectsSamePhaseAsNoTransition) {
    EXPECT_FALSE(is_legal_candidate_id_transition(
        kCompactionCandidateIntentPhaseReserved, kCompactionCandidateIntentPhaseReserved,
        100, 200, 100, 200, 0, 0));
}

namespace {

// Builds a full 3-frame legal .x1 chain for the PostSeal branch, seq 1..3.
std::array<CompactionIntentTransitionWire, 3> make_legal_chain(const CompactionCandidateIntentWire& intent) {
    std::array<CompactionIntentTransitionWire, 3> chain{};
    chain[0] = make_sample_transition(1, kCompactionCandidateIntentPhaseBuilding,
                                       kCompactionCandidateIntentPhaseReserved, intent);
    // seq1's prev_transition_mac must be all-zero (already default-initialized).
    // Compute seq1's own mac by encoding it (mac field gets filled by encode).
    std::array<std::byte, 32> dummy_key_bytes{};
    for (std::size_t i = 0; i < dummy_key_bytes.size(); ++i) dummy_key_bytes[i] = static_cast<std::byte>(i);
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf1{};
    encode_compaction_intent_transition_wire(buf1, chain[0], dummy_key_bytes);
    std::memcpy(chain[0].mac, buf1.data() + (kCompactionIntentTransitionWireBytes - 32), 32);

    chain[1] = make_sample_transition(2, kCompactionCandidateIntentPhaseReserved,
                                       kCompactionCandidateIntentPhaseStartedPublished, intent);
    std::memcpy(chain[1].prev_transition_mac, chain[0].mac, 32);
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf2{};
    encode_compaction_intent_transition_wire(buf2, chain[1], dummy_key_bytes);
    std::memcpy(chain[1].mac, buf2.data() + (kCompactionIntentTransitionWireBytes - 32), 32);

    chain[2] = make_sample_transition(3, kCompactionCandidateIntentPhaseStartedPublished,
                                       kCompactionCandidateIntentPhasePostSealFinalizing, intent);
    std::memcpy(chain[2].prev_transition_mac, chain[1].mac, 32);
    std::array<std::byte, kCompactionIntentTransitionWireBytes> buf3{};
    encode_compaction_intent_transition_wire(buf3, chain[2], dummy_key_bytes);
    std::memcpy(chain[2].mac, buf3.data() + (kCompactionIntentTransitionWireBytes - 32), 32);

    return chain;
}

}  // namespace

TEST(ValidateX1ChainRaw, EmptyChainIsEmpty) {
    const CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseBuilding);
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(compaction_codec_detail::walk_x1_chain_raw(intent, {}, terminal), X1ChainStatus::Empty);
}

TEST(ValidateX1ChainRaw, ValidThreeFrameChainReturnsValid) {
    CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhasePostSealFinalizing);
    const auto chain = make_legal_chain(intent);
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(compaction_codec_detail::walk_x1_chain_raw(intent, chain, terminal), X1ChainStatus::Valid);
    EXPECT_EQ(0, std::memcmp(terminal.data(), chain[2].mac, 32));
}

TEST(ValidateX1ChainRaw, TooManyFramesRejected) {
    const CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhasePostSealFinalizing);
    const auto chain3 = make_legal_chain(intent);
    std::array<CompactionIntentTransitionWire, 4> chain4{};
    std::copy(chain3.begin(), chain3.end(), chain4.begin());
    chain4[3] = chain3[2];
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(compaction_codec_detail::walk_x1_chain_raw(intent, chain4, terminal), X1ChainStatus::TooManyFrames);
}

TEST(ValidateX1ChainRaw, GapInSeqRejected) {
    CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    auto chain = make_legal_chain(intent);
    std::array<CompactionIntentTransitionWire, 1> just_first{chain[0]};
    just_first[0].transition_seq = 2;  // should be 1
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(compaction_codec_detail::walk_x1_chain_raw(intent, just_first, terminal), X1ChainStatus::Gap);
}

TEST(ValidateX1ChainRaw, ForeignBindingRejected) {
    CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    auto chain = make_legal_chain(intent);
    std::array<CompactionIntentTransitionWire, 1> just_first{chain[0]};
    just_first[0].build_nonce ^= 0xFFu;
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(compaction_codec_detail::walk_x1_chain_raw(intent, just_first, terminal), X1ChainStatus::ForeignBinding);
}

TEST(ValidateX1ChainRaw, IllegalEdgeRejected) {
    CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    auto chain = make_legal_chain(intent);
    std::array<CompactionIntentTransitionWire, 1> just_first{chain[0]};
    just_first[0].to_phase = kCompactionCandidateIntentPhasePostSealFinalizing;  // Building->PostSeal: illegal skip
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(compaction_codec_detail::walk_x1_chain_raw(intent, just_first, terminal), X1ChainStatus::IllegalEdge);
}

TEST(ValidateX1ChainRaw, MacChainBrokenRejected) {
    CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseStartedPublished);
    auto chain = make_legal_chain(intent);
    std::array<CompactionIntentTransitionWire, 2> two{chain[0], chain[1]};
    two[1].prev_transition_mac[0] ^= 0x01;  // no longer matches chain[0].mac
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(compaction_codec_detail::walk_x1_chain_raw(intent, two, terminal), X1ChainStatus::MacChainBroken);
}

TEST(ValidateX1ChainRaw, Seq1RequiresZeroPrevMac) {
    CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseReserved);
    auto chain = make_legal_chain(intent);
    std::array<CompactionIntentTransitionWire, 1> just_first{chain[0]};
    just_first[0].prev_transition_mac[0] = 0x01;
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(compaction_codec_detail::walk_x1_chain_raw(intent, just_first, terminal), X1ChainStatus::MacChainBroken);
}

TEST(ValidateX1ChainRaw, TerminalBranchConflictWhenIntentPhaseDoesntMatchChainEnd) {
    // Intent says AbandonFinalizing, but the chain's seq=3 frame raises to PostSealFinalizing.
    CompactionCandidateIntentWire intent = make_sample_intent(kCompactionCandidateIntentPhaseAbandonFinalizing);
    const auto chain = make_legal_chain(intent);  // chain[2].to_phase == PostSealFinalizing
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(compaction_codec_detail::walk_x1_chain_raw(intent, chain, terminal),
              X1ChainStatus::TerminalBranchConflict);
}

TEST(ValidateIntentTransition, ValidBuildingToPostSealChainAccepted) {
    CompactionCandidateIntentWire before = make_sample_intent(kCompactionCandidateIntentPhaseBuilding);
    CompactionCandidateIntentWire after = before;
    after.phase = kCompactionCandidateIntentPhasePostSealFinalizing;
    after.candidate_id = 100;
    after.request_id = 200;
    const auto chain = make_legal_chain(after);
    EXPECT_TRUE(validate_intent_transition(before, after, chain));
}

TEST(ValidateIntentTransition, RejectsWhenImmutableFieldChanged) {
    CompactionCandidateIntentWire before = make_sample_intent(kCompactionCandidateIntentPhaseBuilding);
    CompactionCandidateIntentWire after = before;
    after.phase = kCompactionCandidateIntentPhasePostSealFinalizing;
    after.candidate_id = 100;
    after.request_id = 200;
    after.kek_key_id++;  // permanently-immutable field changed
    const auto chain = make_legal_chain(after);
    EXPECT_FALSE(validate_intent_transition(before, after, chain));
}

// ===========================================================================
// cleanup_auth_flags truth table
// ===========================================================================

namespace {
std::array<std::byte, 32> zero_mac() { return {}; }
std::array<std::byte, 32> nonzero_mac() {
    std::array<std::byte, 32> m{};
    m[0] = std::byte{0x01};
    return m;
}
}  // namespace

TEST(IsLegalCleanupAuthFlags, PreSealAbandonClearRequiresBits1To4Zero) {
    const auto zm = zero_mac();
    EXPECT_TRUE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPreSealAbandonClear, 0,
                                             /*gate_absent_at_create=*/false, zm));
    EXPECT_TRUE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPreSealAbandonClear,
                                             kCompactionIntentGcAuthFlagJournalDrain, false, zm));
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPreSealAbandonClear,
                                              kCompactionIntentGcAuthFlagPostSealBound, false, zm));
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPreSealAbandonClear,
                                              kCompactionIntentGcAuthFlagGenGone, false, zm));
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPreSealAbandonClear,
                                              kCompactionIntentGcAuthFlagResumeAuthorized, false, zm));
}

TEST(IsLegalCleanupAuthFlags, PostSealFinalizingClearRequiresJournalDrainAndPostSealBound) {
    const auto nz = nonzero_mac();
    constexpr std::uint8_t kBoth = kCompactionIntentGcAuthFlagJournalDrain | kCompactionIntentGcAuthFlagPostSealBound;
    EXPECT_TRUE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPostSealFinalizingClear, kBoth, false, nz));
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPostSealFinalizingClear,
                                              kCompactionIntentGcAuthFlagJournalDrain, false, nz));
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPostSealFinalizingClear,
                                              kCompactionIntentGcAuthFlagPostSealBound, false, nz));
}

TEST(IsLegalCleanupAuthFlags, AbandonFinalizingClearRequiresDrainGenGoneResumeAuthorized) {
    const auto nz = nonzero_mac();
    constexpr std::uint8_t kAll = kCompactionIntentGcAuthFlagJournalDrain | kCompactionIntentGcAuthFlagGenGone |
                                   kCompactionIntentGcAuthFlagResumeAuthorized;
    EXPECT_TRUE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionAbandonFinalizingClear, kAll, false, nz));
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionAbandonFinalizingClear,
                                              static_cast<std::uint8_t>(kAll & ~kCompactionIntentGcAuthFlagGenGone),
                                              false, nz));
}

TEST(IsLegalCleanupAuthFlags, GateAbsentOnlyLegalWithZeroTrailerMac) {
    const auto zm = zero_mac();
    const auto nz = nonzero_mac();
    constexpr std::uint8_t kFlags = kCompactionIntentGcAuthFlagJournalDrain | kCompactionIntentGcAuthFlagPostSealBound |
                                     kCompactionIntentGcAuthFlagGateAbsentAtCreate;
    EXPECT_TRUE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPostSealFinalizingClear, kFlags,
                                             /*gate_absent_at_create=*/true, zm));
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPostSealFinalizingClear, kFlags,
                                              true, nz));  // bit4 set but trailer mac non-zero
}

TEST(IsLegalCleanupAuthFlags, LivePathRejectsBit4SetOrZeroTrailerMac) {
    const auto zm = zero_mac();
    const auto nz = nonzero_mac();
    constexpr std::uint8_t kFlags = kCompactionIntentGcAuthFlagJournalDrain | kCompactionIntentGcAuthFlagPostSealBound;
    EXPECT_TRUE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPostSealFinalizingClear, kFlags, false, nz));
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPostSealFinalizingClear, kFlags, false,
                                              zm));  // live path but zero trailer mac
}

TEST(IsLegalCleanupAuthFlags, RejectsUndefinedHighBits) {
    const auto zm = zero_mac();
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionPreSealAbandonClear, 0b1000'0000, false, zm));
}

TEST(IsLegalCleanupAuthFlags, RejectsUnknownDisposition) {
    const auto zm = zero_mac();
    EXPECT_FALSE(is_legal_cleanup_auth_flags(kCompactionIntentGcDispositionAbandonFinalizingClear + 1, 0, false, zm));
}

// ===========================================================================
// Property test: decode never crashes/UBs on arbitrary bytes
// ===========================================================================

TEST(CompactionWireCodecProperty, DecodeNeverCrashesOnRandomBytes) {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    const auto key = make_key(9);

    for (int trial = 0; trial < 2000; ++trial) {
        const std::size_t len = static_cast<std::size_t>(trial % 400);
        std::vector<std::byte> bytes(len);
        for (auto& b : bytes) b = static_cast<std::byte>(byte_dist(rng));

        std::optional<VerifiedCompactionCandidateIntent> out1;
        (void)decode_compaction_candidate_intent_wire(bytes, key, out1);
        std::optional<VerifiedTransition> out2;
        (void)decode_compaction_intent_transition_wire(bytes, key, out2);
        std::optional<VerifiedGcAuthorized> out3;
        (void)decode_compaction_intent_gc_authorized_wire(bytes, key, out3);
    }
    SUCCEED();
}
