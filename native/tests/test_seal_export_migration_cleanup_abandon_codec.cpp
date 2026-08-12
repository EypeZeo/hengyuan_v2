// Pure in-memory round-trip, tamper-detection, version/length-gate, and
// semantic-validation tests for seal_export_migration_cleanup_abandon_codec.hpp
// (Round E Slice 2b). No file I/O anywhere in this file -- this codec has no
// I/O layer at all yet.
#include <gtest/gtest.h>
#include <hengyuan/seal_export_migration_cleanup_abandon_codec.hpp>

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

void zero_bytes(std::uint8_t (&arr)[32]) {
    for (int i = 0; i < 32; ++i) arr[i] = 0;
}

SealExportStartedMigrationWire make_sample_migration() {
    SealExportStartedMigrationWire v{};
    v.format_version = kSealExportStartedMigrationFormatVersion;
    v.total_bytes = kSealExportStartedMigrationWireBytes;
    v.store_uuid_lo = 0x1111111111111111ull;
    v.store_uuid_hi = 0x2222222222222222ull;
    v.candidate_id = 100;
    v.request_id = 200;
    v.legacy_kek_key_id = 5;
    v.v2_kek_key_id = 7;
    fill_bytes(v.legacy_file_digest, 0x10);
    fill_bytes(v.v2_file_digest, 0x20);
    fill_bytes(v.legacy_mac, 0x30);
    fill_bytes(v.v2_mac, 0x40);
    return v;
}

// A legal NativeV2 SealStartedCleanupTombstoneWire: present_mask==0b001,
// digest_V/digest_M all-zero (no V/M file exists for NativeV2).
SealStartedCleanupTombstoneWire make_sample_cleanup_native() {
    SealStartedCleanupTombstoneWire v{};
    v.format_version = kSealStartedCleanupFormatVersion;
    v.total_bytes = kSealStartedCleanupWireBytes;
    v.store_uuid_lo = 0x1111111111111111ull;
    v.store_uuid_hi = 0x2222222222222222ull;
    v.candidate_id = 100;
    v.request_id = 200;
    v.kek_key_id = 7;
    v.started_kind = kSealStartedKindNativeV2;
    v.present_mask = 0b001;
    v.phase = kSealStartedCleanupPhaseAuthorized;
    v.reserved0 = 0;
    v.source_generation = 5;
    v.baseline_tip_seq = 42;
    fill_bytes(v.baseline_tip_mac, 0x10);
    v.baseline_key_id = 3;
    v.new_generation = 6;
    v.new_final_seq = 99;
    fill_bytes(v.new_final_tip_mac, 0x20);
    v.new_key_id = 4;
    fill_bytes(v.content_root, 0x30);
    fill_bytes(v.digest_L, 0x40);
    zero_bytes(v.digest_V);
    zero_bytes(v.digest_M);
    return v;
}

// A legal MigratedV2 SealStartedCleanupTombstoneWire: present_mask==0b111,
// all three digests real (nonzero) SHA-256 outputs.
SealStartedCleanupTombstoneWire make_sample_cleanup_migrated() {
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.started_kind = kSealStartedKindMigratedV2;
    v.present_mask = 0b111;
    fill_bytes(v.digest_V, 0x50);
    fill_bytes(v.digest_M, 0x60);
    return v;
}

// A legal SealStartedAbandonWire, NativeV2, no-C case (bit3=0, digest_C
// all-zero).
SealStartedAbandonWire make_sample_abandon_no_c() {
    SealStartedAbandonWire v{};
    v.format_version = kSealStartedAbandonFormatVersion;
    v.total_bytes = kSealStartedAbandonWireBytes;
    v.store_uuid_lo = 0x1111111111111111ull;
    v.store_uuid_hi = 0x2222222222222222ull;
    v.candidate_id = 100;
    v.request_id = 200;
    v.kek_key_id = 7;
    v.started_kind = kSealStartedKindNativeV2;
    v.abandon_reason = kSealStartedAbandonReasonNotFound;
    v.present_mask = 0b0001;  // L only, bit3(C)=0
    v.phase = kSealStartedAbandonPhaseAuthorized;
    v.source_generation = 5;
    v.baseline_tip_seq = 42;
    fill_bytes(v.baseline_tip_mac, 0x10);
    v.baseline_key_id = 3;
    fill_bytes(v.content_root, 0x30);
    zero_bytes(v.digest_C);
    return v;
}

