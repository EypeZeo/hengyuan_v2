// SPDX-License-Identifier: proprietary
// seal_journal_precondition_codec.hpp — real encode/decode for the two
// "durable precondition" wire types Round D's own "Round D/E/F 边界" names as
// Round E's first requirement (docs/SPEC_INVARIANTS.md's "Seal-journal Round
// E Slice 1" entry): SealIdWatermark and SealExportStartedWire.
//
// Governance: L1 (pure computation, no file I/O), same split as
// compaction_intent_codec.hpp / durable_frame_codec.hpp -- this file never
// opens a file or touches a filesystem.
//
// SCOPE, read this before trusting anything below (this design went through
// one round of external Architect review -- v1 was rejected for downgrading
// SealExportStartedWire's topology validation to optional and for leaving
// SealIdWatermark's allocator-safety semantics undefined; see docs/
// SPEC_INVARIANTS.md's ledger entry for the full history): this file gives
// both types real, tested, MAC-verified encode/decode PLUS field-shape
// semantic validation. It does NOT mean either type is wired to any
// production write path -- no file I/O, no IntentStore/CandidateLease/
// manager integration, no receipt concept, no phase-raising API exist
// anywhere in this codebase yet. It does NOT implement real C/A (`.clr`/
// `.abd`) codec -- that is a separate, not-yet-started Round E work item.
//
// SealJournalPreconditionDecodeStatus is a separate enum from compaction_
// intent_codec.hpp's CompactionWireDecodeStatus on purpose -- these two type
// families have no real coupling, and reusing one enum for both would imply
// a relationship that doesn't exist.
//
// VERIFIED TYPES (same idiom compaction_intent_codec.hpp established):
// VerifiedSealIdWatermark/VerifiedSealExportStarted can only be constructed
// by a successful decode_*() call, and hold their value BY COPY (never a
// span/reference into the caller's buffer or key material) -- "MAC already
// verified" is a type-level guarantee, not a runtime hope, and the verified
// object's lifetime is never tied to the input buffer's.
//
// DECODER LENGTH CONTRACT: both decode_*() functions accept
// std::span<const std::byte> and only require `in.size() >= kXxxWireBytes`
// (short -> Truncated); they read/authenticate exactly the first
// kXxxWireBytes bytes and do not care whether the caller's span has trailing
// bytes beyond that -- this is not a security gap (the MAC covers only the
// authenticated content, so trailing garbage cannot influence the decoded
// result either way), it is this codebase's already-established layering
// (matching decode_compaction_candidate_intent_wire): a stricter "must be
// EXACTLY N bytes, not one more" contract belongs to a future I/O read
// layer (mirroring compaction_breadcrumb_io.hpp's read_validated_exact(),
// which reads exactly N bytes and probes for one extra byte to reject
// "too long"), not to this pure in-memory codec, because this file has no
// I/O layer at all yet.

#pragma once

#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/durable_frame_codec.hpp>  // reuses hy::detail::write_u8/write_u32_le/
                                              // write_u64_le/write_bytes/read_u8/read_u32_le/
                                              // read_u64_le/read_bytes
#include <hengyuan/sha256.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace hy {

namespace seal_journal_precondition_codec_detail {

inline std::span<const std::byte> domain_bytes(std::string_view domain) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(domain.data()), domain.size());
}

inline constexpr std::string_view kSealIdWatermarkDomain = "HY-SEALIDWM-v1";
inline constexpr std::string_view kSealExportStartedDomain = "HY-SEALSTART-v2";

}  // namespace seal_journal_precondition_codec_detail

enum class SealJournalPreconditionDecodeStatus : std::uint8_t {
    Ok = 0,
    Truncated = 1,          // fewer bytes available than the wire type needs
    UnknownVersion = 2,     // format_version != the one currently-active format version
                             // (SealIdWatermark has no format_version field -- never
                             // returned by its own decode function)
    TotalBytesInvalid = 3,  // total_bytes field != this type's fixed wire size
                             // (SealIdWatermark has no total_bytes field either)
    MalformedField = 4,     // a field-shape/semantic rule was violated (see
                             // validate_seal_export_started_shape() below, or
                             // SealIdWatermark's next_candidate_id/next_request_id==0
                             // rejection) -- physically well-formed bytes, MAC not
                             // yet checked, but the content itself cannot be legal
    ChecksumMismatch = 5,    // physically complete, shape-valid wire whose mac
                             // doesn't match its own content -- Corrupt, not Truncated
};

// ===========================================================================
// SealIdWatermark
// ===========================================================================

