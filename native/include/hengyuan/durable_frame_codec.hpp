// SPDX-License-Identifier: proprietary
// durable_frame_codec.hpp — pure encode/decode for one durable-log frame.
//
// Governance: L1 (pure computation, no file I/O). This file never opens a
// file or touches a filesystem -- durable_audit_sink.hpp is where the bytes
// this file produces/consumes actually get written/read from disk. Kept
// separate so the wire format itself is independently round-trip-testable
// without any real-file machinery (test_durable_frame_codec.cpp).
//
// FRAME LAYOUT (little-endian, every field written explicitly, never a raw
// struct memcpy -- padding between C++ struct members would otherwise leak
// into the wire format, per SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md's own
// stated rule):
//
//   [format_version: u8 = 4][record_type: u8][key_id: u32][sequence_number: u64]
//   [time_kind: u8][recorded_utc_ms: i64][payload_length: u32]
//   [payload: payload_length bytes -- an encoded AuditRecord, always exactly
//    kAuditRecordWireSize bytes for DurableRecordType::OrderEvent, the only
//    record type this round ever writes]
//   [prev_mac: 32 bytes -- the previous frame's own mac; all-zero at
//    sequence_number == 0]
//   [mac: 32 bytes = HMAC-SHA256(everything above, INCLUDING prev_mac)]
//
// Fixed per-frame overhead (everything except the payload):
// 1+1+4+8+1+8+4+32+32 = 91 bytes. payload_length is a spec-unpinned width
// choice (u32) -- flagged as such, not derived from spec text byte-for-byte;
// AuditRecord is far under 4 GiB. See docs/SPEC_INVARIANTS.md's
// durable-audit-log entry for the full set of this round's scope decisions
// (only OrderEvent frames, no compaction/external-anchor networking, etc).
//
// FORMAT v3 -> v4 (Phase 0, 轨道 key-rotation substrate): v3 had no key_id
// field at all -- callers passed a single hmac_key span with zero key-
// selection logic, which made real key rotation (SUBMITPORT_REAL_
// IMPLEMENTATION_SPEC.md:1225, §6.1.1.2) structurally impossible to verify
// correctly on decode. v4 inserts key_id right after record_type (an
// identity field belongs with the other identity fields at the front of the
// header, same placement philosophy as the tip-anchor's own key_id field in
// durable_audit_sink.hpp). This is a real wire-format break, not an additive
// change -- deliberately so: v3 landed one day before this decision with
// zero real deployed data anywhere in this repo's history (confirmed via
// `git log`), so the migration cost of the break is genuinely zero, same
// reasoning this file's original scope note already used to justify skipping
// v1/v2 legacy decode support entirely.

#pragma once

#include <hengyuan/audit_trail.hpp>
#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/sha256.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace hy {

inline constexpr std::uint8_t kFrameFormatVersion = 4;
inline constexpr std::size_t kMacLen = 32;

// --- AuditRecord wire encoding (the OrderEvent payload) ---
//
// Field order matches AuditRecord's declaration order (audit_trail.hpp).
// client_order_id/detail_msg are written as their full fixed-size buffers
// verbatim (including trailing zero bytes past the null terminator) -- every
// AuditRecord in this codebase is freshly zero-initialized before being
// filled (`AuditRecord ar{};`), so those trailing bytes are always zero, not
// leaked prior contents.
inline constexpr std::size_t kAuditRecordWireSize =
    8 +                              // timestamp_ms
    1 +                              // event_type
    1 +                              // mode
    4 +                              // symbol_id
    (kClientOrderIdLen + 1) +        // client_order_id (37 bytes)
    8 +                              // exchange_order_id
    8 +                              // price_ticks
    8 +                              // qty_ticks
    8 +                              // detail_code
    64 +                             // detail_msg
    1 +                              // resulting_state
    8 +                              // filled_qty_ticks
    8;                               // avg_fill_price_ticks

namespace detail {

inline void write_u8(std::byte*& p, std::uint8_t v) noexcept {
    *p = static_cast<std::byte>(v);
    ++p;
}

inline void write_u32_le(std::byte*& p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) {
        *p = static_cast<std::byte>(v >> (8 * i));
        ++p;
    }
}

inline void write_u64_le(std::byte*& p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        *p = static_cast<std::byte>(v >> (8 * i));
        ++p;
    }
}

inline void write_i64_le(std::byte*& p, std::int64_t v) noexcept {
    write_u64_le(p, static_cast<std::uint64_t>(v));
}

