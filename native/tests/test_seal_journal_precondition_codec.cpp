// Pure in-memory round-trip, tamper-detection, and semantic-validation tests
// for seal_journal_precondition_codec.hpp (Round E Slice 1). No file I/O
// anywhere in this file -- this codec has no I/O layer at all yet.
#include <gtest/gtest.h>
#include <hengyuan/seal_journal_precondition_codec.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
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

SealIdWatermark make_sample_watermark() {
    SealIdWatermark w{};
    w.store_uuid_lo = 0x1111111111111111ull;
    w.store_uuid_hi = 0x2222222222222222ull;
    w.next_candidate_id = 42;
    w.next_request_id = 43;
    return w;
}

// A legal SealExportStartedWire: mask covers slots {0, 2, 5} (3 producers),
// each with a distinct nonzero ring_id, all other slots zero.
SealExportStartedWire make_sample_started() {
    SealExportStartedWire v{};
    v.format_version = kSealExportStartedFormatVersion;
    v.total_bytes = kSealExportStartedWireBytes;
    v.store_uuid_lo = 0x1111111111111111ull;
    v.store_uuid_hi = 0x2222222222222222ull;
    v.candidate_id = 100;
    v.source_generation = 5;
    v.baseline_tip_seq = 42;
    fill_bytes(v.baseline_tip_mac, 0x10);
    v.baseline_key_id = 3;
    v.new_generation = 6;
    v.new_final_seq = 99;
    fill_bytes(v.new_final_tip_mac, 0x20);
    v.new_key_id = 4;
    v.request_id = 200;
    fill_bytes(v.content_root, 0x30);
    v.kek_key_id = 7;
    v.registered_producer_mask = 0b0010'0101;  // bits 0, 2, 5
    v.producer_count = 3;
    v.ring_id[0] = 101;
    v.ring_id[2] = 102;
    v.ring_id[5] = 103;
    return v;
}

}  // namespace

// ===========================================================================
// SealIdWatermark codec
// ===========================================================================

