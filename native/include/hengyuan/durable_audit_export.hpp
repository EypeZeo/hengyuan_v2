// SPDX-License-Identifier: proprietary
// durable_audit_export.hpp — 批次 6 6c: OFFLINE export of a durable audit log (the HMAC-chained OrderEvent
// frames DurableAuditSink writes) to newline-delimited JSON, so the order/fill audit trail can be read by
// something other than the process that wrote it (py_core, an operator, a review).
//
// WHAT IT IS. A read-only verifier + formatter. It reads the log's bytes, walks every frame the way
// DurableAuditSink::run_recovery_scan() does -- peek the frame's key id, look the key up, decode (which checks the
// frame's HMAC), require the sequence number to be exactly the next one and prev_mac to be the previous frame's
// mac -- and then cross-checks the tip anchor (proves the tail was not cut off) and the store-identity sidecar. Only
// if ALL of that holds does it write anything: the export is verify-then-write, so a log that fails verification
// yields NO output file, not a partial one.
//
// THE TWO KINDS OF "DAMAGED", KEPT APART (the reason this file classifies instead of returning a bool):
//   * a TORN TAIL -- the last frame is an incomplete prefix, which is what a crash in the middle of an append
//     leaves behind, and which the writer's own recovery scan also discards. It is REPORTED (manifest
//     `torn_tail_bytes`, and the caller warns) and everything before it is exported. The tip anchor must still agree
//     with the last complete frame.
//   * everything else -- a complete frame whose MAC does not match, a key id the ring does not hold, an unknown
//     version or malformed field, a sequence gap, a broken prev_mac chain, a frame of a record type this log never
//     holds, a tip anchor that is ahead of the log (a cut-off tail: rollback), one that does not match, a store
//     identity that does not verify. Each is a HARD error with its own status and the position it was found at.
//     Nothing is exported, and the CLI exits non-zero.
//
// OFFLINE ONLY. The writer holds an exclusive lock for its lifetime; export_audit_log_file() refuses to run while
// it is held (or when that cannot be determined). The source directory is only ever read: the log, the tip
// anchor and the store-identity file are opened for reading, and the lock file is only probed, never created.
//
// WHAT IT IS NOT. Not a PnL ledger: an AuditRecord has no trade id, no fee, no maker/taker flag, and
// position_truth.hpp records no average entry price or realized PnL. This is the order/fill audit TRAIL. It does not
// re-validate order-state transitions either (the recovery scan does); a log whose chain verifies but whose states
// are illegal is exported as it is and the consumer can see it. And it defines no key ceremony: the ring comes from
// the caller. load_audit_key_file() below is only this tool's INPUT contract for the wrapped keys.
//
// OUTPUT. Line 1 is a manifest object; every further line is one frame. `"type"` tells them apart. The output file
// is created with CREATE_NEW semantics (an existing file is an error, never overwritten) and 0600 where the platform
// has that notion.

#pragma once

#include <hengyuan/durable_audit_sink.hpp>  // tip anchor / store identity decoders, DurableAuditSink's file naming
#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/key_ring.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hy {

inline constexpr std::string_view kAuditExportFormat = "hy-audit-export/1";

enum class ExportStatus : std::uint8_t {
    Ok = 0,                 // verified and exported (a torn tail, if any, is in the verification, not an error)
    IoError = 1,            // could not read the source, or an allocation failed
    LogTooLarge = 2,        // over kMaxDurableLogBytes
    WriterLive = 3,         // the writer's lock is held: not an offline log
    WriterUnknown = 4,      // the lock cannot be probed: cannot show the writer is gone
    UnknownKeyId = 5,       // a frame names a key the ring does not hold
    UnknownVersion = 6,     // a frame with another format version
    MalformedFrame = 7,     // an enum out of range, or a payload length that is not an OrderEvent's
    ChecksumMismatch = 8,   // a complete frame whose HMAC does not match its content
    SequenceGap = 9,        // a frame whose sequence number is not the next one
    ChainBroken = 10,       // a frame whose prev_mac is not the previous frame's mac
    UnexpectedRecordType = 11,  // an intact frame of a record type this log never holds
    AnchorMissing = 12,     // a non-empty log without its tip anchor (and that was not allowed)
    AnchorInvalid = 13,     // the anchor is malformed, or its key is unknown, or its MAC fails
    AnchorAheadOfLog = 14,  // the anchor records a tail the log no longer has: rollback / tail deletion
    AnchorMismatch = 15,    // the anchor's mac is not that of the log's frame at its sequence number
    StoreIdentityInvalid = 16,  // the identity sidecar exists but does not verify
    OutputExists = 17,      // the output path already exists (CREATE_NEW)
    OutputError = 18,       // could not create or write the output
};