inline void write_bytes(std::byte*& p, const void* src, std::size_t n) noexcept {
    std::memcpy(p, src, n);
    p += n;
}

inline std::uint8_t read_u8(const std::byte*& p) noexcept {
    auto v = static_cast<std::uint8_t>(*p);
    ++p;
    return v;
}

inline std::uint32_t read_u32_le(const std::byte*& p) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
    }
    p += 4;
    return v;
}

inline std::uint64_t read_u64_le(const std::byte*& p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
    }
    p += 8;
    return v;
}

inline std::int64_t read_i64_le(const std::byte*& p) noexcept {
    return static_cast<std::int64_t>(read_u64_le(p));
}

inline void read_bytes(const std::byte*& p, void* dst, std::size_t n) noexcept {
    std::memcpy(dst, p, n);
    p += n;
}

inline bool is_legal_audit_event_type(std::uint8_t v) noexcept {
    return v <= static_cast<std::uint8_t>(AuditEventType::OrderSubmitPrepared);
}

inline bool is_legal_execution_mode(std::uint8_t v) noexcept {
    return v <= static_cast<std::uint8_t>(ExecutionMode::Live);
}

inline bool is_legal_order_state(std::uint8_t v) noexcept {
    return v <= static_cast<std::uint8_t>(OrderState::EscalatedToOperator);
}

inline bool is_legal_durable_record_type(std::uint8_t v) noexcept {
    return v <= static_cast<std::uint8_t>(DurableRecordType::SealJournalApplied);
}

inline bool is_legal_frame_time_kind(std::uint8_t v) noexcept {
    return v <= static_cast<std::uint8_t>(FrameTimeKind::UnknownBootstrap);
}

}  // namespace detail

// Writes exactly kAuditRecordWireSize bytes to `out` (caller-sized). Never
// fails -- AuditRecord's fields are all fixed-width, there is no encoding
// error state for this specific struct.
inline void encode_audit_record(std::span<std::byte, kAuditRecordWireSize> out,
                                 const AuditRecord& rec) noexcept {
    std::byte* p = out.data();
    detail::write_i64_le(p, rec.timestamp_ms);
    detail::write_u8(p, static_cast<std::uint8_t>(rec.event_type));
    detail::write_u8(p, static_cast<std::uint8_t>(rec.mode));
    detail::write_u32_le(p, rec.symbol_id);
    detail::write_bytes(p, rec.client_order_id, kClientOrderIdLen + 1);
    detail::write_i64_le(p, rec.exchange_order_id);
    detail::write_i64_le(p, rec.price_ticks);
    detail::write_i64_le(p, rec.qty_ticks);
    detail::write_i64_le(p, rec.detail_code);
    detail::write_bytes(p, rec.detail_msg, sizeof(rec.detail_msg));
    detail::write_u8(p, static_cast<std::uint8_t>(rec.resulting_state));
    detail::write_i64_le(p, rec.filled_qty_ticks);
    detail::write_i64_le(p, rec.avg_fill_price_ticks);
}

// Decodes an AuditRecord from exactly kAuditRecordWireSize bytes. Returns
// false (leaving `out` in an unspecified state) if any enum field is out of
// its legal range -- never trusts the byte to already be a valid enumerator,
// matching this codebase's decode-time range-check discipline throughout.
inline bool decode_audit_record(std::span<const std::byte, kAuditRecordWireSize> in,
                                 AuditRecord& out) noexcept {
    const std::byte* p = in.data();
    out.timestamp_ms = detail::read_i64_le(p);

    const std::uint8_t event_type_raw = detail::read_u8(p);
    if (!detail::is_legal_audit_event_type(event_type_raw)) return false;
    out.event_type = static_cast<AuditEventType>(event_type_raw);

    const std::uint8_t mode_raw = detail::read_u8(p);
    if (!detail::is_legal_execution_mode(mode_raw)) return false;
    out.mode = static_cast<ExecutionMode>(mode_raw);

    out.symbol_id = detail::read_u32_le(p);
    detail::read_bytes(p, out.client_order_id, kClientOrderIdLen + 1);
    // Never trust the wire to have actually null-terminated within bounds --
    // a corrupt (but checksum-matching only by coincidence in a fuzz/attack
    // scenario) record must not be treated as a valid C string later.
    out.client_order_id[kClientOrderIdLen] = '\0';

    out.exchange_order_id = detail::read_i64_le(p);
    out.price_ticks = detail::read_i64_le(p);
    out.qty_ticks = detail::read_i64_le(p);
    out.detail_code = detail::read_i64_le(p);
    detail::read_bytes(p, out.detail_msg, sizeof(out.detail_msg));
    out.detail_msg[sizeof(out.detail_msg) - 1] = '\0';

    const std::uint8_t resulting_state_raw = detail::read_u8(p);
    if (!detail::is_legal_order_state(resulting_state_raw)) return false;
    out.resulting_state = static_cast<OrderState>(resulting_state_raw);

    out.filled_qty_ticks = detail::read_i64_le(p);
    out.avg_fill_price_ticks = detail::read_i64_le(p);
    return true;
}

