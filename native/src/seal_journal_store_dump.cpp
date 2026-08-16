// SPDX-License-Identifier: proprietary
// seal_journal_store_dump.cpp — read-only overview tool for one
// seal-journal/<store_uuid_lo_hex16><store_uuid_hi_hex16>/ store directory.
// Lists every candidate's <candidate_id_hex16>.jhw (SealJournalCommitWatermark)
// and <candidate_id_hex16>-<journal_seq_hex16>.jts (SealJournalTombstoneWire)
// side by side, grouped per candidate, with a one-line summary.
//
// This answers a question the existing seal_journal_cross_file_audit.cpp does
// not: that tool audits the ONE .jhw selected by a breadcrumb directory's
// Intent; this tool lists ALL candidates' files in a whole store directory at
// once (a store legally holds MANY candidates' files side by side — see
// seal_journal_store_lease.hpp's header comment).
//
// Deliberately NOT a production code path: no CandidateLease, no
// SealJournalStoreLease, no lock files — plain std::filesystem::directory_iterator
// + std::ifstream, the same diagnostic-tool discipline as
// seal_journal_cross_file_audit.cpp ("a diagnostic tool must not pretend to
// be a production I/O path").
//
// CRYPTOGRAPHY CAVEAT: fixtures are MAC'd under this tool's OWN deterministic
// test key (HY-AUDIT-FIXTURE-KEK-1, byte-identical to the cross-file audit
// tool's), never under production key material. --kek-hex can override the
// KEK, but the ring still only ever holds this tool's own test plaintext keys
// (7 and 8). Files signed under any other key report "key not in ring". This
// is a fixture-analysis tool, not an operator diagnostic for live directories.
//
// Usage:
//   seal_journal_store_dump --dir <store directory> [--kek-hex <64hex>]
//
// Exit codes: 0 = scanned (regardless of per-file validity; decode failures
// are printed, not fatal), 2 = usage error, 3 = directory unreadable.

#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_journal_commit_tombstone_codec.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hy {
namespace {

// ===========================================================================
// Deterministic test key material — byte-identical to
// seal_journal_cross_file_audit.cpp's (see the CRYPTOGRAPHY CAVEAT above)
// ===========================================================================

constexpr std::uint32_t kDumpPrimaryKeyId = 7;
constexpr std::uint32_t kDumpSecondaryKeyId = 8;

constexpr std::array<std::byte, kKekSize> kDumpKek = {
    std::byte{'H'}, std::byte{'Y'}, std::byte{'-'}, std::byte{'A'}, std::byte{'U'}, std::byte{'D'},
    std::byte{'I'}, std::byte{'T'}, std::byte{'-'}, std::byte{'F'}, std::byte{'I'}, std::byte{'X'},
    std::byte{'T'}, std::byte{'U'}, std::byte{'R'}, std::byte{'E'}, std::byte{'-'}, std::byte{'K'},
    std::byte{'E'}, std::byte{'K'}, std::byte{'-'}, std::byte{'1'}, std::byte{'-'}, std::byte{'2'},
    std::byte{'0'}, std::byte{'2'}, std::byte{'6'}, std::byte{'-'}, std::byte{'0'}, std::byte{'8'},
    std::byte{'-'}, std::byte{'1'}};

constexpr std::array<std::byte, 32> kDumpPlaintextKey7 = {
    std::byte{'H'}, std::byte{'Y'}, std::byte{'-'}, std::byte{'A'}, std::byte{'U'}, std::byte{'D'},
    std::byte{'I'}, std::byte{'T'}, std::byte{'-'}, std::byte{'K'}, std::byte{'E'}, std::byte{'Y'},
    std::byte{'-'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'},
    std::byte{'0'}, std::byte{'0'}, std::byte{'7'}, std::byte{'-'}, std::byte{'f'}, std::byte{'i'},
    std::byte{'x'}, std::byte{'t'}, std::byte{'u'}, std::byte{'r'}, std::byte{'e'}, std::byte{'-'},
    std::byte{'7'}, std::byte{'7'}};

constexpr std::array<std::byte, 32> kDumpPlaintextKey8 = {
    std::byte{'H'}, std::byte{'Y'}, std::byte{'-'}, std::byte{'A'}, std::byte{'U'}, std::byte{'D'},
    std::byte{'I'}, std::byte{'T'}, std::byte{'-'}, std::byte{'K'}, std::byte{'E'}, std::byte{'Y'},
    std::byte{'-'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'},
    std::byte{'0'}, std::byte{'0'}, std::byte{'8'}, std::byte{'-'}, std::byte{'f'}, std::byte{'i'},
    std::byte{'x'}, std::byte{'t'}, std::byte{'u'}, std::byte{'r'}, std::byte{'e'}, std::byte{'-'},
    std::byte{'8'}, std::byte{'8'}};

// ===========================================================================
// Small helpers
// ===========================================================================

// Fixed-width 16-hex-digit lowercase formatting — duplicated from
// seal_journal_store_io.hpp / compaction_breadcrumb_io.hpp (the same
// trust-boundary reason: a diagnostic tool does not link production I/O).
void format_hex16(std::uint64_t v, std::array<char, 17>& out) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[v & 0xFu];
        v >>= 4;
    }
    out[16] = '\0';
}

// Parses exactly 16 lowercase/uppercase hex digits into a uint64. Returns
// false on any non-hex character or wrong length.
bool parse_hex16(std::string_view s, std::uint64_t& out) noexcept {
    if (s.size() != 16) return false;
    std::uint64_t v = 0;
    for (const char c : s) {
        int n = -1;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        if (n < 0) return false;
        v = (v << 4) | static_cast<std::uint64_t>(n);
    }
    out = v;
    return true;
}

bool ends_with(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

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

// Reads the whole file as bytes; empty on I/O failure. A diagnostic tool may
// allocate freely — the zero-alloc mandate is for the trading hot path.
std::vector<std::byte> read_file_bytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz < 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<std::byte> buf(static_cast<std::size_t>(sz));
    if (sz > 0 && !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz))) {
        return {};
    }
    return buf;
}

