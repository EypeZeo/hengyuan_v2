// SPDX-License-Identifier: proprietary
// seal_journal_commit_tombstone_codec.hpp — real encode/decode for the two
// seal-journal commit/tombstone wire types (docs/SPEC_INVARIANTS.md's
// "Seal-journal Round E Slice 2a" entry): SealJournalCommitWatermark and
// SealJournalTombstoneWire.
//
// Governance: L1 (pure computation, no file I/O), same split as
// compaction_intent_codec.hpp / seal_journal_precondition_codec.hpp /
// durable_frame_codec.hpp -- this file never opens a file or touches a
// filesystem.
//
// SCOPE, read this before trusting anything below (this design went through
// one round of external Architect review -- two rule gaps were found and
// fixed in the v2 spec, P0-3's nonzero-id rules and P1-2's UINT64_MAX
// fence; see docs/SPEC_INVARIANTS.md's ledger entry for the full history):
// this file gives both types real, tested, MAC-verified encode/decode PLUS
// field-shape semantic validation. It does NOT mean either type is wired to
// any production write path -- no file I/O, no IntentStore/CandidateLease/
// manager integration, no receipt concept, no phase-raising API exist
// anywhere in this codebase yet. It does NOT implement real C/A (`.clr`/
// `.abd`) codec -- that is a separate, parallel Slice 2b work item. It does
// NOT touch SealJournalOriginKey -- that type is an in-memory dedup index
// key (two fields, no mac[32], no wire format), it needs no codec.
//
// SealJournalCommitTombstoneDecodeStatus is a separate enum from
// compaction_intent_codec.hpp's CompactionWireDecodeStatus and from
// seal_journal_precondition_codec.hpp's SealJournalPreconditionDecodeStatus
// on purpose -- these type families have no real coupling, and reusing one
// enum across families would imply relationships that don't exist.
//
// VERIFIED TYPES (same idiom the other codec files established):
// VerifiedSealJournalCommitWatermark/VerifiedSealJournalTombstoneWire can
// only be constructed by a successful decode_*() call, and hold their value
// BY COPY (never a span/reference into the caller's buffer or key material)
// -- "MAC already verified" is a type-level guarantee, not a runtime hope,
// and the verified object's lifetime is never tied to the input buffer's.
//
// DECODER LENGTH CONTRACT: both decode_*() functions accept
// std::span<const std::byte> and only require `in.size() >= kXxxWireBytes`
// (short -> Truncated); they read/authenticate exactly the first
// kXxxWireBytes bytes and do not care whether the caller's span has trailing
// bytes beyond that -- this is not a security gap (the MAC covers only the
// authenticated content, so trailing garbage cannot influence the decoded
// result either way), it is this codebase's already-established layering: a
// stricter "must be EXACTLY N bytes" contract belongs to a future I/O read
// layer, not to this pure in-memory codec.
//
// WRITE-SIDE SHAPE GATE (added by the v2 review): both encode_*() functions
// MUST call their validate_*_shape() BEFORE writing a single byte --
// semantically illegal input returns 0 (no bytes written, no MAC computed).
// This is the fail-closed guarantee that no future write path can
// accidentally sign a semantically illegal value into a "MAC-correct"
// durable record.
//
// UINT64_MAX FENCE (P1-2, added by the v2 review) -- read this twice:
//   * READER (this file): highest_committed_journal_seq == UINT64_MAX is a
//     LEGAL, decodable value. A fully MAC-verified UINT64_MAX state means
//     the counter is exhausted ("this candidate's journal cannot advance
//     any further"); operators must be able to read and diagnose that
//     state, so decode must NOT reject it because the value "looks large".
//     Same precedent as SealIdWatermark's next_candidate_id/next_request_id
//     in seal_journal_precondition_codec.hpp.
//   * WRITER (a FUTURE function that advances the watermark, i.e. computes
//     highest_committed_journal_seq + 1): MUST fail closed BEFORE the +1
//     when the current value is UINT64_MAX -- unchecked +1 wraps to 0,
//     which is the real bug source. THIS SLICE HAS NO SUCH WRITE PATH, so
//     the fence itself has no code here; the rule is documented instead,
//     and docs/SPEC_INVARIANTS.md's Round E Slice 2a entry lists it as a
//     MUST-ACCEPT acceptance item for any future slice that implements
//     journal-watermark advancement. Do not "clean up" this comment as
//     dead prose -- it is the standing contract for that future slice.
//
// ORDERING: semantic validation (MalformedField) runs BEFORE the MAC
// comparison, matching compaction_intent_codec.hpp /
// seal_journal_precondition_codec.hpp -- fail-fast field-shape checks come
// first, the MAC check is last and is the sole "trustworthy or not" gate.
//
// entry_mac: the field's comment says "MUST equal the Applied / journal
// entry_mac" -- that is a CROSS-FILE consistency requirement (compare
// against another journal entry record's mac) that a single-file decode
// function cannot prove. This codec deliberately treats entry_mac as an
// ordinary 32-byte field and does NOT verify that rule.

