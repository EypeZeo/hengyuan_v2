// SPDX-License-Identifier: proprietary
// seal_export_migration_cleanup_abandon_codec.hpp — real encode/decode for
// the three remaining "Round B" seal-journal Wire types (docs/SPEC_
// INVARIANTS.md's "Seal-journal Round E Slice 2b" entry): SealExportStarted
// MigrationWire (.mig), SealStartedCleanupTombstoneWire (.clr), and
// SealStartedAbandonWire (.abd). SealIdWatermark/SealExportStartedWire were
// covered by Round E Slice 1 (seal_journal_precondition_codec.hpp);
// SealJournalCommitWatermark/SealJournalTombstoneWire ("Round A" cluster)
// are covered by the parallel Slice 2a track
// (seal_journal_commit_tombstone_codec.hpp).
//
// Governance: L1 (pure computation, no file I/O), same split as every other
// codec file in this repo -- this file never opens a file or touches a
// filesystem.
//
// SCOPE, read this before trusting anything below: this file gives all
// three types real, tested, MAC-verified encode/decode PLUS field-shape
// semantic validation (present_mask closed-set per started_kind, digest
// canonical-zero for absent companions, generation+1 algebra, id
// non-zero). It does NOT mean any of these types is wired to any
// production write path -- no file I/O, no IntentStore/CandidateLease/
// manager integration, no receipt concept, no phase-raising API exist
// anywhere in this codebase yet.
//
// SealStartedWireDecodeStatus is a separate enum from every other codec
// file's decode-status enum on purpose -- these type families have no real
// coupling, and reusing one enum would imply a relationship that doesn't
// exist.
//
// DECODE GATE ORDER (fixed, all three types): length gate (in.size() >=
// kXxxWireBytes, else Truncated) -> format_version/total_bytes check (else
// UnknownVersion/TotalBytesInvalid) -> remaining fields' LE decode ->
// validate_*_shape() semantic check (else MalformedField/ReservedNonzero)
// -> HMAC comparison (else ChecksumMismatch) -> VerifiedX. This is NOT a
// "MAC already covers it so it can be skipped" simplification: bytes signed
// under the same KEK by a future ABI version, or by a misbehaving producer,
// can still carry a valid MAC. A decoder that blindly reads the current
// fixed offsets would misinterpret different-version bytes as current-
// version field values, defeating the fail-closed upgrade boundary this
// gate exists to provide.
//
// WRITE-SIDE SHAPE GATE: all three encode_*() functions in this file call
// their matching validate_*_shape() BEFORE writing any byte, and return 0
// (no output produced, no MAC computed) on semantic failure. This is a
// known gap NOT retrofitted onto Round E Slice 1's own two encoders
// (encode_seal_id_watermark_wire / encode_seal_export_started_wire in
// seal_journal_precondition_codec.hpp both still write+sign unconditionally
// -- shape is decode-only there) -- fixing that is out of scope for this
// round to avoid touching already-merged Slice 1 code; it is tracked as a
// known follow-up in this file's ledger entry. Because production encode_*
// now refuses semantically-invalid input, this file's own tests construct
// "MAC valid but semantically invalid" fixtures via a raw signer helper
// local to the test file, not via these production encoders.
//
// VERIFIED TYPES (same idiom every other codec in this repo established):
// VerifiedSealExportStartedMigration/VerifiedSealStartedCleanupTombstone/
// VerifiedSealStartedAbandon can only be constructed by a successful
// decode_*() call, and hold their value BY COPY (never a span/reference
// into the caller's buffer or key material).
//
// DECODER LENGTH CONTRACT: decode_*() functions accept
// std::span<const std::byte> and only require `in.size() >= kXxxWireBytes`
// (short -> Truncated); they do not care whether the caller's span has
// trailing bytes beyond that -- the same established layering as every
// other codec in this repo (a stricter "exactly N bytes" contract belongs
// to a future I/O read layer this file has no part of).

