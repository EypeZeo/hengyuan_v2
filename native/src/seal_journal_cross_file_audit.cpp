// SPDX-License-Identifier: proprietary
// seal_journal_cross_file_audit.cpp — read-only diagnostic/audit tool for
// the seal-journal breadcrumb directory family. Deliberately NOT a
// production code path: it never mutates a breadcrumb file, never acquires a
// CandidateLease (no lock file creation, no handles), and links only
// hengyuan_core's header-only codecs. It exists to answer one question
// empirically:
//
//   "Which MUST-equal cross-file rules does receipt validation need to
//    check, and can each of them actually be detected on disk?"
//
// The answer feeds the future IntentPhaseAdvancer design's step-2 field
// mapping table (which current-durable-fact field must equal which
// newly-written-field): every rule below carries its source annotation
// (durable_control_plane.hpp / seal_journal_precondition_codec.hpp /
// seal_export_migration_cleanup_abandon_codec.hpp / compaction_intent_codec.hpp
// comments) so the mapping table can cite where each equality comes from.
//
// Scope: the SAME seven durable-precondition types the coordinator's
// load_all_breadcrumb_preconditions() (seal_journal_breadcrumb_precondition_
// aggregate.hpp) reads, plus the CompactionCandidateIntent and its `.x1`
// chain:
//   seal-id-watermark, compaction-candidate-intent,
//   compaction-intent-x-<nonce>-<seq>.x1 (1..3),
//   seal-export-started (L), seal-export-started.v2 (V),
//   seal-export-started.mig (M), seal-export-started.clr (C),
//   seal-export-started.abd (A)
// plus the eighth artifact, the SealJournalCommitWatermark
// (<candidate_id_hex16>.jhw), which lives in a DIFFERENT directory scope
// (seal-journal/<store_uuid>/ — see scan_seal_journal_store() and the CLI's
// --seal-journal-dir). The GenerationSeal/bridge artifact (not implemented
// in this repo) is out of scope; rules whose source annotation references
// bridge.prev_* are enforced only as equality between the two carriers that
// must both equal it (Intent and Started).
//
// CRYPTOGRAPHY CAVEAT: fixtures are MAC'd under this tool's OWN deterministic
// test key (HY-AUDIT-FIXTURE-KEK-1), never under production key material.
// --audit only succeeds in decoding files signed with a key the tool's ring
// knows (keys 7/8 by default, or --kek-hex for the KEK); it reports
// "key not in ring" for anything else. This is a fixture-analysis tool, not
// an operator diagnostic for live directories.
//
// Usage:
//   seal_journal_cross_file_audit [--self-check]              (default)
//   seal_journal_cross_file_audit --gen <dir> [--scenario success|abandon|native]
//                                [--inject <rule-id>] [--kek-hex <64hex>]
//                                [--seal-journal-dir <dir>]
//   seal_journal_cross_file_audit --audit <dir> [--kek-hex <64hex>]
//                                [--seal-journal-dir <dir>]
//   seal_journal_cross_file_audit --list-rules
//
// --seal-journal-dir points at the seal-journal/<store_uuid>/ directory
// holding <candidate_id_hex16>.jhw; defaults to <dir>/seal-journal (the
// layout --gen writes). Without a scannable .jhw every JHW.* rule reports
// N/A.
//
// --self-check: for every rule, builds a fixture directory in the temp
// directory with exactly that rule's field tampered (all files still
// individually MAC-valid), audits it, and asserts the violated-rule set
// matches the expected set EXACTLY (both directions: the rule detects its
// violation, and no other rule false-positives). The clean fixtures for all
// three scenarios and an empty directory must report zero violations. This
// is the same "the control must still fail" discipline as the repo's TSan
// negative controls and TLA+ controls.

#include <hengyuan/compaction_intent_codec.hpp>
#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_export_migration_cleanup_abandon_codec.hpp>
#include <hengyuan/seal_journal_commit_tombstone_codec.hpp>
#include <hengyuan/seal_journal_precondition_codec.hpp>
#include <hengyuan/sha256.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hy {
namespace {

// ===========================================================================
// Deterministic test key material (see the CRYPTOGRAPHY CAVEAT above)
// ===========================================================================

constexpr std::uint32_t kAuditWatermarkKeyId = 7;  // watermark wire carries no kek_key_id
constexpr std::uint32_t kAuditPrimaryKeyId = 7;
constexpr std::uint32_t kAuditSecondaryKeyId = 8;  // used by KEK-rotation-style injections

constexpr std::array<std::byte, kKekSize> kAuditKek = {
    std::byte{'H'}, std::byte{'Y'}, std::byte{'-'}, std::byte{'A'}, std::byte{'U'}, std::byte{'D'},
    std::byte{'I'}, std::byte{'T'}, std::byte{'-'}, std::byte{'F'}, std::byte{'I'}, std::byte{'X'},
    std::byte{'T'}, std::byte{'U'}, std::byte{'R'}, std::byte{'E'}, std::byte{'-'}, std::byte{'K'},
    std::byte{'E'}, std::byte{'K'}, std::byte{'-'}, std::byte{'1'}, std::byte{'-'}, std::byte{'2'},
    std::byte{'0'}, std::byte{'2'}, std::byte{'6'}, std::byte{'-'}, std::byte{'0'}, std::byte{'8'},
    std::byte{'-'}, std::byte{'1'}};

constexpr std::array<std::byte, 32> kAuditPlaintextKey7 = {
    std::byte{'H'}, std::byte{'Y'}, std::byte{'-'}, std::byte{'A'}, std::byte{'U'}, std::byte{'D'},
    std::byte{'I'}, std::byte{'T'}, std::byte{'-'}, std::byte{'K'}, std::byte{'E'}, std::byte{'Y'},
    std::byte{'-'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'},
    std::byte{'0'}, std::byte{'0'}, std::byte{'7'}, std::byte{'-'}, std::byte{'f'}, std::byte{'i'},
    std::byte{'x'}, std::byte{'t'}, std::byte{'u'}, std::byte{'r'}, std::byte{'e'}, std::byte{'-'},
    std::byte{'7'}, std::byte{'7'}};

constexpr std::array<std::byte, 32> kAuditPlaintextKey8 = {
    std::byte{'H'}, std::byte{'Y'}, std::byte{'-'}, std::byte{'A'}, std::byte{'U'}, std::byte{'D'},
    std::byte{'I'}, std::byte{'T'}, std::byte{'-'}, std::byte{'K'}, std::byte{'E'}, std::byte{'Y'},
    std::byte{'-'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'},
    std::byte{'0'}, std::byte{'0'}, std::byte{'8'}, std::byte{'-'}, std::byte{'f'}, std::byte{'i'},
    std::byte{'x'}, std::byte{'t'}, std::byte{'u'}, std::byte{'r'}, std::byte{'e'}, std::byte{'-'},
    std::byte{'8'}, std::byte{'8'}};

// ===========================================================================
// Small helpers
// ===========================================================================

constexpr std::string_view kWatermarkName = "seal-id-watermark";
constexpr std::string_view kIntentName = "compaction-candidate-intent";
constexpr std::string_view kLName = "seal-export-started";        // L
constexpr std::string_view kVName = "seal-export-started.v2";     // V
constexpr std::string_view kMName = "seal-export-started.mig";    // M
constexpr std::string_view kCName = "seal-export-started.clr";    // C
constexpr std::string_view kAName = "seal-export-started.abd";    // A
// The .jhw commit watermark lives under seal-journal/<store_uuid>/, NOT the
// breadcrumb directory -- fixtures keep one store per breadcrumb dir, so the
// store_uuid path component is collapsed (it adds no discriminating power to
// the rule table).
constexpr std::string_view kSealJournalDirName = "seal-journal";

// Mirrors compaction_breadcrumb_io.hpp's format_hex16() (the tool duplicates
// the 6-line formatter rather than pulling in the Windows-native I/O header).
void format_hex16(std::uint64_t v, std::array<char, 17>& out) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[v & 0xFu];
        v >>= 4;
    }
    out[16] = '\0';
}

std::string x1_file_name(std::uint64_t build_nonce, std::uint32_t seq) {
    std::array<char, 17> nonce_hex{};
    std::array<char, 17> seq_hex{};
    format_hex16(build_nonce, nonce_hex);
    format_hex16(static_cast<std::uint64_t>(seq), seq_hex);
    return std::string("compaction-intent-x-") + nonce_hex.data() + "-" + seq_hex.data() + ".x1";
}

// <candidate_id_hex16>.jhw -- the SealJournalCommitWatermark file name is the
// candidate_id it binds (durable_control_plane.hpp's SealJournalCommitWatermark
// comment: seal-journal/<store_uuid...>/<candidate_id_hex16>.jhw).
std::string jhw_file_name(std::uint64_t candidate_id) {
    std::array<char, 17> id_hex{};
    format_hex16(candidate_id, id_hex);
    return std::string(id_hex.data()) + ".jhw";
}

void fill_arr(std::uint8_t (&arr)[32], std::uint8_t seed) {
    for (std::size_t i = 0; i < 32; ++i) {
        arr[i] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i));
    }
}

std::array<std::uint8_t, 32> file_sha256(std::span<const std::byte> bytes) noexcept {
    return crypto::sha256(bytes).bytes;
}

// ===========================================================================
// File I/O (read-only for --audit; --gen writes fixtures)
// ===========================================================================

enum class FileReadStatus : std::uint8_t { Ok, NotFound, WrongSize, IoError };

template <std::size_t N>
FileReadStatus read_file_exact(const std::filesystem::path& dir, std::string_view name,
                               std::array<std::byte, N>& out) {
    std::ifstream f(dir / std::filesystem::path(name), std::ios::binary);
    if (!f.is_open()) return FileReadStatus::NotFound;
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(N));
    if (f.gcount() != static_cast<std::streamsize>(N)) return FileReadStatus::WrongSize;
    char probe{};
    f.read(&probe, 1);
    if (f.gcount() != 0) return FileReadStatus::WrongSize;
    return FileReadStatus::Ok;
}

bool write_file(const std::filesystem::path& dir, std::string_view name, std::span<const std::byte> bytes) {
    std::ofstream f(dir / std::filesystem::path(name), std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

// Rule ids are used as self-check fixture subdirectory names, but several
// contain characters Windows forbids in file names ('<', '>', ':', etc.).
// The directory component is sanitized; rule ids themselves are untouched.
std::string sanitize_dir_component(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*') {
            out += '_';
        } else {
            out += c;
        }
    }
    return out;
}

// ===========================================================================
// Scan + decode of a breadcrumb directory (the read-only audit front end)
// ===========================================================================

struct FileRef {
    bool present{false};   // file found with the exact expected size
    bool decoded{false};   // MAC + shape verified with a ring-known key
    std::array<std::uint8_t, 32> file_sha256{};  // of the exact file bytes
    std::string note;      // "absent" / "wrong size" / "io error" / decode reason
};

struct BreadcrumbSet {
    FileRef wm{};
    SealIdWatermark wm_wire{};
    FileRef intent{};
    CompactionCandidateIntentWire intent_wire{};
    std::array<FileRef, 3> x1{};
    std::array<CompactionIntentTransitionWire, 3> x1_wire{};
    FileRef l{};
    SealExportStartedWire l_wire{};
    FileRef v{};
    SealExportStartedWire v_wire{};
    FileRef m{};
    SealExportStartedMigrationWire m_wire{};
    FileRef c{};
    SealStartedCleanupTombstoneWire c_wire{};
    FileRef a{};
    SealStartedAbandonWire a_wire{};
    FileRef jhw{};
    SealJournalCommitWatermark jhw_wire{};
    std::string jhw_file_name{};  // resolved <candidate_id_hex16>.jhw, or empty
};

// Decode wrappers: return "" on success, else a short reason. The watermark
// has no kek_key_id on the wire -- production resolves it externally (the
// aggregate takes it as a parameter), this tool uses the fixed audit key.
const char* decode_watermark(const std::array<std::byte, kSealIdWatermarkWireBytes>& raw, KeyRing& ring,
                             SealIdWatermark& out) noexcept {
    const PinResult pin = ring.pin_key(kAuditWatermarkKeyId);
    if (pin.status != PinStatus::Pinned) return "key not in ring";
    std::optional<VerifiedSealIdWatermark> verified;
    const auto st = decode_seal_id_watermark_wire(raw, pin.handle->key_bytes(), verified);
    if (st != SealJournalPreconditionDecodeStatus::Ok) return "decode failed";
    out = verified->value();
    return "";
}

const char* decode_intent(const std::array<std::byte, kCompactionCandidateIntentWireBytes>& raw, KeyRing& ring,
                          CompactionCandidateIntentWire& out) noexcept {
    std::uint32_t key_id = 0;
    if (!peek_compaction_candidate_intent_kek_key_id(raw, key_id)) return "peek failed";
    const PinResult pin = ring.pin_key(key_id);
    if (pin.status != PinStatus::Pinned) return "key not in ring";
    std::optional<VerifiedCompactionCandidateIntent> verified;
    const auto st = decode_compaction_candidate_intent_wire(raw, pin.handle->key_bytes(), verified);
    if (st != CompactionWireDecodeStatus::Ok) return "decode failed";
    out = verified->value();
    return "";
}

const char* decode_transition(const std::array<std::byte, kCompactionIntentTransitionWireBytes>& raw,
                              KeyRing& ring, CompactionIntentTransitionWire& out) noexcept {
    std::uint32_t key_id = 0;
    if (!peek_compaction_candidate_intent_kek_key_id(raw, key_id)) return "peek failed";
    const PinResult pin = ring.pin_key(key_id);
    if (pin.status != PinStatus::Pinned) return "key not in ring";
    std::optional<VerifiedTransition> verified;
    const auto st = decode_compaction_intent_transition_wire(raw, pin.handle->key_bytes(), verified);
    if (st != CompactionWireDecodeStatus::Ok) return "decode failed";
    out = verified->value();
    return "";
}

