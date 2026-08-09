// SPDX-License-Identifier: proprietary
// compaction_intent_codec.hpp — real encode/decode for the three
// CompactionCandidateIntent-family Wire types promoted from mac[32]-only
// markers in durable_control_plane.hpp (Round D, docs/SPEC_INVARIANTS.md's
// "Seal-journal Round D" entry).
//
// Governance: L1 (pure computation, no file I/O), same split as
// durable_frame_codec.hpp / control_plane_frame_codec.hpp -- this file never
// opens a file or touches a filesystem; compaction_lease.hpp (Round D) is
// where the bytes this file produces/consumes actually get written/read
// from disk.
//
// SCOPE, read this before trusting anything below (six design revisions
// converged on this boundary -- see the ledger entry for why): this file
// gives all three types real, tested, MAC-verified encode/decode, and gives
// the ID-lifecycle/generation/`.x1`-chain rules real, tested pure-function
// implementations. It does NOT mean any of these types is actually written
// to disk for anything beyond Intent genesis -- `validate_intent_transition`
// and `validate_x1_chain` exist so the RULES are correct and exhaustively
// tested now, for Round E/F to consume once real C/A codec + an
// authenticated capture receipt exist; no production code path in this
// round calls them to actually mutate durable state.
//
// VERIFIED TYPES (Round D review P2-2): `validate_x1_chain` takes
// `VerifiedCompactionCandidateIntent`/`VerifiedTransition`, not raw structs
// -- these can only be constructed by a successful decode_*() call, making
// "MAC already verified" a type-level precondition instead of a runtime
// hope. `validate_intent_transition` deliberately takes raw structs (it is
// a pure shape/semantic check, independent of whether the `.x1` evidence
// backing a hypothetical transition has been MAC-verified) and shares its
// chain-walking logic with the verified path via a private raw-struct
// helper, so the rule is defined exactly once.

#pragma once

#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/durable_frame_codec.hpp>  // reuses hy::detail::write_u8/write_u32_le/
                                              // write_u64_le/write_bytes/read_u8/read_u32_le/
                                              // read_u64_le/read_bytes
#include <hengyuan/sha256.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace hy {

// --- Local additions to hy::detail (u16 primitives durable_frame_codec.hpp
// never needed -- none of its fields are 16-bit; the three Compaction*Wire
// types have reserved0/1 fields that are). Extending an existing open
// namespace, not duplicating the u8/u32/u64/bytes primitives already there.
namespace detail {

inline void write_u16_le(std::byte*& p, std::uint16_t v) noexcept {
    *p = static_cast<std::byte>(v & 0xFFu);
    ++p;
    *p = static_cast<std::byte>((v >> 8) & 0xFFu);
    ++p;
}

inline std::uint16_t read_u16_le(const std::byte*& p) noexcept {
    std::uint16_t v = static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[0])) |
                       static_cast<std::uint16_t>(static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[1])) << 8);
    p += 2;
    return v;
}

}  // namespace detail

namespace compaction_codec_detail {

inline std::span<const std::byte> domain_bytes(std::string_view domain) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(domain.data()), domain.size());
}

inline constexpr std::string_view kCompactionCandidateIntentDomain = "HY-COMPINTENT-v1";
inline constexpr std::string_view kCompactionIntentTransitionDomain = "HY-COMPINTENT-X-v1";
inline constexpr std::string_view kCompactionIntentGcAuthorizedDomain = "HY-COMPINTENT-GC-v2";

}  // namespace compaction_codec_detail

enum class CompactionWireDecodeStatus : std::uint8_t {
    Ok = 0,
    Truncated = 1,          // fewer bytes available than the wire type needs
    UnknownVersion = 2,     // format_version != the one currently-active format version
    TotalBytesInvalid = 3,  // total_bytes field != this type's fixed wire size
    MalformedField = 4,     // phase/from_phase/to_phase/terminal_disposition/
                             // cleanup_auth_flags-reserved-bits out of legal range
    ReservedNonzero = 5,    // a reserved field is non-zero
    ChecksumMismatch = 6,   // physically complete, well-formed wire whose mac doesn't
                             // match its own content -- Corrupt, not Truncated
};

// ===========================================================================
// CompactionCandidateIntentWire
// ===========================================================================