#pragma once

#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/durable_frame_codec.hpp>  // reuses hy::detail::write_u8/write_u32_le/
                                              // write_u64_le/write_bytes/read_u8/read_u32_le/
                                              // read_u64_le/read_bytes
#include <hengyuan/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace hy {

namespace seal_journal_commit_tombstone_codec_detail {

inline std::span<const std::byte> domain_bytes(std::string_view domain) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(domain.data()), domain.size());
}

inline constexpr std::string_view kSealJournalCommitWatermarkDomain = "HY-SEALJRNHW-v1";
inline constexpr std::string_view kSealJournalTombstoneDomain = "HY-SEALJRNTS-v1";

}  // namespace seal_journal_commit_tombstone_codec_detail

enum class SealJournalCommitTombstoneDecodeStatus : std::uint8_t {
    Ok = 0,
    Truncated = 1,          // fewer bytes available than the wire type needs
    UnknownVersion = 2,     // format_version != the currently-active format version
                            // (SealJournalCommitWatermark has no format_version
                            // field -- never returned by its own decode function)
    TotalBytesInvalid = 3,  // total_bytes field != this type's fixed wire size
                            // (SealJournalCommitWatermark has no total_bytes field
                            // either)
    MalformedField = 4,     // a field-shape/semantic rule was violated (see
                            // validate_seal_journal_commit_watermark_shape() /
                            // validate_seal_journal_tombstone_shape() below) --
                            // physically well-formed bytes, MAC not yet checked,
                            // but the content itself cannot be legal
    ChecksumMismatch = 5,   // physically complete, shape-valid wire whose mac
                            // doesn't match its own content -- Corrupt, not
                            // Truncated
};

// ===========================================================================
// SealJournalCommitWatermark
// ===========================================================================

// candidate_id != 0 -- ids are allocated by SealIdWatermark (next_candidate_id
// starts at 1), 0 is never a legal reserved id. highest_committed_journal_seq
// is deliberately NOT checked here: 0 = "none yet" is a legal initial state
// (struct comment), and UINT64_MAX must be readable for exhaustion diagnosis
// (see this file's header comment, P1-2). Called from encode BEFORE writing
// any byte, and from decode BEFORE the MAC comparison.
inline bool validate_seal_journal_commit_watermark_shape(const SealJournalCommitWatermark& v) noexcept {
    return v.candidate_id != 0;
}

// v.mac is ignored on input and overwritten in the returned bytes -- callers
// never hand-compute a mac. Returns kSealJournalCommitWatermarkWireBytes, or
// 0 (having written NOTHING to out, no MAC computed) if the value fails
// validate_seal_journal_commit_watermark_shape() -- the write-side shape
// gate, so no future write path can sign a semantically illegal value.
inline std::size_t encode_seal_journal_commit_watermark_wire(
    std::span<std::byte, kSealJournalCommitWatermarkWireBytes> out,
    const SealJournalCommitWatermark& v,
    std::span<const std::byte> hmac_key) noexcept {
    if (!validate_seal_journal_commit_watermark_shape(v)) return 0;

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

class VerifiedSealJournalCommitWatermark;

SealJournalCommitTombstoneDecodeStatus decode_seal_journal_commit_watermark_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealJournalCommitWatermark>& out) noexcept;