inline constexpr const char* export_status_name(ExportStatus s) noexcept {
    switch (s) {
        case ExportStatus::Ok: return "Ok";
        case ExportStatus::IoError: return "IoError";
        case ExportStatus::LogTooLarge: return "LogTooLarge";
        case ExportStatus::WriterLive: return "WriterLive";
        case ExportStatus::WriterUnknown: return "WriterUnknown";
        case ExportStatus::UnknownKeyId: return "UnknownKeyId";
        case ExportStatus::UnknownVersion: return "UnknownVersion";
        case ExportStatus::MalformedFrame: return "MalformedFrame";
        case ExportStatus::ChecksumMismatch: return "ChecksumMismatch";
        case ExportStatus::SequenceGap: return "SequenceGap";
        case ExportStatus::ChainBroken: return "ChainBroken";
        case ExportStatus::UnexpectedRecordType: return "UnexpectedRecordType";
        case ExportStatus::AnchorMissing: return "AnchorMissing";
        case ExportStatus::AnchorInvalid: return "AnchorInvalid";
        case ExportStatus::AnchorAheadOfLog: return "AnchorAheadOfLog";
        case ExportStatus::AnchorMismatch: return "AnchorMismatch";
        case ExportStatus::StoreIdentityInvalid: return "StoreIdentityInvalid";
        case ExportStatus::OutputExists: return "OutputExists";
        case ExportStatus::OutputError: return "OutputError";
    }
    return "?";
}

enum class AnchorState : std::uint8_t {
    NotChecked = 0,
    Verified = 1,        // present, verified, and consistent with the log
    AbsentAllowed = 2,   // no anchor, and the caller said that is acceptable (the export cannot show no rollback)
    EmptyLog = 3,        // nothing to anchor
};

enum class IdentityState : std::uint8_t {
    Absent = 0,    // no store-identity file (the writer treats that as a degraded, non-fatal state)
    Verified = 1,
};

struct ExportOptions {
    // A log without a tip anchor cannot be shown to have kept its tail. The default refuses; setting this exports
    // it anyway and says so in the manifest (`"tip_anchor":"absent-allowed"`).
    bool allow_missing_anchor{false};
};

// What verify_audit_log() found. status == Ok means every complete frame verified (and the anchor and identity
// checks passed); torn_tail_bytes may still be non-zero. Otherwise error_offset/error_sequence say where.
struct AuditLogVerification {
    ExportStatus status{ExportStatus::Ok};
    std::uint64_t frames{0};                        // complete, verified frames
    std::size_t verified_bytes{0};                  // the log bytes those frames occupy
    std::size_t torn_tail_bytes{0};                 // an incomplete last frame's bytes (0 = none)
    std::size_t error_offset{0};                    // byte offset of the frame that failed (status != Ok)
    std::uint64_t error_sequence{0};                // the sequence number that frame was expected to carry
    std::array<std::byte, kMacLen> tail_mac{};      // the last verified frame's mac (valid when frames > 0)
    std::array<std::uint32_t, kMaxLiveKeys> key_ids{};  // distinct key ids seen, in first-appearance order
    std::size_t key_id_count{0};
    std::size_t log_bytes{0};
    AnchorState anchor{AnchorState::NotChecked};
    IdentityState identity{IdentityState::Absent};
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};

    bool ok() const noexcept { return status == ExportStatus::Ok; }
};

namespace detail {

inline void export_hex_append(std::string& out, std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (const std::byte b : bytes) {
        const auto v = static_cast<unsigned>(b);
        out.push_back(kDigits[(v >> 4) & 0xFu]);
        out.push_back(kDigits[v & 0xFu]);
    }
}

inline void export_hex16_append(std::string& out, std::uint64_t v) {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) out.push_back(kDigits[(v >> (4 * i)) & 0xFu]);
}