// A legal SealStartedAbandonWire, MigratedV2, with-C case (bit3=1, digest_C
// a real nonzero digest).
SealStartedAbandonWire make_sample_abandon_with_c() {
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.started_kind = kSealStartedKindMigratedV2;
    v.present_mask = 0b1111;  // L+V+M+C
    fill_bytes(v.digest_C, 0x70);
    return v;
}

// ===========================================================================
// Raw signer helpers -- construct "MAC valid but semantically invalid" wire
// bytes without going through the (now shape-gated) production encoders.
// These duplicate the field-write sequence from
// seal_export_migration_cleanup_abandon_codec.hpp's encode_*() functions,
// deliberately WITHOUT the validate_*_shape() call at the top -- test-only,
// never exposed outside this file.
// ===========================================================================

void raw_encode_migration(std::span<std::byte, kSealExportStartedMigrationWireBytes> out,
                           const SealExportStartedMigrationWire& v, std::span<const std::byte> hmac_key) {
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_u32_le(p, v.format_version);
    detail::write_u32_le(p, v.total_bytes);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u64_le(p, v.candidate_id);
    detail::write_u64_le(p, v.request_id);
    detail::write_u32_le(p, v.legacy_kek_key_id);
    detail::write_u32_le(p, v.v2_kek_key_id);
    detail::write_bytes(p, v.legacy_file_digest, sizeof(v.legacy_file_digest));
    detail::write_bytes(p, v.v2_file_digest, sizeof(v.v2_file_digest));
    detail::write_bytes(p, v.legacy_mac, sizeof(v.legacy_mac));
    detail::write_bytes(p, v.v2_mac, sizeof(v.v2_mac));
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>("HY-SEALSTARTMIG-v2"), 19));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());
}

void raw_encode_cleanup(std::span<std::byte, kSealStartedCleanupWireBytes> out,
                         const SealStartedCleanupTombstoneWire& v, std::span<const std::byte> hmac_key) {
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_u32_le(p, v.format_version);
    detail::write_u32_le(p, v.total_bytes);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u64_le(p, v.candidate_id);
    detail::write_u64_le(p, v.request_id);
    detail::write_u32_le(p, v.kek_key_id);
    detail::write_u8(p, v.started_kind);
    detail::write_u8(p, v.present_mask);
    detail::write_u8(p, v.phase);
    detail::write_u8(p, v.reserved0);
    detail::write_u32_le(p, v.source_generation);
    detail::write_u64_le(p, v.baseline_tip_seq);
    detail::write_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    detail::write_u32_le(p, v.baseline_key_id);
    detail::write_u32_le(p, v.new_generation);
    detail::write_u64_le(p, v.new_final_seq);
    detail::write_bytes(p, v.new_final_tip_mac, sizeof(v.new_final_tip_mac));
    detail::write_u32_le(p, v.new_key_id);
    detail::write_bytes(p, v.content_root, sizeof(v.content_root));
    detail::write_bytes(p, v.digest_L, sizeof(v.digest_L));
    detail::write_bytes(p, v.digest_V, sizeof(v.digest_V));
    detail::write_bytes(p, v.digest_M, sizeof(v.digest_M));
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>("HY-SEALSTARTCLR-v2"), 19));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());
}

