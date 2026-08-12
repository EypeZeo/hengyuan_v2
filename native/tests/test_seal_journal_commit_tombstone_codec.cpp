// Pure in-memory round-trip, tamper-detection, and semantic-validation tests
// for seal_journal_commit_tombstone_codec.hpp (Round E Slice 2a). No file
// I/O anywhere in this file -- this codec has no I/O layer at all yet.
#include <gtest/gtest.h>
#include <hengyuan/seal_journal_commit_tombstone_codec.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <span>
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

SealJournalCommitWatermark make_sample_watermark() {
    SealJournalCommitWatermark w{};
    w.store_uuid_lo = 0x1111111111111111ull;
    w.store_uuid_hi = 0x2222222222222222ull;
    w.candidate_id = 42;
    w.highest_committed_journal_seq = 7;
    w.kek_key_id = 9;
    return w;
}

SealJournalTombstoneWire make_sample_tombstone() {
    SealJournalTombstoneWire v{};
    v.format_version = kSealJournalTombstoneFormatVersion;
    v.total_bytes = kSealJournalTombstoneBytes;
    v.store_uuid_lo = 0x3333333333333333ull;
    v.store_uuid_hi = 0x4444444444444444ull;
    v.kek_key_id = 5;
    v.candidate_id = 42;
    v.journal_seq = 7;
    fill_bytes(v.entry_mac, 0x40);
    return v;
}