// A JSON string literal for arbitrary bytes: ASCII printable as is, `"` and `\` escaped, every other byte as
// \u00XX (so the line is valid JSON whatever the field held -- a NUL-padded buffer, a byte >= 0x80).
inline void export_json_string(std::string& out, const char* data, std::size_t len) {
    static constexpr char kDigits[] = "0123456789abcdef";
    out.push_back('"');
    for (std::size_t i = 0; i < len; ++i) {
        const auto c = static_cast<unsigned char>(data[i]);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        } else {
            out += "\\u00";
            out.push_back(kDigits[c >> 4]);
            out.push_back(kDigits[c & 0xFu]);
        }
    }
    out.push_back('"');
}

// The field is a NUL-terminated string inside a fixed buffer of `cap` bytes (AuditRecord's client_order_id /
// detail_msg): find that terminator ourselves rather than reach for a POSIX-only strnlen.
inline void export_json_cstr(std::string& out, const char* buf, std::size_t cap) {
    std::size_t len = 0;
    while (len < cap && buf[len] != '\0') ++len;
    export_json_string(out, buf, len);
}

inline const char* export_mode_word(ExecutionMode m) noexcept {
    return m == ExecutionMode::Live ? "LIVE" : "DRY_RUN";
}

inline const char* export_time_kind_word(FrameTimeKind k) noexcept {
    return k == FrameTimeKind::ServerCorrectedUtc ? "SERVER_CORRECTED_UTC" : "UNKNOWN_BOOTSTRAP";
}

inline void export_note_key_id(AuditLogVerification& v, std::uint32_t key_id) noexcept {
    for (std::size_t i = 0; i < v.key_id_count; ++i) {
        if (v.key_ids[i] == key_id) return;
    }
    if (v.key_id_count < v.key_ids.size()) v.key_ids[v.key_id_count++] = key_id;
}

inline ExportStatus export_status_of(FrameDecodeStatus s) noexcept {
    switch (s) {
        case FrameDecodeStatus::Ok: return ExportStatus::Ok;
        case FrameDecodeStatus::Truncated: return ExportStatus::Ok;  // handled by the caller: a torn tail
        case FrameDecodeStatus::UnknownVersion: return ExportStatus::UnknownVersion;
        case FrameDecodeStatus::MalformedEnum: return ExportStatus::MalformedFrame;
        case FrameDecodeStatus::PayloadLengthInvalid: return ExportStatus::MalformedFrame;
        case FrameDecodeStatus::ChecksumMismatch: return ExportStatus::ChecksumMismatch;
    }
    return ExportStatus::MalformedFrame;
}

}  // namespace detail

