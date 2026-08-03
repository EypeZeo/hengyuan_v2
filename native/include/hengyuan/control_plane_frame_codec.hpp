// SPDX-License-Identifier: proprietary
// control_plane_frame_codec.hpp — pure encode/decode for the 11 control-plane
// record types ControlPlaneLogSink (control_plane_log_sink.hpp) actually
// writes, extending durable_frame_codec.hpp's v4 keyed-frame envelope.
//
// Governance: L1 (pure computation, no file I/O), same as durable_frame_codec.hpp.
//
// Deliberately a SEPARATE file from durable_frame_codec.hpp rather than an
// extension of it -- see docs/SPEC_INVARIANTS.md's "Phase 1" entry. That file's
// own header comment states its scope as OrderEvent-only; tripling its size
// with 11 unrelated record types would mix Phase-0-merged, already-tested
// code with this round's new code in the same diff history going forward.
// What IS reused is the primitive layer -- detail::write_u8/write_u32_le/
// write_u64_le/write_i64_le/write_bytes and the matching read_* functions,
// plus kFrameFormatVersion/kMacLen/FrameDecodeStatus -- all defined in
// durable_frame_codec.hpp's `hy`/`hy::detail` namespaces and reopened here,
// not duplicated.
//
// TRANSCRIBE, NEVER INVENT (durable_control_plane.hpp's own governing rule,
// applies identically here): every payload field below is written in the
// struct's own declaration order, byte-for-byte, with no invented fields or
// default values. Field-by-field layouts were verified against direct reads
// of durable_control_plane.hpp (lines 259-658) and account_truth.hpp:81-111
// (SymbolRules) before writing this file -- see docs/SPEC_INVARIANTS.md's
// "Phase 1" entry for the resulting byte-count table.
//
// SCOPE: this file implements MECHANICS ONLY -- encode/decode/MAC-chain
// correctness. It does NOT implement any of the cross-frame business
// validation rules documented in DurableControlPlaneSink's own SPEC-METHOD
// comments (wait_generation sequencing, attempt_ordinal no-gap, compaction
// session baseline checks, etc.) -- those are explicitly deferred, see
// control_plane_log_sink.hpp's class header and the ledger entry above.
// Decode-time validation here is limited to: (a) enum class fields that would
// otherwise let an out-of-range byte silently become a typed enum value
// (FreezeClearKind, FreezeProbePurpose -- mirroring decode_audit_record()'s
// own precedent of validating real enum-class fields but not raw uint8_t data
// fields like `source`/`admit_mode`/`tracker`), and (b) reserved padding
// bytes, which must be zero on encode and are rejected if nonzero on decode
// (so one logical record never has more than one valid wire representation).

#pragma once

#include <hengyuan/account_truth.hpp>
#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/durable_frame_codec.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace hy {

// Fixed per-frame overhead shared by every control-plane frame type: the
// header (format_version..payload_length, same 27-byte shape
// durable_frame_codec.hpp's kOrderEventFrameSize already uses) plus
// prev_mac + mac. Named here (not re-exported from durable_frame_codec.hpp,
// which does not expose this as a standalone symbol) so every kXFrameSize
// constant below reads as "envelope + payload" rather than repeating the
// magic number 91 eleven times.
inline constexpr std::size_t kFrameEnvelopeOverhead = 1 + 1 + 4 + 8 + 1 + 8 + 4 + kMacLen + kMacLen;  // 91
static_assert(kFrameEnvelopeOverhead == 91, "control-plane frame envelope must match the v4 fixed overhead");

namespace detail {

inline constexpr std::size_t kControlPlaneHeaderSize = 1 + 1 + 4 + 8 + 1 + 8 + 4;  // 27
static_assert(kFrameEnvelopeOverhead == kControlPlaneHeaderSize + kMacLen + kMacLen);

inline void write_control_plane_header(std::byte*& p, DurableRecordType record_type, std::uint32_t key_id,
                                        std::uint64_t sequence_number, FrameTimeKind time_kind,
                                        std::int64_t recorded_utc_ms, std::uint32_t payload_length) noexcept {
    write_u8(p, kFrameFormatVersion);
    write_u8(p, static_cast<std::uint8_t>(record_type));
    write_u32_le(p, key_id);
    write_u64_le(p, sequence_number);
    write_u8(p, static_cast<std::uint8_t>(time_kind));
    write_i64_le(p, recorded_utc_ms);
    write_u32_le(p, payload_length);
}

struct ControlPlaneFrameHeader {
    DurableRecordType record_type{DurableRecordType::OrderEvent};
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    std::uint32_t payload_length{0};
};

// Reads and range-validates the fixed 27-byte header only -- does not touch
// payload or MAC. Mirrors decode_order_event_frame()'s own header-parsing
// half exactly (same field order, same version/record_type/time_kind checks).
inline FrameDecodeStatus read_control_plane_header(std::span<const std::byte> in,
                                                    ControlPlaneFrameHeader& out) noexcept {
    if (in.size() < kControlPlaneHeaderSize) return FrameDecodeStatus::Truncated;
    const std::byte* p = in.data();
    const std::uint8_t version = read_u8(p);
    if (version != kFrameFormatVersion) return FrameDecodeStatus::UnknownVersion;
    const std::uint8_t record_type_raw = read_u8(p);
    if (!is_legal_durable_record_type(record_type_raw)) return FrameDecodeStatus::MalformedEnum;
    out.record_type = static_cast<DurableRecordType>(record_type_raw);
    out.key_id = read_u32_le(p);
    out.sequence_number = read_u64_le(p);
    const std::uint8_t time_kind_raw = read_u8(p);
    if (!is_legal_frame_time_kind(time_kind_raw)) return FrameDecodeStatus::MalformedEnum;
    out.time_kind = static_cast<FrameTimeKind>(time_kind_raw);
    out.recorded_utc_ms = read_i64_le(p);
    out.payload_length = read_u32_le(p);
    return FrameDecodeStatus::Ok;
}

inline bool is_legal_freeze_clear_kind(std::uint8_t v) noexcept {
    return v <= static_cast<std::uint8_t>(FreezeClearKind::OperatorAuthorized);
}

inline bool is_legal_freeze_probe_purpose(std::uint8_t v) noexcept {
    return v <= static_cast<std::uint8_t>(FreezeProbePurpose::ClockRepublishOrVerify);
}

}  // namespace detail

// ===========================================================================
// RateLimitFreezePayload -- DurableRecordType::RateLimitFreeze
// ===========================================================================

inline constexpr std::size_t kRateLimitFreezePayloadWireSize =
    8 +  // recorded_utc_ms
    8 +  // deadline_utc_ms
    8 +  // conservative_wait_ms
    1 +  // source
    3 +  // pad[3]
    4 +  // freeze_epoch
    4;   // wait_generation
static_assert(kRateLimitFreezePayloadWireSize == 36);