// ===========================================================================
// Decode + MAC classification (three-state: valid / invalid / decode failed)
// ===========================================================================

enum class WireStatus : std::uint8_t { Valid, Invalid, DecodeFailed };

const char* wire_status_name(WireStatus s) noexcept {
    switch (s) {
        case WireStatus::Valid: return "valid";
        case WireStatus::Invalid: return "invalid";
        case WireStatus::DecodeFailed: return "decode failed";
    }
    return "?";
}

// "decode failed" labels from the codec's status enum, minus Ok/ChecksumMismatch.
const char* decode_failure_name(SealJournalCommitTombstoneDecodeStatus st) noexcept {
    switch (st) {
        case SealJournalCommitTombstoneDecodeStatus::Truncated: return "truncated";
        case SealJournalCommitTombstoneDecodeStatus::UnknownVersion: return "unknown version";
        case SealJournalCommitTombstoneDecodeStatus::TotalBytesInvalid: return "total_bytes invalid";
        case SealJournalCommitTombstoneDecodeStatus::MalformedField: return "malformed field";
        default: return "decode failed";
    }
}

struct JhwDecode {
    SealJournalCommitWatermark wire{};
    WireStatus status{WireStatus::DecodeFailed};
    std::string note;
};

struct JtsDecode {
    SealJournalTombstoneWire wire{};
    WireStatus status{WireStatus::DecodeFailed};
    std::string note;
};

// peek kek_key_id -> pin that one key from the ring -> decode. Any failure
// becomes a printed status line, never an exception.
JhwDecode decode_jhw_file(std::span<const std::byte> bytes, KeyRing& ring) {
    JhwDecode out;
    if (bytes.size() < kSealJournalCommitWatermarkWireBytes) {
        out.note = "truncated";
        return out;
    }
    std::uint32_t key_id = 0;
    if (!peek_seal_journal_commit_watermark_kek_key_id(bytes, key_id)) {
        out.note = "peek failed";
        return out;
    }
    const PinResult pin = ring.pin_key(key_id);
    if (pin.status != PinStatus::Pinned) {
        out.status = WireStatus::Invalid;
        out.note = "key " + std::to_string(key_id) + " not in ring";
        return out;
    }
    std::optional<VerifiedSealJournalCommitWatermark> verified;
    const auto st = decode_seal_journal_commit_watermark_wire(bytes, pin.handle->key_bytes(), verified);
    if (st == SealJournalCommitTombstoneDecodeStatus::Ok) {
        out.wire = verified->value();
        out.status = WireStatus::Valid;
        out.note = "ok";
    } else if (st == SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch) {
        out.status = WireStatus::Invalid;
        out.note = "MAC mismatch";
    } else {
        out.note = decode_failure_name(st);
    }
    return out;
}