TEST(SealIdWatermarkCodec, RoundTripsAllFieldsExactly) {
    const auto key = make_key(1);
    const SealIdWatermark v = make_sample_watermark();
    std::array<std::byte, kSealIdWatermarkWireBytes> buf{};
    EXPECT_EQ(encode_seal_id_watermark_wire(buf, v, key), kSealIdWatermarkWireBytes);

    std::optional<VerifiedSealIdWatermark> out;
    ASSERT_EQ(decode_seal_id_watermark_wire(buf, key, out), SealJournalPreconditionDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    const auto& d = out->value();
    EXPECT_EQ(d.store_uuid_lo, v.store_uuid_lo);
    EXPECT_EQ(d.store_uuid_hi, v.store_uuid_hi);
    EXPECT_EQ(d.next_candidate_id, v.next_candidate_id);
    EXPECT_EQ(d.next_request_id, v.next_request_id);
}

TEST(SealIdWatermarkCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(1);
    const SealIdWatermark v = make_sample_watermark();
    std::array<std::byte, kSealIdWatermarkWireBytes> buf{};
    encode_seal_id_watermark_wire(buf, v, key);
    buf[10] ^= std::byte{0x01};  // flip a bit inside store_uuid_hi
    std::optional<VerifiedSealIdWatermark> out;
    EXPECT_EQ(decode_seal_id_watermark_wire(buf, key, out), SealJournalPreconditionDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());
}

TEST(SealIdWatermarkCodec, RejectsWrongKey) {
    const auto key = make_key(1);
    const auto wrong_key = make_key(2);
    const SealIdWatermark v = make_sample_watermark();
    std::array<std::byte, kSealIdWatermarkWireBytes> buf{};
    encode_seal_id_watermark_wire(buf, v, key);
    std::optional<VerifiedSealIdWatermark> out;
    EXPECT_EQ(decode_seal_id_watermark_wire(buf, wrong_key, out), SealJournalPreconditionDecodeStatus::ChecksumMismatch);
}

TEST(SealIdWatermarkCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(1);
    const SealIdWatermark v = make_sample_watermark();
    std::array<std::byte, kSealIdWatermarkWireBytes> buf{};
    encode_seal_id_watermark_wire(buf, v, key);
    std::optional<VerifiedSealIdWatermark> out;
    EXPECT_EQ(decode_seal_id_watermark_wire(std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              SealJournalPreconditionDecodeStatus::Truncated);
}

TEST(SealIdWatermarkCodec, RejectsZeroNextCandidateId) {
    const auto key = make_key(1);
    SealIdWatermark v = make_sample_watermark();
    v.next_candidate_id = 0;
    std::array<std::byte, kSealIdWatermarkWireBytes> buf{};
    encode_seal_id_watermark_wire(buf, v, key);  // MAC computed over the (already illegal) content
    std::optional<VerifiedSealIdWatermark> out;
    EXPECT_EQ(decode_seal_id_watermark_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealIdWatermarkCodec, RejectsZeroNextRequestId) {
    const auto key = make_key(1);
    SealIdWatermark v = make_sample_watermark();
    v.next_request_id = 0;
    std::array<std::byte, kSealIdWatermarkWireBytes> buf{};
    encode_seal_id_watermark_wire(buf, v, key);
    std::optional<VerifiedSealIdWatermark> out;
    EXPECT_EQ(decode_seal_id_watermark_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

// UINT64_MAX is a legal, decodable value -- the upper-bound fence belongs to
// a future reservation-advancing write path this slice does not have (see
// this codec's own header comment / docs/SPEC_INVARIANTS.md's Round E Slice 1
// entry). This test name says "Accepts" on purpose, to guard against someone
// later adding a spurious upper-bound rejection here.
TEST(SealIdWatermarkCodec, AcceptsMaxNextCandidateIdWithValidMac) {
    const auto key = make_key(1);
    SealIdWatermark v = make_sample_watermark();
    v.next_candidate_id = std::numeric_limits<std::uint64_t>::max();
    std::array<std::byte, kSealIdWatermarkWireBytes> buf{};
    encode_seal_id_watermark_wire(buf, v, key);
    std::optional<VerifiedSealIdWatermark> out;
    ASSERT_EQ(decode_seal_id_watermark_wire(buf, key, out), SealJournalPreconditionDecodeStatus::Ok);
    EXPECT_EQ(out->value().next_candidate_id, std::numeric_limits<std::uint64_t>::max());
}

// ===========================================================================
// SealExportStartedWire codec
// ===========================================================================

TEST(SealExportStartedCodec, RoundTripsAllFieldsExactly) {
    const auto key = make_key(3);
    const SealExportStartedWire v = make_sample_started();
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    EXPECT_EQ(encode_seal_export_started_wire(buf, v, key), kSealExportStartedWireBytes);

    std::optional<VerifiedSealExportStarted> out;
    ASSERT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    const auto& d = out->value();
    EXPECT_EQ(d.format_version, v.format_version);
    EXPECT_EQ(d.total_bytes, v.total_bytes);
    EXPECT_EQ(d.store_uuid_lo, v.store_uuid_lo);
    EXPECT_EQ(d.store_uuid_hi, v.store_uuid_hi);
    EXPECT_EQ(d.candidate_id, v.candidate_id);
    EXPECT_EQ(d.source_generation, v.source_generation);
    EXPECT_EQ(d.baseline_tip_seq, v.baseline_tip_seq);
    EXPECT_EQ(0, std::memcmp(d.baseline_tip_mac, v.baseline_tip_mac, 32));
    EXPECT_EQ(d.baseline_key_id, v.baseline_key_id);
    EXPECT_EQ(d.new_generation, v.new_generation);
    EXPECT_EQ(d.new_final_seq, v.new_final_seq);
    EXPECT_EQ(0, std::memcmp(d.new_final_tip_mac, v.new_final_tip_mac, 32));
    EXPECT_EQ(d.new_key_id, v.new_key_id);
    EXPECT_EQ(d.request_id, v.request_id);
    EXPECT_EQ(0, std::memcmp(d.content_root, v.content_root, 32));
    EXPECT_EQ(d.kek_key_id, v.kek_key_id);
    EXPECT_EQ(d.registered_producer_mask, v.registered_producer_mask);
    EXPECT_EQ(d.producer_count, v.producer_count);
    for (std::size_t i = 0; i < kMaxSealHandoffProducers; ++i) {
        EXPECT_EQ(d.ring_id[i], v.ring_id[i]) << "ring_id[" << i << "]";
    }
}

TEST(SealExportStartedCodec, RejectsUnknownFormatVersion) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.format_version = 99;
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::UnknownVersion);
}

TEST(SealExportStartedCodec, RejectsWrongTotalBytes) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.total_bytes = 999;
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::TotalBytesInvalid);
}

TEST(SealExportStartedCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(3);
    const SealExportStartedWire v = make_sample_started();
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    buf[20] ^= std::byte{0x01};  // flip a bit inside store_uuid_hi
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());
}

TEST(SealExportStartedCodec, RejectsWrongKey) {
    const auto key = make_key(3);
    const auto wrong_key = make_key(4);
    const SealExportStartedWire v = make_sample_started();
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, wrong_key, out), SealJournalPreconditionDecodeStatus::ChecksumMismatch);
}

TEST(SealExportStartedCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(3);
    const SealExportStartedWire v = make_sample_started();
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              SealJournalPreconditionDecodeStatus::Truncated);
}

TEST(SealExportStartedCodec, LegacyV1_192ByteBuffer_RejectedAsTruncated) {
    const auto key = make_key(3);
    std::array<std::byte, kSealExportStartedLegacyV1Bytes> legacy{};
    std::byte* p = legacy.data();
    detail::write_u32_le(p, 1u);  // format_version = 1 (legacy)
    detail::write_u32_le(p, static_cast<std::uint32_t>(kSealExportStartedLegacyV1Bytes));
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(legacy, key, out), SealJournalPreconditionDecodeStatus::Truncated);
    EXPECT_FALSE(out.has_value());
}