const char* decode_started(const std::array<std::byte, kSealExportStartedWireBytes>& raw, KeyRing& ring,
                           SealExportStartedWire& out) noexcept {
    std::uint32_t key_id = 0;
    if (!peek_seal_export_started_kek_key_id(raw, key_id)) return "peek failed";
    const PinResult pin = ring.pin_key(key_id);
    if (pin.status != PinStatus::Pinned) return "key not in ring";
    std::optional<VerifiedSealExportStarted> verified;
    const auto st = decode_seal_export_started_wire(raw, pin.handle->key_bytes(), verified);
    if (st != SealJournalPreconditionDecodeStatus::Ok) return "decode failed";
    out = verified->value();
    return "";
}

const char* decode_migration(const std::array<std::byte, kSealExportStartedMigrationWireBytes>& raw,
                             KeyRing& ring, SealExportStartedMigrationWire& out) noexcept {
    std::uint32_t key_id = 0;
    if (!peek_seal_export_started_migration_v2_kek_key_id(raw, key_id)) return "peek failed";
    const PinResult pin = ring.pin_key(key_id);
    if (pin.status != PinStatus::Pinned) return "key not in ring";
    std::optional<VerifiedSealExportStartedMigration> verified;
    const auto st = decode_seal_export_started_migration_wire(raw, pin.handle->key_bytes(), verified);
    if (st != SealStartedWireDecodeStatus::Ok) return "decode failed";
    out = verified->value();
    return "";
}

const char* decode_cleanup(const std::array<std::byte, kSealStartedCleanupWireBytes>& raw, KeyRing& ring,
                           SealStartedCleanupTombstoneWire& out) noexcept {
    std::uint32_t key_id = 0;
    if (!peek_seal_started_cleanup_kek_key_id(raw, key_id)) return "peek failed";
    const PinResult pin = ring.pin_key(key_id);
    if (pin.status != PinStatus::Pinned) return "key not in ring";
    std::optional<VerifiedSealStartedCleanupTombstone> verified;
    const auto st = decode_seal_started_cleanup_tombstone_wire(raw, pin.handle->key_bytes(), verified);
    if (st != SealStartedWireDecodeStatus::Ok) return "decode failed";
    out = verified->value();
    return "";
}

const char* decode_abandon(const std::array<std::byte, kSealStartedAbandonWireBytes>& raw, KeyRing& ring,
                           SealStartedAbandonWire& out) noexcept {
    std::uint32_t key_id = 0;
    if (!peek_seal_started_abandon_kek_key_id(raw, key_id)) return "peek failed";
    const PinResult pin = ring.pin_key(key_id);
    if (pin.status != PinStatus::Pinned) return "key not in ring";
    std::optional<VerifiedSealStartedAbandon> verified;
    const auto st = decode_seal_started_abandon_wire(raw, pin.handle->key_bytes(), verified);
    if (st != SealStartedWireDecodeStatus::Ok) return "decode failed";
    out = verified->value();
    return "";
}

const char* decode_jhw(const std::array<std::byte, kSealJournalCommitWatermarkWireBytes>& raw, KeyRing& ring,
                       SealJournalCommitWatermark& out) noexcept {
    std::uint32_t key_id = 0;
    if (!peek_seal_journal_commit_watermark_kek_key_id(raw, key_id)) return "peek failed";
    const PinResult pin = ring.pin_key(key_id);
    if (pin.status != PinStatus::Pinned) return "key not in ring";
    std::optional<VerifiedSealJournalCommitWatermark> verified;
    const auto st = decode_seal_journal_commit_watermark_wire(raw, pin.handle->key_bytes(), verified);
    if (st != SealJournalCommitTombstoneDecodeStatus::Ok) return "decode failed";
    out = verified->value();
    return "";
}

template <std::size_t N>
void fill_file_ref(FileRef& ref, const std::array<std::byte, N>& raw) {
    ref.file_sha256 = file_sha256(raw);
    ref.present = true;
}

BreadcrumbSet scan_directory(const std::filesystem::path& dir, KeyRing& ring) {
    BreadcrumbSet s{};

    std::array<std::byte, kSealIdWatermarkWireBytes> wm_raw{};
    const auto wm_read = read_file_exact(dir, kWatermarkName, wm_raw);
    if (wm_read == FileReadStatus::Ok) {
        fill_file_ref(s.wm, wm_raw);
        const char* why = decode_watermark(wm_raw, ring, s.wm_wire);
        s.wm.decoded = (why[0] == '\0');
        s.wm.note = s.wm.decoded ? "ok" : why;
    } else {
        s.wm.note = (wm_read == FileReadStatus::NotFound) ? "absent" : "unreadable";
    }

    std::array<std::byte, kCompactionCandidateIntentWireBytes> intent_raw{};
    const auto intent_read = read_file_exact(dir, kIntentName, intent_raw);
    if (intent_read == FileReadStatus::Ok) {
        fill_file_ref(s.intent, intent_raw);
        const char* why = decode_intent(intent_raw, ring, s.intent_wire);
        s.intent.decoded = (why[0] == '\0');
        s.intent.note = s.intent.decoded ? "ok" : why;
    } else {
        s.intent.note = (intent_read == FileReadStatus::NotFound) ? "absent" : "unreadable";
    }

    if (s.intent.decoded) {
        for (std::size_t i = 0; i < 3; ++i) {
            const std::string name =
                x1_file_name(s.intent_wire.build_nonce, static_cast<std::uint32_t>(i + 1));
            std::array<std::byte, kCompactionIntentTransitionWireBytes> x1_raw{};
            const auto x1_read = read_file_exact(dir, name, x1_raw);
            if (x1_read == FileReadStatus::Ok) {
                fill_file_ref(s.x1[i], x1_raw);
                const char* why = decode_transition(x1_raw, ring, s.x1_wire[i]);
                s.x1[i].decoded = (why[0] == '\0');
                s.x1[i].note = s.x1[i].decoded ? "ok" : why;
            } else {
                s.x1[i].note = (x1_read == FileReadStatus::NotFound) ? "absent" : "unreadable";
            }
        }
    }

    std::array<std::byte, kSealExportStartedWireBytes> l_raw{};
    const auto l_read = read_file_exact(dir, kLName, l_raw);
    if (l_read == FileReadStatus::Ok) {
        fill_file_ref(s.l, l_raw);
        const char* why = decode_started(l_raw, ring, s.l_wire);
        s.l.decoded = (why[0] == '\0');
        s.l.note = s.l.decoded ? "ok" : why;
    } else {
        s.l.note = (l_read == FileReadStatus::NotFound) ? "absent" : "unreadable";
    }

    std::array<std::byte, kSealExportStartedWireBytes> v_raw{};
    const auto v_read = read_file_exact(dir, kVName, v_raw);
    if (v_read == FileReadStatus::Ok) {
        fill_file_ref(s.v, v_raw);
        const char* why = decode_started(v_raw, ring, s.v_wire);
        s.v.decoded = (why[0] == '\0');
        s.v.note = s.v.decoded ? "ok" : why;
    } else {
        s.v.note = (v_read == FileReadStatus::NotFound) ? "absent" : "unreadable";
    }

    std::array<std::byte, kSealExportStartedMigrationWireBytes> m_raw{};
    const auto m_read = read_file_exact(dir, kMName, m_raw);
    if (m_read == FileReadStatus::Ok) {
        fill_file_ref(s.m, m_raw);
        const char* why = decode_migration(m_raw, ring, s.m_wire);
        s.m.decoded = (why[0] == '\0');
        s.m.note = s.m.decoded ? "ok" : why;
    } else {
        s.m.note = (m_read == FileReadStatus::NotFound) ? "absent" : "unreadable";
    }

    std::array<std::byte, kSealStartedCleanupWireBytes> c_raw{};
    const auto c_read = read_file_exact(dir, kCName, c_raw);
    if (c_read == FileReadStatus::Ok) {
        fill_file_ref(s.c, c_raw);
        const char* why = decode_cleanup(c_raw, ring, s.c_wire);
        s.c.decoded = (why[0] == '\0');
        s.c.note = s.c.decoded ? "ok" : why;
    } else {
        s.c.note = (c_read == FileReadStatus::NotFound) ? "absent" : "unreadable";
    }

    std::array<std::byte, kSealStartedAbandonWireBytes> a_raw{};
    const auto a_read = read_file_exact(dir, kAName, a_raw);
    if (a_read == FileReadStatus::Ok) {
        fill_file_ref(s.a, a_raw);
        const char* why = decode_abandon(a_raw, ring, s.a_wire);
        s.a.decoded = (why[0] == '\0');
        s.a.note = s.a.decoded ? "ok" : why;
    } else {
        s.a.note = (a_read == FileReadStatus::NotFound) ? "absent" : "unreadable";
    }

    return s;
}

// Scans the seal-journal/<store_uuid>/ directory (a DIFFERENT directory scope
// than the breadcrumb dir scan_directory() walks -- SealJournalStoreLease, not
// CandidateLease) for the single <candidate_id_hex16>.jhw commit watermark.
struct SealJournalScanResult {
    FileRef jhw{};
    SealJournalCommitWatermark jhw_wire{};
    std::string jhw_file_name{};  // resolved <candidate_id_hex16>.jhw, or empty
};

// candidate_id selects WHICH .jhw to read (the file name IS the candidate_id
// it binds, durable_control_plane.hpp); 0 = no selection available (Intent
// undecodable) and the .jhw is reported as absent. A store holding several
// .jhw files is legal (one per candidate); this tool audits the one for the
// candidate the breadcrumb directory's Intent names.
SealJournalScanResult scan_seal_journal_store(const std::filesystem::path& dir, std::uint64_t candidate_id,
                                              KeyRing& ring) {
    SealJournalScanResult out{};
    if (candidate_id == 0) {
        out.jhw.note = "no Intent candidate_id to select a .jhw file";
        return out;
    }
    out.jhw_file_name = jhw_file_name(candidate_id);

    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> jhw_raw{};
    const auto jhw_read = read_file_exact(dir, out.jhw_file_name, jhw_raw);
    if (jhw_read == FileReadStatus::Ok) {
        fill_file_ref(out.jhw, jhw_raw);
        const char* why = decode_jhw(jhw_raw, ring, out.jhw_wire);
        out.jhw.decoded = (why[0] == '\0');
        out.jhw.note = out.jhw.decoded ? "ok" : why;
    } else {
        out.jhw.note = (jhw_read == FileReadStatus::NotFound) ? "absent" : "unreadable";
    }
    return out;
}

// Breadcrumb dir scan + separate seal-journal store scan merged into one set
// (the .jhw rules live in the same single rule table as everything else).
BreadcrumbSet scan_with_seal_journal(const std::filesystem::path& breadcrumb_dir,
                                     const std::filesystem::path& seal_journal_dir, KeyRing& ring) {
    BreadcrumbSet s = scan_directory(breadcrumb_dir, ring);
    const std::uint64_t hint = (s.intent.present && s.intent.decoded) ? s.intent_wire.candidate_id : 0;
    const SealJournalScanResult jhw_scan = scan_seal_journal_store(seal_journal_dir, hint, ring);
    s.jhw = jhw_scan.jhw;
    s.jhw_wire = jhw_scan.jhw_wire;
    s.jhw_file_name = jhw_scan.jhw_file_name;
    return s;
}

// ===========================================================================
// Rule table — every MUST-equal cross-file rule, with its source annotation
// ===========================================================================

enum class RuleStatus : std::uint8_t { Verified, Violated, Unverifiable };

struct RuleResult {
    RuleStatus status{RuleStatus::Unverifiable};
    std::string detail;
};

struct Rule {
    std::string id;
    std::string desc;
    std::size_t idx{0};  // 0-based x1 frame index for per-frame rules; unused otherwise
    RuleResult (*eval)(const BreadcrumbSet&, std::size_t) noexcept;
};

RuleResult rule_verified() noexcept {
    RuleResult r;
    r.status = RuleStatus::Verified;
    return r;
}

RuleResult rule_unverifiable(std::string_view why) noexcept {
    RuleResult r;
    r.status = RuleStatus::Unverifiable;
    r.detail = std::string(why);
    return r;
}

RuleResult rule_violated(std::string_view what) noexcept {
    RuleResult r;
    r.status = RuleStatus::Violated;
    r.detail = std::string(what);
    return r;
}

template <typename T>
RuleResult check_pair(const FileRef& a, std::string_view an, T av, const FileRef& b, std::string_view bn,
                      T bv) noexcept {
    if (!a.present) return rule_unverifiable(std::string(an) + " absent");
    if (!b.present) return rule_unverifiable(std::string(bn) + " absent");
    if (!a.decoded) return rule_unverifiable(std::string(an) + " not decodable (" + a.note + ")");
    if (!b.decoded) return rule_unverifiable(std::string(bn) + " not decodable (" + b.note + ")");
    if (av == bv) return rule_verified();
    return rule_violated("differs");
}

RuleResult check_pair_bytes(const FileRef& a, std::string_view an, const std::uint8_t* av,
                            const FileRef& b, std::string_view bn, const std::uint8_t* bv) noexcept {
    if (!a.present) return rule_unverifiable(std::string(an) + " absent");
    if (!b.present) return rule_unverifiable(std::string(bn) + " absent");
    if (!a.decoded) return rule_unverifiable(std::string(an) + " not decodable (" + a.note + ")");
    if (!b.decoded) return rule_unverifiable(std::string(bn) + " not decodable (" + b.note + ")");
    if (std::memcmp(av, bv, 32) == 0) return rule_verified();
    return rule_violated("differs");
}