void raw_encode_abandon(std::span<std::byte, kSealStartedAbandonWireBytes> out, const SealStartedAbandonWire& v,
                         std::span<const std::byte> hmac_key) {
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_u32_le(p, v.format_version);
    detail::write_u32_le(p, v.total_bytes);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u64_le(p, v.candidate_id);
    detail::write_u64_le(p, v.request_id);
    detail::write_u32_le(p, v.kek_key_id);
    detail::write_u8(p, v.started_kind);
    detail::write_u8(p, v.abandon_reason);
    detail::write_u8(p, v.present_mask);
    detail::write_u8(p, v.phase);
    detail::write_u32_le(p, v.source_generation);
    detail::write_u64_le(p, v.baseline_tip_seq);
    detail::write_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    detail::write_u32_le(p, v.baseline_key_id);
    detail::write_bytes(p, v.content_root, sizeof(v.content_root));
    detail::write_bytes(p, v.digest_C, sizeof(v.digest_C));
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>("HY-SEALSTARTABD-v1"), 19));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());
}

}  // namespace

// ===========================================================================
// SealExportStartedMigrationWire codec
// ===========================================================================

TEST(SealExportStartedMigrationCodec, RoundTripsAllFieldsExactly) {
    const auto key = make_key(1);
    const SealExportStartedMigrationWire v = make_sample_migration();
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    EXPECT_EQ(encode_seal_export_started_migration_wire(buf, v, key), kSealExportStartedMigrationWireBytes);

    std::optional<VerifiedSealExportStartedMigration> out;
    ASSERT_EQ(decode_seal_export_started_migration_wire(buf, key, out), SealStartedWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    const auto& d = out->value();
    EXPECT_EQ(d.format_version, v.format_version);
    EXPECT_EQ(d.total_bytes, v.total_bytes);
    EXPECT_EQ(d.store_uuid_lo, v.store_uuid_lo);
    EXPECT_EQ(d.store_uuid_hi, v.store_uuid_hi);
    EXPECT_EQ(d.candidate_id, v.candidate_id);
    EXPECT_EQ(d.request_id, v.request_id);
    EXPECT_EQ(d.legacy_kek_key_id, v.legacy_kek_key_id);
    EXPECT_EQ(d.v2_kek_key_id, v.v2_kek_key_id);
    EXPECT_EQ(0, std::memcmp(d.legacy_file_digest, v.legacy_file_digest, 32));
    EXPECT_EQ(0, std::memcmp(d.v2_file_digest, v.v2_file_digest, 32));
    EXPECT_EQ(0, std::memcmp(d.legacy_mac, v.legacy_mac, 32));
    EXPECT_EQ(0, std::memcmp(d.v2_mac, v.v2_mac, 32));
}

TEST(SealExportStartedMigrationCodec, RejectsUnknownFormatVersion) {
    const auto key = make_key(1);
    SealExportStartedMigrationWire v = make_sample_migration();
    v.format_version = 99;
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    raw_encode_migration(buf, v, key);
    std::optional<VerifiedSealExportStartedMigration> out;
    EXPECT_EQ(decode_seal_export_started_migration_wire(buf, key, out), SealStartedWireDecodeStatus::UnknownVersion);
    EXPECT_FALSE(out.has_value());
}

TEST(SealExportStartedMigrationCodec, RejectsWrongTotalBytes) {
    const auto key = make_key(1);
    SealExportStartedMigrationWire v = make_sample_migration();
    v.total_bytes = 999;
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    raw_encode_migration(buf, v, key);
    std::optional<VerifiedSealExportStartedMigration> out;
    EXPECT_EQ(decode_seal_export_started_migration_wire(buf, key, out), SealStartedWireDecodeStatus::TotalBytesInvalid);
    EXPECT_FALSE(out.has_value());
}

TEST(SealExportStartedMigrationCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(1);
    const SealExportStartedMigrationWire v = make_sample_migration();
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    encode_seal_export_started_migration_wire(buf, v, key);
    buf[20] ^= std::byte{0x01};
    std::optional<VerifiedSealExportStartedMigration> out;
    EXPECT_EQ(decode_seal_export_started_migration_wire(buf, key, out), SealStartedWireDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());
}

TEST(SealExportStartedMigrationCodec, RejectsWrongKey) {
    const auto key = make_key(1);
    const auto wrong_key = make_key(2);
    const SealExportStartedMigrationWire v = make_sample_migration();
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    encode_seal_export_started_migration_wire(buf, v, key);
    std::optional<VerifiedSealExportStartedMigration> out;
    EXPECT_EQ(decode_seal_export_started_migration_wire(buf, wrong_key, out), SealStartedWireDecodeStatus::ChecksumMismatch);
}

TEST(SealExportStartedMigrationCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(1);
    const SealExportStartedMigrationWire v = make_sample_migration();
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    encode_seal_export_started_migration_wire(buf, v, key);
    std::optional<VerifiedSealExportStartedMigration> out;
    EXPECT_EQ(decode_seal_export_started_migration_wire(
                  std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              SealStartedWireDecodeStatus::Truncated);
}

TEST(SealExportStartedMigrationCodec, Draft136ByteBuffer_RejectedAsTruncated) {
    const auto key = make_key(1);
    std::array<std::byte, kSealExportStartedMigrationDraft136Bytes> draft{};
    std::byte* p = draft.data();
    detail::write_u32_le(p, kSealExportStartedMigrationFormatVersion);
    detail::write_u32_le(p, static_cast<std::uint32_t>(kSealExportStartedMigrationDraft136Bytes));
    std::optional<VerifiedSealExportStartedMigration> out;
    EXPECT_EQ(decode_seal_export_started_migration_wire(draft, key, out), SealStartedWireDecodeStatus::Truncated);
    EXPECT_FALSE(out.has_value());
}

TEST(SealExportStartedMigrationCodec, RejectsValidMacWithZeroCandidateId) {
    const auto key = make_key(1);
    SealExportStartedMigrationWire v = make_sample_migration();
    v.candidate_id = 0;
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    raw_encode_migration(buf, v, key);
    std::optional<VerifiedSealExportStartedMigration> out;
    EXPECT_EQ(decode_seal_export_started_migration_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealExportStartedMigrationCodec, RejectsValidMacWithZeroRequestId) {
    const auto key = make_key(1);
    SealExportStartedMigrationWire v = make_sample_migration();
    v.request_id = 0;
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    raw_encode_migration(buf, v, key);
    std::optional<VerifiedSealExportStartedMigration> out;
    EXPECT_EQ(decode_seal_export_started_migration_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealExportStartedMigrationCodec, RefusesToEncodeSemanticallyInvalidInput) {
    const auto key = make_key(1);
    SealExportStartedMigrationWire v = make_sample_migration();
    v.candidate_id = 0;
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    EXPECT_EQ(encode_seal_export_started_migration_wire(buf, v, key), 0u);
}

TEST(SealExportStartedMigrationCodec, OutIsResetAfterSubsequentFailure) {
    const auto key = make_key(1);
    const SealExportStartedMigrationWire v = make_sample_migration();
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    encode_seal_export_started_migration_wire(buf, v, key);
    std::optional<VerifiedSealExportStartedMigration> out;
    ASSERT_EQ(decode_seal_export_started_migration_wire(buf, key, out), SealStartedWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());  // out now holds a stale successful value

    buf[0] ^= std::byte{0xFF};  // corrupt format_version -> UnknownVersion
    EXPECT_EQ(decode_seal_export_started_migration_wire(buf, key, out), SealStartedWireDecodeStatus::UnknownVersion);
    EXPECT_FALSE(out.has_value()) << "out must be cleared, not left holding the prior successful value";
}

TEST(PeekSealExportStartedMigrationV2KekKeyId, ReadsBeforeMacVerification) {
    const auto key = make_key(1);
    SealExportStartedMigrationWire v = make_sample_migration();
    v.v2_kek_key_id = 0xDEAD;
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    encode_seal_export_started_migration_wire(buf, v, key);
    buf.back() ^= std::byte{0xFF};
    std::uint32_t out_key_id = 0;
    EXPECT_TRUE(peek_seal_export_started_migration_v2_kek_key_id(buf, out_key_id));
    EXPECT_EQ(out_key_id, 0xDEADu);
}

TEST(PeekSealExportStartedMigrationV2KekKeyId, RejectsTooShortBuffer) {
    std::array<std::byte, 4> short_buf{};
    std::uint32_t out_key_id = 0;
    EXPECT_FALSE(peek_seal_export_started_migration_v2_kek_key_id(short_buf, out_key_id));
}

// ===========================================================================
// SealStartedCleanupTombstoneWire codec
// ===========================================================================

TEST(SealStartedCleanupCodec, RoundTripsAllFieldsExactly_NativeV2) {
    const auto key = make_key(3);
    const SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    EXPECT_EQ(encode_seal_started_cleanup_tombstone_wire(buf, v, key), kSealStartedCleanupWireBytes);

    std::optional<VerifiedSealStartedCleanupTombstone> out;
    ASSERT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    const auto& d = out->value();
    EXPECT_EQ(d.candidate_id, v.candidate_id);
    EXPECT_EQ(d.request_id, v.request_id);
    EXPECT_EQ(d.started_kind, v.started_kind);
    EXPECT_EQ(d.present_mask, v.present_mask);
    EXPECT_EQ(d.phase, v.phase);
    EXPECT_EQ(d.reserved0, v.reserved0);
    EXPECT_EQ(d.source_generation, v.source_generation);
    EXPECT_EQ(d.new_generation, v.new_generation);
    EXPECT_EQ(0, std::memcmp(d.digest_L, v.digest_L, 32));
    EXPECT_EQ(0, std::memcmp(d.digest_V, v.digest_V, 32));
    EXPECT_EQ(0, std::memcmp(d.digest_M, v.digest_M, 32));
}

TEST(SealStartedCleanupCodec, RoundTripsAllFieldsExactly_MigratedV2) {
    const auto key = make_key(3);
    const SealStartedCleanupTombstoneWire v = make_sample_cleanup_migrated();
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    EXPECT_EQ(encode_seal_started_cleanup_tombstone_wire(buf, v, key), kSealStartedCleanupWireBytes);

    std::optional<VerifiedSealStartedCleanupTombstone> out;
    ASSERT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->value().present_mask, 0b111);
}

TEST(SealStartedCleanupCodec, RejectsUnknownFormatVersion) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.format_version = 99;
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::UnknownVersion);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsWrongTotalBytes) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.total_bytes = 999;
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::TotalBytesInvalid);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(3);
    const SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    encode_seal_started_cleanup_tombstone_wire(buf, v, key);
    buf[20] ^= std::byte{0x01};
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsWrongKey) {
    const auto key = make_key(3);
    const auto wrong_key = make_key(4);
    const SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    encode_seal_started_cleanup_tombstone_wire(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, wrong_key, out), SealStartedWireDecodeStatus::ChecksumMismatch);
}