// RAW SIGNERS -- production encode_*() functions refuse semantically illegal
// input (write-side shape gate: return 0, write nothing, compute no MAC), so
// the "MAC correct but semantically illegal" negative tests cannot use them.
// These helpers are the production encode logic minus the
// validate_*_shape() step -- identical field writes, identical MAC domain --
// visible only in this test file. They are deliberately NOT part of the
// production API; their only purpose is to construct "field-legal encoding,
// MAC correct, but semantically illegal" bytes for the rejection tests.
std::size_t raw_sign_watermark(std::span<std::byte, kSealJournalCommitWatermarkWireBytes> out,
                               const SealJournalCommitWatermark& v,
                               std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;

    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u64_le(p, v.candidate_id);
    detail::write_u64_le(p, v.highest_committed_journal_seq);
    detail::write_u32_le(p, v.kek_key_id);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(seal_journal_commit_tombstone_codec_detail::domain_bytes(
        seal_journal_commit_tombstone_codec_detail::kSealJournalCommitWatermarkDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kSealJournalCommitWatermarkWireBytes;
}

std::size_t raw_sign_tombstone(std::span<std::byte, kSealJournalTombstoneBytes> out,
                               const SealJournalTombstoneWire& v,
                               std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;

    detail::write_u32_le(p, v.format_version);
    detail::write_u32_le(p, v.total_bytes);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u32_le(p, v.kek_key_id);
    detail::write_u64_le(p, v.candidate_id);
    detail::write_u64_le(p, v.journal_seq);
    detail::write_bytes(p, v.entry_mac, sizeof(v.entry_mac));

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(seal_journal_commit_tombstone_codec_detail::domain_bytes(
        seal_journal_commit_tombstone_codec_detail::kSealJournalTombstoneDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kSealJournalTombstoneBytes;
}

bool is_closed_enum_value(SealJournalCommitTombstoneDecodeStatus s) noexcept {
    switch (s) {
        case SealJournalCommitTombstoneDecodeStatus::Ok:
        case SealJournalCommitTombstoneDecodeStatus::Truncated:
        case SealJournalCommitTombstoneDecodeStatus::UnknownVersion:
        case SealJournalCommitTombstoneDecodeStatus::TotalBytesInvalid:
        case SealJournalCommitTombstoneDecodeStatus::MalformedField:
        case SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch:
            return true;
    }
    return false;
}

bool all_zero(std::span<const std::byte> s) noexcept {
    for (std::byte b : s) {
        if (b != std::byte{0}) return false;
    }
    return true;
}

}  // namespace

// ===========================================================================
// SealJournalCommitWatermark codec
// ===========================================================================

TEST(SealJournalCommitWatermarkCodec, RoundTripsAllFieldsExactly) {
    const auto key = make_key(1);
    const SealJournalCommitWatermark v = make_sample_watermark();
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    EXPECT_EQ(encode_seal_journal_commit_watermark_wire(buf, v, key), kSealJournalCommitWatermarkWireBytes);

    std::optional<VerifiedSealJournalCommitWatermark> out;
    ASSERT_EQ(decode_seal_journal_commit_watermark_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    const auto& d = out->value();
    EXPECT_EQ(d.store_uuid_lo, v.store_uuid_lo);
    EXPECT_EQ(d.store_uuid_hi, v.store_uuid_hi);
    EXPECT_EQ(d.candidate_id, v.candidate_id);
    EXPECT_EQ(d.highest_committed_journal_seq, v.highest_committed_journal_seq);
    EXPECT_EQ(d.kek_key_id, v.kek_key_id);
}

TEST(SealJournalCommitWatermarkCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(1);
    const SealJournalCommitWatermark v = make_sample_watermark();
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    encode_seal_journal_commit_watermark_wire(buf, v, key);
    buf[10] ^= std::byte{0x01};  // flip a bit inside store_uuid_hi
    std::optional<VerifiedSealJournalCommitWatermark> out;
    EXPECT_EQ(decode_seal_journal_commit_watermark_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());
}

TEST(SealJournalCommitWatermarkCodec, RejectsWrongKey) {
    const auto key = make_key(1);
    const auto wrong_key = make_key(2);
    const SealJournalCommitWatermark v = make_sample_watermark();
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    encode_seal_journal_commit_watermark_wire(buf, v, key);
    std::optional<VerifiedSealJournalCommitWatermark> out;
    EXPECT_EQ(decode_seal_journal_commit_watermark_wire(buf, wrong_key, out),
              SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch);
}

TEST(SealJournalCommitWatermarkCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(1);
    const SealJournalCommitWatermark v = make_sample_watermark();
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    encode_seal_journal_commit_watermark_wire(buf, v, key);
    std::optional<VerifiedSealJournalCommitWatermark> out;
    EXPECT_EQ(decode_seal_journal_commit_watermark_wire(
                  std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              SealJournalCommitTombstoneDecodeStatus::Truncated);
    EXPECT_FALSE(out.has_value());
}

// candidate_id==0 with a VALID MAC must be rejected -- built via raw_sign_watermark
// because the production encoder's write-side shape gate refuses to sign it.
TEST(SealJournalCommitWatermarkCodec, RejectsValidMacWithZeroCandidateId) {
    const auto key = make_key(1);
    SealJournalCommitWatermark v = make_sample_watermark();
    v.candidate_id = 0;
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    raw_sign_watermark(buf, v, key);  // MAC over the (already illegal) content
    std::optional<VerifiedSealJournalCommitWatermark> out;
    EXPECT_EQ(decode_seal_journal_commit_watermark_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

// highest_committed_journal_seq==0 IS legal ("0 = none yet" per the struct
// comment) -- the name says "Accepts" on purpose, to guard against someone
// later misreading the tombstone's journal_seq!=0 rule (different meaning!)
// and adding a spurious zero-rejection here.
TEST(SealJournalCommitWatermarkCodec, AcceptsValidMacWithZeroHighestCommittedJournalSeq) {
    const auto key = make_key(1);
    SealJournalCommitWatermark v = make_sample_watermark();
    v.highest_committed_journal_seq = 0;
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    encode_seal_journal_commit_watermark_wire(buf, v, key);
    std::optional<VerifiedSealJournalCommitWatermark> out;
    ASSERT_EQ(decode_seal_journal_commit_watermark_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->value().highest_committed_journal_seq, 0u);
}

// UINT64_MAX is a legal, decodable value -- the upper-bound fence belongs to
// a future watermark-advancing write path this slice does not have (P1-2,
// see the codec's own header comment / docs/SPEC_INVARIANTS.md's Round E
// Slice 2a entry, where it is a mandatory acceptance item for any future
// advancing slice). This test name says "Accepts" on purpose, to guard
// against someone later adding a spurious upper-bound rejection here.
TEST(SealJournalCommitWatermarkCodec, AcceptsValidMacWithMaxHighestCommittedJournalSeq) {
    const auto key = make_key(1);
    SealJournalCommitWatermark v = make_sample_watermark();
    v.highest_committed_journal_seq = std::numeric_limits<std::uint64_t>::max();
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    encode_seal_journal_commit_watermark_wire(buf, v, key);
    std::optional<VerifiedSealJournalCommitWatermark> out;
    ASSERT_EQ(decode_seal_journal_commit_watermark_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->value().highest_committed_journal_seq, std::numeric_limits<std::uint64_t>::max());
}

// Regression: a failing decode must leave `out` EMPTY even when it
// previously held a successfully decoded value -- must not rely on the
// implementation having out.reset() (that is exactly the code this test
// exists to catch being removed).
TEST(SealJournalCommitWatermarkCodec, DecodeFailureLeavesOutEmpty) {
    const auto key = make_key(1);
    const SealJournalCommitWatermark v = make_sample_watermark();
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    encode_seal_journal_commit_watermark_wire(buf, v, key);

    std::optional<VerifiedSealJournalCommitWatermark> out;
    ASSERT_EQ(decode_seal_journal_commit_watermark_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());

    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> tampered = buf;
    tampered[3] ^= std::byte{0x01};
    EXPECT_EQ(decode_seal_journal_commit_watermark_wire(tampered, key, out),
              SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());

    ASSERT_TRUE(decode_seal_journal_commit_watermark_wire(buf, key, out) ==
                SealJournalCommitTombstoneDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(decode_seal_journal_commit_watermark_wire(
                  std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              SealJournalCommitTombstoneDecodeStatus::Truncated);
    EXPECT_FALSE(out.has_value());
}

// Write-side shape gate: semantically illegal input must return 0 and write
// NOTHING (no bytes, no MAC) -- the buffer must remain all-zero.
TEST(SealJournalCommitWatermarkCodec, RefusesToEncodeSemanticallyInvalidInput) {
    const auto key = make_key(1);
    SealJournalCommitWatermark v = make_sample_watermark();
    v.candidate_id = 0;
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    EXPECT_EQ(encode_seal_journal_commit_watermark_wire(buf, v, key), 0u);
    EXPECT_TRUE(all_zero(buf));
}

TEST(PeekSealJournalCommitWatermarkKekKeyId, ReadsBeforeMacVerification) {
    const auto key = make_key(1);
    SealJournalCommitWatermark v = make_sample_watermark();
    v.kek_key_id = 0xDEAD;
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    encode_seal_journal_commit_watermark_wire(buf, v, key);
    buf.back() ^= std::byte{0xFF};  // corrupt the mac -- peek must still work
    std::uint32_t out_key_id = 0;
    EXPECT_TRUE(peek_seal_journal_commit_watermark_kek_key_id(buf, out_key_id));
    EXPECT_EQ(out_key_id, 0xDEADu);
}

TEST(PeekSealJournalCommitWatermarkKekKeyId, RejectsTooShortBuffer) {
    std::array<std::byte, 4> short_buf{};
    std::uint32_t out_key_id = 0;
    EXPECT_FALSE(peek_seal_journal_commit_watermark_kek_key_id(short_buf, out_key_id));
}

// ===========================================================================
// SealJournalTombstoneWire codec
// ===========================================================================

TEST(SealJournalTombstoneWireCodec, RoundTripsAllFieldsExactly) {
    const auto key = make_key(3);
    const SealJournalTombstoneWire v = make_sample_tombstone();
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    EXPECT_EQ(encode_seal_journal_tombstone_wire(buf, v, key), kSealJournalTombstoneBytes);

    std::optional<VerifiedSealJournalTombstoneWire> out;
    ASSERT_EQ(decode_seal_journal_tombstone_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    const auto& d = out->value();
    EXPECT_EQ(d.format_version, v.format_version);
    EXPECT_EQ(d.total_bytes, v.total_bytes);
    EXPECT_EQ(d.store_uuid_lo, v.store_uuid_lo);
    EXPECT_EQ(d.store_uuid_hi, v.store_uuid_hi);
    EXPECT_EQ(d.kek_key_id, v.kek_key_id);
    EXPECT_EQ(d.candidate_id, v.candidate_id);
    EXPECT_EQ(d.journal_seq, v.journal_seq);
    EXPECT_EQ(0, std::memcmp(d.entry_mac, v.entry_mac, 32));
}

TEST(SealJournalTombstoneWireCodec, RejectsUnknownFormatVersion) {
    const auto key = make_key(3);
    SealJournalTombstoneWire v = make_sample_tombstone();
    v.format_version = 99;
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    encode_seal_journal_tombstone_wire(buf, v, key);
    std::optional<VerifiedSealJournalTombstoneWire> out;
    EXPECT_EQ(decode_seal_journal_tombstone_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::UnknownVersion);
    EXPECT_FALSE(out.has_value());
}

TEST(SealJournalTombstoneWireCodec, RejectsWrongTotalBytes) {
    const auto key = make_key(3);
    SealJournalTombstoneWire v = make_sample_tombstone();
    v.total_bytes = 999;
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    encode_seal_journal_tombstone_wire(buf, v, key);
    std::optional<VerifiedSealJournalTombstoneWire> out;
    EXPECT_EQ(decode_seal_journal_tombstone_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::TotalBytesInvalid);
    EXPECT_FALSE(out.has_value());
}

TEST(SealJournalTombstoneWireCodec, DetectsSingleByteTamperViaMac) {
    const auto key = make_key(3);
    const SealJournalTombstoneWire v = make_sample_tombstone();
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    encode_seal_journal_tombstone_wire(buf, v, key);
    buf[20] ^= std::byte{0x01};  // flip a bit inside store_uuid_hi
    std::optional<VerifiedSealJournalTombstoneWire> out;
    EXPECT_EQ(decode_seal_journal_tombstone_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());
}

TEST(SealJournalTombstoneWireCodec, RejectsWrongKey) {
    const auto key = make_key(3);
    const auto wrong_key = make_key(4);
    const SealJournalTombstoneWire v = make_sample_tombstone();
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    encode_seal_journal_tombstone_wire(buf, v, key);
    std::optional<VerifiedSealJournalTombstoneWire> out;
    EXPECT_EQ(decode_seal_journal_tombstone_wire(buf, wrong_key, out),
              SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());
}

TEST(SealJournalTombstoneWireCodec, RejectsTruncatedBuffer) {
    const auto key = make_key(3);
    const SealJournalTombstoneWire v = make_sample_tombstone();
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    encode_seal_journal_tombstone_wire(buf, v, key);
    std::optional<VerifiedSealJournalTombstoneWire> out;
    EXPECT_EQ(decode_seal_journal_tombstone_wire(
                  std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              SealJournalCommitTombstoneDecodeStatus::Truncated);
    EXPECT_FALSE(out.has_value());
}

// candidate_id==0 with a VALID MAC must be rejected -- built via
// raw_sign_tombstone because the production encoder's write-side shape gate
// refuses to sign it.
TEST(SealJournalTombstoneWireCodec, RejectsValidMacWithZeroCandidateId) {
    const auto key = make_key(3);
    SealJournalTombstoneWire v = make_sample_tombstone();
    v.candidate_id = 0;
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    raw_sign_tombstone(buf, v, key);  // MAC over the (already illegal) content
    std::optional<VerifiedSealJournalTombstoneWire> out;
    EXPECT_EQ(decode_seal_journal_tombstone_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

// journal_seq==0 with a VALID MAC must be rejected -- journal seqs start at
// 1 (BINANCE_PRIVATE_REST_L4_SPEC.md:5059), 0 cannot be any real entry's
// number. Deliberately the OPPOSITE rule from the watermark's
// highest_committed_journal_seq==0 (which is legal); the two are not
// analogous.
TEST(SealJournalTombstoneWireCodec, RejectsValidMacWithZeroJournalSeq) {
    const auto key = make_key(3);
    SealJournalTombstoneWire v = make_sample_tombstone();
    v.journal_seq = 0;
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    raw_sign_tombstone(buf, v, key);  // MAC over the (already illegal) content
    std::optional<VerifiedSealJournalTombstoneWire> out;
    EXPECT_EQ(decode_seal_journal_tombstone_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::MalformedField);
    EXPECT_FALSE(out.has_value());
}

// Regression: same contract as the watermark's DecodeFailureLeavesOutEmpty.
TEST(SealJournalTombstoneWireCodec, DecodeFailureLeavesOutEmpty) {
    const auto key = make_key(3);
    const SealJournalTombstoneWire v = make_sample_tombstone();
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    encode_seal_journal_tombstone_wire(buf, v, key);

    std::optional<VerifiedSealJournalTombstoneWire> out;
    ASSERT_EQ(decode_seal_journal_tombstone_wire(buf, key, out),
              SealJournalCommitTombstoneDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());

    std::array<std::byte, kSealJournalTombstoneBytes> tampered = buf;
    tampered[10] ^= std::byte{0x01};  // flip a bit inside store_uuid_lo (past the
                                      // format_version/total_bytes header checks)
    EXPECT_EQ(decode_seal_journal_tombstone_wire(tampered, key, out),
              SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch);
    EXPECT_FALSE(out.has_value());

    ASSERT_TRUE(decode_seal_journal_tombstone_wire(buf, key, out) ==
                SealJournalCommitTombstoneDecodeStatus::Ok);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(decode_seal_journal_tombstone_wire(
                  std::span<const std::byte>(buf.data(), buf.size() - 1), key, out),
              SealJournalCommitTombstoneDecodeStatus::Truncated);
    EXPECT_FALSE(out.has_value());
}

// Write-side shape gate: both candidate_id==0 and journal_seq==0 must
// return 0 and write NOTHING.
TEST(SealJournalTombstoneWireCodec, RefusesToEncodeSemanticallyInvalidInput) {
    const auto key = make_key(3);

    SealJournalTombstoneWire v = make_sample_tombstone();
    v.candidate_id = 0;
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    EXPECT_EQ(encode_seal_journal_tombstone_wire(buf, v, key), 0u);
    EXPECT_TRUE(all_zero(buf));

    v = make_sample_tombstone();
    v.journal_seq = 0;
    EXPECT_EQ(encode_seal_journal_tombstone_wire(buf, v, key), 0u);
    EXPECT_TRUE(all_zero(buf));
}

TEST(PeekSealJournalTombstoneKekKeyId, ReadsBeforeMacVerification) {
    const auto key = make_key(3);
    SealJournalTombstoneWire v = make_sample_tombstone();
    v.kek_key_id = 0xDEAD;
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    encode_seal_journal_tombstone_wire(buf, v, key);
    buf.back() ^= std::byte{0xFF};  // corrupt the mac -- peek must still work
    std::uint32_t out_key_id = 0;
    EXPECT_TRUE(peek_seal_journal_tombstone_kek_key_id(buf, out_key_id));
    EXPECT_EQ(out_key_id, 0xDEADu);
}

TEST(PeekSealJournalTombstoneKekKeyId, RejectsTooShortBuffer) {
    std::array<std::byte, 4> short_buf{};
    std::uint32_t out_key_id = 0;
    EXPECT_FALSE(peek_seal_journal_tombstone_kek_key_id(short_buf, out_key_id));
}

// ===========================================================================
// Property test: decode never crashes/UBs on arbitrary bytes
// ===========================================================================

TEST(SealJournalCommitTombstoneCodecProperty, DecodeNeverCrashesOnRandomBytes) {
    std::mt19937 rng(0xBEEF5EEDu);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    const auto key = make_key(9);

    for (int trial = 0; trial < 2000; ++trial) {
        const std::size_t len = static_cast<std::size_t>(trial % 200);
        std::vector<std::byte> bytes(len);
        for (auto& b : bytes) b = static_cast<std::byte>(byte_dist(rng));

        std::optional<VerifiedSealJournalCommitWatermark> out1;
        const auto s1 = decode_seal_journal_commit_watermark_wire(bytes, key, out1);
        EXPECT_TRUE(is_closed_enum_value(s1)) << "watermark decode, trial " << trial;
        if (s1 != SealJournalCommitTombstoneDecodeStatus::Ok) {
            EXPECT_FALSE(out1.has_value()) << "watermark decode, trial " << trial;
        } else {
            std::optional<VerifiedSealJournalCommitWatermark> out1b;
            const auto s1b = decode_seal_journal_commit_watermark_wire(bytes, key, out1b);
            ASSERT_EQ(s1b, SealJournalCommitTombstoneDecodeStatus::Ok) << "trial " << trial;
            ASSERT_TRUE(out1b.has_value());
            EXPECT_EQ(out1b->value().store_uuid_lo, out1->value().store_uuid_lo);
            EXPECT_EQ(out1b->value().store_uuid_hi, out1->value().store_uuid_hi);
            EXPECT_EQ(out1b->value().candidate_id, out1->value().candidate_id);
            EXPECT_EQ(out1b->value().highest_committed_journal_seq, out1->value().highest_committed_journal_seq);
            EXPECT_EQ(out1b->value().kek_key_id, out1->value().kek_key_id);
            EXPECT_EQ(0, std::memcmp(out1b->value().mac, out1->value().mac, 32));
        }

        std::optional<VerifiedSealJournalTombstoneWire> out2;
        const auto s2 = decode_seal_journal_tombstone_wire(bytes, key, out2);
        EXPECT_TRUE(is_closed_enum_value(s2)) << "tombstone decode, trial " << trial;
        if (s2 != SealJournalCommitTombstoneDecodeStatus::Ok) {
            EXPECT_FALSE(out2.has_value()) << "tombstone decode, trial " << trial;
        } else {
            std::optional<VerifiedSealJournalTombstoneWire> out2b;
            const auto s2b = decode_seal_journal_tombstone_wire(bytes, key, out2b);
            ASSERT_EQ(s2b, SealJournalCommitTombstoneDecodeStatus::Ok) << "trial " << trial;
            ASSERT_TRUE(out2b.has_value());
            EXPECT_EQ(out2b->value().format_version, out2->value().format_version);
            EXPECT_EQ(out2b->value().total_bytes, out2->value().total_bytes);
            EXPECT_EQ(out2b->value().store_uuid_lo, out2->value().store_uuid_lo);
            EXPECT_EQ(out2b->value().store_uuid_hi, out2->value().store_uuid_hi);
            EXPECT_EQ(out2b->value().kek_key_id, out2->value().kek_key_id);
            EXPECT_EQ(out2b->value().candidate_id, out2->value().candidate_id);
            EXPECT_EQ(out2b->value().journal_seq, out2->value().journal_seq);
            EXPECT_EQ(0, std::memcmp(out2b->value().entry_mac, out2->value().entry_mac, 32));
            EXPECT_EQ(0, std::memcmp(out2b->value().mac, out2->value().mac, 32));
        }
    }
    SUCCEED();
}