// Comparator for "wire field == SHA-256 of a file's bytes" rules.
RuleResult check_field_vs_file_sha(const FileRef& wire_file, std::string_view wire_label,
                                   const std::uint8_t* field, const FileRef& target_file,
                                   std::string_view target_label) noexcept {
    if (!wire_file.present) return rule_unverifiable(std::string(wire_label) + " absent");
    if (!target_file.present) return rule_unverifiable(std::string(target_label) + " absent");
    if (!wire_file.decoded) return rule_unverifiable(std::string(wire_label) + " not decodable (" + wire_file.note + ")");
    if (std::memcmp(field, target_file.file_sha256.data(), 32) == 0) return rule_verified();
    return rule_violated("digest differs from file bytes");
}

// Builds the full rule list (97 rules). Per-frame x1 rules are appended for
// each frame index 0..2; their evals N/A themselves when that frame is absent.
std::vector<Rule> build_rules() {
    std::vector<Rule> rules;

    // ---- store identity: every artifact must carry the same store_uuid ----
    // Source: durable_control_plane.hpp — all seven wires embed store_uuid_lo/hi;
    // the aggregate (seal_journal_breadcrumb_precondition_aggregate.hpp) loads all
    // of them through one CandidateLease scoped to one breadcrumb directory.
    rules.push_back({"L.store_uuid_lo==V.store_uuid_lo", "L and V must be same store (lo)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_lo, s.v, "V", s.v_wire.store_uuid_lo);
                     }});
    rules.push_back({"L.store_uuid_hi==V.store_uuid_hi", "L and V must be same store (hi)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_hi, s.v, "V", s.v_wire.store_uuid_hi);
                     }});
    rules.push_back({"L.store_uuid_lo==M.store_uuid_lo", "L and M must be same store (lo)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_lo, s.m, "M", s.m_wire.store_uuid_lo);
                     }});
    rules.push_back({"L.store_uuid_hi==M.store_uuid_hi", "L and M must be same store (hi)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_hi, s.m, "M", s.m_wire.store_uuid_hi);
                     }});
    rules.push_back({"L.store_uuid_lo==C.store_uuid_lo", "L and C must be same store (lo)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_lo, s.c, "C", s.c_wire.store_uuid_lo);
                     }});
    rules.push_back({"L.store_uuid_hi==C.store_uuid_hi", "L and C must be same store (hi)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_hi, s.c, "C", s.c_wire.store_uuid_hi);
                     }});
    rules.push_back({"L.store_uuid_lo==A.store_uuid_lo", "L and A must be same store (lo)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_lo, s.a, "A", s.a_wire.store_uuid_lo);
                     }});
    rules.push_back({"L.store_uuid_hi==A.store_uuid_hi", "L and A must be same store (hi)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_hi, s.a, "A", s.a_wire.store_uuid_hi);
                     }});
    rules.push_back({"L.store_uuid_lo==Intent.store_uuid_lo", "Intent must be same store as L (lo)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_lo, s.intent, "Intent",
                                           s.intent_wire.store_uuid_lo);
                     }});
    rules.push_back({"L.store_uuid_hi==Intent.store_uuid_hi", "Intent must be same store as L (hi)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_hi, s.intent, "Intent",
                                           s.intent_wire.store_uuid_hi);
                     }});
    rules.push_back({"L.store_uuid_lo==WM.store_uuid_lo", "Watermark must be same store as L (lo)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_lo, s.wm, "WM", s.wm_wire.store_uuid_lo);
                     }});
    rules.push_back({"L.store_uuid_hi==WM.store_uuid_hi", "Watermark must be same store as L (hi)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.store_uuid_hi, s.wm, "WM", s.wm_wire.store_uuid_hi);
                     }});

    // ---- id binding: the Started quintet must agree on candidate/request ids ----
    // Source: durable_control_plane.hpp "Started quintet" banner; the L<->V
    // closed-field-bind rule (L4 spec) and the aggregate's own candidate_id
    // note in seal_journal_breadcrumb_precondition_aggregate.hpp.
    rules.push_back({"L.candidate_id==V.candidate_id", "L and V must bind the same candidate_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.candidate_id, s.v, "V", s.v_wire.candidate_id);
                     }});
    rules.push_back({"L.candidate_id==M.candidate_id", "L and M must bind the same candidate_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.candidate_id, s.m, "M", s.m_wire.candidate_id);
                     }});
    rules.push_back({"L.candidate_id==C.candidate_id", "L and C must bind the same candidate_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.candidate_id, s.c, "C", s.c_wire.candidate_id);
                     }});
    rules.push_back({"L.candidate_id==A.candidate_id", "L and A must bind the same candidate_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.candidate_id, s.a, "A", s.a_wire.candidate_id);
                     }});
    rules.push_back({"L.request_id==V.request_id", "L and V must bind the same request_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.request_id, s.v, "V", s.v_wire.request_id);
                     }});
    rules.push_back({"L.request_id==M.request_id", "L and M must bind the same request_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.request_id, s.m, "M", s.m_wire.request_id);
                     }});
    rules.push_back({"L.request_id==C.request_id", "L and C must bind the same request_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.request_id, s.c, "C", s.c_wire.request_id);
                     }});
    rules.push_back({"L.request_id==A.request_id", "L and A must bind the same request_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.l, "L", s.l_wire.request_id, s.a, "A", s.a_wire.request_id);
                     }});

    // ---- KEK bindings ----
    // Source: durable_control_plane.hpp .mig layout comments — legacy_kek_key_id
    // is "sole KEK used to verify L (explicit; no try-all)", v2_kek_key_id is
    // "== V.kek_key_id; selects KEK for V + M MACs".
    rules.push_back({"M.legacy_kek_key_id==L.kek_key_id", "M names the sole KEK that verifies L", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.m, "M", s.m_wire.legacy_kek_key_id, s.l, "L", s.l_wire.kek_key_id);
                     }});
    rules.push_back({"M.v2_kek_key_id==V.kek_key_id", "M names the KEK that verifies V", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.m, "M", s.m_wire.v2_kek_key_id, s.v, "V", s.v_wire.kek_key_id);
                     }});

    // ---- M's file-integrity fields: digests and trailer MACs of L and V ----
    // Source: durable_control_plane.hpp .mig layout — legacy_file_digest is
    // "SHA-256(entire L file bytes)", legacy_mac is "L trailer MAC", etc.
    rules.push_back({"M.legacy_file_digest==SHA256(L)", "M's legacy_file_digest must be SHA-256 of the L file", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_field_vs_file_sha(s.m, "M", s.m_wire.legacy_file_digest, s.l, "L");
                     }});
    rules.push_back({"M.v2_file_digest==SHA256(V)", "M's v2_file_digest must be SHA-256 of the V file", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_field_vs_file_sha(s.m, "M", s.m_wire.v2_file_digest, s.v, "V");
                     }});
    rules.push_back({"M.legacy_mac==L.mac", "M's legacy_mac must be L's trailer MAC", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair_bytes(s.m, "M", s.m_wire.legacy_mac, s.l, "L", s.l_wire.mac);
                     }});
    rules.push_back({"M.v2_mac==V.mac", "M's v2_mac must be V's trailer MAC", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair_bytes(s.m, "M", s.m_wire.v2_mac, s.v, "V", s.v_wire.mac);
                     }});

    // ---- C's PostSealCommittedProof: nine fields must equal Started's ----
    // Source: durable_control_plane.hpp .clr layout — "PostSealCommittedProof
    // (round-58 P0) -- MAC-bound; all MUST hold at CREATE and at every
    // CleanupInProgress resume", each field annotated "== Started.X".
    rules.push_back({"C.source_generation==L.source_generation", "== Started.source_generation == bridge.prev_generation", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.c, "C", s.c_wire.source_generation, s.l, "L", s.l_wire.source_generation);
                     }});
    rules.push_back({"C.baseline_tip_seq==L.baseline_tip_seq", "== Started.baseline_tip_seq == bridge.prev_tip_seq", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.c, "C", s.c_wire.baseline_tip_seq, s.l, "L", s.l_wire.baseline_tip_seq);
                     }});
    rules.push_back({"C.baseline_tip_mac==L.baseline_tip_mac", "== Started / bridge.prev_tip_mac", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair_bytes(s.c, "C", s.c_wire.baseline_tip_mac, s.l, "L",
                                                 s.l_wire.baseline_tip_mac);
                     }});
    rules.push_back({"C.baseline_key_id==L.baseline_key_id", "== Started / bridge.prev_key_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.c, "C", s.c_wire.baseline_key_id, s.l, "L", s.l_wire.baseline_key_id);
                     }});
    rules.push_back({"C.new_generation==L.new_generation", "== Started.new_generation; CURRENT MUST == this", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.c, "C", s.c_wire.new_generation, s.l, "L", s.l_wire.new_generation);
                     }});
    rules.push_back({"C.new_final_seq==L.new_final_seq", "== Started.new_final_seq", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.c, "C", s.c_wire.new_final_seq, s.l, "L", s.l_wire.new_final_seq);
                     }});
    rules.push_back({"C.new_final_tip_mac==L.new_final_tip_mac", "== Started.new_final_tip_mac", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair_bytes(s.c, "C", s.c_wire.new_final_tip_mac, s.l, "L",
                                                 s.l_wire.new_final_tip_mac);
                     }});
    rules.push_back({"C.new_key_id==L.new_key_id", "== Started.new_key_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.c, "C", s.c_wire.new_key_id, s.l, "L", s.l_wire.new_key_id);
                     }});
    rules.push_back({"C.content_root==L.content_root", "== Started.content_root == GenerationSeal.content_root", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair_bytes(s.c, "C", s.c_wire.content_root, s.l, "L", s.l_wire.content_root);
                     }});

    // ---- C's digests of the L/V/M files ----
    // Source: durable_control_plane.hpp .clr layout — digest_L "SHA-256(L) or
    // zeros if bit0 clear", digest_V/digest_M real digests for MigratedV2
    // (NativeV2 must be all-zero, decode-enforced).
    rules.push_back({"C.digest_L==SHA256(L)", "C's digest_L must be SHA-256 of the L file", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_field_vs_file_sha(s.c, "C", s.c_wire.digest_L, s.l, "L");
                     }});
    rules.push_back({"C.digest_V==SHA256(V)", "C's digest_V must be SHA-256 of the V file (MigratedV2)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (s.c.present && s.c.decoded && s.c_wire.started_kind == kSealStartedKindNativeV2) {
                             return rule_unverifiable("NativeV2: digest_V must be zero (decode-enforced)");
                         }
                         return check_field_vs_file_sha(s.c, "C", s.c_wire.digest_V, s.v, "V");
                     }});
    rules.push_back({"C.digest_M==SHA256(M)", "C's digest_M must be SHA-256 of the M file (MigratedV2)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (s.c.present && s.c.decoded && s.c_wire.started_kind == kSealStartedKindNativeV2) {
                             return rule_unverifiable("NativeV2: digest_M must be zero (decode-enforced)");
                         }
                         return check_field_vs_file_sha(s.c, "C", s.c_wire.digest_M, s.m, "M");
                     }});

    // ---- A's bind fields must equal Started's ----
    // Source: durable_control_plane.hpp .abd layout — each annotated
    // "== Started.X" (baseline_tip_seq/mac/key_id, content_root);
    // source_generation mirrors the same bridge binding.
    rules.push_back({"A.source_generation==L.source_generation", "== Started.source_generation == bridge.prev_generation", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.a, "A", s.a_wire.source_generation, s.l, "L", s.l_wire.source_generation);
                     }});
    rules.push_back({"A.baseline_tip_seq==L.baseline_tip_seq", "== Started.baseline_tip_seq == bridge.prev_tip_seq", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.a, "A", s.a_wire.baseline_tip_seq, s.l, "L", s.l_wire.baseline_tip_seq);
                     }});
    rules.push_back({"A.baseline_tip_mac==L.baseline_tip_mac", "== Started / bridge.prev_tip_mac", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair_bytes(s.a, "A", s.a_wire.baseline_tip_mac, s.l, "L",
                                                 s.l_wire.baseline_tip_mac);
                     }});
    rules.push_back({"A.baseline_key_id==L.baseline_key_id", "== Started / bridge.prev_key_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.a, "A", s.a_wire.baseline_key_id, s.l, "L", s.l_wire.baseline_key_id);
                     }});
    rules.push_back({"A.content_root==L.content_root", "== Started.content_root == GenerationSeal.content_root", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair_bytes(s.a, "A", s.a_wire.content_root, s.l, "L", s.l_wire.content_root);
                     }});
    rules.push_back({"A.digest_C==SHA256(C)", "A's digest_C must be SHA-256 of the (unauthorized) C file", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (s.a.present && s.a.decoded &&
                             (s.a_wire.present_mask & seal_started_wire_codec_detail::kMaskBitC) == 0) {
                             return rule_unverifiable("present_mask bit3(C) clear: digest_C is zero by shape");
                         }
                         return check_field_vs_file_sha(s.a, "A", s.a_wire.digest_C, s.c, "C");
                     }});

    // ---- Watermark allocated-range: ids must be within what the watermark
    //      has already allocated ----
    // Source: durable_control_plane.hpp — SealIdWatermark's next_* is "the
    // NEXT allocatable value"; intent codec: "Forbidden: raise Reserved
    // without durable SealIdWatermark advance binding the same
    // candidate_id/request_id".
    rules.push_back({"Intent.candidate_id<WM.next_candidate_id", "Intent ids must be within the watermark-allocated range", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.intent.present) return rule_unverifiable("Intent absent");
                         if (!s.wm.present) return rule_unverifiable("WM absent");
                         if (!s.intent.decoded) return rule_unverifiable("Intent not decodable");
                         if (!s.wm.decoded) return rule_unverifiable("WM not decodable");
                         if (s.intent_wire.phase < kCompactionCandidateIntentPhaseReserved) {
                             return rule_unverifiable("Intent.phase < Reserved: ids not yet bound");
                         }
                         if (s.intent_wire.candidate_id < s.wm_wire.next_candidate_id) return rule_verified();
                         return rule_violated("Intent.candidate_id beyond watermark");
                     }});
    rules.push_back({"Intent.request_id<WM.next_request_id", "Intent request ids must be within the watermark-allocated range", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.intent.present) return rule_unverifiable("Intent absent");
                         if (!s.wm.present) return rule_unverifiable("WM absent");
                         if (!s.intent.decoded) return rule_unverifiable("Intent not decodable");
                         if (!s.wm.decoded) return rule_unverifiable("WM not decodable");
                         if (s.intent_wire.phase < kCompactionCandidateIntentPhaseReserved) {
                             return rule_unverifiable("Intent.phase < Reserved: ids not yet bound");
                         }
                         if (s.intent_wire.request_id < s.wm_wire.next_request_id) return rule_verified();
                         return rule_violated("Intent.request_id beyond watermark");
                     }});
    rules.push_back({"L.candidate_id<WM.next_candidate_id", "L's candidate_id must be within the watermark-allocated range", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.l.present) return rule_unverifiable("L absent");
                         if (!s.wm.present) return rule_unverifiable("WM absent");
                         if (!s.l.decoded) return rule_unverifiable("L not decodable");
                         if (!s.wm.decoded) return rule_unverifiable("WM not decodable");
                         if (s.l_wire.candidate_id < s.wm_wire.next_candidate_id) return rule_verified();
                         return rule_violated("L.candidate_id beyond watermark");
                     }});
    rules.push_back({"L.request_id<WM.next_request_id", "L's request_id must be within the watermark-allocated range", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.l.present) return rule_unverifiable("L absent");
                         if (!s.wm.present) return rule_unverifiable("WM absent");
                         if (!s.l.decoded) return rule_unverifiable("L not decodable");
                         if (!s.wm.decoded) return rule_unverifiable("WM not decodable");
                         if (s.l_wire.request_id < s.wm_wire.next_request_id) return rule_verified();
                         return rule_violated("L.request_id beyond watermark");
                     }});

    // ---- Intent <-> L binding (the Started record must be for THIS intent) ----
    // Source: intent codec — "Forbidden: raise StartedPublished before durable
    // NativeV2/MigratedV2 Started for those ids"; baseline/generation fields
    // must both equal the same bridge.prev_* values.
    rules.push_back({"Intent.candidate_id==L.candidate_id", "Started must be for the Intent's candidate_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (s.intent.present && s.intent.decoded &&
                             s.intent_wire.phase < kCompactionCandidateIntentPhaseStartedPublished) {
                             return rule_unverifiable("Intent.phase < StartedPublished");
                         }
                         return check_pair(s.intent, "Intent", s.intent_wire.candidate_id, s.l, "L",
                                           s.l_wire.candidate_id);
                     }});
    rules.push_back({"Intent.request_id==L.request_id", "Started must be for the Intent's request_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (s.intent.present && s.intent.decoded &&
                             s.intent_wire.phase < kCompactionCandidateIntentPhaseStartedPublished) {
                             return rule_unverifiable("Intent.phase < StartedPublished");
                         }
                         return check_pair(s.intent, "Intent", s.intent_wire.request_id, s.l, "L",
                                           s.l_wire.request_id);
                     }});
    rules.push_back({"Intent.baseline_tip_seq==L.baseline_tip_seq", "both must equal bridge.prev_tip_seq", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.intent, "Intent", s.intent_wire.baseline_tip_seq, s.l, "L",
                                           s.l_wire.baseline_tip_seq);
                     }});
    rules.push_back({"Intent.baseline_tip_mac==L.baseline_tip_mac", "both must equal bridge.prev_tip_mac", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair_bytes(s.intent, "Intent", s.intent_wire.baseline_tip_mac, s.l, "L",
                                                 s.l_wire.baseline_tip_mac);
                     }});
    rules.push_back({"Intent.baseline_key_id==L.baseline_key_id", "both must equal bridge.prev_key_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.intent, "Intent", s.intent_wire.baseline_key_id, s.l, "L",
                                           s.l_wire.baseline_key_id);
                     }});
    rules.push_back({"Intent.source_generation==L.source_generation", "Intent's source generation must be Started's", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.intent, "Intent", s.intent_wire.source_generation, s.l, "L",
                                           s.l_wire.source_generation);
                     }});
    rules.push_back({"Intent.target_generation==L.new_generation", "Intent's target generation must be Started's new generation", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.intent, "Intent", s.intent_wire.target_generation, s.l, "L",
                                           s.l_wire.new_generation);
                     }});

    // ---- x1 bind: every transition's bind fields must equal Intent's ----
    // Source: walk_x1_chain_raw() (compaction_intent_codec.hpp) —
    // "Bind MUST match Intent: store_uuid, kek_key_id, baseline 4-tuple,
    // generations, build_nonce. candidate_id/request_id MUST equal the
    // post-Reserved Intent ids (nonzero from seq=1)."
    for (std::size_t i = 0; i < 3; ++i) {
        // Captureless lambdas only (Rule stores plain function pointers); the
        // frame index rides in Rule::idx; the rule id names the frame so the
        // injection matrix can key on it.
        const std::string tag = "X1[" + std::to_string(i + 1) + "]";
        rules.push_back({tag + ".store_uuid_lo==Intent.store_uuid_lo", "x1 bind field must match Intent", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].store_uuid_lo, s.intent, "Intent",
                                               s.intent_wire.store_uuid_lo);
                         }});
        rules.push_back({tag + ".store_uuid_hi==Intent.store_uuid_hi", "x1 bind field must match Intent", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].store_uuid_hi, s.intent, "Intent",
                                               s.intent_wire.store_uuid_hi);
                         }});
        rules.push_back({tag + ".kek_key_id==Intent.kek_key_id", "x1 bind field must match Intent", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].kek_key_id, s.intent, "Intent",
                                               s.intent_wire.kek_key_id);
                         }});
        rules.push_back({tag + ".source_generation==Intent.source_generation", "x1 bind field must match Intent", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].source_generation, s.intent, "Intent",
                                               s.intent_wire.source_generation);
                         }});
        rules.push_back({tag + ".target_generation==Intent.target_generation", "x1 bind field must match Intent", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].target_generation, s.intent, "Intent",
                                               s.intent_wire.target_generation);
                         }});
        rules.push_back({tag + ".baseline_tip_seq==Intent.baseline_tip_seq", "x1 bind field must match Intent", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].baseline_tip_seq, s.intent, "Intent",
                                               s.intent_wire.baseline_tip_seq);
                         }});
        rules.push_back({tag + ".baseline_tip_mac==Intent.baseline_tip_mac", "x1 bind field must match Intent", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair_bytes(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                                     s.x1_wire[idx].baseline_tip_mac, s.intent, "Intent",
                                                     s.intent_wire.baseline_tip_mac);
                         }});
        rules.push_back({tag + ".baseline_key_id==Intent.baseline_key_id", "x1 bind field must match Intent", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].baseline_key_id, s.intent, "Intent",
                                               s.intent_wire.baseline_key_id);
                         }});
        rules.push_back({tag + ".build_nonce==Intent.build_nonce", "x1 bind field must match Intent (build_nonce also names the file)", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].build_nonce, s.intent, "Intent",
                                               s.intent_wire.build_nonce);
                         }});
        rules.push_back({tag + ".candidate_id==Intent.candidate_id", "x1 ids must equal the post-Reserved Intent ids", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].candidate_id, s.intent, "Intent",
                                               s.intent_wire.candidate_id);
                         }});
        rules.push_back({tag + ".request_id==Intent.request_id", "x1 ids must equal the post-Reserved Intent ids", i,
                         [](const BreadcrumbSet& s, std::size_t idx) noexcept {
                             return check_pair(s.x1[idx], "X1[" + std::to_string(idx + 1) + "]",
                                               s.x1_wire[idx].request_id, s.intent, "Intent",
                                               s.intent_wire.request_id);
                         }});
    }

    // ---- x1 chain: prev_transition_mac linkage ----
    // Source: walk_x1_chain_raw() — seq==1 must be all-zero; seq>1 must equal
    // the previous frame's trailer mac (MacChainBroken).
    rules.push_back({"X1[1].prev_transition_mac==ZERO", "first transition's prev_transition_mac must be all-zero", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.x1[0].present) return rule_unverifiable("X1[1] absent");
                         if (!s.x1[0].decoded) return rule_unverifiable("X1[1] not decodable");
                         const std::uint8_t zero[32]{};
                         if (std::memcmp(s.x1_wire[0].prev_transition_mac, zero, 32) == 0) return rule_verified();
                         return rule_violated("prev_transition_mac not zero");
                     }});
    rules.push_back({"X1[2].prev_transition_mac==X1[1].mac", "frame 2 must chain to frame 1's trailer mac", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.x1[1].present) return rule_unverifiable("X1[2] absent");
                         if (!s.x1[1].decoded) return rule_unverifiable("X1[2] not decodable");
                         if (!s.x1[0].present || !s.x1[0].decoded) return rule_unverifiable("X1[1] absent/undecodable");
                         return check_pair_bytes(s.x1[1], "X1[2]", s.x1_wire[1].prev_transition_mac, s.x1[0],
                                                 "X1[1]", s.x1_wire[0].mac);
                     }});
    rules.push_back({"X1[3].prev_transition_mac==X1[2].mac", "frame 3 must chain to frame 2's trailer mac", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.x1[2].present) return rule_unverifiable("X1[3] absent");
                         if (!s.x1[2].decoded) return rule_unverifiable("X1[3] not decodable");
                         if (!s.x1[1].present || !s.x1[1].decoded) return rule_unverifiable("X1[2] absent/undecodable");
                         return check_pair_bytes(s.x1[2], "X1[3]", s.x1_wire[2].prev_transition_mac, s.x1[1],
                                                 "X1[2]", s.x1_wire[1].mac);
                     }});

    // ---- chain terminal: a full 3-frame chain's final to_phase must equal
    //      Intent.phase (walk_x1_chain_raw's TerminalBranchConflict) ----
    rules.push_back({"X1[3].to_phase==Intent.phase", "terminal transition branch must match Intent.phase", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.x1[2].present) return rule_unverifiable("X1[3] absent (chain shorter than 3)");
                         if (!s.x1[2].decoded) return rule_unverifiable("X1[3] not decodable");
                         if (!s.intent.present || !s.intent.decoded) return rule_unverifiable("Intent absent/undecodable");
                         if (s.x1_wire[2].to_phase == s.intent_wire.phase) return rule_verified();
                         return rule_violated("terminal branch conflicts with Intent.phase");
                     }});

    // ---- .jhw (SealJournalCommitWatermark) bindings ----
    // Source: intent_phase_advancer.hpp Step 1b (the real write code) --
    // jhw.store_uuid_lo/hi = before.store_uuid_lo/hi (the Intent's),
    // jhw.candidate_id = candidate_id (the id just bound to the Intent by the
    // watermark advance), jhw.highest_committed_journal_seq = 0,
    // jhw.kek_key_id = before.kek_key_id (the INTENT's own key -- the L
    // record does not exist yet at this advance step, so no L-side rule).
    // Source annotations for the struct fields: durable_control_plane.hpp
    // SealJournalCommitWatermark.
    rules.push_back({"JHW.store_uuid_lo==Intent.store_uuid_lo", "jhw must be for the Intent's store (lo)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.jhw, "JHW", s.jhw_wire.store_uuid_lo, s.intent, "Intent",
                                           s.intent_wire.store_uuid_lo);
                     }});
    rules.push_back({"JHW.store_uuid_hi==Intent.store_uuid_hi", "jhw must be for the Intent's store (hi)", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.jhw, "JHW", s.jhw_wire.store_uuid_hi, s.intent, "Intent",
                                           s.intent_wire.store_uuid_hi);
                     }});
    rules.push_back({"JHW.candidate_id==Intent.candidate_id", "jhw must bind the Intent's candidate_id", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.intent.present) return rule_unverifiable("Intent absent");
                         if (!s.jhw.present) return rule_unverifiable("JHW absent");
                         if (!s.intent.decoded) return rule_unverifiable("Intent not decodable");
                         if (!s.jhw.decoded) return rule_unverifiable("JHW not decodable");
                         if (s.intent_wire.phase < kCompactionCandidateIntentPhaseReserved) {
                             return rule_unverifiable("Intent.phase < Reserved: ids not yet bound");
                         }
                         if (s.intent_wire.candidate_id == s.jhw_wire.candidate_id) return rule_verified();
                         return rule_violated("candidate_id differs");
                     }});
    rules.push_back({"JHW.kek_key_id==Intent.kek_key_id", "jhw must be MAC'd under the Intent's own KEK", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         return check_pair(s.jhw, "JHW", s.jhw_wire.kek_key_id, s.intent, "Intent",
                                           s.intent_wire.kek_key_id);
                     }});
    // STRICT equality, deliberately NOT the `<` interval inequality the
    // existing L/Intent-vs-WM rules use: .jhw's candidate_id binds the exact
    // next_candidate_id value the watermark advance consumed for this Intent,
    // so after the advance it must be exactly WM.next_candidate_id - 1.
    rules.push_back({"JHW.candidate_id+1==WM.next_candidate_id",
                     "jhw binds the exact id the watermark advance consumed", 0,
                     [](const BreadcrumbSet& s, std::size_t) noexcept {
                         if (!s.jhw.present) return rule_unverifiable("JHW absent");
                         if (!s.wm.present) return rule_unverifiable("WM absent");
                         if (!s.jhw.decoded) return rule_unverifiable("JHW not decodable");
                         if (!s.wm.decoded) return rule_unverifiable("WM not decodable");
                         if (s.jhw_wire.candidate_id + 1 == s.wm_wire.next_candidate_id) return rule_verified();
                         return rule_violated("JHW.candidate_id+1 != WM.next_candidate_id");
                     }});
    // NOTE: highest_committed_journal_seq==0 is deliberately NOT a rule --
    // it is a decode-time/shape property of a freshly created watermark
    // ("none yet" is the legal initial state, validate_seal_journal_commit_
    // watermark_shape() fenced it out of the codec by design), not a
    // cross-file MUST-equal relation.

    return rules;
}