TEST(SealStartedCleanupCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(3);
    const SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    encode_seal_started_cleanup_tombstone_wire(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(
                  std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              SealStartedWireDecodeStatus::Truncated);
}

TEST(SealStartedCleanupCodec, Draft176ByteBuffer_RejectedAsTruncated) {
    const auto key = make_key(3);
    std::array<std::byte, kSealStartedCleanupDraft176Bytes> draft{};
    std::byte* p = draft.data();
    detail::write_u32_le(p, kSealStartedCleanupFormatVersion);
    detail::write_u32_le(p, static_cast<std::uint32_t>(kSealStartedCleanupDraft176Bytes));
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(draft, key, out), SealStartedWireDecodeStatus::Truncated);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithZeroCandidateId) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.candidate_id = 0;
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithZeroRequestId) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.request_id = 0;
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithPresentMaskNotMatchingNativeV2) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.present_mask = 0b011;  // L+V, but NativeV2 requires exactly L
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithPresentMaskNotMatchingMigratedV2) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_migrated();
    v.present_mask = 0b011;  // L+V only, but MigratedV2 requires L+V+M
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithNativeV2AbsentVDigestNonzero) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    fill_bytes(v.digest_V, 0x99);  // NativeV2 requires digest_V all-zero
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithNativeV2AbsentMDigestNonzero) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    fill_bytes(v.digest_M, 0x99);  // NativeV2 requires digest_M all-zero
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithReservedNonzero) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.reserved0 = 1;
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::ReservedNonzero);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithPhaseOutOfRange) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.phase = kSealStartedCleanupPhaseClrPending + 1;
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithGenerationNotSourcePlusOne) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.new_generation = v.source_generation + 2;
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RejectsValidMacWithSourceGenerationAtMaxUint32) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.source_generation = std::numeric_limits<std::uint32_t>::max();
    v.new_generation = 0;  // would-be wraparound "+1"
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    raw_encode_cleanup(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedCleanupCodec, RefusesToEncodeSemanticallyInvalidInput) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.reserved0 = 1;
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    EXPECT_EQ(encode_seal_started_cleanup_tombstone_wire(buf, v, key), 0u);
}