JtsDecode decode_jts_file(std::span<const std::byte> bytes, KeyRing& ring) {
    JtsDecode out;
    if (bytes.size() < kSealJournalTombstoneBytes) {
        out.note = "truncated";
        return out;
    }
    std::uint32_t key_id = 0;
    if (!peek_seal_journal_tombstone_kek_key_id(bytes, key_id)) {
        out.note = "peek failed";
        return out;
    }
    const PinResult pin = ring.pin_key(key_id);
    if (pin.status != PinStatus::Pinned) {
        out.status = WireStatus::Invalid;
        out.note = "key " + std::to_string(key_id) + " not in ring";
        return out;
    }
    std::optional<VerifiedSealJournalTombstoneWire> verified;
    const auto st = decode_seal_journal_tombstone_wire(bytes, pin.handle->key_bytes(), verified);
    if (st == SealJournalCommitTombstoneDecodeStatus::Ok) {
        out.wire = verified->value();
        out.status = WireStatus::Valid;
        out.note = "ok";
    } else if (st == SealJournalCommitTombstoneDecodeStatus::ChecksumMismatch) {
        out.status = WireStatus::Invalid;
        out.note = "MAC mismatch";
    } else {
        out.note = decode_failure_name(st);
    }
    return out;
}

// ===========================================================================
// Directory scan + grouped report
// ===========================================================================

struct JhwEntry {
    std::string name{};
    std::uint64_t file_candidate{0};
    JhwDecode dec{};
};

struct JtsEntry {
    std::string name{};
    std::uint64_t file_candidate{0};
    std::uint64_t file_seq{0};
    JtsDecode dec{};
};

struct CandidateFiles {
    std::optional<JhwEntry> jhw{};
    std::vector<JtsEntry> jts{};
};

void print_jhw_line(const JhwEntry& e) {
    if (e.dec.status == WireStatus::Valid) {
        std::array<char, 17> lo_hex{};
        std::array<char, 17> hi_hex{};
        format_hex16(e.dec.wire.store_uuid_lo, lo_hex);
        format_hex16(e.dec.wire.store_uuid_hi, hi_hex);
        std::printf("  .jhw %-41s [valid   ] store_uuid=%s:%s candidate_id=%llu "
                    "highest_committed_seq=%llu kek_key_id=%u\n",
                    e.name.c_str(), lo_hex.data(), hi_hex.data(),
                    static_cast<unsigned long long>(e.dec.wire.candidate_id),
                    static_cast<unsigned long long>(e.dec.wire.highest_committed_journal_seq),
                    e.dec.wire.kek_key_id);
    } else {
        std::printf("  .jhw %-41s [%s] %s\n", e.name.c_str(), wire_status_name(e.dec.status),
                    e.dec.note.c_str());
    }
}

void print_jts_line(const JtsEntry& e) {
    if (e.dec.status == WireStatus::Valid) {
        std::printf("  .jts %-40s [valid   ] journal_seq=%llu kek_key_id=%u\n", e.name.c_str(),
                    static_cast<unsigned long long>(e.dec.wire.journal_seq), e.dec.wire.kek_key_id);
    } else {
        std::printf("  .jts %-40s [%s] journal_seq=%llu %s\n", e.name.c_str(),
                    wire_status_name(e.dec.status),
                    static_cast<unsigned long long>(e.file_seq), e.dec.note.c_str());
    }
}