// A decoded-and-MAC-verified SealJournalCommitWatermark whose candidate_id
// has already been confirmed nonzero. No public constructor accepts an
// arbitrary struct -- the only way to obtain one is a successful
// decode_seal_journal_commit_watermark_wire() call.
class VerifiedSealJournalCommitWatermark {
public:
    const SealJournalCommitWatermark& value() const noexcept { return value_; }

private:
    friend SealJournalCommitTombstoneDecodeStatus decode_seal_journal_commit_watermark_wire(
        std::span<const std::byte>, std::span<const std::byte>,
        std::optional<VerifiedSealJournalCommitWatermark>&) noexcept;
    explicit VerifiedSealJournalCommitWatermark(const SealJournalCommitWatermark& v) noexcept : value_(v) {}
    SealJournalCommitWatermark value_;
};

inline SealJournalCommitTombstoneDecodeStatus decode_seal_journal_commit_watermark_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealJournalCommitWatermark>& out) noexcept {
    out.reset();
    if (in.size() < kSealJournalCommitWatermarkWireBytes) {
        return SealJournalCommitTombstoneDecodeStatus::Truncated;
    }

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    SealJournalCommitWatermark v{};
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.candidate_id = detail::read_u64_le(p);
    v.highest_committed_journal_seq = detail::read_u64_le(p);
    v.kek_key_id = detail::read_u32_le(p);

    // Semantic shape BEFORE the MAC comparison (fail-fast): 0 is never a
    // legal reserved candidate id. highest_committed_journal_seq is
    // deliberately not fenced here -- 0 ("none yet") and UINT64_MAX
    // (exhausted) are both legal, readable states; see this file's header
    // comment (P1-2) for the reader/writer fence boundary.
    if (!validate_seal_journal_commit_watermark_shape(v)) {
        return SealJournalCommitTombstoneDecodeStatus::MalformedField;
    }

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(seal_journal_commit_tombstone_codec_detail::domain_bytes(
        seal_journal_commit_tombstone_codec_detail::kSealJournalCommitWatermarkDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedSealJournalCommitWatermark(v));
    return SealJournalCommitTombstoneDecodeStatus::Ok;
}

// Unauthenticated peek at kek_key_id only -- callers resolve hmac_key from
// KeyRing for THIS key_id BEFORE calling decode. Mirrors the other codec
// files' peek functions: returning false here does not imply Corrupt, the
// real decode call does that classification.
inline bool peek_seal_journal_commit_watermark_kek_key_id(std::span<const std::byte> in,
                                                          std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kKekKeyIdOffset = 8 + 8 + 8 + 8;  // store_uuid_lo, store_uuid_hi,
                                                            // candidate_id, highest_committed_journal_seq
    static_assert(kKekKeyIdOffset == 32);
    if (in.size() < kKekKeyIdOffset + 4) return false;
    const std::byte* p = in.data() + kKekKeyIdOffset;
    out_key_id = detail::read_u32_le(p);
    return true;
}

// ===========================================================================
// SealJournalTombstoneWire
// ===========================================================================

// candidate_id != 0 && journal_seq != 0 -- journal seqs are allocated
// strictly increasing starting at 1 (BINANCE_PRIVATE_REST_L4_SPEC.md:5059),
// 0 cannot be any real entry's number, so an existing tombstone record with
// journal_seq==0 is self-contradictory. NOTE the difference from
// SealJournalCommitWatermark: its highest_committed_journal_seq==0 means
// "this candidate has not committed anything yet" (legal initial state),
// while a tombstone's journal_seq refers to one specific, really-committed
// entry -- the two rules are NOT analogous. Called from encode BEFORE
// writing any byte, and from decode BEFORE the MAC comparison.
inline bool validate_seal_journal_tombstone_shape(const SealJournalTombstoneWire& v) noexcept {
    return v.candidate_id != 0 && v.journal_seq != 0;
}

// v.mac is ignored on input and overwritten in the returned bytes -- callers
// never hand-compute a mac. Returns kSealJournalTombstoneBytes, or 0 (having
// written NOTHING to out, no MAC computed) if the value fails
// validate_seal_journal_tombstone_shape() -- the write-side shape gate.
inline std::size_t encode_seal_journal_tombstone_wire(
    std::span<std::byte, kSealJournalTombstoneBytes> out,
    const SealJournalTombstoneWire& v,
    std::span<const std::byte> hmac_key) noexcept {
    if (!validate_seal_journal_tombstone_shape(v)) return 0;

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

class VerifiedSealJournalTombstoneWire;

SealJournalCommitTombstoneDecodeStatus decode_seal_journal_tombstone_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealJournalTombstoneWire>& out) noexcept;

// A decoded-and-MAC-verified SealJournalTombstoneWire whose candidate_id and
// journal_seq have already been confirmed nonzero. No public constructor
// accepts an arbitrary struct -- the only way to obtain one is a successful
// decode_seal_journal_tombstone_wire() call.
class VerifiedSealJournalTombstoneWire {
public:
    const SealJournalTombstoneWire& value() const noexcept { return value_; }

private:
    friend SealJournalCommitTombstoneDecodeStatus decode_seal_journal_tombstone_wire(
        std::span<const std::byte>, std::span<const std::byte>,
        std::optional<VerifiedSealJournalTombstoneWire>&) noexcept;
    explicit VerifiedSealJournalTombstoneWire(const SealJournalTombstoneWire& v) noexcept : value_(v) {}
    SealJournalTombstoneWire value_;
};

inline SealJournalCommitTombstoneDecodeStatus decode_seal_journal_tombstone_wire(
    std::span<const std::byte> in, std::span<const std::byte> hmac_key,
    std::optional<VerifiedSealJournalTombstoneWire>& out) noexcept {
    out.reset();
    if (in.size() < kSealJournalTombstoneBytes) {
        return SealJournalCommitTombstoneDecodeStatus::Truncated;
    }

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    SealJournalTombstoneWire v{};
    v.format_version = detail::read_u32_le(p);
    if (v.format_version != kSealJournalTombstoneFormatVersion) {
        return SealJournalCommitTombstoneDecodeStatus::UnknownVersion;
    }
    v.total_bytes = detail::read_u32_le(p);
    if (v.total_bytes != kSealJournalTombstoneBytes) {
        return SealJournalCommitTombstoneDecodeStatus::TotalBytesInvalid;
    }
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.kek_key_id = detail::read_u32_le(p);
    v.candidate_id = detail::read_u64_le(p);
    v.journal_seq = detail::read_u64_le(p);
    detail::read_bytes(p, v.entry_mac, sizeof(v.entry_mac));

    // Semantic shape BEFORE the MAC comparison (fail-fast): 0 is never a
    // legal candidate id, and journal_seq starts at 1 -- see
    // validate_seal_journal_tombstone_shape().
    if (!validate_seal_journal_tombstone_shape(v)) {
        return SealJournalCommitTombstoneDecodeStatus::MalformedField;
    }

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    detail::read_bytes(p, v.mac, sizeof(v.mac));

    crypto::HmacSha256 h(hmac_key);
    h.update(seal_journal_commit_tombstone_codec_detail::domain_bytes(
        seal_journal_commit_tombstone_codec_detail::kSealJournalTombstoneDomain));
    h.update(std::span<const std::byte>(content_start, content_len));
    const auto expected = h.finish();
    if (!crypto::constant_time_equal(
            expected, std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.mac), sizeof(v.mac)))) {
        return SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch;
    }

    out.emplace(VerifiedSealJournalTombstoneWire(v));
    return SealJournalCommitTombstoneDecodeStatus::Ok;
}

// Unauthenticated peek at kek_key_id only -- same contract as
// peek_seal_journal_commit_watermark_kek_key_id() above.
inline bool peek_seal_journal_tombstone_kek_key_id(std::span<const std::byte> in,
                                                   std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kKekKeyIdOffset = 4 + 4 + 8 + 8;  // format_version, total_bytes,
                                                            // store_uuid_lo, store_uuid_hi
    static_assert(kKekKeyIdOffset == 24);
    if (in.size() < kKekKeyIdOffset + 4) return false;
    const std::byte* p = in.data() + kKekKeyIdOffset;
    out_key_id = detail::read_u32_le(p);
    return true;
}

}  // namespace hy