TEST(SealStartedCleanupCodec, OutIsResetAfterSubsequentFailure) {
    const auto key = make_key(3);
    const SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    encode_seal_started_cleanup_tombstone_wire(buf, v, key);
    std::optional<VerifiedSealStartedCleanupTombstone> out;
    ASSERT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());

    buf[0] ^= std::byte{0xFF};  // corrupt format_version -> UnknownVersion
    EXPECT_EQ(decode_seal_started_cleanup_tombstone_wire(buf, key, out), SealStartedWireDecodeStatus::UnknownVersion);
    EXPECT_FALSE(out.has_value()) << "out must be cleared, not left holding the prior successful value";
}

TEST(PeekSealStartedCleanupKekKeyId, ReadsBeforeMacVerification) {
    const auto key = make_key(3);
    SealStartedCleanupTombstoneWire v = make_sample_cleanup_native();
    v.kek_key_id = 0xDEAD;
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    encode_seal_started_cleanup_tombstone_wire(buf, v, key);
    buf.back() ^= std::byte{0xFF};
    std::uint32_t out_key_id = 0;
    EXPECT_TRUE(peek_seal_started_cleanup_kek_key_id(buf, out_key_id));
    EXPECT_EQ(out_key_id, 0xDEADu);
}

TEST(PeekSealStartedCleanupKekKeyId, RejectsTooShortBuffer) {
    std::array<std::byte, 4> short_buf{};
    std::uint32_t out_key_id = 0;
    EXPECT_FALSE(peek_seal_started_cleanup_kek_key_id(short_buf, out_key_id));
}