inline void encode_rate_limit_freeze_payload(std::span<std::byte, kRateLimitFreezePayloadWireSize> out,
                                              const RateLimitFreezePayload& v) noexcept {
    std::byte* p = out.data();
    detail::write_i64_le(p, v.recorded_utc_ms);
    detail::write_i64_le(p, v.deadline_utc_ms);
    detail::write_i64_le(p, v.conservative_wait_ms);
    detail::write_u8(p, v.source);
    detail::write_u8(p, 0);
    detail::write_u8(p, 0);
    detail::write_u8(p, 0);
    detail::write_u32_le(p, v.freeze_epoch);
    detail::write_u32_le(p, v.wait_generation);
}

inline bool decode_rate_limit_freeze_payload(std::span<const std::byte, kRateLimitFreezePayloadWireSize> in,
                                              RateLimitFreezePayload& out) noexcept {
    const std::byte* p = in.data();
    out.recorded_utc_ms = detail::read_i64_le(p);
    out.deadline_utc_ms = detail::read_i64_le(p);
    out.conservative_wait_ms = detail::read_i64_le(p);
    out.source = detail::read_u8(p);
    const std::uint8_t pad0 = detail::read_u8(p);
    const std::uint8_t pad1 = detail::read_u8(p);
    const std::uint8_t pad2 = detail::read_u8(p);
    if (pad0 != 0 || pad1 != 0 || pad2 != 0) return false;
    out.freeze_epoch = detail::read_u32_le(p);
    out.wait_generation = detail::read_u32_le(p);
    return true;
}

inline constexpr std::size_t kRateLimitFreezeFrameSize = kFrameEnvelopeOverhead + kRateLimitFreezePayloadWireSize;

inline std::size_t encode_rate_limit_freeze_frame(std::span<std::byte> out, std::uint32_t key_id,
                                                   std::uint64_t sequence_number, FrameTimeKind time_kind,
                                                   std::int64_t recorded_utc_ms,
                                                   const RateLimitFreezePayload& payload,
                                                   std::span<const std::byte, kMacLen> prev_mac,
                                                   std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kRateLimitFreezeFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::RateLimitFreeze, key_id, sequence_number, time_kind,
                                        recorded_utc_ms,
                                        static_cast<std::uint32_t>(kRateLimitFreezePayloadWireSize));
    encode_rate_limit_freeze_payload(
        std::span<std::byte, kRateLimitFreezePayloadWireSize>(p, kRateLimitFreezePayloadWireSize), payload);
    p += kRateLimitFreezePayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kRateLimitFreezeFrameSize;
}

struct DecodedRateLimitFreezeFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    RateLimitFreezePayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_rate_limit_freeze_frame(std::span<const std::byte> in,
                                                         std::span<const std::byte> hmac_key,
                                                         DecodedRateLimitFreezeFrame& out,
                                                         std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::RateLimitFreeze) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kRateLimitFreezePayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kRateLimitFreezeFrameSize;
    if (in.size() < kRateLimitFreezeFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    RateLimitFreezePayload payload{};
    if (!decode_rate_limit_freeze_payload(
            std::span<const std::byte, kRateLimitFreezePayloadWireSize>(p, kRateLimitFreezePayloadWireSize),
            payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kRateLimitFreezePayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// FreezeProbeAttemptPayload -- DurableRecordType::FreezeProbeAttempt
// ===========================================================================

inline constexpr std::size_t kFreezeProbeAttemptPayloadWireSize =
    4 +  // freeze_epoch
    4 +  // attempt_ordinal
    1 +  // cleared (bool)
    1 +  // purpose
    8;   // not_before_utc_ms
static_assert(kFreezeProbeAttemptPayloadWireSize == 18);

inline void encode_freeze_probe_attempt_payload(std::span<std::byte, kFreezeProbeAttemptPayloadWireSize> out,
                                                 const FreezeProbeAttemptPayload& v) noexcept {
    std::byte* p = out.data();
    detail::write_u32_le(p, v.freeze_epoch);
    detail::write_u32_le(p, v.attempt_ordinal);
    detail::write_u8(p, v.cleared ? 1u : 0u);
    detail::write_u8(p, static_cast<std::uint8_t>(v.purpose));
    detail::write_i64_le(p, v.not_before_utc_ms);
}

inline bool decode_freeze_probe_attempt_payload(std::span<const std::byte, kFreezeProbeAttemptPayloadWireSize> in,
                                                 FreezeProbeAttemptPayload& out) noexcept {
    const std::byte* p = in.data();
    out.freeze_epoch = detail::read_u32_le(p);
    out.attempt_ordinal = detail::read_u32_le(p);
    out.cleared = detail::read_u8(p) != 0;
    const std::uint8_t purpose_raw = detail::read_u8(p);
    if (!detail::is_legal_freeze_probe_purpose(purpose_raw)) return false;
    out.purpose = static_cast<FreezeProbePurpose>(purpose_raw);
    out.not_before_utc_ms = detail::read_i64_le(p);
    return true;
}

inline constexpr std::size_t kFreezeProbeAttemptFrameSize =
    kFrameEnvelopeOverhead + kFreezeProbeAttemptPayloadWireSize;

inline std::size_t encode_freeze_probe_attempt_frame(std::span<std::byte> out, std::uint32_t key_id,
                                                      std::uint64_t sequence_number, FrameTimeKind time_kind,
                                                      std::int64_t recorded_utc_ms,
                                                      const FreezeProbeAttemptPayload& payload,
                                                      std::span<const std::byte, kMacLen> prev_mac,
                                                      std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kFreezeProbeAttemptFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::FreezeProbeAttempt, key_id, sequence_number, time_kind,
                                        recorded_utc_ms,
                                        static_cast<std::uint32_t>(kFreezeProbeAttemptPayloadWireSize));
    encode_freeze_probe_attempt_payload(
        std::span<std::byte, kFreezeProbeAttemptPayloadWireSize>(p, kFreezeProbeAttemptPayloadWireSize), payload);
    p += kFreezeProbeAttemptPayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kFreezeProbeAttemptFrameSize;
}

struct DecodedFreezeProbeAttemptFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    FreezeProbeAttemptPayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_freeze_probe_attempt_frame(std::span<const std::byte> in,
                                                            std::span<const std::byte> hmac_key,
                                                            DecodedFreezeProbeAttemptFrame& out,
                                                            std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::FreezeProbeAttempt) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kFreezeProbeAttemptPayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kFreezeProbeAttemptFrameSize;
    if (in.size() < kFreezeProbeAttemptFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    FreezeProbeAttemptPayload payload{};
    if (!decode_freeze_probe_attempt_payload(
            std::span<const std::byte, kFreezeProbeAttemptPayloadWireSize>(p, kFreezeProbeAttemptPayloadWireSize),
            payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kFreezeProbeAttemptPayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// FreezeClearPayload -- DurableRecordType::FreezeClear
// (embeds FreezeTimeProbeProof inline, not as a separately-framed record)
// ===========================================================================

inline constexpr std::size_t kFreezeTimeProbeProofWireSize =
    8 +   // server_time_ms
    8 +   // bound_deadline_utc_ms
    4 +   // clock_snapshot_seq
    8 +   // clock_offset_ms
    8 +   // request_nonce
    64 +  // tls_verified_host[64]
    32;   // time_response_mac[32]
static_assert(kFreezeTimeProbeProofWireSize == 132);

inline void encode_freeze_time_probe_proof(std::span<std::byte, kFreezeTimeProbeProofWireSize> out,
                                            const FreezeTimeProbeProof& v) noexcept {
    std::byte* p = out.data();
    detail::write_i64_le(p, v.server_time_ms);
    detail::write_i64_le(p, v.bound_deadline_utc_ms);
    detail::write_u32_le(p, v.clock_snapshot_seq);
    detail::write_i64_le(p, v.clock_offset_ms);
    detail::write_u64_le(p, v.request_nonce);
    detail::write_bytes(p, v.tls_verified_host, sizeof(v.tls_verified_host));
    detail::write_bytes(p, v.time_response_mac, sizeof(v.time_response_mac));
}

inline void decode_freeze_time_probe_proof(std::span<const std::byte, kFreezeTimeProbeProofWireSize> in,
                                            FreezeTimeProbeProof& out) noexcept {
    const std::byte* p = in.data();
    out.server_time_ms = detail::read_i64_le(p);
    out.bound_deadline_utc_ms = detail::read_i64_le(p);
    out.clock_snapshot_seq = detail::read_u32_le(p);
    out.clock_offset_ms = detail::read_i64_le(p);
    out.request_nonce = detail::read_u64_le(p);
    detail::read_bytes(p, out.tls_verified_host, sizeof(out.tls_verified_host));
    detail::read_bytes(p, out.time_response_mac, sizeof(out.time_response_mac));
}

inline constexpr std::size_t kFreezeClearPayloadWireSize =
    4 +                              // freeze_epoch
    1 +                              // clear_kind
    3 +                              // pad[3]
    kFreezeTimeProbeProofWireSize +  // time_proof
    8 +                              // bound_conservative_wait_ms
    4 +                              // bound_wait_generation
    8 +                              // store_uuid_lo
    8 +                              // store_uuid_hi
    4 +                              // bound_generation
    8 +                              // bound_tip_seq
    32 +                             // bound_tip_mac[32]
    8 +                              // wall_utc_ms
    8 +                              // expires_utc_ms
    8 +                              // nonce
    4 +                              // kek_key_id
    32 +                             // operator_id[32]
    32;                              // mac[32]
static_assert(kFreezeClearPayloadWireSize == 304);

inline void encode_freeze_clear_payload(std::span<std::byte, kFreezeClearPayloadWireSize> out,
                                        const FreezeClearPayload& v) noexcept {
    std::byte* p = out.data();
    detail::write_u32_le(p, v.freeze_epoch);
    detail::write_u8(p, static_cast<std::uint8_t>(v.clear_kind));
    detail::write_u8(p, 0);
    detail::write_u8(p, 0);
    detail::write_u8(p, 0);
    encode_freeze_time_probe_proof(std::span<std::byte, kFreezeTimeProbeProofWireSize>(p, kFreezeTimeProbeProofWireSize),
                                    v.time_proof);
    p += kFreezeTimeProbeProofWireSize;
    detail::write_i64_le(p, v.bound_conservative_wait_ms);
    detail::write_u32_le(p, v.bound_wait_generation);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u32_le(p, v.bound_generation);
    detail::write_u64_le(p, v.bound_tip_seq);
    detail::write_bytes(p, v.bound_tip_mac, sizeof(v.bound_tip_mac));
    detail::write_i64_le(p, v.wall_utc_ms);
    detail::write_i64_le(p, v.expires_utc_ms);
    detail::write_u64_le(p, v.nonce);
    detail::write_u32_le(p, v.kek_key_id);
    detail::write_bytes(p, v.operator_id, sizeof(v.operator_id));
    detail::write_bytes(p, v.mac, sizeof(v.mac));
}

inline bool decode_freeze_clear_payload(std::span<const std::byte, kFreezeClearPayloadWireSize> in,
                                        FreezeClearPayload& out) noexcept {
    const std::byte* p = in.data();
    out.freeze_epoch = detail::read_u32_le(p);
    const std::uint8_t clear_kind_raw = detail::read_u8(p);
    if (!detail::is_legal_freeze_clear_kind(clear_kind_raw)) return false;
    out.clear_kind = static_cast<FreezeClearKind>(clear_kind_raw);
    const std::uint8_t pad0 = detail::read_u8(p);
    const std::uint8_t pad1 = detail::read_u8(p);
    const std::uint8_t pad2 = detail::read_u8(p);
    if (pad0 != 0 || pad1 != 0 || pad2 != 0) return false;
    decode_freeze_time_probe_proof(
        std::span<const std::byte, kFreezeTimeProbeProofWireSize>(p, kFreezeTimeProbeProofWireSize), out.time_proof);
    p += kFreezeTimeProbeProofWireSize;
    out.bound_conservative_wait_ms = detail::read_i64_le(p);
    out.bound_wait_generation = detail::read_u32_le(p);
    out.store_uuid_lo = detail::read_u64_le(p);
    out.store_uuid_hi = detail::read_u64_le(p);
    out.bound_generation = detail::read_u32_le(p);
    out.bound_tip_seq = detail::read_u64_le(p);
    detail::read_bytes(p, out.bound_tip_mac, sizeof(out.bound_tip_mac));
    out.wall_utc_ms = detail::read_i64_le(p);
    out.expires_utc_ms = detail::read_i64_le(p);
    out.nonce = detail::read_u64_le(p);
    out.kek_key_id = detail::read_u32_le(p);
    detail::read_bytes(p, out.operator_id, sizeof(out.operator_id));
    detail::read_bytes(p, out.mac, sizeof(out.mac));
    return true;
}

inline constexpr std::size_t kFreezeClearFrameSize = kFrameEnvelopeOverhead + kFreezeClearPayloadWireSize;

inline std::size_t encode_freeze_clear_frame(std::span<std::byte> out, std::uint32_t key_id,
                                              std::uint64_t sequence_number, FrameTimeKind time_kind,
                                              std::int64_t recorded_utc_ms, const FreezeClearPayload& payload,
                                              std::span<const std::byte, kMacLen> prev_mac,
                                              std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kFreezeClearFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::FreezeClear, key_id, sequence_number, time_kind,
                                        recorded_utc_ms, static_cast<std::uint32_t>(kFreezeClearPayloadWireSize));
    encode_freeze_clear_payload(std::span<std::byte, kFreezeClearPayloadWireSize>(p, kFreezeClearPayloadWireSize),
                                payload);
    p += kFreezeClearPayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kFreezeClearFrameSize;
}

struct DecodedFreezeClearFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    FreezeClearPayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_freeze_clear_frame(std::span<const std::byte> in, std::span<const std::byte> hmac_key,
                                                    DecodedFreezeClearFrame& out,
                                                    std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::FreezeClear) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kFreezeClearPayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kFreezeClearFrameSize;
    if (in.size() < kFreezeClearFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    FreezeClearPayload payload{};
    if (!decode_freeze_clear_payload(
            std::span<const std::byte, kFreezeClearPayloadWireSize>(p, kFreezeClearPayloadWireSize), payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kFreezeClearPayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// FreezeWaitArmPayload -- DurableRecordType::FreezeWaitArm
// ===========================================================================

inline constexpr std::size_t kFreezeWaitArmPayloadWireSize =
    4 +  // freeze_epoch
    8 +  // bound_conservative_wait_ms
    4 +  // wait_generation
    4;   // arm_ordinal
static_assert(kFreezeWaitArmPayloadWireSize == 20);

inline void encode_freeze_wait_arm_payload(std::span<std::byte, kFreezeWaitArmPayloadWireSize> out,
                                            const FreezeWaitArmPayload& v) noexcept {
    std::byte* p = out.data();
    detail::write_u32_le(p, v.freeze_epoch);
    detail::write_i64_le(p, v.bound_conservative_wait_ms);
    detail::write_u32_le(p, v.wait_generation);
    detail::write_u32_le(p, v.arm_ordinal);
}

inline bool decode_freeze_wait_arm_payload(std::span<const std::byte, kFreezeWaitArmPayloadWireSize> in,
                                            FreezeWaitArmPayload& out) noexcept {
    const std::byte* p = in.data();
    out.freeze_epoch = detail::read_u32_le(p);
    out.bound_conservative_wait_ms = detail::read_i64_le(p);
    out.wait_generation = detail::read_u32_le(p);
    out.arm_ordinal = detail::read_u32_le(p);
    return true;
}

inline constexpr std::size_t kFreezeWaitArmFrameSize = kFrameEnvelopeOverhead + kFreezeWaitArmPayloadWireSize;

inline std::size_t encode_freeze_wait_arm_frame(std::span<std::byte> out, std::uint32_t key_id,
                                                 std::uint64_t sequence_number, FrameTimeKind time_kind,
                                                 std::int64_t recorded_utc_ms, const FreezeWaitArmPayload& payload,
                                                 std::span<const std::byte, kMacLen> prev_mac,
                                                 std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kFreezeWaitArmFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::FreezeWaitArm, key_id, sequence_number, time_kind,
                                        recorded_utc_ms, static_cast<std::uint32_t>(kFreezeWaitArmPayloadWireSize));
    encode_freeze_wait_arm_payload(std::span<std::byte, kFreezeWaitArmPayloadWireSize>(p, kFreezeWaitArmPayloadWireSize),
                                   payload);
    p += kFreezeWaitArmPayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kFreezeWaitArmFrameSize;
}

struct DecodedFreezeWaitArmFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    FreezeWaitArmPayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_freeze_wait_arm_frame(std::span<const std::byte> in,
                                                       std::span<const std::byte> hmac_key,
                                                       DecodedFreezeWaitArmFrame& out,
                                                       std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::FreezeWaitArm) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kFreezeWaitArmPayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kFreezeWaitArmFrameSize;
    if (in.size() < kFreezeWaitArmFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    FreezeWaitArmPayload payload{};
    if (!decode_freeze_wait_arm_payload(
            std::span<const std::byte, kFreezeWaitArmPayloadWireSize>(p, kFreezeWaitArmPayloadWireSize), payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kFreezeWaitArmPayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// FreezeWaitSatisfiedPayload -- DurableRecordType::FreezeWaitSatisfied
// ===========================================================================

inline constexpr std::size_t kFreezeWaitSatisfiedPayloadWireSize =
    4 +  // freeze_epoch
    8 +  // bound_conservative_wait_ms
    4 +  // wait_generation
    4 +  // satisfaction_ordinal
    4 +  // arm_ordinal
    8 +  // arm_frame_seq
    8;   // elapsed_steady_ms_claimed
static_assert(kFreezeWaitSatisfiedPayloadWireSize == 40);

inline void encode_freeze_wait_satisfied_payload(std::span<std::byte, kFreezeWaitSatisfiedPayloadWireSize> out,
                                                  const FreezeWaitSatisfiedPayload& v) noexcept {
    std::byte* p = out.data();
    detail::write_u32_le(p, v.freeze_epoch);
    detail::write_i64_le(p, v.bound_conservative_wait_ms);
    detail::write_u32_le(p, v.wait_generation);
    detail::write_u32_le(p, v.satisfaction_ordinal);
    detail::write_u32_le(p, v.arm_ordinal);
    detail::write_u64_le(p, v.arm_frame_seq);
    detail::write_i64_le(p, v.elapsed_steady_ms_claimed);
}

inline bool decode_freeze_wait_satisfied_payload(std::span<const std::byte, kFreezeWaitSatisfiedPayloadWireSize> in,
                                                  FreezeWaitSatisfiedPayload& out) noexcept {
    const std::byte* p = in.data();
    out.freeze_epoch = detail::read_u32_le(p);
    out.bound_conservative_wait_ms = detail::read_i64_le(p);
    out.wait_generation = detail::read_u32_le(p);
    out.satisfaction_ordinal = detail::read_u32_le(p);
    out.arm_ordinal = detail::read_u32_le(p);
    out.arm_frame_seq = detail::read_u64_le(p);
    out.elapsed_steady_ms_claimed = detail::read_i64_le(p);
    return true;
}

inline constexpr std::size_t kFreezeWaitSatisfiedFrameSize =
    kFrameEnvelopeOverhead + kFreezeWaitSatisfiedPayloadWireSize;

inline std::size_t encode_freeze_wait_satisfied_frame(std::span<std::byte> out, std::uint32_t key_id,
                                                       std::uint64_t sequence_number, FrameTimeKind time_kind,
                                                       std::int64_t recorded_utc_ms,
                                                       const FreezeWaitSatisfiedPayload& payload,
                                                       std::span<const std::byte, kMacLen> prev_mac,
                                                       std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kFreezeWaitSatisfiedFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::FreezeWaitSatisfied, key_id, sequence_number, time_kind,
                                        recorded_utc_ms,
                                        static_cast<std::uint32_t>(kFreezeWaitSatisfiedPayloadWireSize));
    encode_freeze_wait_satisfied_payload(
        std::span<std::byte, kFreezeWaitSatisfiedPayloadWireSize>(p, kFreezeWaitSatisfiedPayloadWireSize), payload);
    p += kFreezeWaitSatisfiedPayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kFreezeWaitSatisfiedFrameSize;
}

struct DecodedFreezeWaitSatisfiedFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    FreezeWaitSatisfiedPayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_freeze_wait_satisfied_frame(std::span<const std::byte> in,
                                                             std::span<const std::byte> hmac_key,
                                                             DecodedFreezeWaitSatisfiedFrame& out,
                                                             std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::FreezeWaitSatisfied) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kFreezeWaitSatisfiedPayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kFreezeWaitSatisfiedFrameSize;
    if (in.size() < kFreezeWaitSatisfiedFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    FreezeWaitSatisfiedPayload payload{};
    if (!decode_freeze_wait_satisfied_payload(
            std::span<const std::byte, kFreezeWaitSatisfiedPayloadWireSize>(p, kFreezeWaitSatisfiedPayloadWireSize),
            payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kFreezeWaitSatisfiedPayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// FreezeEpochWatermarkPayload -- DurableRecordType::FreezeEpochWatermark
// ===========================================================================

inline constexpr std::size_t kFreezeEpochWatermarkPayloadWireSize = 4;  // next_freeze_epoch

inline void encode_freeze_epoch_watermark_payload(std::span<std::byte, kFreezeEpochWatermarkPayloadWireSize> out,
                                                   const FreezeEpochWatermarkPayload& v) noexcept {
    std::byte* p = out.data();
    detail::write_u32_le(p, v.next_freeze_epoch);
}

inline bool decode_freeze_epoch_watermark_payload(
    std::span<const std::byte, kFreezeEpochWatermarkPayloadWireSize> in,
    FreezeEpochWatermarkPayload& out) noexcept {
    const std::byte* p = in.data();
    out.next_freeze_epoch = detail::read_u32_le(p);
    return true;
}

inline constexpr std::size_t kFreezeEpochWatermarkFrameSize =
    kFrameEnvelopeOverhead + kFreezeEpochWatermarkPayloadWireSize;

inline std::size_t encode_freeze_epoch_watermark_frame(std::span<std::byte> out, std::uint32_t key_id,
                                                        std::uint64_t sequence_number, FrameTimeKind time_kind,
                                                        std::int64_t recorded_utc_ms,
                                                        const FreezeEpochWatermarkPayload& payload,
                                                        std::span<const std::byte, kMacLen> prev_mac,
                                                        std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kFreezeEpochWatermarkFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::FreezeEpochWatermark, key_id, sequence_number,
                                        time_kind, recorded_utc_ms,
                                        static_cast<std::uint32_t>(kFreezeEpochWatermarkPayloadWireSize));
    encode_freeze_epoch_watermark_payload(
        std::span<std::byte, kFreezeEpochWatermarkPayloadWireSize>(p, kFreezeEpochWatermarkPayloadWireSize), payload);
    p += kFreezeEpochWatermarkPayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kFreezeEpochWatermarkFrameSize;
}

struct DecodedFreezeEpochWatermarkFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    FreezeEpochWatermarkPayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_freeze_epoch_watermark_frame(std::span<const std::byte> in,
                                                              std::span<const std::byte> hmac_key,
                                                              DecodedFreezeEpochWatermarkFrame& out,
                                                              std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::FreezeEpochWatermark) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kFreezeEpochWatermarkPayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kFreezeEpochWatermarkFrameSize;
    if (in.size() < kFreezeEpochWatermarkFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    FreezeEpochWatermarkPayload payload{};
    if (!decode_freeze_epoch_watermark_payload(
            std::span<const std::byte, kFreezeEpochWatermarkPayloadWireSize>(p, kFreezeEpochWatermarkPayloadWireSize),
            payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kFreezeEpochWatermarkPayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// EndpointWeightConfig -- DurableRecordType::EndpointWeightConfigSet
// ===========================================================================

inline constexpr std::size_t kEndpointWeightConfigPayloadWireSize =
    4 +       // config_version
    6 * 4 +   // weights[6]
    4;        // safety_pad
static_assert(kEndpointWeightConfigPayloadWireSize == 32);

inline void encode_endpoint_weight_config_payload(std::span<std::byte, kEndpointWeightConfigPayloadWireSize> out,
                                                   const EndpointWeightConfig& v) noexcept {
    std::byte* p = out.data();
    detail::write_u32_le(p, v.config_version);
    for (std::uint32_t w : v.weights) detail::write_u32_le(p, w);
    detail::write_u32_le(p, v.safety_pad);
}

inline bool decode_endpoint_weight_config_payload(
    std::span<const std::byte, kEndpointWeightConfigPayloadWireSize> in, EndpointWeightConfig& out) noexcept {
    const std::byte* p = in.data();
    out.config_version = detail::read_u32_le(p);
    for (auto& w : out.weights) w = detail::read_u32_le(p);
    out.safety_pad = detail::read_u32_le(p);
    return true;
}

inline constexpr std::size_t kEndpointWeightConfigFrameSize =
    kFrameEnvelopeOverhead + kEndpointWeightConfigPayloadWireSize;

inline std::size_t encode_endpoint_weight_config_frame(std::span<std::byte> out, std::uint32_t key_id,
                                                        std::uint64_t sequence_number, FrameTimeKind time_kind,
                                                        std::int64_t recorded_utc_ms,
                                                        const EndpointWeightConfig& payload,
                                                        std::span<const std::byte, kMacLen> prev_mac,
                                                        std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kEndpointWeightConfigFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::EndpointWeightConfigSet, key_id, sequence_number,
                                        time_kind, recorded_utc_ms,
                                        static_cast<std::uint32_t>(kEndpointWeightConfigPayloadWireSize));
    encode_endpoint_weight_config_payload(
        std::span<std::byte, kEndpointWeightConfigPayloadWireSize>(p, kEndpointWeightConfigPayloadWireSize), payload);
    p += kEndpointWeightConfigPayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kEndpointWeightConfigFrameSize;
}

struct DecodedEndpointWeightConfigFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    EndpointWeightConfig payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_endpoint_weight_config_frame(std::span<const std::byte> in,
                                                              std::span<const std::byte> hmac_key,
                                                              DecodedEndpointWeightConfigFrame& out,
                                                              std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::EndpointWeightConfigSet) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kEndpointWeightConfigPayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kEndpointWeightConfigFrameSize;
    if (in.size() < kEndpointWeightConfigFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    EndpointWeightConfig payload{};
    if (!decode_endpoint_weight_config_payload(
            std::span<const std::byte, kEndpointWeightConfigPayloadWireSize>(p, kEndpointWeightConfigPayloadWireSize),
            payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kEndpointWeightConfigPayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// RateLimitUsageSnapshotPayload -- DurableRecordType::RateLimitUsageSnapshot
// ===========================================================================

inline constexpr std::size_t kRateLimitUsageEntryWireSize =
    1 +  // tracker
    7 +  // interval_suffix[7]
    8 +  // bucket_start_server_ms
    4;   // used
static_assert(kRateLimitUsageEntryWireSize == 20);

inline constexpr std::size_t kRateLimitUsageSnapshotPayloadWireSize =
    8 +                            // recorded_utc_ms
    4 +                            // entry_count
    8 * kRateLimitUsageEntryWireSize;  // entries[8], always all 8 written
static_assert(kRateLimitUsageSnapshotPayloadWireSize == 172);

inline void encode_rate_limit_usage_snapshot_payload(
    std::span<std::byte, kRateLimitUsageSnapshotPayloadWireSize> out,
    const RateLimitUsageSnapshotPayload& v) noexcept {
    std::byte* p = out.data();
    detail::write_i64_le(p, v.recorded_utc_ms);
    detail::write_u32_le(p, v.entry_count);
    for (const auto& e : v.entries) {
        detail::write_u8(p, e.tracker);
        detail::write_bytes(p, e.interval_suffix, sizeof(e.interval_suffix));
        detail::write_i64_le(p, e.bucket_start_server_ms);
        detail::write_u32_le(p, e.used);
    }
}

inline bool decode_rate_limit_usage_snapshot_payload(
    std::span<const std::byte, kRateLimitUsageSnapshotPayloadWireSize> in,
    RateLimitUsageSnapshotPayload& out) noexcept {
    const std::byte* p = in.data();
    out.recorded_utc_ms = detail::read_i64_le(p);
    out.entry_count = detail::read_u32_le(p);
    for (auto& e : out.entries) {
        e.tracker = detail::read_u8(p);
        detail::read_bytes(p, e.interval_suffix, sizeof(e.interval_suffix));
        e.bucket_start_server_ms = detail::read_i64_le(p);
        e.used = detail::read_u32_le(p);
    }
    return true;
}

inline constexpr std::size_t kRateLimitUsageSnapshotFrameSize =
    kFrameEnvelopeOverhead + kRateLimitUsageSnapshotPayloadWireSize;

inline std::size_t encode_rate_limit_usage_snapshot_frame(std::span<std::byte> out, std::uint32_t key_id,
                                                           std::uint64_t sequence_number, FrameTimeKind time_kind,
                                                           std::int64_t recorded_utc_ms,
                                                           const RateLimitUsageSnapshotPayload& payload,
                                                           std::span<const std::byte, kMacLen> prev_mac,
                                                           std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kRateLimitUsageSnapshotFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::RateLimitUsageSnapshot, key_id, sequence_number,
                                        time_kind, recorded_utc_ms,
                                        static_cast<std::uint32_t>(kRateLimitUsageSnapshotPayloadWireSize));
    encode_rate_limit_usage_snapshot_payload(
        std::span<std::byte, kRateLimitUsageSnapshotPayloadWireSize>(p, kRateLimitUsageSnapshotPayloadWireSize),
        payload);
    p += kRateLimitUsageSnapshotPayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kRateLimitUsageSnapshotFrameSize;
}

struct DecodedRateLimitUsageSnapshotFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    RateLimitUsageSnapshotPayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_rate_limit_usage_snapshot_frame(std::span<const std::byte> in,
                                                                 std::span<const std::byte> hmac_key,
                                                                 DecodedRateLimitUsageSnapshotFrame& out,
                                                                 std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::RateLimitUsageSnapshot) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kRateLimitUsageSnapshotPayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kRateLimitUsageSnapshotFrameSize;
    if (in.size() < kRateLimitUsageSnapshotFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    RateLimitUsageSnapshotPayload payload{};
    if (!decode_rate_limit_usage_snapshot_payload(
            std::span<const std::byte, kRateLimitUsageSnapshotPayloadWireSize>(
                p, kRateLimitUsageSnapshotPayloadWireSize),
            payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kRateLimitUsageSnapshotPayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// OperatorOverridePayload -- DurableRecordType::OperatorOverride
// ===========================================================================

inline constexpr std::size_t kOperatorOverridePayloadWireSize =
    8 +   // store_uuid_lo
    8 +   // store_uuid_hi
    8 +   // local_tip_seq
    32 +  // local_tip_mac[32]
    4 +   // local_generation
    4 +   // last_remote_acked_generation
    8 +   // last_remote_acked_seq
    32 +  // last_remote_acked_mac[32]
    1 +   // admit_mode
    8 +   // wall_utc_ms
    8 +   // expires_utc_ms
    8 +   // nonce
    4 +   // kek_key_id
    4 +   // reason_code
    32 +  // operator_id[32]
    32;   // mac[32]
static_assert(kOperatorOverridePayloadWireSize == 201);

inline void encode_operator_override_payload(std::span<std::byte, kOperatorOverridePayloadWireSize> out,
                                              const OperatorOverridePayload& v) noexcept {
    std::byte* p = out.data();
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u64_le(p, v.local_tip_seq);
    detail::write_bytes(p, v.local_tip_mac, sizeof(v.local_tip_mac));
    detail::write_u32_le(p, v.local_generation);
    detail::write_u32_le(p, v.last_remote_acked_generation);
    detail::write_u64_le(p, v.last_remote_acked_seq);
    detail::write_bytes(p, v.last_remote_acked_mac, sizeof(v.last_remote_acked_mac));
    detail::write_u8(p, v.admit_mode);
    detail::write_i64_le(p, v.wall_utc_ms);
    detail::write_i64_le(p, v.expires_utc_ms);
    detail::write_u64_le(p, v.nonce);
    detail::write_u32_le(p, v.kek_key_id);
    detail::write_u32_le(p, v.reason_code);
    detail::write_bytes(p, v.operator_id, sizeof(v.operator_id));
    detail::write_bytes(p, v.mac, sizeof(v.mac));
}

inline bool decode_operator_override_payload(std::span<const std::byte, kOperatorOverridePayloadWireSize> in,
                                              OperatorOverridePayload& out) noexcept {
    const std::byte* p = in.data();
    out.store_uuid_lo = detail::read_u64_le(p);
    out.store_uuid_hi = detail::read_u64_le(p);
    out.local_tip_seq = detail::read_u64_le(p);
    detail::read_bytes(p, out.local_tip_mac, sizeof(out.local_tip_mac));
    out.local_generation = detail::read_u32_le(p);
    out.last_remote_acked_generation = detail::read_u32_le(p);
    out.last_remote_acked_seq = detail::read_u64_le(p);
    detail::read_bytes(p, out.last_remote_acked_mac, sizeof(out.last_remote_acked_mac));
    out.admit_mode = detail::read_u8(p);
    out.wall_utc_ms = detail::read_i64_le(p);
    out.expires_utc_ms = detail::read_i64_le(p);
    out.nonce = detail::read_u64_le(p);
    out.kek_key_id = detail::read_u32_le(p);
    out.reason_code = detail::read_u32_le(p);
    detail::read_bytes(p, out.operator_id, sizeof(out.operator_id));
    detail::read_bytes(p, out.mac, sizeof(out.mac));
    return true;
}

inline constexpr std::size_t kOperatorOverrideFrameSize = kFrameEnvelopeOverhead + kOperatorOverridePayloadWireSize;

inline std::size_t encode_operator_override_frame(std::span<std::byte> out, std::uint32_t key_id,
                                                   std::uint64_t sequence_number, FrameTimeKind time_kind,
                                                   std::int64_t recorded_utc_ms,
                                                   const OperatorOverridePayload& payload,
                                                   std::span<const std::byte, kMacLen> prev_mac,
                                                   std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kOperatorOverrideFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::OperatorOverride, key_id, sequence_number, time_kind,
                                        recorded_utc_ms,
                                        static_cast<std::uint32_t>(kOperatorOverridePayloadWireSize));
    encode_operator_override_payload(
        std::span<std::byte, kOperatorOverridePayloadWireSize>(p, kOperatorOverridePayloadWireSize), payload);
    p += kOperatorOverridePayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kOperatorOverrideFrameSize;
}

struct DecodedOperatorOverrideFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    OperatorOverridePayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_operator_override_frame(std::span<const std::byte> in,
                                                         std::span<const std::byte> hmac_key,
                                                         DecodedOperatorOverrideFrame& out,
                                                         std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::OperatorOverride) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kOperatorOverridePayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kOperatorOverrideFrameSize;
    if (in.size() < kOperatorOverrideFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    OperatorOverridePayload payload{};
    if (!decode_operator_override_payload(
            std::span<const std::byte, kOperatorOverridePayloadWireSize>(p, kOperatorOverridePayloadWireSize),
            payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kOperatorOverridePayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// GenerationBridgePayload -- DurableRecordType::GenerationBridge
// ===========================================================================

inline constexpr std::size_t kGenerationBridgePayloadWireSize =
    4 +   // prev_generation
    8 +   // prev_tip_seq
    32 +  // prev_tip_mac[32]
    4 +   // prev_key_id
    4 +   // new_generation
    8 +   // new_genesis_seq
    4 +   // new_key_id
    32;   // bridge_mac[32]
static_assert(kGenerationBridgePayloadWireSize == 96);

inline void encode_generation_bridge_payload(std::span<std::byte, kGenerationBridgePayloadWireSize> out,
                                              const GenerationBridgePayload& v) noexcept {
    std::byte* p = out.data();
    detail::write_u32_le(p, v.prev_generation);
    detail::write_u64_le(p, v.prev_tip_seq);
    detail::write_bytes(p, v.prev_tip_mac, sizeof(v.prev_tip_mac));
    detail::write_u32_le(p, v.prev_key_id);
    detail::write_u32_le(p, v.new_generation);
    detail::write_u64_le(p, v.new_genesis_seq);
    detail::write_u32_le(p, v.new_key_id);
    detail::write_bytes(p, v.bridge_mac, sizeof(v.bridge_mac));
}

inline bool decode_generation_bridge_payload(std::span<const std::byte, kGenerationBridgePayloadWireSize> in,
                                              GenerationBridgePayload& out) noexcept {
    const std::byte* p = in.data();
    out.prev_generation = detail::read_u32_le(p);
    out.prev_tip_seq = detail::read_u64_le(p);
    detail::read_bytes(p, out.prev_tip_mac, sizeof(out.prev_tip_mac));
    out.prev_key_id = detail::read_u32_le(p);
    out.new_generation = detail::read_u32_le(p);
    out.new_genesis_seq = detail::read_u64_le(p);
    out.new_key_id = detail::read_u32_le(p);
    detail::read_bytes(p, out.bridge_mac, sizeof(out.bridge_mac));
    return true;
}

inline constexpr std::size_t kGenerationBridgeFrameSize = kFrameEnvelopeOverhead + kGenerationBridgePayloadWireSize;

inline std::size_t encode_generation_bridge_frame(std::span<std::byte> out, std::uint32_t key_id,
                                                   std::uint64_t sequence_number, FrameTimeKind time_kind,
                                                   std::int64_t recorded_utc_ms,
                                                   const GenerationBridgePayload& payload,
                                                   std::span<const std::byte, kMacLen> prev_mac,
                                                   std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kGenerationBridgeFrameSize) return 0;
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::GenerationBridge, key_id, sequence_number, time_kind,
                                        recorded_utc_ms, static_cast<std::uint32_t>(kGenerationBridgePayloadWireSize));
    encode_generation_bridge_payload(
        std::span<std::byte, kGenerationBridgePayloadWireSize>(p, kGenerationBridgePayloadWireSize), payload);
    p += kGenerationBridgePayloadWireSize;
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kGenerationBridgeFrameSize;
}

struct DecodedGenerationBridgeFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    GenerationBridgePayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

inline FrameDecodeStatus decode_generation_bridge_frame(std::span<const std::byte> in,
                                                         std::span<const std::byte> hmac_key,
                                                         DecodedGenerationBridgeFrame& out,
                                                         std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::GenerationBridge) return FrameDecodeStatus::MalformedEnum;
    if (hdr.payload_length != kGenerationBridgePayloadWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kGenerationBridgeFrameSize;
    if (in.size() < kGenerationBridgeFrameSize) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;
    GenerationBridgePayload payload{};
    if (!decode_generation_bridge_payload(
            std::span<const std::byte, kGenerationBridgePayloadWireSize>(p, kGenerationBridgePayloadWireSize),
            payload)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kGenerationBridgePayloadWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

// ===========================================================================
// SymbolRegistrySnapshotPayload + std::span<const SymbolRules> entries --
// DurableRecordType::SymbolRegistrySnapshot. The ONE variable-length payload
// among the 11 -- payload_length already supports arbitrary lengths in the
// v4 envelope, so no envelope change is needed, only care in the bounds
// checking (see class header note on validate-before-multiply ordering).
// ===========================================================================

inline constexpr std::size_t kSymbolRulesWireSize =
    kSymbolNameLen +  // symbol[20]
    1 +               // is_trading
    8 +               // min_qty_ticks
    8 +               // max_qty_ticks
    8 +               // step_size_ticks
    8 +               // min_price_ticks
    8 +               // max_price_ticks
    8 +               // tick_size_ticks
    8 +               // min_notional_ticks
    4;                // rules_version
static_assert(kSymbolRulesWireSize == 81);

inline void encode_symbol_rules(std::span<std::byte, kSymbolRulesWireSize> out, const SymbolRules& v) noexcept {
    std::byte* p = out.data();
    detail::write_bytes(p, v.symbol, sizeof(v.symbol));
    detail::write_u8(p, v.is_trading ? 1u : 0u);
    detail::write_i64_le(p, v.min_qty_ticks);
    detail::write_i64_le(p, v.max_qty_ticks);
    detail::write_i64_le(p, v.step_size_ticks);
    detail::write_i64_le(p, v.min_price_ticks);
    detail::write_i64_le(p, v.max_price_ticks);
    detail::write_i64_le(p, v.tick_size_ticks);
    detail::write_i64_le(p, v.min_notional_ticks);
    detail::write_u32_le(p, v.rules_version);
}

inline bool decode_symbol_rules(std::span<const std::byte, kSymbolRulesWireSize> in, SymbolRules& out) noexcept {
    const std::byte* p = in.data();
    detail::read_bytes(p, out.symbol, sizeof(out.symbol));
    out.is_trading = detail::read_u8(p) != 0;
    out.min_qty_ticks = detail::read_i64_le(p);
    out.max_qty_ticks = detail::read_i64_le(p);
    out.step_size_ticks = detail::read_i64_le(p);
    out.min_price_ticks = detail::read_i64_le(p);
    out.max_price_ticks = detail::read_i64_le(p);
    out.tick_size_ticks = detail::read_i64_le(p);
    out.min_notional_ticks = detail::read_i64_le(p);
    out.rules_version = detail::read_u32_le(p);
    return true;
}

// durable_control_plane.hpp:531's own comment references "<= kMaxSymbols", a
// constant that is NOT defined in that header (only in the unrelated
// binance_json_parser.hpp/input_validator.hpp, at the same value) -- see
// docs/SPEC_INVARIANTS.md's "Phase 1" entry. Defined locally here rather than
// pulling in either of those unrelated files as a dependency.
inline constexpr std::size_t kMaxSnapshotSymbols = 64;

inline constexpr std::size_t kSnapshotPayloadFixedWireSize =
    8 +  // timestamp_ms
    4 +  // rules_version
    4;   // symbol_count
static_assert(kSnapshotPayloadFixedWireSize == 16);

inline constexpr std::size_t kMaxSnapshotFrameSize =
    kFrameEnvelopeOverhead + kSnapshotPayloadFixedWireSize + kMaxSnapshotSymbols * kSymbolRulesWireSize;
static_assert(kMaxSnapshotFrameSize == 91 + 16 + 64 * 81);

// Variable-length encode: entries.size() must equal payload.symbol_count and
// must not exceed kMaxSnapshotSymbols -- both checked BEFORE any arithmetic
// that sizes a read/write, never after (entry counts derived from an
// untrusted frame header must be bounds-checked before being multiplied by
// kSymbolRulesWireSize to compute an offset/length). Returns 0 (no partial
// write) on any mechanical shape violation or if `out` is too small.
inline std::size_t encode_snapshot_frame(std::span<std::byte> out, std::uint32_t key_id,
                                          std::uint64_t sequence_number, FrameTimeKind time_kind,
                                          std::int64_t recorded_utc_ms, const SymbolRegistrySnapshotPayload& snap,
                                          std::span<const SymbolRules> entries,
                                          std::span<const std::byte, kMacLen> prev_mac,
                                          std::span<const std::byte> hmac_key) noexcept {
    if (entries.size() > kMaxSnapshotSymbols) return 0;
    if (entries.size() != snap.symbol_count) return 0;

    const std::size_t payload_len = kSnapshotPayloadFixedWireSize + entries.size() * kSymbolRulesWireSize;
    const std::size_t frame_len = kFrameEnvelopeOverhead + payload_len;
    if (out.size() < frame_len) return 0;

    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_control_plane_header(p, DurableRecordType::SymbolRegistrySnapshot, key_id, sequence_number,
                                        time_kind, recorded_utc_ms, static_cast<std::uint32_t>(payload_len));
    detail::write_i64_le(p, snap.timestamp_ms);
    detail::write_u32_le(p, snap.rules_version);
    detail::write_u32_le(p, snap.symbol_count);
    for (const auto& rule : entries) {
        encode_symbol_rules(std::span<std::byte, kSymbolRulesWireSize>(p, kSymbolRulesWireSize), rule);
        p += kSymbolRulesWireSize;
    }
    detail::write_bytes(p, prev_mac.data(), kMacLen);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return frame_len;
}

struct DecodedSnapshotFrame {
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    SymbolRegistrySnapshotPayload payload{};
    std::array<std::byte, kMacLen> prev_mac{};
    std::array<std::byte, kMacLen> mac{};
};

// out_entries: fixed-capacity caller-provided buffer (kMaxSnapshotSymbols
// slots, zero heap) -- mirrors recover_control_plane()'s own
// std::array<FreezeProbeAttemptPayload, 8> fixed-capacity-out-buffer idiom.
// out_entry_count is set to the decoded symbol_count on Ok.
inline FrameDecodeStatus decode_snapshot_frame(std::span<const std::byte> in, std::span<const std::byte> hmac_key,
                                                DecodedSnapshotFrame& out,
                                                std::span<SymbolRules, kMaxSnapshotSymbols> out_entries,
                                                std::size_t& out_entry_count,
                                                std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;
    out_entry_count = 0;
    detail::ControlPlaneFrameHeader hdr{};
    const auto hdr_status = detail::read_control_plane_header(in, hdr);
    if (hdr_status != FrameDecodeStatus::Ok) return hdr_status;
    if (hdr.record_type != DurableRecordType::SymbolRegistrySnapshot) return FrameDecodeStatus::MalformedEnum;

    // Bounds-check the untrusted payload_length BEFORE deriving an entry
    // count from it, and bounds-check that derived count BEFORE using it in
    // any multiplication that sizes a read -- never multiply-then-compare.
    if (hdr.payload_length < kSnapshotPayloadFixedWireSize) return FrameDecodeStatus::PayloadLengthInvalid;
    const std::size_t variable_bytes = hdr.payload_length - kSnapshotPayloadFixedWireSize;
    if (variable_bytes % kSymbolRulesWireSize != 0) return FrameDecodeStatus::PayloadLengthInvalid;
    const std::size_t entry_count = variable_bytes / kSymbolRulesWireSize;
    if (entry_count > kMaxSnapshotSymbols) return FrameDecodeStatus::PayloadLengthInvalid;

    const std::size_t frame_len = kFrameEnvelopeOverhead + static_cast<std::size_t>(hdr.payload_length);
    out_frame_size = frame_len;
    if (in.size() < frame_len) return FrameDecodeStatus::Truncated;

    const std::byte* const content_start = in.data();
    const std::byte* p = in.data() + detail::kControlPlaneHeaderSize;

    SymbolRegistrySnapshotPayload payload{};
    payload.timestamp_ms = detail::read_i64_le(p);
    payload.rules_version = detail::read_u32_le(p);
    payload.symbol_count = detail::read_u32_le(p);
    if (payload.symbol_count != entry_count) return FrameDecodeStatus::PayloadLengthInvalid;

    for (std::size_t i = 0; i < entry_count; ++i) {
        if (!decode_symbol_rules(std::span<const std::byte, kSymbolRulesWireSize>(p, kSymbolRulesWireSize),
                                  out_entries[i])) {
            return FrameDecodeStatus::MalformedEnum;
        }
        p += kSymbolRulesWireSize;
    }

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);
    if (std::memcmp(expected_mac.bytes.data(), mac.data(), kMacLen) != 0) return FrameDecodeStatus::ChecksumMismatch;

    out.key_id = hdr.key_id;
    out.sequence_number = hdr.sequence_number;
    out.time_kind = hdr.time_kind;
    out.recorded_utc_ms = hdr.recorded_utc_ms;
    out.payload = payload;
    out.prev_mac = prev_mac;
    out.mac = mac;
    out_entry_count = entry_count;
    return FrameDecodeStatus::Ok;
}

}  // namespace hy