// ===========================================================================
// Fixture generation (the "realistically constructed" test fixture files)
// ===========================================================================

enum class Scenario : std::uint8_t {
    PostSealSuccessMigrated,  // watermark, intent(PostSealFinalizing), x1 1..3, L, V, M, C
    ClrAbandonMigrated,       // above + A (C present but unauthorized, A written against it)
    NativeV2StartedPublished  // watermark, intent(StartedPublished), x1 1..2, L only
};

struct FixtureParams {
    Scenario scenario{Scenario::PostSealSuccessMigrated};
    std::uint64_t store_uuid_lo{0x1111111111111111ull};
    std::uint64_t store_uuid_hi{0x2222222222222222ull};
    std::uint32_t kek_key_id{kAuditPrimaryKeyId};
    std::uint64_t candidate_id{100};
    std::uint64_t request_id{200};
    std::uint32_t source_generation{5};
    std::uint64_t baseline_tip_seq{42};
    std::uint32_t baseline_key_id{3};
    std::uint32_t new_key_id{4};
    std::uint64_t build_nonce{0xABCDEF0123456789ull};
    std::uint8_t producer_mask{0b0011};
    std::uint8_t producer_count{2};
    std::array<SealHandoffRingId, kMaxSealHandoffProducers> ring_ids{0x1001, 0x1002, 0, 0, 0, 0, 0, 0};
};