// ===========================================================================
// SealStartedAbandonWire codec
// ===========================================================================

TEST(SealStartedAbandonCodec, RoundTripsAllFieldsExactly_NoC) {
    const auto key = make_key(5);
    const SealStartedAbandonWire v = make_sample_abandon_no_c();
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    EXPECT_EQ(encode_seal_started_abandon_wire(buf, v, key), kSealStartedAbandonWireBytes);

    std::optional<VerifiedSealStartedAbandon> out;
    ASSERT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    const auto& d = out->value();
    EXPECT_EQ(d.candidate_id, v.candidate_id);
    EXPECT_EQ(d.abandon_reason, v.abandon_reason);
    EXPECT_EQ(d.present_mask, v.present_mask);
    EXPECT_EQ(0, std::memcmp(d.digest_C, v.digest_C, 32));
}

TEST(SealStartedAbandonCodec, RoundTripsAllFieldsExactly_WithC) {
    const auto key = make_key(5);
    const SealStartedAbandonWire v = make_sample_abandon_with_c();
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    EXPECT_EQ(encode_seal_started_abandon_wire(buf, v, key), kSealStartedAbandonWireBytes);

    std::optional<VerifiedSealStartedAbandon> out;
    ASSERT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->value().present_mask, 0b1111);
    EXPECT_EQ(0, std::memcmp(out->value().digest_C, v.digest_C, 32));
}