// Verifies `log` (the whole file), optionally against its tip anchor and store-identity file. Pure: no I/O.
// Never throws for a bad log (only for an allocation failure, which the file-level caller maps to IoError).
inline AuditLogVerification verify_audit_log(std::span<const std::byte> log, const KeyRing& ring,
                                             bool anchor_present, std::span<const std::byte> anchor_bytes,
                                             bool identity_present, std::span<const std::byte> identity_bytes,
                                             const ExportOptions& opts = ExportOptions{}) {
    AuditLogVerification v;
    v.log_bytes = log.size();

    // The anchor first: its sequence number tells the scan which frame's mac to remember.
    bool have_anchor = false;
    detail::DecodedTipAnchor anchor{};
    if (anchor_present) {
        std::uint32_t anchor_key_id = 0;
        std::array<std::byte, kKeyBlockSize> anchor_key{};
        if (!detail::peek_tip_anchor_key_id(anchor_bytes, anchor_key_id) || !ring.active_key(anchor_key_id, anchor_key) ||
            !detail::decode_tip_anchor(anchor_bytes, std::span<const std::byte>(anchor_key.data(), anchor_key.size()),
                                       anchor)) {
            v.status = ExportStatus::AnchorInvalid;
            return v;
        }
        have_anchor = true;
    }

    std::size_t offset = 0;
    std::uint64_t expected_seq = 0;
    std::array<std::byte, kMacLen> running_prev_mac{};  // zero at sequence 0
    std::array<std::byte, kMacLen> mac_at_anchor{};
    bool saw_anchor_seq = false;

    while (offset < log.size()) {
        const std::span<const std::byte> remaining = log.subspan(offset);

        // Resolve THIS frame's key. A peek that cannot even read the key id is not corruption: too few bytes is a
        // torn tail, and decode_order_event_frame's own length check classifies it as Truncated.
        std::uint32_t frame_key_id = 0;
        std::array<std::byte, kKeyBlockSize> key_block{};
        if (peek_frame_key_id(remaining, frame_key_id)) {
            if (!ring.active_key(frame_key_id, key_block)) {
                v.status = ExportStatus::UnknownKeyId;
                v.error_offset = offset;
                v.error_sequence = expected_seq;
                return v;
            }
        }

        DecodedOrderFrame frame{};
        std::size_t frame_size = 0;
        const FrameDecodeStatus st = decode_order_event_frame(
            remaining, std::span<const std::byte>(key_block.data(), key_block.size()), frame, frame_size);
        if (st == FrameDecodeStatus::Truncated) {
            v.torn_tail_bytes = remaining.size();  // only legal at the physical end: what a crashed append leaves
            break;
        }
        if (st != FrameDecodeStatus::Ok) {
            v.status = detail::export_status_of(st);
            v.error_offset = offset;
            v.error_sequence = expected_seq;
            return v;
        }
        if (frame.sequence_number != expected_seq) {
            v.status = ExportStatus::SequenceGap;
            v.error_offset = offset;
            v.error_sequence = expected_seq;
            return v;
        }
        if (frame.prev_mac != running_prev_mac) {
            v.status = ExportStatus::ChainBroken;
            v.error_offset = offset;
            v.error_sequence = expected_seq;
            return v;
        }
        if (frame.record_type != DurableRecordType::OrderEvent) {
            v.status = ExportStatus::UnexpectedRecordType;
            v.error_offset = offset;
            v.error_sequence = expected_seq;
            return v;
        }
        detail::export_note_key_id(v, frame.key_id);
        if (have_anchor && frame.sequence_number == anchor.sequence_number) {
            mac_at_anchor = frame.mac;
            saw_anchor_seq = true;
        }
        running_prev_mac = frame.mac;
        ++expected_seq;
        offset += frame_size;
    }
    v.frames = expected_seq;
    v.verified_bytes = offset;
    v.tail_mac = running_prev_mac;

    // The tip anchor: proof that the tail was not cut off (and that the frames it vouches for are the ones present).
    if (v.frames == 0) {
        if (have_anchor) {  // the anchor claims a tip; the log has none
            v.status = ExportStatus::AnchorAheadOfLog;
            return v;
        }
        v.anchor = AnchorState::EmptyLog;
    } else if (!have_anchor) {
        if (!opts.allow_missing_anchor) {
            v.status = ExportStatus::AnchorMissing;
            return v;
        }
        v.anchor = AnchorState::AbsentAllowed;
    } else {
        const std::uint64_t tip_seq = v.frames - 1;
        if (anchor.sequence_number > tip_seq) {
            v.status = ExportStatus::AnchorAheadOfLog;
            return v;
        }
        // At the tip the two macs must agree; behind it (the late-fsync window: crashed between "fsync log" and
        // "fsync anchor") the log's own frame at that sequence -- already proven part of the chain -- must carry
        // the mac the anchor claims.
        if (!saw_anchor_seq || mac_at_anchor != anchor.mac) {
            v.status = ExportStatus::AnchorMismatch;
            return v;
        }
        v.anchor = AnchorState::Verified;
    }

    if (identity_present) {
        std::uint32_t identity_key_id = 0;
        std::array<std::byte, kKeyBlockSize> identity_key{};
        detail::DecodedStoreIdentity identity{};
        if (!detail::peek_store_identity_key_id(identity_bytes, identity_key_id) ||
            !ring.active_key(identity_key_id, identity_key) ||
            !detail::decode_store_identity(identity_bytes,
                                           std::span<const std::byte>(identity_key.data(), identity_key.size()),
                                           identity)) {
            v.status = ExportStatus::StoreIdentityInvalid;
            return v;
        }
        v.identity = IdentityState::Verified;
        v.store_uuid_lo = identity.store_uuid_lo;
        v.store_uuid_hi = identity.store_uuid_hi;
    }
    return v;
}