// v.mac is ignored on input and overwritten in the returned bytes -- callers
// never hand-compute a mac. Returns kSealIdWatermarkWireBytes.
inline std::size_t encode_seal_id_watermark_wire(std::span<std::byte, kSealIdWatermarkWireBytes> out,
                                                    const SealIdWatermark& v,
                                                    std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;

    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u64_le(p, v.next_candidate_id);
    detail::write_u64_le(p, v.next_request_id);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(seal_journal_precondition_codec_detail::domain_bytes(
        seal_journal_precondition_codec_detail::kSealIdWatermarkDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kSealIdWatermarkWireBytes;
}

class VerifiedSealIdWatermark;

SealJournalPreconditionDecodeStatus decode_seal_id_watermark_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealIdWatermark>& out) noexcept;

// A decoded-and-MAC-verified SealIdWatermark whose next_candidate_id/
// next_request_id have already been confirmed nonzero. No public constructor
// accepts an arbitrary struct -- the only way to obtain one is a successful
// decode_seal_id_watermark_wire() call.
class VerifiedSealIdWatermark {
public:
    const SealIdWatermark& value() const noexcept { return value_; }

private:
    friend SealJournalPreconditionDecodeStatus decode_seal_id_watermark_wire(
        std::span<const std::byte>, std::span<const std::byte>,
        std::optional<VerifiedSealIdWatermark>&) noexcept;
    explicit VerifiedSealIdWatermark(const SealIdWatermark& v) noexcept : value_(v) {}
    SealIdWatermark value_;
};

inline SealJournalPreconditionDecodeStatus decode_seal_id_watermark_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealIdWatermark>& out) noexcept {
    out.reset();
    if (in.size() < kSealIdWatermarkWireBytes) return SealJournalPreconditionDecodeStatus::Truncated;

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    SealIdWatermark v{};
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.next_candidate_id = detail::read_u64_le(p);
    v.next_request_id = detail::read_u64_le(p);

    // Allocator safety (docs/SPEC_INVARIANTS.md's Round E Slice 1 entry): 0 is
    // never a legal next-allocatable id -- a persisted next_*==0 watermark is
    // an already-contradictory/corrupt state, reject before spending the MAC
    // computation. UINT64_MAX is deliberately NOT rejected here -- see this
    // file's own header comment and the ledger entry for why the upper-bound
    // fence belongs to a future reservation-advancing write path this slice
    // does not have.
    if (v.next_candidate_id == 0 || v.next_request_id == 0) {
        return SealJournalPreconditionDecodeStatus::MalformedField;
    }

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(seal_journal_precondition_codec_detail::domain_bytes(
        seal_journal_precondition_codec_detail::kSealIdWatermarkDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return SealJournalPreconditionDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedSealIdWatermark(v));
    return SealJournalPreconditionDecodeStatus::Ok;
}

// ===========================================================================
// SealExportStartedWire
// ===========================================================================

// Unauthenticated peek at kek_key_id only -- callers resolve hmac_key from
// KeyRing for THIS key_id BEFORE calling decode. Mirrors compaction_intent_
// codec.hpp's peek_compaction_candidate_intent_kek_key_id(): returning false
// here does not imply Corrupt, the real decode call does that classification.
inline bool peek_seal_export_started_kek_key_id(std::span<const std::byte> in,
                                                  std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kKekKeyIdOffset =
        4 + 4 + 8 + 8 + 8 + 4 + 8 + 32 + 4 + 4 + 8 + 32 + 4 + 8 + 32;  // = 168, matches
                                                                        // durable_control_plane.hpp's
                                                                        // own "+168 u32 kek_key_id"
                                                                        // offset comment
    static_assert(kKekKeyIdOffset == 168);
    if (in.size() < kKekKeyIdOffset + 4) return false;
    const std::byte* p = in.data() + kKekKeyIdOffset;
    out_key_id = detail::read_u32_le(p);
    return true;
}

// Topology/generation/id shape validation, independent of MAC verification --
// this is the P0-1 fix from the external review: a MAC-valid but
// topologically-forged SealExportStartedWire (fabricated producer_count,
// zero ring_id in a set slot, nonzero ring_id in an unset slot, duplicate
// ring_id, zero candidate_id/request_id, or a generation that doesn't
// advance by exactly one) must never become a VerifiedSealExportStarted.
// Called from decode BEFORE the MAC comparison (matching compaction_intent_
// codec.hpp's own MalformedField-before-MAC ordering) -- kek_key_id/
// baseline_key_id/new_key_id's 0 value is deliberately NOT checked here, see
// this file's header comment / the ledger entry for the investigation.
inline bool validate_seal_export_started_shape(const SealExportStartedWire& v) noexcept {
    if (v.registered_producer_mask == 0) return false;
    // No-op for the current kMaxSealHandoffProducers==8 (a uint8_t mask can
    // never have a bit outside an 8-bit capacity), written as a real range
    // check so it becomes load-bearing if that constant is ever lowered.
    if ((static_cast<unsigned>(v.registered_producer_mask) &
         ~((1u << kMaxSealHandoffProducers) - 1u)) != 0) {
        return false;
    }

    if (v.producer_count == 0 || v.producer_count > kMaxSealHandoffProducers) return false;
    if (static_cast<int>(v.producer_count) != std::popcount(v.registered_producer_mask)) return false;

    for (std::size_t i = 0; i < kMaxSealHandoffProducers; ++i) {
        const bool bit_set = (v.registered_producer_mask & (1u << i)) != 0;
        if (bit_set) {
            if (v.ring_id[i] == 0) return false;
            for (std::size_t j = 0; j < i; ++j) {
                const bool other_bit_set = (v.registered_producer_mask & (1u << j)) != 0;
                if (other_bit_set && v.ring_id[j] == v.ring_id[i]) return false;
            }
        } else {
            if (v.ring_id[i] != 0) return false;
        }
    }

    if (v.candidate_id == 0) return false;
    if (v.request_id == 0) return false;
    if (v.source_generation == std::numeric_limits<std::uint32_t>::max()) return false;
    if (v.new_generation != v.source_generation + 1) return false;

    return true;
}

// v.mac is ignored on input and overwritten in the returned bytes. Returns
// kSealExportStartedWireBytes.
inline std::size_t encode_seal_export_started_wire(
    std::span<std::byte, kSealExportStartedWireBytes> out, const SealExportStartedWire& v,
    std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;

    detail::write_u32_le(p, v.format_version);
    detail::write_u32_le(p, v.total_bytes);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u64_le(p, v.candidate_id);
    detail::write_u32_le(p, v.source_generation);
    detail::write_u64_le(p, v.baseline_tip_seq);
    detail::write_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    detail::write_u32_le(p, v.baseline_key_id);
    detail::write_u32_le(p, v.new_generation);
    detail::write_u64_le(p, v.new_final_seq);
    detail::write_bytes(p, v.new_final_tip_mac, sizeof(v.new_final_tip_mac));
    detail::write_u32_le(p, v.new_key_id);
    detail::write_u64_le(p, v.request_id);
    detail::write_bytes(p, v.content_root, sizeof(v.content_root));
    detail::write_u32_le(p, v.kek_key_id);
    detail::write_u8(p, v.registered_producer_mask);
    detail::write_u8(p, v.producer_count);
    for (std::size_t i = 0; i < kMaxSealHandoffProducers; ++i) {
        detail::write_u32_le(p, v.ring_id[i]);
    }

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(seal_journal_precondition_codec_detail::domain_bytes(
        seal_journal_precondition_codec_detail::kSealExportStartedDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kSealExportStartedWireBytes;
}

class VerifiedSealExportStarted;

SealJournalPreconditionDecodeStatus decode_seal_export_started_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealExportStarted>& out) noexcept;

// A decoded-and-MAC-verified SealExportStartedWire whose topology/generation/
// id shape has already been confirmed by validate_seal_export_started_shape().
// No public constructor accepts an arbitrary struct.
class VerifiedSealExportStarted {
public:
    const SealExportStartedWire& value() const noexcept { return value_; }

private:
    friend SealJournalPreconditionDecodeStatus decode_seal_export_started_wire(
        std::span<const std::byte>, std::span<const std::byte>,
        std::optional<VerifiedSealExportStarted>&) noexcept;
    explicit VerifiedSealExportStarted(const SealExportStartedWire& v) noexcept : value_(v) {}
    SealExportStartedWire value_;
};

inline SealJournalPreconditionDecodeStatus decode_seal_export_started_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealExportStarted>& out) noexcept {
    out.reset();
    if (in.size() < kSealExportStartedWireBytes) return SealJournalPreconditionDecodeStatus::Truncated;

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    SealExportStartedWire v{};
    v.format_version = detail::read_u32_le(p);
    if (v.format_version != kSealExportStartedFormatVersion) {
        return SealJournalPreconditionDecodeStatus::UnknownVersion;
    }
    v.total_bytes = detail::read_u32_le(p);
    if (v.total_bytes != kSealExportStartedWireBytes) {
        return SealJournalPreconditionDecodeStatus::TotalBytesInvalid;
    }
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.candidate_id = detail::read_u64_le(p);
    v.source_generation = detail::read_u32_le(p);
    v.baseline_tip_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    v.baseline_key_id = detail::read_u32_le(p);
    v.new_generation = detail::read_u32_le(p);
    v.new_final_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.new_final_tip_mac, sizeof(v.new_final_tip_mac));
    v.new_key_id = detail::read_u32_le(p);
    v.request_id = detail::read_u64_le(p);
    detail::read_bytes(p, v.content_root, sizeof(v.content_root));
    v.kek_key_id = detail::read_u32_le(p);
    v.registered_producer_mask = detail::read_u8(p);
    v.producer_count = detail::read_u8(p);
    for (std::size_t i = 0; i < kMaxSealHandoffProducers; ++i) {
        v.ring_id[i] = detail::read_u32_le(p);
    }

    if (!validate_seal_export_started_shape(v)) {
        return SealJournalPreconditionDecodeStatus::MalformedField;
    }

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(seal_journal_precondition_codec_detail::domain_bytes(
        seal_journal_precondition_codec_detail::kSealExportStartedDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return SealJournalPreconditionDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedSealExportStarted(v));
    return SealJournalPreconditionDecodeStatus::Ok;
}

}  // namespace hy