// --- Full frame encode/decode ---

// Fixed per-frame size for an OrderEvent frame carrying one AuditRecord --
// every frame this round ever writes is exactly this many bytes (no
// variable-length payloads exist for this record type).
inline constexpr std::size_t kOrderEventFrameSize =
    1 + 1 + 4 + 8 + 1 + 8 + 4 +      // format_version..payload_length (incl. key_id)
    kAuditRecordWireSize +
    kMacLen +                        // prev_mac
    kMacLen;                         // mac

// Encodes one DurableRecordType::OrderEvent frame into `out`. Returns
// kOrderEventFrameSize on success, 0 if `out` is smaller than that. `key_id`
// identifies which KeyRing-managed key `hmac_key` actually is -- the caller
// must have already resolved `hmac_key` from a KeyRing lookup for this
// exact `key_id` (key_ring.hpp); this function has no key-selection logic
// of its own, it only writes the identifier into the frame header.
inline std::size_t encode_order_event_frame(
    std::span<std::byte> out,
    std::uint32_t key_id,
    std::uint64_t sequence_number,
    FrameTimeKind time_kind,
    std::int64_t recorded_utc_ms,
    const AuditRecord& record,
    std::span<const std::byte, kMacLen> prev_mac,
    std::span<const std::byte> hmac_key) noexcept {
    if (out.size() < kOrderEventFrameSize) return 0;

    std::byte* p = out.data();
    const std::byte* const content_start = p;

    detail::write_u8(p, kFrameFormatVersion);
    detail::write_u8(p, static_cast<std::uint8_t>(DurableRecordType::OrderEvent));
    detail::write_u32_le(p, key_id);
    detail::write_u64_le(p, sequence_number);
    detail::write_u8(p, static_cast<std::uint8_t>(time_kind));
    detail::write_i64_le(p, recorded_utc_ms);
    detail::write_u32_le(p, static_cast<std::uint32_t>(kAuditRecordWireSize));

    encode_audit_record(std::span<std::byte, kAuditRecordWireSize>(p, kAuditRecordWireSize), record);
    p += kAuditRecordWireSize;

    detail::write_bytes(p, prev_mac.data(), kMacLen);

    // MAC covers everything written so far, INCLUDING prev_mac -- this is
    // what makes the log a hash CHAIN, not just individually-checksummed
    // frames: tampering with (or replaying) an earlier frame changes its mac,
    // which every later frame's mac transitively depends on.
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(
        hmac_key, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);

    return kOrderEventFrameSize;
}

enum class FrameDecodeStatus : std::uint8_t {
    Ok = 0,
    Truncated = 1,             // fewer bytes available than the frame needs -- only a
                                // legal outcome at true EOF (torn tail write)
    UnknownVersion = 2,        // format_version != kFrameFormatVersion
    MalformedEnum = 3,         // record_type/time_kind/event_type/mode/resulting_state
                                // out of their legal range
    PayloadLengthInvalid = 4,  // payload_length != kAuditRecordWireSize
    ChecksumMismatch = 5,      // physically complete, well-formed frame whose mac
                                // doesn't match its own content -- Corrupt regardless
                                // of position in the log, never treated like Truncated
};

struct DecodedOrderFrame {
    DurableRecordType record_type{DurableRecordType::OrderEvent};
    std::uint32_t key_id{0};
    std::uint64_t sequence_number{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};
    AuditRecord record{};
    std::array<std::byte, kMacLen> prev_mac{};  // as read from the wire, NOT validated
                                                 // against any expectation here -- chaining
                                                 // across frames is the scanning caller's job
                                                 // (recovery_scan(), durable_audit_sink.hpp)
    std::array<std::byte, kMacLen> mac{};        // this frame's own mac, already verified
                                                  // against its content by the time Ok is returned
};