TEST(SealStartedAbandonCodec, RejectsUnknownFormatVersion) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.format_version = 99;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    raw_encode_abandon(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::UnknownVersion);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, RejectsWrongTotalBytes) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.total_bytes = 999;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    raw_encode_abandon(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::TotalBytesInvalid);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(5);
    const SealStartedAbandonWire v = make_sample_abandon_no_c();
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    encode_seal_started_abandon_wire(buf, v, key);
    buf[20] ^= std::byte{0x01};
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, RejectsWrongKey) {
    const auto key = make_key(5);
    const auto wrong_key = make_key(6);
    const SealStartedAbandonWire v = make_sample_abandon_no_c();
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    encode_seal_started_abandon_wire(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, wrong_key, out), SealStartedWireDecodeStatus::ChecksumMismatch);
}

TEST(SealStartedAbandonCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(5);
    const SealStartedAbandonWire v = make_sample_abandon_no_c();
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    encode_seal_started_abandon_wire(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              SealStartedWireDecodeStatus::Truncated);
}

TEST(SealStartedAbandonCodec, RejectsValidMacWithZeroCandidateId) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.candidate_id = 0;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    raw_encode_abandon(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, RejectsValidMacWithZeroRequestId) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.request_id = 0;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    raw_encode_abandon(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, RejectsValidMacWithPresentMaskNotMatchingNativeV2) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.present_mask = 0b0011;  // L+V, but NativeV2 requires exactly L in low 3 bits
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    raw_encode_abandon(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, RejectsValidMacWithHighBitsSetInPresentMask) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.present_mask = static_cast<std::uint8_t>(v.present_mask | 0b1'0000);  // bit4 out of range
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    raw_encode_abandon(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, RejectsValidMacWithUnsetCBitButNonzeroDigest) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    fill_bytes(v.digest_C, 0x77);  // bit3(C) is unset, so digest_C must be all-zero
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    raw_encode_abandon(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, RejectsValidMacWithUnknownAbandonReason) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.abandon_reason = 99;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    raw_encode_abandon(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, RejectsValidMacWithPhaseOutOfRange) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.phase = kSealStartedAbandonPhaseAbdPending + 1;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    raw_encode_abandon(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

TEST(SealStartedAbandonCodec, RefusesToEncodeSemanticallyInvalidInput) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.abandon_reason = 99;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    EXPECT_EQ(encode_seal_started_abandon_wire(buf, v, key), 0u);
}

TEST(SealStartedAbandonCodec, OutIsResetAfterSubsequentFailure) {
    const auto key = make_key(5);
    const SealStartedAbandonWire v = make_sample_abandon_no_c();
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    encode_seal_started_abandon_wire(buf, v, key);
    std::optional<VerifiedSealStartedAbandon> out;
    ASSERT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());

    buf[0] ^= std::byte{0xFF};  // corrupt format_version -> UnknownVersion
    EXPECT_EQ(decode_seal_started_abandon_wire(buf, key, out), SealStartedWireDecodeStatus::UnknownVersion);
    EXPECT_FALSE(out.has_value()) << "out must be cleared, not left holding the prior successful value";
}

TEST(PeekSealStartedAbandonKekKeyId, ReadsBeforeMacVerification) {
    const auto key = make_key(5);
    SealStartedAbandonWire v = make_sample_abandon_no_c();
    v.kek_key_id = 0xDEAD;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    encode_seal_started_abandon_wire(buf, v, key);
    buf.back() ^= std::byte{0xFF};
    std::uint32_t out_key_id = 0;
    EXPECT_TRUE(peek_seal_started_abandon_kek_key_id(buf, out_key_id));
    EXPECT_EQ(out_key_id, 0xDEADu);
}

TEST(PeekSealStartedAbandonKekKeyId, RejectsTooShortBuffer) {
    std::array<std::byte, 4> short_buf{};
    std::uint32_t out_key_id = 0;
    EXPECT_FALSE(peek_seal_started_abandon_kek_key_id(short_buf, out_key_id));
}