struct Wires {
    SealIdWatermark wm{};
    CompactionCandidateIntentWire intent{};
    std::array<CompactionIntentTransitionWire, 3> x1{};
    SealExportStartedWire l{};
    SealExportStartedWire v{};
    SealExportStartedMigrationWire m{};
    SealStartedCleanupTombstoneWire c{};
    SealStartedAbandonWire a{};
    SealJournalCommitWatermark jhw{};
};

// A pin cache: one PinnedKeyHandle per key id, alive for the whole build.
class KeyHolder {
public:
    explicit KeyHolder(KeyRing& ring) noexcept : ring_(ring) {}
    std::span<const std::byte> get(std::uint32_t key_id) {
        for (const auto& p : pins_) {
            if (p.status == PinStatus::Pinned && p.handle->key_id() == key_id) {
                return p.handle->key_bytes();
            }
        }
        pins_.push_back(ring_.pin_key(key_id));
        return pins_.back().handle->key_bytes();
    }

private:
    KeyRing& ring_;
    std::vector<PinResult> pins_;
};

Wires make_wires(const FixtureParams& p) {
    Wires w{};

    w.wm.store_uuid_lo = p.store_uuid_lo;
    w.wm.store_uuid_hi = p.store_uuid_hi;
    w.wm.next_candidate_id = p.candidate_id + 1;
    w.wm.next_request_id = p.request_id + 1;

    w.intent.format_version = kCompactionCandidateIntentFormatVersion;
    w.intent.total_bytes = kCompactionCandidateIntentWireBytes;
    w.intent.store_uuid_lo = p.store_uuid_lo;
    w.intent.store_uuid_hi = p.store_uuid_hi;
    w.intent.kek_key_id = p.kek_key_id;
    w.intent.source_generation = p.source_generation;
    w.intent.target_generation = p.source_generation + 1;
    w.intent.baseline_tip_seq = p.baseline_tip_seq;
    fill_arr(w.intent.baseline_tip_mac, 0x10);
    w.intent.baseline_key_id = p.baseline_key_id;
    w.intent.build_nonce = p.build_nonce;
    w.intent.candidate_id = p.candidate_id;
    w.intent.request_id = p.request_id;
    w.intent.phase = (p.scenario == Scenario::PostSealSuccessMigrated)
                         ? kCompactionCandidateIntentPhasePostSealFinalizing
                         : (p.scenario == Scenario::ClrAbandonMigrated
                                ? kCompactionCandidateIntentPhaseAbandonFinalizing
                                : kCompactionCandidateIntentPhaseStartedPublished);

    const std::uint32_t frame_count =
        (p.scenario == Scenario::NativeV2StartedPublished) ? 2u : 3u;
    const std::uint8_t terminal_to =
        (p.scenario == Scenario::ClrAbandonMigrated) ? kCompactionCandidateIntentPhaseAbandonFinalizing
                                                     : kCompactionCandidateIntentPhasePostSealFinalizing;
    for (std::uint32_t s = 1; s <= frame_count; ++s) {
        CompactionIntentTransitionWire& t = w.x1[s - 1];
        t.format_version = kCompactionIntentTransitionFormatVersion;
        t.total_bytes = kCompactionIntentTransitionWireBytes;
        t.store_uuid_lo = p.store_uuid_lo;
        t.store_uuid_hi = p.store_uuid_hi;
        t.kek_key_id = p.kek_key_id;
        t.from_phase = (s == 1) ? kCompactionCandidateIntentPhaseBuilding
                                : (s == 2) ? kCompactionCandidateIntentPhaseReserved
                                           : kCompactionCandidateIntentPhaseStartedPublished;
        t.to_phase = (s == 1) ? kCompactionCandidateIntentPhaseReserved
                              : (s == 2) ? kCompactionCandidateIntentPhaseStartedPublished : terminal_to;
        t.transition_seq = s;
        t.source_generation = p.source_generation;
        t.target_generation = p.source_generation + 1;
        t.baseline_tip_seq = p.baseline_tip_seq;
        fill_arr(t.baseline_tip_mac, 0x10);
        t.baseline_key_id = p.baseline_key_id;
        t.build_nonce = p.build_nonce;
        t.candidate_id = p.candidate_id;
        t.request_id = p.request_id;
    }

    const auto fill_started = [&p](SealExportStartedWire& l) {
        l.format_version = kSealExportStartedFormatVersion;
        l.total_bytes = kSealExportStartedWireBytes;
        l.store_uuid_lo = p.store_uuid_lo;
        l.store_uuid_hi = p.store_uuid_hi;
        l.candidate_id = p.candidate_id;
        l.request_id = p.request_id;
        l.source_generation = p.source_generation;
        l.new_generation = p.source_generation + 1;
        l.baseline_tip_seq = p.baseline_tip_seq;
        fill_arr(l.baseline_tip_mac, 0x10);
        l.baseline_key_id = p.baseline_key_id;
        l.new_final_seq = 99;
        fill_arr(l.new_final_tip_mac, 0x20);
        l.new_key_id = p.new_key_id;
        fill_arr(l.content_root, 0x30);
        l.kek_key_id = p.kek_key_id;
    };
    fill_started(w.l);
    w.l.registered_producer_mask = p.producer_mask;
    w.l.producer_count = p.producer_count;
    for (std::size_t i = 0; i < kMaxSealHandoffProducers; ++i) w.l.ring_id[i] = p.ring_ids[i];
    fill_started(w.v);
    // L<->V differ in topology tuple + kek per the spec's closed-field-bind
    // rule: give V its own topology so the fixture is realistic, not copied.
    w.v.registered_producer_mask = 0b0001;
    w.v.producer_count = 1;
    for (std::size_t i = 0; i < kMaxSealHandoffProducers; ++i) w.v.ring_id[i] = 0;
    w.v.ring_id[0] = 0x2001;

    w.m.format_version = kSealExportStartedMigrationFormatVersion;
    w.m.total_bytes = kSealExportStartedMigrationWireBytes;
    w.m.store_uuid_lo = p.store_uuid_lo;
    w.m.store_uuid_hi = p.store_uuid_hi;
    w.m.candidate_id = p.candidate_id;
    w.m.request_id = p.request_id;
    w.m.legacy_kek_key_id = p.kek_key_id;
    w.m.v2_kek_key_id = p.kek_key_id;

    w.c.format_version = kSealStartedCleanupFormatVersion;
    w.c.total_bytes = kSealStartedCleanupWireBytes;
    w.c.store_uuid_lo = p.store_uuid_lo;
    w.c.store_uuid_hi = p.store_uuid_hi;
    w.c.candidate_id = p.candidate_id;
    w.c.request_id = p.request_id;
    w.c.kek_key_id = p.kek_key_id;
    w.c.started_kind = kSealStartedKindMigratedV2;
    w.c.present_mask = 0b111;
    w.c.phase = kSealStartedCleanupPhaseAuthorized;
    w.c.source_generation = p.source_generation;
    w.c.baseline_tip_seq = p.baseline_tip_seq;
    fill_arr(w.c.baseline_tip_mac, 0x10);
    w.c.baseline_key_id = p.baseline_key_id;
    w.c.new_generation = p.source_generation + 1;
    w.c.new_final_seq = 99;
    fill_arr(w.c.new_final_tip_mac, 0x20);
    w.c.new_key_id = p.new_key_id;
    fill_arr(w.c.content_root, 0x30);

    w.a.format_version = kSealStartedAbandonFormatVersion;
    w.a.total_bytes = kSealStartedAbandonWireBytes;
    w.a.store_uuid_lo = p.store_uuid_lo;
    w.a.store_uuid_hi = p.store_uuid_hi;
    w.a.candidate_id = p.candidate_id;
    w.a.request_id = p.request_id;
    w.a.kek_key_id = p.kek_key_id;
    w.a.started_kind = kSealStartedKindMigratedV2;
    w.a.abandon_reason = kSealStartedAbandonReasonNotFound;
    w.a.present_mask = 0b1111;  // L|V|M|C
    w.a.phase = kSealStartedAbandonPhaseAuthorized;
    w.a.source_generation = p.source_generation;
    w.a.baseline_tip_seq = p.baseline_tip_seq;
    fill_arr(w.a.baseline_tip_mac, 0x10);
    w.a.baseline_key_id = p.baseline_key_id;
    fill_arr(w.a.content_root, 0x30);

    // .jhw mirrors intent_phase_advancer.hpp Step 1b's real write code:
    // store_uuid from the Intent, candidate_id = the id bound by the
    // watermark advance, kek_key_id = the Intent's own key, and
    // highest_committed_journal_seq = 0 ("none yet").
    w.jhw.store_uuid_lo = p.store_uuid_lo;
    w.jhw.store_uuid_hi = p.store_uuid_hi;
    w.jhw.candidate_id = p.candidate_id;
    w.jhw.highest_committed_journal_seq = 0;
    w.jhw.kek_key_id = p.kek_key_id;

    return w;
}

// ===========================================================================
// Injections — one per rule, mutating exactly that rule's (first) field.
// Every file stays individually MAC-valid: after the mutation, all
// DEPENDENT derived fields (M's digests/macs over L/V bytes, C's digests
// over L/V/M bytes, A's digest_C over C bytes, x1[s+1].prev_transition_mac
// over x1[s]'s mac) are re-derived EXCEPT the injected one, so a fixture
// violates exactly the rule(s) reading the mutated field.
// ===========================================================================

// Mutators are captureless (InjectionSpec stores them as plain function
// pointers); per-x1-frame injections carry frame/field in the spec and all
// share one generic mutator. field is std::string, not string_view -- the
// field-name array lives inside build_injections() and must not outlive it
// as a dangling view.
using Mutator = void (*)(Wires&, std::size_t, std::string_view) noexcept;

struct InjectionSpec {
    Scenario scenario;
    std::size_t frame{0};
    std::string field{};
    Mutator mutate;
};

// Generic per-x1-frame field mutation, dispatched by field name.
void apply_x1_field_mutation(Wires& w, std::size_t frame, std::string_view field) noexcept {
    CompactionIntentTransitionWire& t = w.x1[frame];
    if (field == "store_uuid_lo") {
        t.store_uuid_lo ^= 0x8000ull;
    } else if (field == "store_uuid_hi") {
        t.store_uuid_hi ^= 0x8000ull;
    } else if (field == "kek_key_id") {
        t.kek_key_id = kAuditSecondaryKeyId;
    } else if (field == "source_generation") {
        t.source_generation += 1u;
    } else if (field == "target_generation") {
        t.target_generation += 1u;
    } else if (field == "baseline_tip_seq") {
        t.baseline_tip_seq += 1ull;
    } else if (field == "baseline_tip_mac") {
        t.baseline_tip_mac[0] ^= 0x01;
    } else if (field == "baseline_key_id") {
        t.baseline_key_id += 1u;
    } else if (field == "build_nonce") {
        t.build_nonce += 1ull;
    } else if (field == "candidate_id") {
        t.candidate_id += 100ull;
    } else if (field == "request_id") {
        t.request_id += 100ull;
    }
}

