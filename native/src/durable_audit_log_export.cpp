// SPDX-License-Identifier: proprietary
// durable_audit_log_export.cpp — 批次 6 6c: offline CLI over durable_audit_export.hpp. Verifies one
// DurableAuditSink log end to end (chain, tip anchor, store identity) and, only if that holds, writes it out as
// newline-delimited JSON. See durable_audit_export.hpp's own header comment for the full design.
//
// Deliberately NOT a production code path, same discipline as seal_journal_store_dump.cpp: no CandidateLease, no
// write access to the source directory at all -- the log, tip anchor and store-identity files are opened
// read-only, and the writer's lock is only probed, never acquired.
//
// KEK/keys come from the command line -- --kek-hex + --key-file, the same "diagnostic tool takes its key material
// as arguments" precedent seal_journal_store_dump.cpp's own --kek-hex already established -- never from
// KekLoader (that class's whole point is a file path that is NEVER argv/env-sourced; this tool is not that path).
// --key-file is this tool's OWN input contract (see durable_audit_export.hpp's "wrapped-key file" section), not a
// key ceremony: no production key persistence exists in this codebase yet, so there is nothing else to point at.
//
// Usage:
//   durable_audit_log_export --log <sink base path> --out <ndjson path> --kek-hex <64hex> --key-file <path>
//                             [--allow-missing-anchor]
//   durable_audit_log_export --usage
//
// Exit codes: 0 = exported, 2 = usage error, 3 = key file did not load, 4 = verification/export failed (see the
// printed ExportStatus).

#include <hengyuan/durable_audit_export.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace hy {
namespace {

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

const char* audit_key_file_status_name(AuditKeyFileStatus s) noexcept {
    switch (s) {
        case AuditKeyFileStatus::Ok: return "Ok";
        case AuditKeyFileStatus::BadMagic: return "BadMagic";
        case AuditKeyFileStatus::BadLength: return "BadLength";
        case AuditKeyFileStatus::TooManyKeys: return "TooManyKeys";
        case AuditKeyFileStatus::TagMismatch: return "TagMismatch";
        case AuditKeyFileStatus::DuplicateKeyId: return "DuplicateKeyId";
    }
    return "?";
}

void print_usage() {
    std::printf(
        "usage:\n"
        "  durable_audit_log_export --log <sink base path> --out <ndjson path>\n"
        "                            --kek-hex <64hex> --key-file <path> [--allow-missing-anchor]\n"
        "  durable_audit_log_export --usage\n"
        "--log       the DurableAuditSink base path (no suffix) -- e.g. state/audit.log.\n"
        "            Its .lock/.tip/.storeid.tip siblings are derived from this path, same as the writer's own.\n"
        "            Refuses if the writer's .lock is currently held (this is an OFFLINE tool).\n"
        "--out       output NDJSON path; must not already exist (never overwritten).\n"
        "--kek-hex   the operator-provisioned KEK, %zu hex characters. Never a file path -- this is a diagnostic\n"
        "            tool's own argument convention, not KekLoader's file-based one.\n"
        "--key-file  a HYKEYS01 wrapped-key file (durable_audit_export.hpp's own format): the key(s) the log was\n"
        "            signed with, wrapped under --kek-hex. This tool's own input contract, not a key ceremony.\n"
        "--allow-missing-anchor  export even without a .tip file (the export then cannot show the tail was not\n"
        "            cut off; the manifest's tip_anchor field says \"absent-allowed\" so the consumer can see it).\n",
        kKekSize * 2);
}

}  // namespace
}  // namespace hy

int main(int argc, char** argv) {
    using namespace hy;

    std::string log_arg, out_arg, key_file_arg;
    std::array<std::byte, kKekSize> kek{};
    bool have_kek = false;
    ExportOptions opts;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--log") {
            if (i + 1 < argc) log_arg = argv[++i];
        } else if (arg == "--out") {
            if (i + 1 < argc) out_arg = argv[++i];
        } else if (arg == "--key-file") {
            if (i + 1 < argc) key_file_arg = argv[++i];
        } else if (arg == "--kek-hex") {
            if (i + 1 < argc) {
                if (!parse_kek_hex(argv[++i], kek)) {
                    std::fprintf(stderr, "error: --kek-hex must be %zu hex characters\n", kKekSize * 2);
                    return 2;
                }
                have_kek = true;
            }
        } else if (arg == "--allow-missing-anchor") {
            opts.allow_missing_anchor = true;
        } else if (arg == "--usage" || arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", arg.c_str());
            print_usage();
            return 2;
        }
    }

    if (log_arg.empty() || out_arg.empty() || key_file_arg.empty() || !have_kek) {
        print_usage();
        return 2;
    }

    std::ifstream key_stream(key_file_arg, std::ios::binary);
    if (!key_stream) {
        std::fprintf(stderr, "error: could not open --key-file '%s'\n", key_file_arg.c_str());
        return 3;
    }
    key_stream.seekg(0, std::ios::end);
    const std::streamoff key_file_size = key_stream.tellg();
    if (key_file_size < 0) {
        std::fprintf(stderr, "error: could not read --key-file '%s'\n", key_file_arg.c_str());
        return 3;
    }
    std::vector<std::byte> key_file_bytes(static_cast<std::size_t>(key_file_size));
    if (key_file_size > 0) {
        key_stream.seekg(0, std::ios::beg);
        key_stream.read(reinterpret_cast<char*>(key_file_bytes.data()), key_file_size);
    }

    KeyRing ring(kek);
    const AuditKeyFileStatus key_status = load_audit_key_file(key_file_bytes, ring);
    if (key_status != AuditKeyFileStatus::Ok) {
        std::fprintf(stderr, "error: --key-file did not load: %s\n", audit_key_file_status_name(key_status));
        return 3;
    }

    const ExportFileResult result = export_audit_log_file(log_arg, out_arg, ring, opts);
    const AuditLogVerification& v = result.verification;
    std::printf("status=%s frames=%llu verified_bytes=%zu torn_tail_bytes=%zu tip_anchor=%s\n",
               export_status_name(result.status), static_cast<unsigned long long>(v.frames), v.verified_bytes,
               v.torn_tail_bytes,
               v.anchor == AnchorState::Verified       ? "verified"
               : v.anchor == AnchorState::AbsentAllowed ? "absent-allowed"
               : v.anchor == AnchorState::EmptyLog      ? "empty-log"
                                                        : "not-checked");
    if (result.status != ExportStatus::Ok) {
        std::fprintf(stderr, "error: at log offset %zu (expected sequence %llu)\n", v.error_offset,
                     static_cast<unsigned long long>(v.error_sequence));
        return 4;
    }
    std::printf("wrote %s\n", out_arg.c_str());
    return 0;
}