TEST(SealExportStartedCodec, Draft234ByteBuffer_RejectedAsTruncated) {
    const auto key = make_key(3);
    std::array<std::byte, kSealExportStartedDraft234Bytes> draft{};
    std::byte* p = draft.data();
    detail::write_u32_le(p, kSealExportStartedFormatVersion);
    detail::write_u32_le(p, static_cast<std::uint32_t>(kSealExportStartedDraft234Bytes));
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(draft, key, out), SealJournalPreconditionDecodeStatus::Truncated);
    EXPECT_FALSE(out.has_value());
}

TEST(PeekSealExportStartedKekKeyId, ReadsBeforeMacVerification) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.kek_key_id = 0xDEAD;
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    buf.back() ^= std::byte{0xFF};  // corrupt the mac -- peek must still work
    std::uint32_t out_key_id = 0;
    EXPECT_TRUE(peek_seal_export_started_kek_key_id(buf, out_key_id));
    EXPECT_EQ(out_key_id, 0xDEADu);
}

TEST(PeekSealExportStartedKekKeyId, RejectsTooShortBuffer) {
    std::array<std::byte, 4> short_buf{};
    std::uint32_t out_key_id = 0;
    EXPECT_FALSE(peek_seal_export_started_kek_key_id(short_buf, out_key_id));
}

// ===========================================================================
// SealExportStartedWire: "MAC valid but semantically invalid" rejections
// (P0-1: validate_seal_export_started_shape() must run before the MAC check
// and must independently reject every one of these, not just tamper cases)
// ===========================================================================

TEST(SealExportStartedCodec, RejectsValidMacWithEmptyTopologyMask) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.registered_producer_mask = 0;
    v.producer_count = 0;
    for (auto& r : v.ring_id) r = 0;
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);  // MAC over the (already illegal) content
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

// kMaxSealHandoffProducers==8 currently makes this always pass for any
// uint8_t mask -- written anyway so the check is real coverage if that
// constant is ever lowered (see validate_seal_export_started_shape()'s own
// comment). Confirms the codec at least does not spuriously reject a full
// 8-slot mask.
TEST(SealExportStartedCodec, AcceptsValidMacWithMaskAtFullCapacity) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.registered_producer_mask = 0xFF;
    v.producer_count = 8;
    for (std::size_t i = 0; i < kMaxSealHandoffProducers; ++i) v.ring_id[i] = static_cast<std::uint32_t>(200 + i);
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::Ok);
}

TEST(SealExportStartedCodec, RejectsValidMacWithProducerCountNotMatchingPopcount) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.producer_count = 2;  // mask has 3 bits set
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
}

TEST(SealExportStartedCodec, RejectsValidMacWithSetSlotZeroRingId) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.ring_id[2] = 0;  // bit 2 is set in the mask
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
}

TEST(SealExportStartedCodec, RejectsValidMacWithUnsetSlotNonzeroRingId) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.ring_id[1] = 999;  // bit 1 is NOT set in the mask
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
}

TEST(SealExportStartedCodec, RejectsValidMacWithDuplicateRingId) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.ring_id[5] = v.ring_id[0];  // both set slots now share the same ring_id
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
}

TEST(SealExportStartedCodec, RejectsValidMacWithZeroCandidateId) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.candidate_id = 0;
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
}

TEST(SealExportStartedCodec, RejectsValidMacWithZeroRequestId) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.request_id = 0;
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
}

TEST(SealExportStartedCodec, RejectsValidMacWithGenerationNotSourcePlusOne) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.new_generation = v.source_generation + 2;  // skip
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
}

TEST(SealExportStartedCodec, RejectsValidMacWithSourceGenerationAtMaxUint32) {
    const auto key = make_key(3);
    SealExportStartedWire v = make_sample_started();
    v.source_generation = std::numeric_limits<std::uint32_t>::max();
    v.new_generation = 0;  // would-be wraparound "+1"
    std::array<std::byte, kSealExportStartedWireBytes> buf{};
    encode_seal_export_started_wire(buf, v, key);
    std::optional<VerifiedSealExportStarted> out;
    EXPECT_EQ(decode_seal_export_started_wire(buf, key, out), SealJournalPreconditionDecodeStatus::MalformedField);
}

// ===========================================================================
// Property test: decode never crashes/UBs on arbitrary bytes
// ===========================================================================

TEST(SealJournalPreconditionCodecProperty, DecodeNeverCrashesOnRandomBytes) {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    const auto key = make_key(9);

    for (int trial = 0; trial < 2000; ++trial) {
        const std::size_t len = static_cast<std::size_t>(trial % 400);
        std::vector<std::byte> bytes(len);
        for (auto& b : bytes) b = static_cast<std::byte>(byte_dist(rng));

        std::optional<VerifiedSealIdWatermark> out1;
        (void)decode_seal_id_watermark_wire(bytes, key, out1);
        std::optional<VerifiedSealExportStarted> out2;
        (void)decode_seal_export_started_wire(bytes, key, out2);
    }
    SUCCEED();
}