// The manifest line (with its newline) for a verified log.
inline std::string audit_export_manifest_line(const AuditLogVerification& v) {
    std::string s;
    s.reserve(512);
    s += "{\"type\":\"manifest\",\"format\":";
    detail::export_json_string(s, kAuditExportFormat.data(), kAuditExportFormat.size());
    s += ",\"frame_format_version\":";
    s += std::to_string(static_cast<unsigned>(kFrameFormatVersion));
    s += ",\"frames\":";
    s += std::to_string(v.frames);
    if (v.frames > 0) {
        s += ",\"first_seq\":0,\"tail_seq\":";
        s += std::to_string(v.frames - 1);
        s += ",\"tail_mac\":\"";
        detail::export_hex_append(s, v.tail_mac);
        s += "\"";
    } else {
        s += ",\"first_seq\":null,\"tail_seq\":null,\"tail_mac\":null";
    }
    s += ",\"key_ids\":[";
    for (std::size_t i = 0; i < v.key_id_count; ++i) {
        if (i != 0) s += ',';
        s += std::to_string(v.key_ids[i]);
    }
    s += "],\"log_bytes\":";
    s += std::to_string(v.log_bytes);
    s += ",\"verified_bytes\":";
    s += std::to_string(v.verified_bytes);
    s += ",\"torn_tail_bytes\":";
    s += std::to_string(v.torn_tail_bytes);
    s += ",\"tip_anchor\":\"";
    switch (v.anchor) {
        case AnchorState::Verified: s += "verified"; break;
        case AnchorState::AbsentAllowed: s += "absent-allowed"; break;
        case AnchorState::EmptyLog: s += "empty-log"; break;
        case AnchorState::NotChecked: s += "not-checked"; break;
    }
    s += "\",\"store_uuid\":";
    if (v.identity == IdentityState::Verified) {
        s += '"';
        detail::export_hex16_append(s, v.store_uuid_lo);
        detail::export_hex16_append(s, v.store_uuid_hi);
        s += '"';
    } else {
        s += "null";
    }
    s += "}\n";
    return s;
}

// One frame's line (with its newline). Stable field order; every string escaped.
inline std::string audit_export_frame_line(const DecodedOrderFrame& f) {
    const AuditRecord& r = f.record;
    std::string s;
    s.reserve(512);
    s += "{\"type\":\"frame\",\"seq\":";
    s += std::to_string(f.sequence_number);
    s += ",\"key_id\":";
    s += std::to_string(f.key_id);
    s += ",\"time_kind\":\"";
    s += detail::export_time_kind_word(f.time_kind);
    s += "\",\"recorded_utc_ms\":";
    s += std::to_string(f.recorded_utc_ms);
    s += ",\"timestamp_ms\":";
    s += std::to_string(r.timestamp_ms);
    s += ",\"event\":\"";
    s += audit_event_name(r.event_type);
    s += "\",\"mode\":\"";
    s += detail::export_mode_word(r.mode);
    s += "\",\"symbol_id\":";
    s += std::to_string(r.symbol_id);
    s += ",\"client_order_id\":";
    detail::export_json_cstr(s, r.client_order_id, sizeof(r.client_order_id));
    s += ",\"exchange_order_id\":";
    s += std::to_string(r.exchange_order_id);
    s += ",\"side\":\"";
    s += order_side_name(r.side);
    s += "\",\"price_ticks\":";
    s += std::to_string(r.price_ticks);
    s += ",\"qty_ticks\":";
    s += std::to_string(r.qty_ticks);
    s += ",\"resulting_state\":\"";
    s += order_state_name(r.resulting_state);
    s += "\",\"filled_qty_ticks\":";
    s += std::to_string(r.filled_qty_ticks);
    s += ",\"avg_fill_price_ticks\":";
    s += std::to_string(r.avg_fill_price_ticks);
    s += ",\"detail_code\":";
    s += std::to_string(r.detail_code);
    s += ",\"detail_msg\":";
    detail::export_json_cstr(s, r.detail_msg, sizeof(r.detail_msg));
    s += ",\"mac\":\"";
    detail::export_hex_append(s, f.mac);
    s += "\"}\n";
    return s;
}