// ===========================================================================
// Property test: decode never crashes/UBs on arbitrary bytes, and its
// contract holds -- status is always one of the seven known values,
// status != Ok implies out is empty, status == Ok implies re-decoding the
// same bytes is deterministic.
// ===========================================================================

namespace {

bool is_known_status(SealStartedWireDecodeStatus s) {
    switch (s) {
        case SealStartedWireDecodeStatus::Ok:
        case SealStartedWireDecodeStatus::Truncated:
        case SealStartedWireDecodeStatus::UnknownVersion:
        case SealStartedWireDecodeStatus::TotalBytesInvalid:
        case SealStartedWireDecodeStatus::MalformedField:
        case SealStartedWireDecodeStatus::ReservedNonzero:
        case SealStartedWireDecodeStatus::ChecksumMismatch:
            return true;
    }
    return false;
}

}  // namespace

TEST(SealStartedWireCodecProperty, DecodeNeverCrashesOnRandomBytes) {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    const auto key = make_key(9);

    for (int trial = 0; trial < 2000; ++trial) {
        const std::size_t len = static_cast<std::size_t>(trial % 400);
        std::vector<std::byte> bytes(len);
        for (auto& b : bytes) b = static_cast<std::byte>(byte_dist(rng));

        std::optional<VerifiedSealExportStartedMigration> out1;
        const auto s1 = decode_seal_export_started_migration_wire(bytes, key, out1);
        ASSERT_TRUE(is_known_status(s1));
        EXPECT_EQ(s1 != SealStartedWireDecodeStatus::Ok, !out1.has_value());
        if (s1 == SealStartedWireDecodeStatus::Ok) {
            std::optional<VerifiedSealExportStartedMigration> out1b;
            const auto s1b = decode_seal_export_started_migration_wire(bytes, key, out1b);
            EXPECT_EQ(s1b, SealStartedWireDecodeStatus::Ok);
            ASSERT_TRUE(out1b.has_value());
            EXPECT_EQ(0, std::memcmp(&out1->value(), &out1b->value(), sizeof(SealExportStartedMigrationWire)));
        }

        std::optional<VerifiedSealStartedCleanupTombstone> out2;
        const auto s2 = decode_seal_started_cleanup_tombstone_wire(bytes, key, out2);
        ASSERT_TRUE(is_known_status(s2));
        EXPECT_EQ(s2 != SealStartedWireDecodeStatus::Ok, !out2.has_value());
        if (s2 == SealStartedWireDecodeStatus::Ok) {
            std::optional<VerifiedSealStartedCleanupTombstone> out2b;
            const auto s2b = decode_seal_started_cleanup_tombstone_wire(bytes, key, out2b);
            EXPECT_EQ(s2b, SealStartedWireDecodeStatus::Ok);
            ASSERT_TRUE(out2b.has_value());
            EXPECT_EQ(0, std::memcmp(&out2->value(), &out2b->value(), sizeof(SealStartedCleanupTombstoneWire)));
        }

        std::optional<VerifiedSealStartedAbandon> out3;
        const auto s3 = decode_seal_started_abandon_wire(bytes, key, out3);
        ASSERT_TRUE(is_known_status(s3));
        EXPECT_EQ(s3 != SealStartedWireDecodeStatus::Ok, !out3.has_value());
        if (s3 == SealStartedWireDecodeStatus::Ok) {
            std::optional<VerifiedSealStartedAbandon> out3b;
            const auto s3b = decode_seal_started_abandon_wire(bytes, key, out3b);
            EXPECT_EQ(s3b, SealStartedWireDecodeStatus::Ok);
            ASSERT_TRUE(out3b.has_value());
            EXPECT_EQ(0, std::memcmp(&out3->value(), &out3b->value(), sizeof(SealStartedAbandonWire)));
        }
    }
    SUCCEED();
}