#pragma once

#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/durable_frame_codec.hpp>  // reuses hy::detail::write_u8/write_u32_le/
                                              // write_u64_le/write_bytes/read_u8/read_u32_le/
                                              // read_u64_le/read_bytes
#include <hengyuan/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace hy {

namespace seal_started_wire_codec_detail {

inline std::span<const std::byte> domain_bytes(std::string_view domain) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(domain.data()), domain.size());
}

inline constexpr std::string_view kMigrationDomain = "HY-SEALSTARTMIG-v2";
inline constexpr std::string_view kCleanupDomain = "HY-SEALSTARTCLR-v2";
inline constexpr std::string_view kAbandonDomain = "HY-SEALSTARTABD-v1";

// present_mask bit constants shared by Cleanup (.clr, bits 0..2) and Abandon
// (.abd, bits 0..3) -- both mirror the L4 spec's "bit0=L, bit1=V, bit2=M
// (,bit3=C for .abd) at authorize time" comment already transcribed onto
// durable_control_plane.hpp's own struct definitions.
inline constexpr std::uint8_t kMaskBitL = 0b001;
inline constexpr std::uint8_t kMaskBitV = 0b010;
inline constexpr std::uint8_t kMaskBitM = 0b100;
inline constexpr std::uint8_t kMaskBitC = 0b1000;
// Closed file-set per started_kind, "at authorize time" (CREATE_NEW happens
// once, before any unlink) -- NativeV2's Started file set is {L} alone;
// MigratedV2's is {L,V,M}.
inline constexpr std::uint8_t kNativeV2Mask = kMaskBitL;
inline constexpr std::uint8_t kMigratedV2Mask = kMaskBitL | kMaskBitV | kMaskBitM;

inline bool all_zero(const std::uint8_t* p, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

}  // namespace seal_started_wire_codec_detail

enum class SealStartedWireDecodeStatus : std::uint8_t {
    Ok = 0,
    Truncated = 1,          // fewer bytes available than the wire type needs
    UnknownVersion = 2,     // format_version != the currently-active format version
    TotalBytesInvalid = 3,  // total_bytes field != this type's fixed wire size
    MalformedField = 4,     // a field-shape/semantic rule was violated (see this
                             // file's validate_*_shape() functions) -- physically
                             // well-formed bytes, MAC not yet checked, but the
                             // content itself cannot be legal
    ReservedNonzero = 5,    // a reserved field is non-zero (Cleanup's reserved0)
    ChecksumMismatch = 6,   // physically complete, shape-valid wire whose mac
                             // doesn't match its own content -- Corrupt, not Truncated
};

// ===========================================================================
// SealExportStartedMigrationWire (.mig)
// ===========================================================================

// No topology/id-cross-field structure beyond id non-zero -- there is no
// present_mask/started_kind/phase in this wire type.
inline bool validate_seal_export_started_migration_shape(
    const SealExportStartedMigrationWire& v) noexcept {
    if (v.candidate_id == 0) return false;
    if (v.request_id == 0) return false;
    // legacy_file_digest/v2_file_digest/legacy_mac/v2_mac are cryptographic
    // outputs -- deliberately NOT checked for "must be nonzero": crypto
    // outputs can legitimately be zero, and no L4 rule names a "must be
    // zero under condition X" case for these four fields the way it does
    // for Cleanup/Abandon's digest fields below.
    return true;
}

// Unauthenticated peek at v2_kek_key_id only -- callers resolve hmac_key
// from KeyRing for THIS key_id BEFORE calling decode.
inline bool peek_seal_export_started_migration_v2_kek_key_id(std::span<const std::byte> in,
                                                                std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kOffset = 4 + 4 + 8 + 8 + 8 + 8 + 4;  // = 44
    static_assert(kOffset == 44);
    if (in.size() < kOffset + 4) return false;
    const std::byte* p = in.data() + kOffset;
    out_key_id = detail::read_u32_le(p);
    return true;
}