int run_dump(const std::filesystem::path& dir, KeyRing& ring) {
    std::map<std::uint64_t, CandidateFiles> candidates;
    std::vector<std::string> warnings;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) {
                warnings.push_back("ignoring non-regular entry " + entry.path().filename().string());
                continue;
            }
            const std::string name = entry.path().filename().string();

            if (ends_with(name, ".jhw")) {
                const std::string_view base(name.data(), name.size() - 4);
                std::uint64_t file_candidate = 0;
                if (!parse_hex16(base, file_candidate)) {
                    warnings.push_back("unrecognized .jhw filename " + name + " (expected <16hex>.jhw)");
                    continue;
                }
                JhwEntry e;
                e.name = name;
                e.file_candidate = file_candidate;
                const auto bytes = read_file_bytes(entry.path());
                e.dec = decode_jhw_file(bytes, ring);
                if (e.dec.status == WireStatus::Valid && e.dec.wire.candidate_id != file_candidate) {
                    warnings.push_back(name + ": filename candidate_id disagrees with decoded "
                                       "candidate_id " + std::to_string(e.dec.wire.candidate_id));
                }
                candidates[file_candidate].jhw = std::move(e);
            } else if (ends_with(name, ".jts")) {
                const std::string_view base(name.data(), name.size() - 4);
                if (base.size() != 33 || base[16] != '-') {
                    warnings.push_back("unrecognized .jts filename " + name +
                                       " (expected <16hex>-<16hex>.jts)");
                    continue;
                }
                std::uint64_t file_candidate = 0;
                std::uint64_t file_seq = 0;
                if (!parse_hex16(base.substr(0, 16), file_candidate) ||
                    !parse_hex16(base.substr(17, 16), file_seq)) {
                    warnings.push_back("unrecognized .jts filename " + name +
                                       " (expected <16hex>-<16hex>.jts)");
                    continue;
                }
                JtsEntry e;
                e.name = name;
                e.file_candidate = file_candidate;
                e.file_seq = file_seq;
                const auto bytes = read_file_bytes(entry.path());
                e.dec = decode_jts_file(bytes, ring);
                if (e.dec.status == WireStatus::Valid) {
                    if (e.dec.wire.candidate_id != file_candidate) {
                        warnings.push_back(name + ": filename candidate_id disagrees with decoded "
                                           "candidate_id " + std::to_string(e.dec.wire.candidate_id));
                    }
                    if (e.dec.wire.journal_seq != file_seq) {
                        warnings.push_back(name + ": filename journal_seq disagrees with decoded "
                                           "journal_seq " + std::to_string(e.dec.wire.journal_seq));
                    }
                }
                candidates[file_candidate].jts.push_back(std::move(e));
            } else {
                warnings.push_back("ignoring unrecognized file " + name);
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::printf("error: cannot scan %s: %s\n", dir.string().c_str(), e.what());
        return 3;
    }

    std::printf("== seal-journal store dump: %s ==\n", dir.string().c_str());

    std::size_t total_jhw = 0;
    std::size_t total_jts = 0;
    for (auto& [candidate, cf] : candidates) {
        std::array<char, 17> id_hex{};
        format_hex16(candidate, id_hex);
        std::printf("candidate %s:\n", id_hex.data());
        if (cf.jhw.has_value()) {
            print_jhw_line(*cf.jhw);
            ++total_jhw;
        } else {
            std::printf("  (no .jhw for this candidate)\n");
        }
        std::sort(cf.jts.begin(), cf.jts.end(),
                  [](const JtsEntry& a, const JtsEntry& b) { return a.file_seq < b.file_seq; });
        for (const JtsEntry& e : cf.jts) {
            print_jts_line(e);
            ++total_jts;
        }
    }

    if (!warnings.empty()) {
        std::printf("warnings:\n");
        for (const std::string& w : warnings) std::printf("  %s\n", w.c_str());
    }

    std::printf("summary: %zu distinct candidate(s), %zu .jhw, %zu .jts\n", candidates.size(),
                total_jhw, total_jts);
    return 0;
}

// ===========================================================================
// CLI
// ===========================================================================

void print_usage() {
    std::printf(
        "usage:\n"
        "  seal_journal_store_dump --dir <seal-journal store directory> [--kek-hex <64hex>]\n"
        "  seal_journal_store_dump --usage\n"
        "--dir      a seal-journal/<store_uuid>/ directory holding <candidate_id_hex16>.jhw\n"
        "           and <candidate_id_hex16>-<journal_seq_hex16>.jts files\n"
        "--kek-hex  override the KEK used to unwrap the ring's test keys (default:\n"
        "           this tool's own deterministic HY-AUDIT-FIXTURE-KEK-1, never production\n"
        "           key material); the ring still only holds test plaintext keys 7 and 8\n");
}

}  // namespace
}  // namespace hy

int main(int argc, char** argv) {
    using namespace hy;

    std::string dir_arg;
    std::array<std::byte, kKekSize> kek = kDumpKek;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dir") {
            if (i + 1 < argc) dir_arg = argv[++i];
        } else if (arg == "--kek-hex") {
            if (i + 1 < argc && !parse_kek_hex(argv[++i], kek)) {
                std::printf("error: --kek-hex must be %zu hex characters\n", kKekSize * 2);
                return 2;
            }
        } else if (arg == "--usage" || arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else {
            std::printf("error: unknown argument '%s'\n", arg.c_str());
            print_usage();
            return 2;
        }
    }

    if (dir_arg.empty()) {
        print_usage();
        return 2;
    }

    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ring.add_key(kDumpPrimaryKeyId, kDumpPlaintextKey7, rec);
    ring.add_key(kDumpSecondaryKeyId, kDumpPlaintextKey8, rec);

    return run_dump(dir_arg, ring);
}