// Unauthenticated peek at kek_key_id only -- callers resolve hmac_key from
// KeyRing for THIS key_id (key_ring.hpp's pin_key(), Round D) BEFORE calling
// decode. Mirrors durable_frame_codec.hpp's peek_frame_key_id(): returning
// false here does not imply Corrupt, the real decode call does that
// classification.
inline bool peek_compaction_candidate_intent_kek_key_id(std::span<const std::byte> in,
                                                          std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kKekKeyIdOffset = 4 + 4 + 8 + 8;  // format_version, total_bytes,
                                                             // store_uuid_lo, store_uuid_hi
    if (in.size() < kKekKeyIdOffset + 4) return false;
    const std::byte* p = in.data() + kKekKeyIdOffset;
    out_key_id = detail::read_u32_le(p);
    return true;
}

// v.mac is ignored on input and overwritten in the returned bytes -- callers
// never hand-compute a mac. Returns kCompactionCandidateIntentWireBytes.
inline std::size_t encode_compaction_candidate_intent_wire(
    std::span<std::byte, kCompactionCandidateIntentWireBytes> out,
    const CompactionCandidateIntentWire& v,
    std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;

    detail::write_u32_le(p, v.format_version);
    detail::write_u32_le(p, v.total_bytes);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u32_le(p, v.kek_key_id);
    detail::write_u8(p, v.phase);
    detail::write_u8(p, v.reserved0);
    detail::write_u16_le(p, v.reserved1);
    detail::write_u32_le(p, v.source_generation);
    detail::write_u32_le(p, v.target_generation);
    detail::write_u64_le(p, v.baseline_tip_seq);
    detail::write_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    detail::write_u32_le(p, v.baseline_key_id);
    detail::write_u64_le(p, v.build_nonce);
    detail::write_u64_le(p, v.candidate_id);
    detail::write_u64_le(p, v.request_id);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(compaction_codec_detail::domain_bytes(compaction_codec_detail::kCompactionCandidateIntentDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kCompactionCandidateIntentWireBytes;
}

// Forward declaration; defined after VerifiedCompactionCandidateIntent below.
class VerifiedCompactionCandidateIntent;

CompactionWireDecodeStatus decode_compaction_candidate_intent_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedCompactionCandidateIntent>& out) noexcept;

// A decoded-and-MAC-verified CompactionCandidateIntentWire. No public
// constructor accepts an arbitrary struct -- the only way to obtain one is a
// successful decode_compaction_candidate_intent_wire() call, making "this
// wire's MAC has been verified against hmac_key" a type-level guarantee
// rather than something every caller has to remember to check (Round D
// review P2-2).
class VerifiedCompactionCandidateIntent {
public:
    const CompactionCandidateIntentWire& value() const noexcept { return value_; }

private:
    friend CompactionWireDecodeStatus decode_compaction_candidate_intent_wire(
        std::span<const std::byte>, std::span<const std::byte>,
        std::optional<VerifiedCompactionCandidateIntent>&) noexcept;
    explicit VerifiedCompactionCandidateIntent(const CompactionCandidateIntentWire& v) noexcept : value_(v) {}
    CompactionCandidateIntentWire value_;
};

inline CompactionWireDecodeStatus decode_compaction_candidate_intent_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedCompactionCandidateIntent>& out) noexcept {
    out.reset();
    if (in.size() < kCompactionCandidateIntentWireBytes) return CompactionWireDecodeStatus::Truncated;

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    CompactionCandidateIntentWire v{};
    v.format_version = detail::read_u32_le(p);
    if (v.format_version != kCompactionCandidateIntentFormatVersion) return CompactionWireDecodeStatus::UnknownVersion;
    v.total_bytes = detail::read_u32_le(p);
    if (v.total_bytes != kCompactionCandidateIntentWireBytes) return CompactionWireDecodeStatus::TotalBytesInvalid;
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.kek_key_id = detail::read_u32_le(p);
    v.phase = detail::read_u8(p);
    if (v.phase > kCompactionCandidateIntentPhaseAbandonFinalizing) return CompactionWireDecodeStatus::MalformedField;
    v.reserved0 = detail::read_u8(p);
    if (v.reserved0 != 0) return CompactionWireDecodeStatus::ReservedNonzero;
    v.reserved1 = detail::read_u16_le(p);
    if (v.reserved1 != 0) return CompactionWireDecodeStatus::ReservedNonzero;
    v.source_generation = detail::read_u32_le(p);
    v.target_generation = detail::read_u32_le(p);
    v.baseline_tip_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    v.baseline_key_id = detail::read_u32_le(p);
    v.build_nonce = detail::read_u64_le(p);
    v.candidate_id = detail::read_u64_le(p);
    v.request_id = detail::read_u64_le(p);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(compaction_codec_detail::domain_bytes(compaction_codec_detail::kCompactionCandidateIntentDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return CompactionWireDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedCompactionCandidateIntent(v));
    return CompactionWireDecodeStatus::Ok;
}

// ===========================================================================
// CompactionIntentTransitionWire (.x1)
// ===========================================================================

inline std::size_t encode_compaction_intent_transition_wire(
    std::span<std::byte, kCompactionIntentTransitionWireBytes> out,
    const CompactionIntentTransitionWire& v,
    std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;

    detail::write_u32_le(p, v.format_version);
    detail::write_u32_le(p, v.total_bytes);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u32_le(p, v.kek_key_id);
    detail::write_u8(p, v.from_phase);
    detail::write_u8(p, v.to_phase);
    detail::write_u16_le(p, v.reserved0);
    detail::write_u32_le(p, v.transition_seq);
    detail::write_u32_le(p, v.source_generation);
    detail::write_u32_le(p, v.target_generation);
    detail::write_u64_le(p, v.baseline_tip_seq);
    detail::write_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    detail::write_u32_le(p, v.baseline_key_id);
    detail::write_u64_le(p, v.build_nonce);
    detail::write_u64_le(p, v.candidate_id);
    detail::write_u64_le(p, v.request_id);
    detail::write_bytes(p, v.prev_transition_mac, sizeof(v.prev_transition_mac));

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(compaction_codec_detail::domain_bytes(compaction_codec_detail::kCompactionIntentTransitionDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kCompactionIntentTransitionWireBytes;
}

class VerifiedTransition;

CompactionWireDecodeStatus decode_compaction_intent_transition_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedTransition>& out) noexcept;

class VerifiedTransition {
public:
    const CompactionIntentTransitionWire& value() const noexcept { return value_; }

private:
    friend CompactionWireDecodeStatus decode_compaction_intent_transition_wire(
        std::span<const std::byte>, std::span<const std::byte>, std::optional<VerifiedTransition>&) noexcept;
    explicit VerifiedTransition(const CompactionIntentTransitionWire& v) noexcept : value_(v) {}
    CompactionIntentTransitionWire value_;
};

inline CompactionWireDecodeStatus decode_compaction_intent_transition_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedTransition>& out) noexcept {
    out.reset();
    if (in.size() < kCompactionIntentTransitionWireBytes) return CompactionWireDecodeStatus::Truncated;

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    CompactionIntentTransitionWire v{};
    v.format_version = detail::read_u32_le(p);
    if (v.format_version != kCompactionIntentTransitionFormatVersion) return CompactionWireDecodeStatus::UnknownVersion;
    v.total_bytes = detail::read_u32_le(p);
    if (v.total_bytes != kCompactionIntentTransitionWireBytes) return CompactionWireDecodeStatus::TotalBytesInvalid;
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.kek_key_id = detail::read_u32_le(p);
    v.from_phase = detail::read_u8(p);
    v.to_phase = detail::read_u8(p);
    if (v.from_phase > kCompactionCandidateIntentPhaseAbandonFinalizing ||
        v.to_phase > kCompactionCandidateIntentPhaseAbandonFinalizing) {
        return CompactionWireDecodeStatus::MalformedField;
    }
    v.reserved0 = detail::read_u16_le(p);
    if (v.reserved0 != 0) return CompactionWireDecodeStatus::ReservedNonzero;
    v.transition_seq = detail::read_u32_le(p);
    v.source_generation = detail::read_u32_le(p);
    v.target_generation = detail::read_u32_le(p);
    v.baseline_tip_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    v.baseline_key_id = detail::read_u32_le(p);
    v.build_nonce = detail::read_u64_le(p);
    v.candidate_id = detail::read_u64_le(p);
    v.request_id = detail::read_u64_le(p);
    detail::read_bytes(p, v.prev_transition_mac, sizeof(v.prev_transition_mac));

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(compaction_codec_detail::domain_bytes(compaction_codec_detail::kCompactionIntentTransitionDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return CompactionWireDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedTransition(v));
    return CompactionWireDecodeStatus::Ok;
}

// ===========================================================================
// CompactionIntentGcAuthorizedWire (.xgc)
//
// Round D can decode/verify this type (fail-closed recognition of a `.xgc`
// that might already exist on disk, e.g. written by a future Round E/F) but
// never constructs/publishes one -- see this file's top-of-file SCOPE note.
// A legacy v1/204B buffer is rejected via UnknownVersion (format_version
// reads 1, only 2 is accepted) before total_bytes is even inspected --
// never silently accepted, padded, or truncated.
// ===========================================================================

inline std::size_t encode_compaction_intent_gc_authorized_wire(
    std::span<std::byte, kCompactionIntentGcAuthorizedWireBytes> out,
    const CompactionIntentGcAuthorizedWire& v,
    std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;

    detail::write_u32_le(p, v.format_version);
    detail::write_u32_le(p, v.total_bytes);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u32_le(p, v.kek_key_id);
    detail::write_u8(p, v.terminal_disposition);
    detail::write_u8(p, v.intent_phase_at_auth);
    detail::write_u16_le(p, v.reserved0);
    detail::write_u32_le(p, v.source_generation);
    detail::write_u32_le(p, v.target_generation);
    detail::write_u64_le(p, v.baseline_tip_seq);
    detail::write_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    detail::write_u32_le(p, v.baseline_key_id);
    detail::write_u64_le(p, v.build_nonce);
    detail::write_u64_le(p, v.candidate_id);
    detail::write_u64_le(p, v.request_id);
    detail::write_bytes(p, v.intent_mac, sizeof(v.intent_mac));
    detail::write_bytes(p, v.terminal_transition_mac, sizeof(v.terminal_transition_mac));
    detail::write_u8(p, v.cleanup_auth_flags);
    detail::write_u8(p, v.started_kind);
    detail::write_u8(p, v.present_mask_at_auth);
    detail::write_u8(p, v.reserved1);
    detail::write_u64_le(p, v.proof_new_final_seq);
    detail::write_bytes(p, v.proof_new_final_tip_mac, sizeof(v.proof_new_final_tip_mac));
    detail::write_u32_le(p, v.proof_new_key_id);
    detail::write_bytes(p, v.proof_content_root, sizeof(v.proof_content_root));
    detail::write_bytes(p, v.gate_trailer_mac, sizeof(v.gate_trailer_mac));

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    crypto::HmacSha256 h(hmac_key);
    h.update(compaction_codec_detail::domain_bytes(compaction_codec_detail::kCompactionIntentGcAuthorizedDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto mac = h.finish();
    detail::write_bytes(p, mac.bytes.data(), mac.bytes.size());

    return kCompactionIntentGcAuthorizedWireBytes;
}

class VerifiedGcAuthorized;

CompactionWireDecodeStatus decode_compaction_intent_gc_authorized_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedGcAuthorized>& out) noexcept;

class VerifiedGcAuthorized {
public:
    const CompactionIntentGcAuthorizedWire& value() const noexcept { return value_; }

private:
    friend CompactionWireDecodeStatus decode_compaction_intent_gc_authorized_wire(
        std::span<const std::byte>, std::span<const std::byte>, std::optional<VerifiedGcAuthorized>&) noexcept;
    explicit VerifiedGcAuthorized(const CompactionIntentGcAuthorizedWire& v) noexcept : value_(v) {}
    CompactionIntentGcAuthorizedWire value_;
};

inline CompactionWireDecodeStatus decode_compaction_intent_gc_authorized_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedGcAuthorized>& out) noexcept {
    out.reset();
    if (in.size() < kCompactionIntentGcAuthorizedWireBytes) return CompactionWireDecodeStatus::Truncated;

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    CompactionIntentGcAuthorizedWire v{};
    v.format_version = detail::read_u32_le(p);
    // Rejects legacy v1 (format_version==1) here, before total_bytes is even
    // inspected -- v1/204B never gets a chance to be accepted, padded, or
    // truncated into a Mode-B-eligible shape.
    if (v.format_version != kCompactionIntentGcAuthorizedFormatVersion) return CompactionWireDecodeStatus::UnknownVersion;
    v.total_bytes = detail::read_u32_le(p);
    if (v.total_bytes != kCompactionIntentGcAuthorizedWireBytes) return CompactionWireDecodeStatus::TotalBytesInvalid;
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.kek_key_id = detail::read_u32_le(p);
    v.terminal_disposition = detail::read_u8(p);
    if (v.terminal_disposition > kCompactionIntentGcDispositionAbandonFinalizingClear) {
        return CompactionWireDecodeStatus::MalformedField;
    }
    v.intent_phase_at_auth = detail::read_u8(p);
    if (v.intent_phase_at_auth > kCompactionCandidateIntentPhaseAbandonFinalizing) {
        return CompactionWireDecodeStatus::MalformedField;
    }
    v.reserved0 = detail::read_u16_le(p);
    if (v.reserved0 != 0) return CompactionWireDecodeStatus::ReservedNonzero;
    v.source_generation = detail::read_u32_le(p);
    v.target_generation = detail::read_u32_le(p);
    v.baseline_tip_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.baseline_tip_mac, sizeof(v.baseline_tip_mac));
    v.baseline_key_id = detail::read_u32_le(p);
    v.build_nonce = detail::read_u64_le(p);
    v.candidate_id = detail::read_u64_le(p);
    v.request_id = detail::read_u64_le(p);
    detail::read_bytes(p, v.intent_mac, sizeof(v.intent_mac));
    detail::read_bytes(p, v.terminal_transition_mac, sizeof(v.terminal_transition_mac));
    v.cleanup_auth_flags = detail::read_u8(p);
    constexpr auto kAllFlagsMask = static_cast<std::uint8_t>(
        kCompactionIntentGcAuthFlagJournalDrain | kCompactionIntentGcAuthFlagPostSealBound |
        kCompactionIntentGcAuthFlagGenGone | kCompactionIntentGcAuthFlagResumeAuthorized |
        kCompactionIntentGcAuthFlagGateAbsentAtCreate);
    if ((v.cleanup_auth_flags & static_cast<std::uint8_t>(~kAllFlagsMask)) != 0) {
        return CompactionWireDecodeStatus::MalformedField;  // undefined high bits set
    }
    v.started_kind = detail::read_u8(p);
    v.present_mask_at_auth = detail::read_u8(p);
    v.reserved1 = detail::read_u8(p);
    if (v.reserved1 != 0) return CompactionWireDecodeStatus::ReservedNonzero;
    v.proof_new_final_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.proof_new_final_tip_mac, sizeof(v.proof_new_final_tip_mac));
    v.proof_new_key_id = detail::read_u32_le(p);
    detail::read_bytes(p, v.proof_content_root, sizeof(v.proof_content_root));
    detail::read_bytes(p, v.gate_trailer_mac, sizeof(v.gate_trailer_mac));

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(compaction_codec_detail::domain_bytes(compaction_codec_detail::kCompactionIntentGcAuthorizedDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return CompactionWireDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedGcAuthorized(v));
    return CompactionWireDecodeStatus::Ok;
}

// ===========================================================================
// Semantic validation (SPEC_INVARIANTS.md's Round D entry: HMAC only proves
// "the writer held the key," not "the transition is business-legal" -- these
// pure functions are the business-legality checks, on top of MAC
// verification, not instead of it).
// ===========================================================================

// Building requires both ids to be 0 (nothing assigned yet); Reserved and
// beyond require both to be non-zero (bound at Reserved, immutable after --
// see is_legal_candidate_id_transition for the one-time 0->nonzero step
// itself). This is the check create_intent_genesis actually exercises in
// production (Building, ids must be 0).
inline bool is_legal_candidate_ids_for_phase(std::uint8_t phase, std::uint64_t candidate_id,
                                              std::uint64_t request_id) noexcept {
    if (phase == kCompactionCandidateIntentPhaseBuilding) {
        return candidate_id == 0 && request_id == 0;
    }
    return candidate_id != 0 && request_id != 0;
}

// target == source + 1, guarding against source == UINT32_MAX overflowing
// the +1. This is the check create_intent_genesis actually exercises in
// production.
inline bool is_legal_generation_transition(std::uint32_t source_generation,
                                            std::uint32_t target_generation) noexcept {
    if (source_generation == std::numeric_limits<std::uint32_t>::max()) return false;
    return target_generation == source_generation + 1;
}

// Every field that must never change across ANY Intent REPLACE, from
// CREATE_NEW (Building) through the terminal phase.  candidate_id/
// request_id are handled separately (is_legal_candidate_id_transition) --
// they DO change exactly once, at Building->Reserved.
inline bool intent_permanently_immutable_fields_match(const CompactionCandidateIntentWire& before,
                                                        const CompactionCandidateIntentWire& after) noexcept {
    return before.store_uuid_lo == after.store_uuid_lo && before.store_uuid_hi == after.store_uuid_hi &&
           before.kek_key_id == after.kek_key_id && before.source_generation == after.source_generation &&
           before.target_generation == after.target_generation &&
           before.baseline_tip_seq == after.baseline_tip_seq &&
           std::memcmp(before.baseline_tip_mac, after.baseline_tip_mac, sizeof(before.baseline_tip_mac)) == 0 &&
           before.baseline_key_id == after.baseline_key_id && before.build_nonce == after.build_nonce;
}

// Two-layer ID lifecycle rule (Round D review v2's P0-1 fix): candidate_id/
// request_id are immutable EXCEPT for exactly one 0/0->nonzero/nonzero step
// at Building->Reserved, and that step's new values must match the `.x1`
// (seq=1, Building->Reserved) transition's own post-raise ids -- the
// transition receipt is what PROVES the bind, not the Intent record alone.
inline bool is_legal_candidate_id_transition(std::uint8_t from_phase, std::uint8_t to_phase,
                                              std::uint64_t before_candidate_id, std::uint64_t before_request_id,
                                              std::uint64_t after_candidate_id, std::uint64_t after_request_id,
                                              std::uint64_t x1_seq1_candidate_id,
                                              std::uint64_t x1_seq1_request_id) noexcept {
    if (from_phase == to_phase) return false;
    const bool building_to_reserved = from_phase == kCompactionCandidateIntentPhaseBuilding &&
                                       to_phase == kCompactionCandidateIntentPhaseReserved;
    if (building_to_reserved) {
        if (before_candidate_id != 0 || before_request_id != 0) return false;
        if (after_candidate_id == 0 || after_request_id == 0) return false;
        return after_candidate_id == x1_seq1_candidate_id && after_request_id == x1_seq1_request_id;
    }
    // Every other legal edge (Reserved->StartedPublished, StartedPublished->
    // *Finalizing) must leave already-bound ids byte-unchanged.
    return before_candidate_id == after_candidate_id && before_request_id == after_request_id &&
           before_candidate_id != 0 && before_request_id != 0;
}

enum class X1ChainStatus : std::uint8_t {
    Valid,
    Empty,
    Gap,                    // transition_seq isn't the expected 1/2/3 in order
    ForeignBinding,         // a transition's bind fields don't match Intent's
    IllegalEdge,            // from_phase/to_phase/seq isn't one of the two legal chains
    MacChainBroken,         // prev_transition_mac doesn't equal the previous frame's mac
                             // (or isn't all-zero for seq==1)
    TerminalBranchConflict, // chain's final to_phase doesn't match Intent.phase
    TooManyFrames,          // more than 3 frames supplied -- the chain is at most 3 long
};

namespace compaction_codec_detail {

// Shared chain-walk, operating on raw structs -- both validate_x1_chain
// (verified path) and validate_intent_transition (raw-struct semantic path)
// call this so the rule is defined exactly once.
inline X1ChainStatus walk_x1_chain_raw(const CompactionCandidateIntentWire& intent,
                                        std::span<const CompactionIntentTransitionWire> x1_frames_by_seq,
                                        std::array<std::uint8_t, 32>& out_terminal_transition_mac) noexcept {
    if (x1_frames_by_seq.empty()) return X1ChainStatus::Empty;
    if (x1_frames_by_seq.size() > 3) return X1ChainStatus::TooManyFrames;

    std::uint8_t expected_from = kCompactionCandidateIntentPhaseBuilding;
    std::array<std::uint8_t, 32> prev_mac{};

    for (std::size_t i = 0; i < x1_frames_by_seq.size(); ++i) {
        const CompactionIntentTransitionWire& t = x1_frames_by_seq[i];
        const auto expected_seq = static_cast<std::uint32_t>(i + 1);
        if (t.transition_seq != expected_seq) return X1ChainStatus::Gap;

        if (t.store_uuid_lo != intent.store_uuid_lo || t.store_uuid_hi != intent.store_uuid_hi ||
            t.kek_key_id != intent.kek_key_id || t.source_generation != intent.source_generation ||
            t.target_generation != intent.target_generation || t.baseline_tip_seq != intent.baseline_tip_seq ||
            std::memcmp(t.baseline_tip_mac, intent.baseline_tip_mac, sizeof(t.baseline_tip_mac)) != 0 ||
            t.baseline_key_id != intent.baseline_key_id || t.build_nonce != intent.build_nonce) {
            return X1ChainStatus::ForeignBinding;
        }

        if (t.from_phase != expected_from) return X1ChainStatus::IllegalEdge;
        const bool legal_edge =
            (t.from_phase == kCompactionCandidateIntentPhaseBuilding &&
             t.to_phase == kCompactionCandidateIntentPhaseReserved && expected_seq == 1) ||
            (t.from_phase == kCompactionCandidateIntentPhaseReserved &&
             t.to_phase == kCompactionCandidateIntentPhaseStartedPublished && expected_seq == 2) ||
            (t.from_phase == kCompactionCandidateIntentPhaseStartedPublished &&
             (t.to_phase == kCompactionCandidateIntentPhasePostSealFinalizing ||
              t.to_phase == kCompactionCandidateIntentPhaseAbandonFinalizing) &&
             expected_seq == 3);
        if (!legal_edge) return X1ChainStatus::IllegalEdge;

        if (expected_seq == 1) {
            std::array<std::uint8_t, 32> zero{};
            if (std::memcmp(t.prev_transition_mac, zero.data(), zero.size()) != 0) {
                return X1ChainStatus::MacChainBroken;
            }
        } else {
            if (std::memcmp(t.prev_transition_mac, prev_mac.data(), prev_mac.size()) != 0) {
                return X1ChainStatus::MacChainBroken;
            }
        }
        std::memcpy(prev_mac.data(), t.mac, prev_mac.size());
        expected_from = t.to_phase;
    }

    if (x1_frames_by_seq.size() == 3) {
        const CompactionIntentTransitionWire& last = x1_frames_by_seq[2];
        if (last.to_phase != intent.phase) return X1ChainStatus::TerminalBranchConflict;
    }

    std::memcpy(out_terminal_transition_mac.data(), x1_frames_by_seq.back().mac, out_terminal_transition_mac.size());
    return X1ChainStatus::Valid;
}

}  // namespace compaction_codec_detail

// Verified-type entry point -- the one a real recovery/verification path
// would use, since it can only be called with MAC-already-verified data.
inline X1ChainStatus validate_x1_chain(const VerifiedCompactionCandidateIntent& intent,
                                        std::span<const VerifiedTransition> x1_frames_by_seq,
                                        std::array<std::uint8_t, 32>& out_terminal_transition_mac) noexcept {
    std::array<CompactionIntentTransitionWire, 3> raw{};
    if (x1_frames_by_seq.size() > raw.size()) return X1ChainStatus::TooManyFrames;
    for (std::size_t i = 0; i < x1_frames_by_seq.size(); ++i) raw[i] = x1_frames_by_seq[i].value();
    return compaction_codec_detail::walk_x1_chain_raw(
        intent.value(), std::span<const CompactionIntentTransitionWire>(raw.data(), x1_frames_by_seq.size()),
        out_terminal_transition_mac);
}

// Raw-struct, semantic-only entry point (no MAC-verified precondition) --
// pure function for Round E/F rule-correctness testing; not called by any
// Round D production code path that mutates durable state.
//
// Validates a FULL genesis-to-current history in one call: `before` must be
// the genuine Building genesis (ids 0/0), `after` must be the current
// snapshot, and `x1_frames_by_seq` must be the complete, legally-chained
// evidence connecting them (walk_x1_chain_raw already enforces that the
// chain binds to `after`'s permanently-immutable fields, follows a legal
// edge sequence, and ends at `after.phase` -- see X1ChainStatus::
// TerminalBranchConflict). What's left to check here: `before` really is
// genesis (not some other Building-phase snapshot with stale ids), and the
// ids bound at Reserved (chain seq=1) match what `after` actually carries.
inline bool validate_intent_transition(const CompactionCandidateIntentWire& before,
                                        const CompactionCandidateIntentWire& after,
                                        std::span<const CompactionIntentTransitionWire> x1_frames_by_seq) noexcept {
    if (!intent_permanently_immutable_fields_match(before, after)) return false;
    if (before.phase != kCompactionCandidateIntentPhaseBuilding) return false;
    if (!is_legal_candidate_ids_for_phase(before.phase, before.candidate_id, before.request_id)) return false;

    std::array<std::uint8_t, 32> terminal_mac{};
    if (compaction_codec_detail::walk_x1_chain_raw(after, x1_frames_by_seq, terminal_mac) != X1ChainStatus::Valid) {
        return false;
    }
    if (!x1_frames_by_seq.empty()) {
        const CompactionIntentTransitionWire& seq1 = x1_frames_by_seq.front();
        if (after.candidate_id != seq1.candidate_id || after.request_id != seq1.request_id) return false;
    }
    return is_legal_candidate_ids_for_phase(after.phase, after.candidate_id, after.request_id);
}

// cleanup_auth_flags legality truth table (docs/SPEC_INVARIANTS.md's Round C
// "为未来 codec/recovery 轮记录的设计输入" subsection, transcribed exactly).
// Not called by any Round D production write path (Round D never produces a
// `.xgc`), but decode_compaction_intent_gc_authorized_wire()'s callers can
// use this to validate a `.xgc` that already exists on disk before trusting
// its disposition.
inline bool is_legal_cleanup_auth_flags(std::uint8_t disposition, std::uint8_t flags, bool gate_absent_at_create,
                                         std::span<const std::byte, 32> gate_trailer_mac) noexcept {
    constexpr std::uint8_t kJournalDrain = kCompactionIntentGcAuthFlagJournalDrain;
    constexpr std::uint8_t kPostSealBound = kCompactionIntentGcAuthFlagPostSealBound;
    constexpr std::uint8_t kGenGone = kCompactionIntentGcAuthFlagGenGone;
    constexpr std::uint8_t kResumeAuthorized = kCompactionIntentGcAuthFlagResumeAuthorized;
    constexpr std::uint8_t kGateAbsentAtCreate = kCompactionIntentGcAuthFlagGateAbsentAtCreate;
    constexpr auto kAllFlagsMask = static_cast<std::uint8_t>(
        kJournalDrain | kPostSealBound | kGenGone | kResumeAuthorized | kGateAbsentAtCreate);

    if ((flags & static_cast<std::uint8_t>(~kAllFlagsMask)) != 0) return false;  // undefined high bits

    const bool flag_bit4_set = (flags & kGateAbsentAtCreate) != 0;
    if (flag_bit4_set != gate_absent_at_create) return false;

    bool zero_trailer = true;
    for (const std::byte b : gate_trailer_mac) {
        if (b != std::byte{0}) {
            zero_trailer = false;
            break;
        }
    }

    switch (disposition) {
        case kCompactionIntentGcDispositionPreSealAbandonClear:
            // Never touches a real gate at all (Building/Reserved -- no
            // Started/C/A was ever admitted) -- bit4 must be 0, trailer mac
            // is always zero, regardless of gate_absent_at_create.
            if (gate_absent_at_create || !zero_trailer) return false;
            return (flags & static_cast<std::uint8_t>(kPostSealBound | kGenGone | kResumeAuthorized |
                                                        kGateAbsentAtCreate)) == 0;
        case kCompactionIntentGcDispositionPostSealFinalizingClear:
            // bit4 only legal on the recovery-reconstruction path, and only
            // there may gate_trailer_mac be all-zero; the live path requires
            // a real, non-zero captured C.mac.
            if (gate_absent_at_create) {
                if (!zero_trailer) return false;
            } else {
                if (zero_trailer) return false;
            }
            return (flags & static_cast<std::uint8_t>(kJournalDrain | kPostSealBound)) ==
                   static_cast<std::uint8_t>(kJournalDrain | kPostSealBound);
        case kCompactionIntentGcDispositionAbandonFinalizingClear:
            if (gate_absent_at_create) {
                if (!zero_trailer) return false;
            } else {
                if (zero_trailer) return false;
            }
            return (flags & static_cast<std::uint8_t>(kJournalDrain | kGenGone | kResumeAuthorized)) ==
                   static_cast<std::uint8_t>(kJournalDrain | kGenGone | kResumeAuthorized);
        default:
            return false;
    }
}

}  // namespace hy