// v.mac is ignored on input and overwritten in the returned bytes. Returns 0
// (no bytes written) if v fails validate_seal_export_started_migration_shape().
inline std::size_t encode_seal_export_started_migration_wire(
    std::span<std::byte, kSealExportStartedMigrationWireBytes> out,
    const SealExportStartedMigrationWire& v, std::span<const std::byte> hmac_key) noexcept {
    if (!validate_seal_export_started_migration_shape(v)) return 0;

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
    h.update(seal_started_wire_codec_detail::domain_bytes(seal_started_wire_codec_detail::kMigrationDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kSealExportStartedMigrationWireBytes;
}

class VerifiedSealExportStartedMigration;

SealStartedWireDecodeStatus decode_seal_export_started_migration_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealExportStartedMigration>& out) noexcept;

class VerifiedSealExportStartedMigration {
public:
    const SealExportStartedMigrationWire& value() const noexcept { return value_; }

private:
    friend SealStartedWireDecodeStatus decode_seal_export_started_migration_wire(
        std::span<const std::byte>, std::span<const std::byte>,
        std::optional<VerifiedSealExportStartedMigration>&) noexcept;
    explicit VerifiedSealExportStartedMigration(const SealExportStartedMigrationWire& v) noexcept
        : value_(v) {}
    SealExportStartedMigrationWire value_;
};

inline SealStartedWireDecodeStatus decode_seal_export_started_migration_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealExportStartedMigration>& out) noexcept {
    out.reset();
    if (in.size() < kSealExportStartedMigrationWireBytes) return SealStartedWireDecodeStatus::Truncated;

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    SealExportStartedMigrationWire v{};
    v.format_version = detail::read_u32_le(p);
    if (v.format_version != kSealExportStartedMigrationFormatVersion) {
        return SealStartedWireDecodeStatus::UnknownVersion;
    }
    v.total_bytes = detail::read_u32_le(p);
    if (v.total_bytes != kSealExportStartedMigrationWireBytes) {
        return SealStartedWireDecodeStatus::TotalBytesInvalid;
    }
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.candidate_id = detail::read_u64_le(p);
    v.request_id = detail::read_u64_le(p);
    v.legacy_kek_key_id = detail::read_u32_le(p);
    v.v2_kek_key_id = detail::read_u32_le(p);
    detail::read_bytes(p, v.legacy_file_digest, sizeof(v.legacy_file_digest));
    detail::read_bytes(p, v.v2_file_digest, sizeof(v.v2_file_digest));
    detail::read_bytes(p, v.legacy_mac, sizeof(v.legacy_mac));
    detail::read_bytes(p, v.v2_mac, sizeof(v.v2_mac));

    if (!validate_seal_export_started_migration_shape(v)) {
        return SealStartedWireDecodeStatus::MalformedField;
    }

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(seal_started_wire_codec_detail::domain_bytes(seal_started_wire_codec_detail::kMigrationDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return SealStartedWireDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedSealExportStartedMigration(v));
    return SealStartedWireDecodeStatus::Ok;
}

// ===========================================================================
// SealStartedCleanupTombstoneWire (.clr)
// ===========================================================================

// Field-shape/semantic validation, independent of MAC verification. Called
// from decode BEFORE the MAC comparison. Deliberately does NOT check the
// seven "== Started.X" fields (baseline_tip_seq/baseline_tip_mac/
// baseline_key_id/new_final_seq/new_final_tip_mac/new_key_id/content_root)
// against a real SealExportStartedWire/bridge record -- that is cross-file
// consistency this single-buffer decode cannot prove, and is explicitly out
// of scope for this round (see this file's own header comment and the
// ledger entry). Also does not check phase monotonicity (REPLACE-only-
// raises-phase is a cross-read property, not a single-decode one).
inline bool validate_seal_started_cleanup_shape(const SealStartedCleanupTombstoneWire& v) noexcept {
    using namespace seal_started_wire_codec_detail;

    if (v.candidate_id == 0) return false;
    if (v.request_id == 0) return false;

    if (v.started_kind != kSealStartedKindNativeV2 && v.started_kind != kSealStartedKindMigratedV2) {
        return false;
    }

    // present_mask is a fixed snapshot taken once at CREATE_NEW (authorize
    // time), before any unlink -- so it must exactly equal the closed file
    // set for started_kind, not merely be a subset of it.
    const std::uint8_t expected_mask =
        (v.started_kind == kSealStartedKindNativeV2) ? kNativeV2Mask : kMigratedV2Mask;
    if (v.present_mask != expected_mask) return false;

    if (v.phase < kSealStartedCleanupPhaseAuthorized || v.phase > kSealStartedCleanupPhaseClrPending) {
        return false;
    }

    if (v.reserved0 != 0) return false;

    // Internal algebraic relation (both operands live in this same wire) --
    // NOT the cross-file "== Started.new_generation" equality, which is a
    // separate, out-of-scope check.
    if (v.source_generation == std::numeric_limits<std::uint32_t>::max()) return false;
    if (v.new_generation != v.source_generation + 1) return false;

    // Canonical-zero: digest_L/digest_V/digest_M are copied from the
    // admitted Started record at CREATE_NEW time (L4 spec: "copy proof
    // fields + digests + kind/mask from admitted Started"). NativeV2Started
    // has no V/M file to copy from, so digest_V/digest_M must be all-zero
    // for NativeV2 (extending digest_L's own documented "or zeros if bit0
    // clear" pattern -- see this file's header comment for the reasoning).
    // MigratedV2 has all three files, so no zero requirement applies there
    // (a real digest is cryptographic output and can legitimately be zero).
    if (v.started_kind == kSealStartedKindNativeV2) {
        if (!all_zero(v.digest_V, sizeof(v.digest_V))) return false;
        if (!all_zero(v.digest_M, sizeof(v.digest_M))) return false;
    }

    return true;
}

inline bool peek_seal_started_cleanup_kek_key_id(std::span<const std::byte> in,
                                                   std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kOffset = 4 + 4 + 8 + 8 + 8 + 8;  // = 40
    static_assert(kOffset == 40);
    if (in.size() < kOffset + 4) return false;
    const std::byte* p = in.data() + kOffset;
    out_key_id = detail::read_u32_le(p);
    return true;
}

inline std::size_t encode_seal_started_cleanup_tombstone_wire(
    std::span<std::byte, kSealStartedCleanupWireBytes> out, const SealStartedCleanupTombstoneWire& v,
    std::span<const std::byte> hmac_key) noexcept {
    if (!validate_seal_started_cleanup_shape(v)) return 0;

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
    h.update(seal_started_wire_codec_detail::domain_bytes(seal_started_wire_codec_detail::kCleanupDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kSealStartedCleanupWireBytes;
}

class VerifiedSealStartedCleanupTombstone;

SealStartedWireDecodeStatus decode_seal_started_cleanup_tombstone_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealStartedCleanupTombstone>& out) noexcept;

class VerifiedSealStartedCleanupTombstone {
public:
    const SealStartedCleanupTombstoneWire& value() const noexcept { return value_; }

private:
    friend SealStartedWireDecodeStatus decode_seal_started_cleanup_tombstone_wire(
        std::span<const std::byte>, std::span<const std::byte>,
        std::optional<VerifiedSealStartedCleanupTombstone>&) noexcept;
    explicit VerifiedSealStartedCleanupTombstone(const SealStartedCleanupTombstoneWire& v) noexcept
        : value_(v) {}
    SealStartedCleanupTombstoneWire value_;
};

inline SealStartedWireDecodeStatus decode_seal_started_cleanup_tombstone_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealStartedCleanupTombstone>& out) noexcept {
    out.reset();
    if (in.size() < kSealStartedCleanupWireBytes) return SealStartedWireDecodeStatus::Truncated;

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    SealStartedCleanupTombstoneWire v{};
    v.format_version = detail::read_u32_le(p);
    if (v.format_version != kSealStartedCleanupFormatVersion) {
        return SealStartedWireDecodeStatus::UnknownVersion;
    }
    v.total_bytes = detail::read_u32_le(p);
    if (v.total_bytes != kSealStartedCleanupWireBytes) {
        return SealStartedWireDecodeStatus::TotalBytesInvalid;
    }
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.candidate_id = detail::read_u64_le(p);
    v.request_id = detail::read_u64_le(p);
    v.kek_key_id = detail::read_u32_le(p);
    v.started_kind = detail::read_u8(p);
    v.present_mask = detail::read_u8(p);
    v.phase = detail::read_u8(p);
    v.reserved0 = detail::read_u8(p);
    v.source_generation = detail::read_u32_le(p);
    v.baseline_tip_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    v.baseline_key_id = detail::read_u32_le(p);
    v.new_generation = detail::read_u32_le(p);
    v.new_final_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.new_final_tip_mac, sizeof(v.new_final_tip_mac));
    v.new_key_id = detail::read_u32_le(p);
    detail::read_bytes(p, v.content_root, sizeof(v.content_root));
    detail::read_bytes(p, v.digest_L, sizeof(v.digest_L));
    detail::read_bytes(p, v.digest_V, sizeof(v.digest_V));
    detail::read_bytes(p, v.digest_M, sizeof(v.digest_M));

    if (v.reserved0 != 0) {
        return SealStartedWireDecodeStatus::ReservedNonzero;
    }
    if (!validate_seal_started_cleanup_shape(v)) {
        return SealStartedWireDecodeStatus::MalformedField;
    }

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(seal_started_wire_codec_detail::domain_bytes(seal_started_wire_codec_detail::kCleanupDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return SealStartedWireDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedSealStartedCleanupTombstone(v));
    return SealStartedWireDecodeStatus::Ok;
}

// ===========================================================================
// SealStartedAbandonWire (.abd)
// ===========================================================================

// Deliberately does NOT check the four "== Started.X" fields
// (baseline_tip_seq/baseline_tip_mac/baseline_key_id/content_root) against a
// real Started/bridge record, phase monotonicity, or bit3==1's digest_C
// against a real C file's content -- all cross-file/cross-read consistency
// this single-buffer decode cannot prove (see this file's header comment).
inline bool validate_seal_started_abandon_shape(const SealStartedAbandonWire& v) noexcept {
    using namespace seal_started_wire_codec_detail;

    if (v.candidate_id == 0) return false;
    if (v.request_id == 0) return false;

    if (v.started_kind != kSealStartedKindNativeV2 && v.started_kind != kSealStartedKindMigratedV2) {
        return false;
    }

    // Low 3 bits (L/V/M) must exactly equal started_kind's closed file set,
    // same derivation as Cleanup's present_mask above. bit3 (C) may
    // legally be either value (L4: A0's no-C sets bit3=0, with-C sets
    // bit3=1 -- both paths are legal). Bits 4..7 are out of range and must
    // be zero.
    const std::uint8_t expected_lvm = (v.started_kind == kSealStartedKindNativeV2) ? kNativeV2Mask : kMigratedV2Mask;
    if ((v.present_mask & (kMaskBitL | kMaskBitV | kMaskBitM)) != expected_lvm) return false;
    if ((v.present_mask & ~static_cast<std::uint8_t>(kMaskBitL | kMaskBitV | kMaskBitM | kMaskBitC)) != 0) {
        return false;
    }

    // The one "must be all-zero" rule here, direct from the L4 spec's A0
    // no-C case: bit3(C)==0 requires digest_C all-zero. bit3==1's digest_C
    // is real cryptographic output (a SHA-256 over the unauthorized C
    // file's bytes) and is not checked against anything here -- comparing
    // it to the actual C file's content is cross-file, out of scope.
    const bool c_bit_set = (v.present_mask & kMaskBitC) != 0;
    if (!c_bit_set && !all_zero(v.digest_C, sizeof(v.digest_C))) return false;

    if (v.abandon_reason != kSealStartedAbandonReasonNotFound) return false;

    if (v.phase < kSealStartedAbandonPhaseAuthorized || v.phase > kSealStartedAbandonPhaseAbdPending) {
        return false;
    }

    return true;
}

inline bool peek_seal_started_abandon_kek_key_id(std::span<const std::byte> in,
                                                   std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kOffset = 4 + 4 + 8 + 8 + 8 + 8;  // = 40
    static_assert(kOffset == 40);
    if (in.size() < kOffset + 4) return false;
    const std::byte* p = in.data() + kOffset;
    out_key_id = detail::read_u32_le(p);
    return true;
}

inline std::size_t encode_seal_started_abandon_wire(std::span<std::byte, kSealStartedAbandonWireBytes> out,
                                                       const SealStartedAbandonWire& v,
                                                       std::span<const std::byte> hmac_key) noexcept {
    if (!validate_seal_started_abandon_shape(v)) return 0;

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
    h.update(seal_started_wire_codec_detail::domain_bytes(seal_started_wire_codec_detail::kAbandonDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kSealStartedAbandonWireBytes;
}

class VerifiedSealStartedAbandon;

SealStartedWireDecodeStatus decode_seal_started_abandon_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealStartedAbandon>& out) noexcept;

class VerifiedSealStartedAbandon {
public:
    const SealStartedAbandonWire& value() const noexcept { return value_; }

private:
    friend SealStartedWireDecodeStatus decode_seal_started_abandon_wire(
        std::span<const std::byte>, std::span<const std::byte>,
        std::optional<VerifiedSealStartedAbandon>&) noexcept;
    explicit VerifiedSealStartedAbandon(const SealStartedAbandonWire& v) noexcept : value_(v) {}
    SealStartedAbandonWire value_;
};

inline SealStartedWireDecodeStatus decode_seal_started_abandon_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealStartedAbandon>& out) noexcept {
    out.reset();
    if (in.size() < kSealStartedAbandonWireBytes) return SealStartedWireDecodeStatus::Truncated;

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    SealStartedAbandonWire v{};
    v.format_version = detail::read_u32_le(p);
    if (v.format_version != kSealStartedAbandonFormatVersion) {
        return SealStartedWireDecodeStatus::UnknownVersion;
    }
    v.total_bytes = detail::read_u32_le(p);
    if (v.total_bytes != kSealStartedAbandonWireBytes) {
        return SealStartedWireDecodeStatus::TotalBytesInvalid;
    }
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.candidate_id = detail::read_u64_le(p);
    v.request_id = detail::read_u64_le(p);
    v.kek_key_id = detail::read_u32_le(p);
    v.started_kind = detail::read_u8(p);
    v.abandon_reason = detail::read_u8(p);
    v.present_mask = detail::read_u8(p);
    v.phase = detail::read_u8(p);
    v.source_generation = detail::read_u32_le(p);
    v.baseline_tip_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    v.baseline_key_id = detail::read_u32_le(p);
    detail::read_bytes(p, v.content_root, sizeof(v.content_root));
    detail::read_bytes(p, v.digest_C, sizeof(v.digest_C));

    if (!validate_seal_started_abandon_shape(v)) {
        return SealStartedWireDecodeStatus::MalformedField;
    }

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(seal_started_wire_codec_detail::domain_bytes(seal_started_wire_codec_detail::kAbandonDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return SealStartedWireDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedSealStartedAbandon(v));
    return SealStartedWireDecodeStatus::Ok;
}

}  // namespace hy