// Writes the manifest and then every frame through `emit` (a callable `bool(std::string_view)`; false = stop).
// `verification` must be the ok() result of verify_audit_log() over the same `log` and `ring`: the frames are decoded
// (and their MACs verified) a second time here, and a frame that no longer decodes -- which would mean the bytes
// changed between the two passes -- is reported as false rather than written.
template <class Emit>
bool emit_audit_export(std::span<const std::byte> log, const KeyRing& ring, const AuditLogVerification& verification,
                       Emit&& emit) {
    if (!verification.ok()) return false;
    if (!emit(std::string_view(audit_export_manifest_line(verification)))) return false;
    std::size_t offset = 0;
    for (std::uint64_t seq = 0; seq < verification.frames; ++seq) {
        const std::span<const std::byte> remaining = log.subspan(offset);
        std::uint32_t key_id = 0;
        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!peek_frame_key_id(remaining, key_id) || !ring.active_key(key_id, key_block)) return false;
        DecodedOrderFrame frame{};
        std::size_t frame_size = 0;
        if (decode_order_event_frame(remaining, std::span<const std::byte>(key_block.data(), key_block.size()), frame,
                                     frame_size) != FrameDecodeStatus::Ok ||
            frame.sequence_number != seq) {
            return false;
        }
        if (!emit(std::string_view(audit_export_frame_line(frame)))) return false;
        offset += frame_size;
    }
    return true;
}

// --- the wrapped-key file: this tool's INPUT contract, not a key ceremony ---------------------------------------
//
// No production key ceremony exists in this codebase yet (every tool uses fixture keys; see startup_recovery.hpp),
// so there is no persisted key store to read. The export needs the keys the audit log was signed with; this is the
// small file format the tool takes them in. It carries only WRAPPED keys (never plaintext), each verified against
// the KEK by KeyRing::load_wrapped_key():
//
//   "HYKEYS01" (8 bytes) | count: u32 LE | count x { key_id: u32 LE | wrapped_key_blob: 64 bytes | tag: 32 bytes }

inline constexpr std::string_view kAuditKeyFileMagic = "HYKEYS01";
inline constexpr std::size_t kAuditKeyFileRecordSize = 4 + kKeyBlockSize + 32;

inline std::vector<std::byte> encode_audit_key_file(std::span<const WrappedKeyRecord> records) {
    std::vector<std::byte> out;
    out.reserve(kAuditKeyFileMagic.size() + 4 + records.size() * kAuditKeyFileRecordSize);
    for (const char c : kAuditKeyFileMagic) out.push_back(static_cast<std::byte>(c));
    auto put_u32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(v >> (8 * i)));
    };
    put_u32(static_cast<std::uint32_t>(records.size()));
    for (const WrappedKeyRecord& r : records) {
        put_u32(r.key_id);
        out.insert(out.end(), r.wrapped_key_blob.begin(), r.wrapped_key_blob.end());
        out.insert(out.end(), r.tag.begin(), r.tag.end());
    }
    return out;
}

enum class AuditKeyFileStatus : std::uint8_t {
    Ok = 0,
    BadMagic = 1,
    BadLength = 2,      // the file is not exactly what its own count says
    TooManyKeys = 3,    // more records than the ring can hold
    TagMismatch = 4,    // a record fails the KEK's tag check (wrong KEK, or a tampered record)
    DuplicateKeyId = 5,
};

inline AuditKeyFileStatus load_audit_key_file(std::span<const std::byte> bytes, KeyRing& ring) {
    constexpr std::size_t kHeader = 8 + 4;
    if (bytes.size() < kHeader) return AuditKeyFileStatus::BadLength;
    for (std::size_t i = 0; i < kAuditKeyFileMagic.size(); ++i) {
        if (static_cast<char>(bytes[i]) != kAuditKeyFileMagic[i]) return AuditKeyFileStatus::BadMagic;
    }
    const std::byte* p = bytes.data() + 8;
    const std::uint32_t count = detail::read_u32_le(p);
    if (count > kMaxLiveKeys) return AuditKeyFileStatus::TooManyKeys;
    if (bytes.size() != kHeader + static_cast<std::size_t>(count) * kAuditKeyFileRecordSize) {
        return AuditKeyFileStatus::BadLength;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        WrappedKeyRecord r{};
        r.key_id = detail::read_u32_le(p);
        detail::read_bytes(p, r.wrapped_key_blob.data(), r.wrapped_key_blob.size());
        detail::read_bytes(p, r.tag.data(), r.tag.size());
        switch (ring.load_wrapped_key(r)) {
            case KeyRingLoadStatus::Ok: break;
            case KeyRingLoadStatus::TagMismatch: return AuditKeyFileStatus::TagMismatch;
            case KeyRingLoadStatus::DuplicateKeyId: return AuditKeyFileStatus::DuplicateKeyId;
            case KeyRingLoadStatus::TableFull: return AuditKeyFileStatus::TooManyKeys;
        }
    }
    return AuditKeyFileStatus::Ok;
}