std::map<std::string, InjectionSpec> build_injections() {
    using S = Scenario;
    std::map<std::string, InjectionSpec> inj;

    const auto reg = [&inj](std::string_view id, Scenario sc, Mutator m) {
        inj.emplace(std::string(id), InjectionSpec{sc, 0, std::string{}, m});
    };

    // --- store identity ---
    reg("L.store_uuid_lo==V.store_uuid_lo", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.v.store_uuid_lo ^= 0x8000ull; });
    reg("L.store_uuid_hi==V.store_uuid_hi", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.v.store_uuid_hi ^= 0x8000ull; });
    reg("L.store_uuid_lo==M.store_uuid_lo", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.m.store_uuid_lo ^= 0x8000ull; });
    reg("L.store_uuid_hi==M.store_uuid_hi", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.m.store_uuid_hi ^= 0x8000ull; });
    reg("L.store_uuid_lo==C.store_uuid_lo", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.store_uuid_lo ^= 0x8000ull; });
    reg("L.store_uuid_hi==C.store_uuid_hi", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.store_uuid_hi ^= 0x8000ull; });
    reg("L.store_uuid_lo==A.store_uuid_lo", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.store_uuid_lo ^= 0x8000ull; });
    reg("L.store_uuid_hi==A.store_uuid_hi", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.store_uuid_hi ^= 0x8000ull; });
    reg("L.store_uuid_lo==Intent.store_uuid_lo", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.intent.store_uuid_lo ^= 0x8000ull; });
    reg("L.store_uuid_hi==Intent.store_uuid_hi", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.intent.store_uuid_hi ^= 0x8000ull; });
    reg("L.store_uuid_lo==WM.store_uuid_lo", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.wm.store_uuid_lo ^= 0x8000ull; });
    reg("L.store_uuid_hi==WM.store_uuid_hi", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.wm.store_uuid_hi ^= 0x8000ull; });

    // --- id binding ---
    reg("L.candidate_id==V.candidate_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.v.candidate_id += 100ull; });
    reg("L.candidate_id==M.candidate_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.m.candidate_id += 100ull; });
    reg("L.candidate_id==C.candidate_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.candidate_id += 100ull; });
    reg("L.candidate_id==A.candidate_id", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.candidate_id += 100ull; });
    reg("L.request_id==V.request_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.v.request_id += 100ull; });
    reg("L.request_id==M.request_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.m.request_id += 100ull; });
    reg("L.request_id==C.request_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.request_id += 100ull; });
    reg("L.request_id==A.request_id", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.request_id += 100ull; });

    // --- KEK bindings (mutation also re-keys the file; ring holds key 8) ---
    reg("M.legacy_kek_key_id==L.kek_key_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.l.kek_key_id = kAuditSecondaryKeyId; });
    reg("M.v2_kek_key_id==V.kek_key_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.v.kek_key_id = kAuditSecondaryKeyId; });

    // --- M file-integrity ---
    reg("M.legacy_file_digest==SHA256(L)", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.m.legacy_file_digest[0] ^= 0x01; });
    reg("M.v2_file_digest==SHA256(V)", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.m.v2_file_digest[0] ^= 0x01; });
    reg("M.legacy_mac==L.mac", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.m.legacy_mac[0] ^= 0x01; });
    reg("M.v2_mac==V.mac", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.m.v2_mac[0] ^= 0x01; });

    // --- C PostSealCommittedProof ---
    reg("C.source_generation==L.source_generation", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept {
            w.c.source_generation += 1u;  // shape requires new_generation == source+1
            w.c.new_generation += 1u;
        });
    reg("C.baseline_tip_seq==L.baseline_tip_seq", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.baseline_tip_seq += 1ull; });
    reg("C.baseline_tip_mac==L.baseline_tip_mac", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.baseline_tip_mac[0] ^= 0x01; });
    reg("C.baseline_key_id==L.baseline_key_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.baseline_key_id += 1u; });
    reg("C.new_generation==L.new_generation", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept {
            w.c.new_generation += 1u;  // shape requires new_generation == source+1
            w.c.source_generation += 1u;
        });
    reg("C.new_final_seq==L.new_final_seq", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.new_final_seq += 1ull; });
    reg("C.new_final_tip_mac==L.new_final_tip_mac", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.new_final_tip_mac[0] ^= 0x01; });
    reg("C.new_key_id==L.new_key_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.new_key_id += 1u; });
    reg("C.content_root==L.content_root", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.content_root[0] ^= 0x01; });

    // --- C digests ---
    reg("C.digest_L==SHA256(L)", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.digest_L[0] ^= 0x01; });
    reg("C.digest_V==SHA256(V)", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.digest_V[0] ^= 0x01; });
    reg("C.digest_M==SHA256(M)", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.c.digest_M[0] ^= 0x01; });

    // --- A bind fields ---
    reg("A.source_generation==L.source_generation", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.source_generation += 1u; });
    reg("A.baseline_tip_seq==L.baseline_tip_seq", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.baseline_tip_seq += 1ull; });
    reg("A.baseline_tip_mac==L.baseline_tip_mac", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.baseline_tip_mac[0] ^= 0x01; });
    reg("A.baseline_key_id==L.baseline_key_id", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.baseline_key_id += 1u; });
    reg("A.content_root==L.content_root", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.content_root[0] ^= 0x01; });
    reg("A.digest_C==SHA256(C)", S::ClrAbandonMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.a.digest_C[0] ^= 0x01; });

    // --- watermark range ---
    reg("Intent.candidate_id<WM.next_candidate_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.wm.next_candidate_id = w.intent.candidate_id; });
    reg("Intent.request_id<WM.next_request_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.wm.next_request_id = w.intent.request_id; });
    reg("L.candidate_id<WM.next_candidate_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.wm.next_candidate_id = w.l.candidate_id; });
    reg("L.request_id<WM.next_request_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.wm.next_request_id = w.l.request_id; });

    // --- Intent <-> L binding ---
    reg("Intent.candidate_id==L.candidate_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.intent.candidate_id += 1000ull; });
    reg("Intent.request_id==L.request_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.intent.request_id += 1000ull; });
    reg("Intent.baseline_tip_seq==L.baseline_tip_seq", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.intent.baseline_tip_seq += 1ull; });
    reg("Intent.baseline_tip_mac==L.baseline_tip_mac", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.intent.baseline_tip_mac[0] ^= 0x01; });
    reg("Intent.baseline_key_id==L.baseline_key_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.intent.baseline_key_id += 1u; });
    reg("Intent.source_generation==L.source_generation", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept {
            w.intent.source_generation += 1u;
            w.intent.target_generation += 1u;
        });
    reg("Intent.target_generation==L.new_generation", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept {
            w.intent.target_generation += 1u;
            w.intent.source_generation += 1u;
        });
    // L-side injections (L bytes feed M's and C's derived fields; those are
    // re-derived after the mutation, so only the direct field rules fire).
    reg("L.baseline_tip_seq==Intent.baseline_tip_seq", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.l.baseline_tip_seq += 1ull; });
    reg("L.baseline_tip_mac==Intent.baseline_tip_mac", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.l.baseline_tip_mac[0] ^= 0x01; });
    reg("L.baseline_key_id==Intent.baseline_key_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.l.baseline_key_id += 1u; });
    reg("L.source_generation==Intent.source_generation", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept {
            w.l.source_generation += 1u;  // L shape requires new_generation == source+1
            w.l.new_generation += 1u;
        });
    reg("L.new_generation==Intent.target_generation", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept {
            w.l.new_generation += 1u;
            w.l.source_generation += 1u;
        });

    // --- x1 bind per frame ---
    const std::string field_ids[] = {"store_uuid_lo", "store_uuid_hi", "kek_key_id", "source_generation",
                                     "target_generation", "baseline_tip_seq", "baseline_tip_mac",
                                     "baseline_key_id", "build_nonce", "candidate_id", "request_id"};
    for (std::size_t frame = 0; frame < 3; ++frame) {
        for (std::string_view field : field_ids) {
            const std::string id = "X1[" + std::to_string(frame + 1) + "]." + std::string(field) +
                                   "==Intent." + std::string(field);
            inj.emplace(id,
                        InjectionSpec{S::PostSealSuccessMigrated, frame, std::string(field),
                                      &apply_x1_field_mutation});
        }
    }

    // --- x1 chain ---
    reg("X1[1].prev_transition_mac==ZERO", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.x1[0].prev_transition_mac[0] = 0x01; });
    reg("X1[2].prev_transition_mac==X1[1].mac", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.x1[1].prev_transition_mac[0] ^= 0x01; });
    reg("X1[3].prev_transition_mac==X1[2].mac", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.x1[2].prev_transition_mac[0] ^= 0x01; });
    reg("X1[3].to_phase==Intent.phase", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.x1[2].to_phase = kCompactionCandidateIntentPhaseAbandonFinalizing; });

    // --- .jhw bindings ---
    reg("JHW.store_uuid_lo==Intent.store_uuid_lo", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.jhw.store_uuid_lo ^= 0x8000ull; });
    reg("JHW.store_uuid_hi==Intent.store_uuid_hi", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.jhw.store_uuid_hi ^= 0x8000ull; });
    reg("JHW.candidate_id==Intent.candidate_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept {
            w.jhw.candidate_id += 100ull;  // stays nonzero (shape), file name still follows Intent
        });
    reg("JHW.kek_key_id==Intent.kek_key_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept {
            w.jhw.kek_key_id = kAuditSecondaryKeyId;  // re-keys the file; ring holds key 8
        });
    // The strict +1 rule: shift next_candidate_id by one -- the `<` range
    // rules still pass (Intent/L ids remain within the allocated range), so
    // this violation is detected by the strict rule and no other.
    reg("JHW.candidate_id+1==WM.next_candidate_id", S::PostSealSuccessMigrated,
        [](Wires& w, std::size_t, std::string_view) noexcept { w.wm.next_candidate_id += 1ull; });

    return inj;
}