// Reads ONLY the key_id field from a frame's header, without validating
// format_version/record_type/checksum or anything else -- callers use this
// to select the correct hmac_key from a KeyRing (key_ring.hpp) BEFORE
// calling decode_order_event_frame, which is what actually verifies the
// frame. Returns false (out_key_id untouched) if `in` is too short to even
// contain the key_id field; this is deliberately not a full decode and
// returning false here does NOT imply Corrupt -- the caller falls through
// to decode_order_event_frame for the real Truncated/UnknownVersion/etc.
// classification.
inline bool peek_frame_key_id(std::span<const std::byte> in, std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kKeyIdOffset = 1 + 1;  // format_version, record_type
    constexpr std::size_t kMinBytes = kKeyIdOffset + 4;
    if (in.size() < kMinBytes) return false;
    const std::byte* p = in.data() + kKeyIdOffset;
    out_key_id = detail::read_u32_le(p);
    return true;
}

// Decodes one frame starting at the beginning of `in`. `in` is expected to be
// "all bytes available from the current read offset through EOF" -- the
// caller (recovery_scan()) is what turns Truncated into "stop scanning,
// that's the torn tail" vs a real corruption. `hmac_key` MUST already be the
// key resolved for this frame's own key_id (via peek_frame_key_id() + a
// KeyRing lookup) -- this function does not resolve keys itself.
//
// out_frame_size is set to kOrderEventFrameSize whenever the frame's own
// length fields were read successfully (i.e. on Ok and on every failure
// EXCEPT Truncated, since Truncated means `in` didn't even contain that much
// to begin with) -- diagnostic value for a caller that wants to know how big
// the frame it rejected was.
inline FrameDecodeStatus decode_order_event_frame(std::span<const std::byte> in,
                                                    std::span<const std::byte> hmac_key,
                                                    DecodedOrderFrame& out,
                                                    std::size_t& out_frame_size) noexcept {
    out_frame_size = 0;

    // Enough to read format_version..payload_length (the fixed header before
    // the payload) -- 27 bytes (includes the 4-byte key_id field, v4).
    constexpr std::size_t kHeaderSize = 1 + 1 + 4 + 8 + 1 + 8 + 4;
    if (in.size() < kHeaderSize) return FrameDecodeStatus::Truncated;

    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    const std::uint8_t version = detail::read_u8(p);
    if (version != kFrameFormatVersion) return FrameDecodeStatus::UnknownVersion;

    const std::uint8_t record_type_raw = detail::read_u8(p);
    if (!detail::is_legal_durable_record_type(record_type_raw)) return FrameDecodeStatus::MalformedEnum;

    const std::uint32_t key_id = detail::read_u32_le(p);

    const std::uint64_t sequence_number = detail::read_u64_le(p);

    const std::uint8_t time_kind_raw = detail::read_u8(p);
    if (!detail::is_legal_frame_time_kind(time_kind_raw)) return FrameDecodeStatus::MalformedEnum;

    const std::int64_t recorded_utc_ms = detail::read_i64_le(p);
    const std::uint32_t payload_length = detail::read_u32_le(p);

    if (payload_length != kAuditRecordWireSize) return FrameDecodeStatus::PayloadLengthInvalid;

    out_frame_size = kOrderEventFrameSize;
    if (in.size() < kOrderEventFrameSize) return FrameDecodeStatus::Truncated;

    AuditRecord record{};
    if (!decode_audit_record(std::span<const std::byte, kAuditRecordWireSize>(p, kAuditRecordWireSize),
                              record)) {
        return FrameDecodeStatus::MalformedEnum;
    }
    p += kAuditRecordWireSize;

    std::array<std::byte, kMacLen> prev_mac{};
    detail::read_bytes(p, prev_mac.data(), kMacLen);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac =
        crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);

    // Constant-time, not std::memcmp (audit SEC-MACCMP-010): memcmp returns at the
    // first differing byte, which turns tag verification into a byte-at-a-time
    // forgery oracle for anyone who can supply candidate frames and time the
    // recovery scan.
    if (!crypto::constant_time_equal(expected_mac, mac)) {
        return FrameDecodeStatus::ChecksumMismatch;
    }

    out.record_type = static_cast<DurableRecordType>(record_type_raw);
    out.key_id = key_id;
    out.sequence_number = sequence_number;
    out.time_kind = static_cast<FrameTimeKind>(time_kind_raw);
    out.recorded_utc_ms = recorded_utc_ms;
    out.record = record;
    out.prev_mac = prev_mac;
    out.mac = mac;
    return FrameDecodeStatus::Ok;
}

}  // namespace hy