// --- files ---------------------------------------------------------------------------------------------------------------

enum class WriterProbe : std::uint8_t {
    NotHeld = 0,  // no lock file, or the lock is free: no live writer
    Held = 1,
    Unknown = 2,
};

// Is the writer's exclusive lock held? Only probes: the lock file is opened for reading and never created, so an
// offline directory is not touched. (DurableAuditSink holds `<log>.lock` exclusively for its whole life: flock on
// POSIX, a no-sharing handle on Windows.)
inline WriterProbe probe_writer_lock(const std::string& lock_path) noexcept {
#if defined(_WIN32)
    const HANDLE h = CreateFileA(lock_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return WriterProbe::NotHeld;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return WriterProbe::NotHeld;
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) return WriterProbe::Held;
    return WriterProbe::Unknown;
#else
    const int fd = ::open(lock_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? WriterProbe::NotHeld : WriterProbe::Unknown;
    WriterProbe result = WriterProbe::NotHeld;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        result = errno == EWOULDBLOCK ? WriterProbe::Held : WriterProbe::Unknown;
    }
    ::close(fd);  // releases a lock this probe took
    return result;
#endif
}

enum class ReadFileStatus : std::uint8_t { Ok = 0, NotFound = 1, TooLarge = 2, Error = 3 };

// Reads a whole file, read-only. `max_bytes` bounds it.
inline ReadFileStatus read_file_bounded(const std::string& path, std::vector<std::byte>& out, std::uint64_t max_bytes) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return ec ? ReadFileStatus::Error : ReadFileStatus::NotFound;
    std::ifstream in(path, std::ios::binary);
    if (!in) return ReadFileStatus::Error;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) return ReadFileStatus::Error;
    if (static_cast<std::uint64_t>(size) > max_bytes) return ReadFileStatus::TooLarge;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), size);
        if (in.gcount() != size) return ReadFileStatus::Error;
    }
    return ReadFileStatus::Ok;
}

// An output file created with CREATE_NEW semantics: an existing path is an error, never overwritten. 0600 on POSIX.
// The object OWNS what it created until commit() succeeds: any failure, and the destructor, remove the file, so a
// half-written export is never left behind looking like a result.
class ExclusiveOutputFile {
public:
    enum class OpenStatus : std::uint8_t { Ok = 0, Exists = 1, Error = 2 };

    ExclusiveOutputFile() = default;
    ExclusiveOutputFile(const ExclusiveOutputFile&) = delete;
    ExclusiveOutputFile& operator=(const ExclusiveOutputFile&) = delete;
    ~ExclusiveOutputFile() { abandon(); }

    OpenStatus create(const std::string& path) noexcept {
        path_ = path;
#if defined(_WIN32)
        handle_ = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            return (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) ? OpenStatus::Exists : OpenStatus::Error;
        }
#else
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd_ < 0) return errno == EEXIST ? OpenStatus::Exists : OpenStatus::Error;
#endif
        open_ = true;
        owns_ = true;
        return OpenStatus::Ok;
    }

    bool write(std::string_view bytes) noexcept {
        if (!open_) return false;
#if defined(_WIN32)
        while (!bytes.empty()) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(bytes.size() > 0x40000000u ? 0x40000000u : bytes.size());
            if (!WriteFile(handle_, bytes.data(), chunk, &written, nullptr) || written == 0) return false;
            bytes.remove_prefix(written);
        }