// Builds the fixture directory. `injection_id` empty = clean consistent set.
// Breadcrumb files go to `dir`; the .jhw commit watermark goes to
// `seal_journal_dir` (the separate directory scope, see header).
bool build_fixture(const std::filesystem::path& dir, const std::filesystem::path& seal_journal_dir,
                   const FixtureParams& p, std::string_view injection_id, KeyRing& ring) {
    Wires w = make_wires(p);

    static const std::map<std::string, InjectionSpec> injections = build_injections();
    const auto injection = injections.find(std::string(injection_id));
    if (!injection_id.empty() && injection == injections.end()) return false;
    if (injection != injections.end()) {
        injection->second.mutate(w, injection->second.frame, injection->second.field);
    }

    KeyHolder kh(ring);

    std::array<std::byte, kSealIdWatermarkWireBytes> wm_b{};
    encode_seal_id_watermark_wire(wm_b, w.wm, kh.get(kAuditWatermarkKeyId));

    std::array<std::byte, kCompactionCandidateIntentWireBytes> intent_b{};
    encode_compaction_candidate_intent_wire(intent_b, w.intent, kh.get(w.intent.kek_key_id));

    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> jhw_b{};
    encode_seal_journal_commit_watermark_wire(jhw_b, w.jhw, kh.get(w.jhw.kek_key_id));

    std::array<std::array<std::byte, kCompactionIntentTransitionWireBytes>, 3> x1_b{};
    encode_compaction_intent_transition_wire(x1_b[0], w.x1[0], kh.get(w.x1[0].kek_key_id));
    for (std::size_t s = 1; s < 3; ++s) {
        const std::string prev_rule =
            "X1[" + std::to_string(s + 1) + "].prev_transition_mac==X1[" + std::to_string(s) + "].mac";
        if (std::string(injection_id) != prev_rule) {
            std::memcpy(w.x1[s].prev_transition_mac, x1_b[s - 1].data() + kCompactionIntentTransitionWireBytes - 32,
                        32);
        }
        encode_compaction_intent_transition_wire(x1_b[s], w.x1[s], kh.get(w.x1[s].kek_key_id));
    }

    std::array<std::byte, kSealExportStartedWireBytes> l_b{};
    encode_seal_export_started_wire(l_b, w.l, kh.get(w.l.kek_key_id));
    std::array<std::byte, kSealExportStartedWireBytes> v_b{};
    encode_seal_export_started_wire(v_b, w.v, kh.get(w.v.kek_key_id));

    const auto l_digest = crypto::sha256(std::span<const std::byte>(l_b));
    const auto v_digest = crypto::sha256(std::span<const std::byte>(v_b));
    if (injection_id != "M.legacy_file_digest==SHA256(L)") {
        std::memcpy(w.m.legacy_file_digest, l_digest.bytes.data(), 32);
    }
    if (injection_id != "M.v2_file_digest==SHA256(V)") {
        std::memcpy(w.m.v2_file_digest, v_digest.bytes.data(), 32);
    }
    if (injection_id != "M.legacy_mac==L.mac") {
        std::memcpy(w.m.legacy_mac, l_b.data() + kSealExportStartedWireBytes - 32, 32);
    }
    if (injection_id != "M.v2_mac==V.mac") {
        std::memcpy(w.m.v2_mac, v_b.data() + kSealExportStartedWireBytes - 32, 32);
    }
    std::array<std::byte, kSealExportStartedMigrationWireBytes> m_b{};
    encode_seal_export_started_migration_wire(m_b, w.m, kh.get(w.m.v2_kek_key_id));

    if (injection_id != "C.digest_L==SHA256(L)") {
        std::memcpy(w.c.digest_L, l_digest.bytes.data(), 32);
    }
    if (injection_id != "C.digest_V==SHA256(V)") {
        std::memcpy(w.c.digest_V, v_digest.bytes.data(), 32);
    }
    if (injection_id != "C.digest_M==SHA256(M)") {
        const auto m_digest = crypto::sha256(std::span<const std::byte>(m_b));
        std::memcpy(w.c.digest_M, m_digest.bytes.data(), 32);
    }
    std::array<std::byte, kSealStartedCleanupWireBytes> c_b{};
    encode_seal_started_cleanup_tombstone_wire(c_b, w.c, kh.get(w.c.kek_key_id));

    if (injection_id != "A.digest_C==SHA256(C)") {
        const auto c_digest = crypto::sha256(std::span<const std::byte>(c_b));
        std::memcpy(w.a.digest_C, c_digest.bytes.data(), 32);
    }
    std::array<std::byte, kSealStartedAbandonWireBytes> a_b{};
    encode_seal_started_abandon_wire(a_b, w.a, kh.get(w.a.kek_key_id));

    const bool with_migration = (p.scenario != Scenario::NativeV2StartedPublished);
    const bool with_cleanup = (p.scenario == Scenario::PostSealSuccessMigrated ||
                               p.scenario == Scenario::ClrAbandonMigrated);
    const bool with_abandon = (p.scenario == Scenario::ClrAbandonMigrated);
    const std::size_t frame_count = (p.scenario == Scenario::NativeV2StartedPublished) ? 2u : 3u;

    // A filesystem error (e.g. an invalid character in the target path) must
    // surface as a failed build, never as an uncaught exception in a tool.
    try {
        std::filesystem::create_directories(dir);
        std::filesystem::create_directories(seal_journal_dir);
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
    bool ok = true;
    ok = ok && write_file(dir, kWatermarkName, wm_b);
    ok = ok && write_file(dir, kIntentName, intent_b);
    for (std::size_t s = 0; s < frame_count; ++s) {
        ok = ok && write_file(dir, x1_file_name(w.intent.build_nonce, static_cast<std::uint32_t>(s + 1)), x1_b[s]);
    }
    ok = ok && write_file(dir, kLName, l_b);
    if (with_migration) {
        ok = ok && write_file(dir, kVName, v_b);
        ok = ok && write_file(dir, kMName, m_b);
    }
    if (with_cleanup) ok = ok && write_file(dir, kCName, c_b);
    if (with_abandon) ok = ok && write_file(dir, kAName, a_b);
    // File name follows the INTENT's candidate_id (not w.jhw.candidate_id) so
    // the candidate_id injection still lands in the file the scanner selects.
    ok = ok && write_file(seal_journal_dir, jhw_file_name(w.intent.candidate_id), jhw_b);
    return ok;
}

// ===========================================================================
// Audit report
// ===========================================================================

struct AuditOutcome {
    int verified{0};
    int violated{0};
    int unverifiable{0};
    std::vector<std::string> violated_ids;
};

AuditOutcome run_rules(const BreadcrumbSet& set, const std::vector<Rule>& rules) {
    AuditOutcome out;
    for (const Rule& r : rules) {
        const RuleResult res = r.eval(set, r.idx);
        switch (res.status) {
            case RuleStatus::Verified:
                ++out.verified;
                break;
            case RuleStatus::Violated:
                ++out.violated;
                out.violated_ids.push_back(r.id);
                break;
            case RuleStatus::Unverifiable:
                ++out.unverifiable;
                break;
        }
    }
    return out;
}

void print_audit(const std::filesystem::path& dir, const std::filesystem::path& seal_journal_dir,
                 const BreadcrumbSet& s, const std::vector<Rule>& rules, bool verbose) {
    std::printf("== breadcrumb directory audit: %s ==\n", dir.string().c_str());
    std::printf("   (seal-journal dir: %s)\n", seal_journal_dir.string().c_str());
    std::printf("files:\n");
    const auto print_file = [](std::string_view name, const FileRef& f) {
        if (!f.present) {
            std::printf("  %-42s : absent\n", std::string(name).c_str());
        } else if (f.decoded) {
            std::printf("  %-42s : ok (MAC + shape verified)\n", std::string(name).c_str());
        } else {
            std::printf("  %-42s : present but undecodable (%s)\n", std::string(name).c_str(), f.note.c_str());
        }
    };
    print_file(kWatermarkName, s.wm);
    print_file(kIntentName, s.intent);
    if (s.intent.decoded) {
        for (std::size_t i = 0; i < 3; ++i) {
            print_file(x1_file_name(s.intent_wire.build_nonce, static_cast<std::uint32_t>(i + 1)), s.x1[i]);
        }
    }
    print_file(kLName, s.l);
    print_file(kVName, s.v);
    print_file(kMName, s.m);
    print_file(kCName, s.c);
    print_file(kAName, s.a);
    if (s.jhw_file_name.empty()) {
        std::printf("  %-42s : absent (no Intent candidate_id to select)\n",
                    "<candidate_id_hex16>.jhw");
    } else {
        print_file(s.jhw_file_name, s.jhw);
    }

    if (s.intent.decoded) {
        std::array<CompactionIntentTransitionWire, 3> frames{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            if (s.x1[i].decoded) {
                frames[count] = s.x1_wire[i];
                ++count;
            }
        }
        if (count > 0) {
            std::array<std::uint8_t, 32> terminal{};
            const auto chain = compaction_codec_detail::walk_x1_chain_raw(
                s.intent_wire, std::span<const CompactionIntentTransitionWire>(frames.data(), count), terminal);
            std::printf("x1 chain (walk_x1_chain_raw): %s\n", [chain]() {
                switch (chain) {
                    case X1ChainStatus::Valid: return "Valid";
                    case X1ChainStatus::Empty: return "Empty";
                    case X1ChainStatus::Gap: return "Gap";
                    case X1ChainStatus::ForeignBinding: return "ForeignBinding";
                    case X1ChainStatus::IllegalEdge: return "IllegalEdge";
                    case X1ChainStatus::MacChainBroken: return "MacChainBroken";
                    case X1ChainStatus::TerminalBranchConflict: return "TerminalBranchConflict";
                    case X1ChainStatus::TooManyFrames: return "TooManyFrames";
                    default: return "?";
                }
            }());
        }
    }

    std::printf("rules (%zu):\n", rules.size());
    for (const Rule& r : rules) {
        const RuleResult res = r.eval(s, r.idx);
        const char* tag = (res.status == RuleStatus::Verified)
                              ? "[VERIFIED]"
                              : (res.status == RuleStatus::Violated ? "[VIOLATED]" : "[N/A     ]");
        if (!verbose && res.status == RuleStatus::Verified) continue;
        std::printf("  %s %-56s %s\n", tag, r.id.c_str(), res.detail.c_str());
    }
}

// ===========================================================================
// Self-check: clean fixtures + per-rule injection matrix
// ===========================================================================

std::filesystem::path make_temp_dir(std::string_view tag) {
    static std::uint64_t counter = 0;
    const auto base = std::filesystem::temp_directory_path() /
                      ("hy_cross_file_audit_" + std::string(tag) + "_" +
                       std::to_string(
                           static_cast<unsigned long long>(
                               std::chrono::steady_clock::now().time_since_epoch().count())) +
                       "_" + std::to_string(counter++));
    std::filesystem::remove_all(base);
    return base;
}

bool check_clean_scenario(const std::filesystem::path& base, Scenario sc, const std::vector<Rule>& rules,
                          KeyRing& ring, const char* label) {
    FixtureParams p;
    p.scenario = sc;
    const auto dir = base / sanitize_dir_component(label);
    const auto sj = dir / kSealJournalDirName;
    if (!build_fixture(dir, sj, p, "", ring)) {
        std::printf("[FAIL] %s: fixture build failed\n", label);
        return false;
    }
    const BreadcrumbSet s = scan_with_seal_journal(dir, sj, ring);
    const AuditOutcome out = run_rules(s, rules);
    const bool ok = (out.violated == 0);
    std::printf("[%s] %s clean fixture: %d verified, %d violated, %d N/A\n", ok ? "PASS" : "FAIL", label,
                out.verified, out.violated, out.unverifiable);
    if (!ok) {
        for (const auto& id : out.violated_ids) std::printf("      unexpected violation: %s\n", id.c_str());
    }
    return ok;
}

bool check_injection(const std::filesystem::path& base, const std::vector<Rule>& rules,
                     const std::map<std::string, InjectionSpec>& injections,
                     const std::map<std::string, std::vector<std::string>>& expected, KeyRing& ring,
                     const std::string& rule_id) {
    const auto inj_it = injections.find(rule_id);
    const auto exp_it = expected.find(rule_id);
    if (inj_it == injections.end() || exp_it == expected.end()) {
        std::printf("[FAIL] %s: missing injection or expected-set entry\n", rule_id.c_str());
        return false;
    }
    FixtureParams p;
    p.scenario = inj_it->second.scenario;
    const auto dir = base / sanitize_dir_component(rule_id);
    const auto sj = dir / kSealJournalDirName;
    if (!build_fixture(dir, sj, p, rule_id, ring)) {
        std::printf("[FAIL] %s: fixture build failed\n", rule_id.c_str());
        return false;
    }
    const BreadcrumbSet s = scan_with_seal_journal(dir, sj, ring);
    const AuditOutcome out = run_rules(s, rules);

    std::vector<std::string> got = out.violated_ids;
    std::sort(got.begin(), got.end());
    std::vector<std::string> want = exp_it->second;
    std::sort(want.begin(), want.end());
    const bool ok = (got == want);
    if (ok) {
        std::string joined;
        for (std::size_t i = 0; i < want.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += want[i];
        }
        std::printf("[PASS] %-56s violated exactly {%s}\n", rule_id.c_str(), joined.c_str());
    } else {
        std::printf("[FAIL] %s\n      expected: {", rule_id.c_str());
        for (std::size_t i = 0; i < want.size(); ++i) std::printf("%s%s", i ? ", " : "", want[i].c_str());
        std::printf("}\n      got:      {");
        for (std::size_t i = 0; i < got.size(); ++i) std::printf("%s%s", i ? ", " : "", got[i].c_str());
        std::printf("}\n");
    }
    return ok;
}

int self_check() {
    const auto kek = std::span<const std::byte, kKekSize>(kAuditKek);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ring.add_key(kAuditPrimaryKeyId, kAuditPlaintextKey7, rec);
    ring.add_key(kAuditSecondaryKeyId, kAuditPlaintextKey8, rec);
    const auto rules = build_rules();
    const auto injections = build_injections();

    bool all_ok = true;
    const auto base = make_temp_dir("selfcheck");

    all_ok &= check_clean_scenario(base, Scenario::PostSealSuccessMigrated, rules, ring, "S1-success");
    all_ok &= check_clean_scenario(base, Scenario::ClrAbandonMigrated, rules, ring, "S2-abandon");
    all_ok &= check_clean_scenario(base, Scenario::NativeV2StartedPublished, rules, ring, "S3-native");

    // empty directory: every rule must report N/A (or verified-vacuously),
    // zero violations, no crash.
    {
        const auto dir = base / "empty";
        std::filesystem::create_directories(dir);
        const BreadcrumbSet s = scan_with_seal_journal(dir, dir / kSealJournalDirName, ring);
        const AuditOutcome out = run_rules(s, rules);
        const bool ok = (out.violated == 0);
        all_ok &= ok;
        std::printf("[%s] empty directory: %d verified, %d violated, %d N/A\n", ok ? "PASS" : "FAIL",
                    out.verified, out.violated, out.unverifiable);
    }

    // Expected violated sets (rule id -> ids that MUST be reported violated
    // for that injection, and NO others). Each is hand-derived from the rule
    // table above: mutating field X of file F breaks exactly the rules that
    // read that field (dependent derived fields are re-derived, not broken).
    std::map<std::string, std::vector<std::string>> expected;
    const auto exp = [&expected](std::string_view id, std::initializer_list<std::string_view> ids) {
        expected.emplace(std::string(id),
                         std::vector<std::string>(ids.begin(), ids.end()));
    };

    exp("L.store_uuid_lo==V.store_uuid_lo", {"L.store_uuid_lo==V.store_uuid_lo"});
    exp("L.store_uuid_hi==V.store_uuid_hi", {"L.store_uuid_hi==V.store_uuid_hi"});
    exp("L.store_uuid_lo==M.store_uuid_lo", {"L.store_uuid_lo==M.store_uuid_lo"});
    exp("L.store_uuid_hi==M.store_uuid_hi", {"L.store_uuid_hi==M.store_uuid_hi"});
    exp("L.store_uuid_lo==C.store_uuid_lo", {"L.store_uuid_lo==C.store_uuid_lo"});
    exp("L.store_uuid_hi==C.store_uuid_hi", {"L.store_uuid_hi==C.store_uuid_hi"});
    exp("L.store_uuid_lo==A.store_uuid_lo", {"L.store_uuid_lo==A.store_uuid_lo"});
    exp("L.store_uuid_hi==A.store_uuid_hi", {"L.store_uuid_hi==A.store_uuid_hi"});
    exp("L.store_uuid_lo==Intent.store_uuid_lo",
        {"JHW.store_uuid_lo==Intent.store_uuid_lo", "L.store_uuid_lo==Intent.store_uuid_lo",
         "X1[1].store_uuid_lo==Intent.store_uuid_lo", "X1[2].store_uuid_lo==Intent.store_uuid_lo",
         "X1[3].store_uuid_lo==Intent.store_uuid_lo"});
    exp("L.store_uuid_hi==Intent.store_uuid_hi",
        {"JHW.store_uuid_hi==Intent.store_uuid_hi", "L.store_uuid_hi==Intent.store_uuid_hi",
         "X1[1].store_uuid_hi==Intent.store_uuid_hi", "X1[2].store_uuid_hi==Intent.store_uuid_hi",
         "X1[3].store_uuid_hi==Intent.store_uuid_hi"});
    exp("L.store_uuid_lo==WM.store_uuid_lo", {"L.store_uuid_lo==WM.store_uuid_lo"});
    exp("L.store_uuid_hi==WM.store_uuid_hi", {"L.store_uuid_hi==WM.store_uuid_hi"});

    exp("L.candidate_id==V.candidate_id", {"L.candidate_id==V.candidate_id"});
    exp("L.candidate_id==M.candidate_id", {"L.candidate_id==M.candidate_id"});
    exp("L.candidate_id==C.candidate_id", {"L.candidate_id==C.candidate_id"});
    exp("L.candidate_id==A.candidate_id", {"L.candidate_id==A.candidate_id"});
    exp("L.request_id==V.request_id", {"L.request_id==V.request_id"});
    exp("L.request_id==M.request_id", {"L.request_id==M.request_id"});
    exp("L.request_id==C.request_id", {"L.request_id==C.request_id"});
    exp("L.request_id==A.request_id", {"L.request_id==A.request_id"});

    exp("M.legacy_kek_key_id==L.kek_key_id", {"M.legacy_kek_key_id==L.kek_key_id"});
    exp("M.v2_kek_key_id==V.kek_key_id", {"M.v2_kek_key_id==V.kek_key_id"});

    exp("M.legacy_file_digest==SHA256(L)", {"M.legacy_file_digest==SHA256(L)"});
    exp("M.v2_file_digest==SHA256(V)", {"M.v2_file_digest==SHA256(V)"});
    exp("M.legacy_mac==L.mac", {"M.legacy_mac==L.mac"});
    exp("M.v2_mac==V.mac", {"M.v2_mac==V.mac"});

    exp("C.source_generation==L.source_generation",
        {"C.source_generation==L.source_generation", "C.new_generation==L.new_generation"});
    exp("C.baseline_tip_seq==L.baseline_tip_seq", {"C.baseline_tip_seq==L.baseline_tip_seq"});
    exp("C.baseline_tip_mac==L.baseline_tip_mac", {"C.baseline_tip_mac==L.baseline_tip_mac"});
    exp("C.baseline_key_id==L.baseline_key_id", {"C.baseline_key_id==L.baseline_key_id"});
    exp("C.new_generation==L.new_generation",
        {"C.source_generation==L.source_generation", "C.new_generation==L.new_generation"});
    exp("C.new_final_seq==L.new_final_seq", {"C.new_final_seq==L.new_final_seq"});
    exp("C.new_final_tip_mac==L.new_final_tip_mac", {"C.new_final_tip_mac==L.new_final_tip_mac"});
    exp("C.new_key_id==L.new_key_id", {"C.new_key_id==L.new_key_id"});
    exp("C.content_root==L.content_root", {"C.content_root==L.content_root"});

    exp("C.digest_L==SHA256(L)", {"C.digest_L==SHA256(L)"});
    exp("C.digest_V==SHA256(V)", {"C.digest_V==SHA256(V)"});
    exp("C.digest_M==SHA256(M)", {"C.digest_M==SHA256(M)"});

    exp("A.source_generation==L.source_generation", {"A.source_generation==L.source_generation"});
    exp("A.baseline_tip_seq==L.baseline_tip_seq", {"A.baseline_tip_seq==L.baseline_tip_seq"});
    exp("A.baseline_tip_mac==L.baseline_tip_mac", {"A.baseline_tip_mac==L.baseline_tip_mac"});
    exp("A.baseline_key_id==L.baseline_key_id", {"A.baseline_key_id==L.baseline_key_id"});
    exp("A.content_root==L.content_root", {"A.content_root==L.content_root"});
    exp("A.digest_C==SHA256(C)", {"A.digest_C==SHA256(C)"});

    exp("Intent.candidate_id<WM.next_candidate_id",
        {"Intent.candidate_id<WM.next_candidate_id", "L.candidate_id<WM.next_candidate_id",
         "JHW.candidate_id+1==WM.next_candidate_id"});
    exp("Intent.request_id<WM.next_request_id",
        {"Intent.request_id<WM.next_request_id", "L.request_id<WM.next_request_id"});
    exp("L.candidate_id<WM.next_candidate_id",
        {"Intent.candidate_id<WM.next_candidate_id", "L.candidate_id<WM.next_candidate_id",
         "JHW.candidate_id+1==WM.next_candidate_id"});
    exp("L.request_id<WM.next_request_id",
        {"Intent.request_id<WM.next_request_id", "L.request_id<WM.next_request_id"});

    exp("Intent.candidate_id==L.candidate_id",
        {"Intent.candidate_id<WM.next_candidate_id", "Intent.candidate_id==L.candidate_id",
         "JHW.candidate_id==Intent.candidate_id",
         "X1[1].candidate_id==Intent.candidate_id", "X1[2].candidate_id==Intent.candidate_id",
         "X1[3].candidate_id==Intent.candidate_id"});
    exp("Intent.request_id==L.request_id",
        {"Intent.request_id<WM.next_request_id", "Intent.request_id==L.request_id",
         "X1[1].request_id==Intent.request_id", "X1[2].request_id==Intent.request_id",
         "X1[3].request_id==Intent.request_id"});
    exp("Intent.baseline_tip_seq==L.baseline_tip_seq",
        {"Intent.baseline_tip_seq==L.baseline_tip_seq", "X1[1].baseline_tip_seq==Intent.baseline_tip_seq",
         "X1[2].baseline_tip_seq==Intent.baseline_tip_seq",
         "X1[3].baseline_tip_seq==Intent.baseline_tip_seq"});
    exp("Intent.baseline_tip_mac==L.baseline_tip_mac",
        {"Intent.baseline_tip_mac==L.baseline_tip_mac",
         "X1[1].baseline_tip_mac==Intent.baseline_tip_mac",
         "X1[2].baseline_tip_mac==Intent.baseline_tip_mac",
         "X1[3].baseline_tip_mac==Intent.baseline_tip_mac"});
    exp("Intent.baseline_key_id==L.baseline_key_id",
        {"Intent.baseline_key_id==L.baseline_key_id", "X1[1].baseline_key_id==Intent.baseline_key_id",
         "X1[2].baseline_key_id==Intent.baseline_key_id",
         "X1[3].baseline_key_id==Intent.baseline_key_id"});
    exp("Intent.source_generation==L.source_generation",
        {"Intent.source_generation==L.source_generation", "Intent.target_generation==L.new_generation",
         "X1[1].source_generation==Intent.source_generation",
         "X1[2].source_generation==Intent.source_generation",
         "X1[3].source_generation==Intent.source_generation",
         "X1[1].target_generation==Intent.target_generation",
         "X1[2].target_generation==Intent.target_generation",
         "X1[3].target_generation==Intent.target_generation"});
    exp("Intent.target_generation==L.new_generation",
        {"Intent.source_generation==L.source_generation", "Intent.target_generation==L.new_generation",
         "X1[1].source_generation==Intent.source_generation",
         "X1[2].source_generation==Intent.source_generation",
         "X1[3].source_generation==Intent.source_generation",
         "X1[1].target_generation==Intent.target_generation",
         "X1[2].target_generation==Intent.target_generation",
         "X1[3].target_generation==Intent.target_generation"});
    exp("L.baseline_tip_seq==Intent.baseline_tip_seq",
        {"Intent.baseline_tip_seq==L.baseline_tip_seq", "C.baseline_tip_seq==L.baseline_tip_seq"});
    exp("L.baseline_tip_mac==Intent.baseline_tip_mac",
        {"Intent.baseline_tip_mac==L.baseline_tip_mac", "C.baseline_tip_mac==L.baseline_tip_mac"});
    exp("L.baseline_key_id==Intent.baseline_key_id",
        {"Intent.baseline_key_id==L.baseline_key_id", "C.baseline_key_id==L.baseline_key_id"});
    exp("L.source_generation==Intent.source_generation",
        {"Intent.source_generation==L.source_generation", "Intent.target_generation==L.new_generation",
         "C.source_generation==L.source_generation", "C.new_generation==L.new_generation"});
    exp("L.new_generation==Intent.target_generation",
        {"Intent.target_generation==L.new_generation", "Intent.source_generation==L.source_generation",
         "C.source_generation==L.source_generation", "C.new_generation==L.new_generation"});

    const std::string field_ids[] = {"store_uuid_lo", "store_uuid_hi", "kek_key_id", "source_generation",
                                     "target_generation", "baseline_tip_seq", "baseline_tip_mac",
                                     "baseline_key_id", "build_nonce", "candidate_id", "request_id"};
    for (std::size_t f = 0; f < 3; ++f) {
        for (std::string_view field : field_ids) {
            const std::string id = "X1[" + std::to_string(f + 1) + "]." + std::string(field) +
                                   "==Intent." + std::string(field);
            exp(id, {id});
        }
    }
    exp("X1[1].prev_transition_mac==ZERO", {"X1[1].prev_transition_mac==ZERO"});
    exp("X1[2].prev_transition_mac==X1[1].mac", {"X1[2].prev_transition_mac==X1[1].mac"});
    exp("X1[3].prev_transition_mac==X1[2].mac", {"X1[3].prev_transition_mac==X1[2].mac"});
    exp("X1[3].to_phase==Intent.phase", {"X1[3].to_phase==Intent.phase"});

    exp("JHW.store_uuid_lo==Intent.store_uuid_lo", {"JHW.store_uuid_lo==Intent.store_uuid_lo"});
    exp("JHW.store_uuid_hi==Intent.store_uuid_hi", {"JHW.store_uuid_hi==Intent.store_uuid_hi"});
    exp("JHW.candidate_id==Intent.candidate_id",
        {"JHW.candidate_id==Intent.candidate_id", "JHW.candidate_id+1==WM.next_candidate_id"});
    exp("JHW.kek_key_id==Intent.kek_key_id", {"JHW.kek_key_id==Intent.kek_key_id"});
    exp("JHW.candidate_id+1==WM.next_candidate_id", {"JHW.candidate_id+1==WM.next_candidate_id"});

    std::size_t checked = 0;
    for (const auto& [rule_id, spec] : injections) {
        all_ok &= check_injection(base, rules, injections, expected, ring, rule_id);
        ++checked;
    }

    std::filesystem::remove_all(base);

    std::printf("self-check: %zu injections + 3 clean scenarios + empty dir checked\n", checked);
    return all_ok ? 0 : 1;
}

// ===========================================================================
// CLI
// ===========================================================================

bool parse_kek_hex(std::string_view hex, std::array<std::byte, kKekSize>& out) {
    if (hex.size() != kKekSize * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < kKekSize; ++i) {
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::byte>(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

void print_usage() {
    std::printf(
        "usage:\n"
        "  seal_journal_cross_file_audit [--self-check]\n"
        "  seal_journal_cross_file_audit --gen <dir> [--scenario success|abandon|native]\n"
        "                                 [--inject <rule-id>] [--kek-hex <64hex>]\n"
        "                                 [--seal-journal-dir <dir>]\n"
        "  seal_journal_cross_file_audit --audit <dir> [--kek-hex <64hex>]\n"
        "                                 [--seal-journal-dir <dir>]\n"
        "  seal_journal_cross_file_audit --list-rules\n"
        "  seal_journal_cross_file_audit --usage\n"
        "--seal-journal-dir defaults to <dir>/seal-journal (the layout --gen writes);\n"
        "without a scannable .jhw there, every JHW.* rule reports N/A.\n");
}

}  // namespace
}  // namespace hy

int main(int argc, char** argv) {
    using namespace hy;

    std::string mode = "self-check";
    std::string gen_dir;
    std::string audit_dir;
    std::string seal_journal_dir;
    std::string scenario_arg;
    std::string inject_arg;
    std::array<std::byte, kKekSize> kek = kAuditKek;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--self-check") {
            mode = "self-check";
        } else if (arg == "--gen") {
            mode = "gen";
            if (i + 1 < argc) gen_dir = argv[++i];
        } else if (arg == "--audit") {
            mode = "audit";
            if (i + 1 < argc) audit_dir = argv[++i];
        } else if (arg == "--seal-journal-dir") {
            if (i + 1 < argc) seal_journal_dir = argv[++i];
        } else if (arg == "--scenario") {
            if (i + 1 < argc) scenario_arg = argv[++i];
        } else if (arg == "--inject") {
            if (i + 1 < argc) inject_arg = argv[++i];
        } else if (arg == "--kek-hex") {
            if (i + 1 < argc && !parse_kek_hex(argv[++i], kek)) {
                std::printf("error: --kek-hex must be %zu hex characters\n", kKekSize * 2);
                return 2;
            }
        } else if (arg == "--list-rules") {
            mode = "list-rules";
        } else if (arg == "--usage" || arg == "-h" || arg == "--help") {
            hy::print_usage();
            return 0;
        } else {
            std::printf("error: unknown argument '%s'\n", arg.c_str());
            hy::print_usage();
            return 2;
        }
    }

    if (mode == "self-check") {
        return hy::self_check();
    }

    if (mode == "list-rules") {
        const auto rules = hy::build_rules();
        for (const Rule& r : rules) {
            std::printf("%-56s %s\n", r.id.c_str(), r.desc.c_str());
        }
        std::printf("total: %zu rules\n", rules.size());
        return 0;
    }

    if (mode == "gen") {
        if (gen_dir.empty()) {
            hy::print_usage();
            return 2;
        }
        Scenario sc = Scenario::PostSealSuccessMigrated;
        if (scenario_arg == "success") sc = Scenario::PostSealSuccessMigrated;
        else if (scenario_arg == "abandon") sc = Scenario::ClrAbandonMigrated;
        else if (scenario_arg == "native") sc = Scenario::NativeV2StartedPublished;
        else if (!scenario_arg.empty()) {
            std::printf("error: unknown --scenario '%s'\n", scenario_arg.c_str());
            return 2;
        }
        FixtureParams p;
        p.scenario = sc;
        KeyRing ring(kek);
        WrappedKeyRecord rec{};
        ring.add_key(kAuditPrimaryKeyId, kAuditPlaintextKey7, rec);
        ring.add_key(kAuditSecondaryKeyId, kAuditPlaintextKey8, rec);
        const std::filesystem::path sj_dir =
            seal_journal_dir.empty() ? std::filesystem::path(gen_dir) / kSealJournalDirName
                                     : std::filesystem::path(seal_journal_dir);
        if (!build_fixture(gen_dir, sj_dir, p, inject_arg, ring)) {
            std::printf("error: fixture build failed (unknown --inject id?)\n");
            return 2;
        }
        std::printf("fixture written to %s (scenario %s, injection '%s', seal-journal %s)\n",
                    gen_dir.c_str(), scenario_arg.empty() ? "success" : scenario_arg.c_str(),
                    inject_arg.empty() ? "-" : inject_arg.c_str(), sj_dir.string().c_str());
        return 0;
    }

    if (mode == "audit") {
        if (audit_dir.empty()) {
            hy::print_usage();
            return 2;
        }
        const auto rules = build_rules();
        KeyRing ring(kek);
        WrappedKeyRecord rec{};
        ring.add_key(kAuditPrimaryKeyId, kAuditPlaintextKey7, rec);
        ring.add_key(kAuditSecondaryKeyId, kAuditPlaintextKey8, rec);
        const std::filesystem::path sj_dir =
            seal_journal_dir.empty() ? std::filesystem::path(audit_dir) / kSealJournalDirName
                                     : std::filesystem::path(seal_journal_dir);
        const auto s = scan_with_seal_journal(audit_dir, sj_dir, ring);
        print_audit(audit_dir, sj_dir, s, rules, /*verbose=*/false);
        const AuditOutcome out = run_rules(s, rules);
        std::printf("summary: %d verified, %d violated, %d N/A\n", out.verified, out.violated, out.unverifiable);
        return out.violated > 0 ? 1 : 0;
    }

    return 2;
}