#else
        while (!bytes.empty()) {
            const ssize_t n = ::write(fd_, bytes.data(), bytes.size());
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
#endif
        return true;
    }

    // Flushes to disk and closes; on success the file is kept, on failure it is removed.
    bool commit() noexcept {
        if (!open_) return false;
#if defined(_WIN32)
        const bool ok = FlushFileBuffers(handle_) != 0;
#else
        const bool ok = ::fsync(fd_) == 0;
#endif
        close_handle();
        if (!ok) {
            remove_file();
            return false;
        }
        owns_ = false;
        return true;
    }

    // Closes and removes the file unless commit() already kept it.
    void abandon() noexcept {
        if (open_) close_handle();
        if (owns_) remove_file();
    }

private:
    void close_handle() noexcept {
#if defined(_WIN32)
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#else
        ::close(fd_);
        fd_ = -1;
#endif
        open_ = false;
    }
    void remove_file() noexcept {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        owns_ = false;
    }

    std::string path_;
    bool open_{false};
    bool owns_{false};
#if defined(_WIN32)
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int fd_{-1};
#endif
};

struct ExportFileResult {
    ExportStatus status{ExportStatus::Ok};
    AuditLogVerification verification{};
};

// The whole offline export of `log_path` to `out_path` (which must not exist), under `ring`'s keys. The tip anchor is
// `<log>.tip`, the store identity `<log>.storeid.tip`, the writer's lock `<log>.lock` (the names DurableAuditSink
// derives). Verify first, write only if everything holds; a failure after the output was created removes it.
inline ExportFileResult export_audit_log_file(const std::string& log_path, const std::string& out_path,
                                              const KeyRing& ring, const ExportOptions& opts = ExportOptions{}) {
    ExportFileResult result;
    try {
        switch (probe_writer_lock(log_path + ".lock")) {
            case WriterProbe::NotHeld: break;
            case WriterProbe::Held: result.status = ExportStatus::WriterLive; return result;
            case WriterProbe::Unknown: result.status = ExportStatus::WriterUnknown; return result;
        }

        std::vector<std::byte> log;
        switch (read_file_bounded(log_path, log, kMaxDurableLogBytes)) {
            case ReadFileStatus::Ok: break;
            case ReadFileStatus::TooLarge: result.status = ExportStatus::LogTooLarge; return result;
            case ReadFileStatus::NotFound:
            case ReadFileStatus::Error: result.status = ExportStatus::IoError; return result;
        }
        std::vector<std::byte> anchor;
        bool anchor_present = false;
        switch (read_file_bounded(log_path + ".tip", anchor, 4096)) {
            case ReadFileStatus::Ok: anchor_present = true; break;
            case ReadFileStatus::NotFound: break;
            case ReadFileStatus::TooLarge:
                result.status = ExportStatus::AnchorInvalid;  // a tip anchor is 77 bytes
                return result;
            case ReadFileStatus::Error: result.status = ExportStatus::IoError; return result;
        }
        std::vector<std::byte> identity;
        bool identity_present = false;
        switch (read_file_bounded(log_path + ".storeid.tip", identity, 4096)) {
            case ReadFileStatus::Ok: identity_present = true; break;
            case ReadFileStatus::NotFound: break;
            case ReadFileStatus::TooLarge:
                result.status = ExportStatus::StoreIdentityInvalid;
                return result;
            case ReadFileStatus::Error: result.status = ExportStatus::IoError; return result;
        }

        result.verification = verify_audit_log(log, ring, anchor_present, anchor, identity_present, identity, opts);
        if (!result.verification.ok()) {
            result.status = result.verification.status;
            return result;
        }

        ExclusiveOutputFile out;
        switch (out.create(out_path)) {
            case ExclusiveOutputFile::OpenStatus::Ok: break;
            case ExclusiveOutputFile::OpenStatus::Exists: result.status = ExportStatus::OutputExists; return result;
            case ExclusiveOutputFile::OpenStatus::Error: result.status = ExportStatus::OutputError; return result;
        }
        if (!emit_audit_export(log, ring, result.verification, [&out](std::string_view line) { return out.write(line); }) ||
            !out.commit()) {
            out.abandon();
            result.status = ExportStatus::OutputError;
        }
        return result;
    } catch (...) {
        result.status = ExportStatus::IoError;
        return result;
    }
}

}  // namespace hy
