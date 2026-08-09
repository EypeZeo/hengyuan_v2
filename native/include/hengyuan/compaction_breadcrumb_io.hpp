// SPDX-License-Identifier: proprietary
// compaction_breadcrumb_io.hpp — internal implementation detail of
// compaction_lease.hpp (Round D, docs/SPEC_INVARIANTS.md's "Seal-journal
// Round D" entry). DO NOT include this file from anywhere except
// compaction_lease.hpp -- it is not part of the public API surface.
//
// This file has two parts:
//
// 1. Platform-independent (top of file, always compiled): the shared
//    result-type vocabulary compaction_lease.hpp exposes on both platforms
//    (PublishCommitState/PublishProvenance/ReadFixedStatus/UnlinkStatus),
//    and ValidatedArtifactName/is_traversal_safe_name -- the type that
//    closes the path-traversal hole from Round D's fifth design revision
//    (a directory HANDLE only fixes the START of pathname resolution, it
//    does NOT reject ".." in the final path component). The functions that
//    actually touch a filesystem accept ONLY a ValidatedArtifactName, never
//    a raw std::string_view/std::filesystem::path -- there is physically no
//    way to call them with an attacker-controlled name, because there is no
//    public constructor for ValidatedArtifactName that accepts one.
//
// 2. POSIX-only (bottom of file, `#ifndef _WIN32`): openat/linkat/
//    renameat2 wrappers, the direct analogue of windows_native_io.hpp on
//    the other platform. compaction_lease.hpp picks whichever half applies
//    at compile time.

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace hy::compaction_detail {

// ===========================================================================
// Platform-independent: shared result vocabulary
// ===========================================================================

enum class PublishCommitState : std::uint8_t {
    DurablyPublished,             // content fsync'd + no-replace publish + parent-dir flush all confirmed
    PublishedNamespaceUncertain,  // no-replace publish itself succeeded but parent-directory
                                   // durability could not be confirmed (Windows, typically) --
                                   // the file IS visible under its final name with correct
                                   // content, but callers MUST NOT treat this as durable for
                                   // any dependent decision; see IntentStore's same-instance
                                   // provenance-memory rule (compaction_intent_store.hpp)
    NotPublished,                 // definite failure (disk full, permission, directory identity
                                   // changed mid-operation, etc.)
};

enum class PublishProvenance : std::uint8_t {
    CreatedThisCallDurable,    // this call created the file and confirmed full durability
    CreatedThisCallUncertain,  // this call created the file but parent-flush durability is unconfirmed
    FoundPreExisting,          // the target already existed before this call (not written by us just now)
};

struct PublishResult {
    PublishCommitState state{PublishCommitState::NotPublished};
    PublishProvenance provenance{PublishProvenance::FoundPreExisting};
    std::error_code ec{};
};

enum class ReadFixedStatus : std::uint8_t {
    Ok,
    NotFound,
    WrongSize,
    NotRegularFile,
    IoError,
};

struct ReadFixedResult {
    ReadFixedStatus status{ReadFixedStatus::IoError};
    std::error_code ec{};
};

// ===========================================================================
// Platform-independent: ValidatedArtifactName (Round D review v5's P0-1 fix)
// ===========================================================================

inline constexpr std::size_t kMaxArtifactNameLen = 63;

// Fixed-width 16-hex-digit lowercase formatting, null-terminated (out[16] =
// '\0') -- never snprintf with a width specifier (which can silently
// truncate on some libc implementations for values wider than expected).
// The output alphabet is exactly [0-9a-f], so it is structurally incapable
// of producing a path separator or "..".
inline void format_hex16(std::uint64_t v, std::array<char, 17>& out) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[v & 0xFu];
        v >>= 4;
    }
    out[16] = '\0';
}

// Rejects: empty, ".", "..", any '/' or '\\', embedded NUL, non-ASCII,
// overlong (> kMaxArtifactNameLen), and a leading '-' (so a name can never
// be misread as a command-line flag by some naive tool that later greps
// this directory's contents). This is the ONLY validation logic for
// artifact names in this codebase -- every ValidatedArtifactName factory
// below routes through it, so it cannot drift out of sync with itself.
//
// Round D itself has exactly one caller of this validator
// (ValidatedArtifactName::for_compaction_candidate_intent(), a fixed
// literal that trivially passes) -- there is no external input that
// reaches this function in production this round. It is tested and kept
// here anyway for Round E/F, whose for_x1(build_nonce, seq)/for_xgc
// (build_nonce) factories WILL format numeric input into a name and need
// this same validator as a second line of defense (the hex formatting
// itself only ever produces [0-9a-f], but the discipline of routing every
// factory through one validator is worth keeping now, before it's load-
// bearing, not after).
inline bool is_traversal_safe_name(std::string_view candidate) noexcept {
    if (candidate.empty()) return false;
    if (candidate.size() > kMaxArtifactNameLen) return false;
    if (candidate == ".") return false;
    if (candidate == "..") return false;
    if (candidate.front() == '-') return false;
    for (const char c : candidate) {
        if (c == '/' || c == '\\') return false;
        if (c == '\0') return false;
        if (static_cast<unsigned char>(c) > 0x7F) return false;  // non-ASCII
    }
    return true;
}

// A filename that is guaranteed (by construction, not by a runtime check a
// caller might forget) to satisfy is_traversal_safe_name(). No public
// constructor accepts an arbitrary string -- Round D's only factory is a
// fixed compile-time literal, matching the "verified type, no public raw
// constructor" idiom compaction_intent_codec.hpp already established for
// decoded wire types.
class ValidatedArtifactName {
public:
    // Round D's only factory -- the fixed genesis filename, safe by
    // construction (a literal, not derived from any external input).
    static ValidatedArtifactName for_compaction_candidate_intent() noexcept {
        return ValidatedArtifactName(std::string_view("compaction-candidate-intent"));
    }

    // Read-only factory Round D DOES need: IntentStore::inspect_x1_chain()
    // (a read-only diagnostic, never a write in Round D -- see
    // compaction_lease.hpp's SCOPE comment) has to be able to name a `.x1`
    // file that might already exist on disk (e.g. left over from a future
    // Round E/F write, or placed there for a test fixture). Formats
    // build_nonce/seq as fixed-width lowercase hex via format_hex16()
    // below -- never snprintf with a width specifier that could silently
    // truncate -- so the result is physically incapable of containing '/',
    // '\\', or ".." (hex digits are drawn from a fixed 16-character
    // alphabet). transition_seq is spec-constrained to {1,2,3}
    // (durable_control_plane.hpp's CompactionIntentTransitionWire comment);
    // IntentStore validates that before calling this, not this factory --
    // keeping this factory's only job "format safely," not "know the
    // business rule."
    static ValidatedArtifactName for_x1(std::uint64_t build_nonce, std::uint32_t transition_seq) noexcept {
        std::array<char, 17> nonce_hex{};
        std::array<char, 17> seq_hex{};
        format_hex16(build_nonce, nonce_hex);
        format_hex16(static_cast<std::uint64_t>(transition_seq), seq_hex);
        std::string name = "compaction-intent-x-";
        name += nonce_hex.data();
        name += "-";
        name += seq_hex.data();
        name += ".x1";
        return ValidatedArtifactName(name);
    }

    // Round E/F (not called by anything in Round D -- Round D never reads
    // OR writes a `.xgc`, see compaction_lease.hpp's SCOPE comment):
    //   for_xgc(build_nonce) -> "compaction-intent-gc-<nonce_hex16>.xgc"

    std::string_view relative_name() const noexcept { return std::string_view(buf_.data(), len_); }

private:
    explicit ValidatedArtifactName(std::string_view name) noexcept {
        // for_compaction_candidate_intent()'s call site is a literal;
        // for_x1()'s is runtime-formatted (from format_hex16(), which is
        // structurally incapable of producing an unsafe character, but
        // "structurally incapable" is exactly the kind of claim worth a
        // debug-build assertion rather than trusting it silently forever).
        // Release builds do not re-pay this cost per call -- the format_hex16
        // alphabet guarantee is the actual safety property, this assert is a
        // regression tripwire for it, not the safety mechanism itself.
        assert(name.size() < kMaxArtifactNameLen);
        assert(is_traversal_safe_name(name));
        len_ = static_cast<std::uint8_t>(name.size() < kMaxArtifactNameLen ? name.size() : kMaxArtifactNameLen);
        std::memcpy(buf_.data(), name.data(), len_);
    }

    std::array<char, kMaxArtifactNameLen> buf_{};
    std::uint8_t len_{0};
};

}  // namespace hy::compaction_detail

// ===========================================================================
// POSIX-only: openat/linkat/renameat2 wrappers
// ===========================================================================
#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <sys/types.h>

namespace hy::compaction_detail {

// Same EINTR-retry idiom as durable_log_store.hpp's detail::retry_on_eintr
// -- duplicated here rather than shared, because this file is a standalone
// detail header not meant to pull in durable_log_store.hpp's broader
// surface (see this file's header comment on why it stays minimal).
template <typename Fn>
inline auto posix_retry_on_eintr(Fn&& fn) noexcept -> decltype(fn()) {
    for (;;) {
        const auto rc = fn();
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
}

// Every attempt gets a unique tmp name (pid + a per-process monotonic
// counter + a coarse timestamp) so a crash mid-publish never blocks a
// later retry from a fresh attempt -- the leftover tmp file from the failed
// attempt sits under ITS OWN unique name forever (harmless, uncollected
// garbage left for a future janitor task, not a correctness problem: it
// never collides with a subsequent attempt's name, and no code path ever
// reads a file by tmp name).
inline std::string make_unique_tmp_name(std::string_view final_name) noexcept {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), ".tmp-%d-%llu-%llu", static_cast<int>(::getpid()),
                  static_cast<unsigned long long>(n),
                  static_cast<unsigned long long>(::time(nullptr)));
    std::string out(final_name);
    out += suffix;
    return out;
}

// Handle-relative (dirfd-relative) no-replace publish: unique-named
// O_CREAT|O_EXCL tmp -> write -> fsync(file) -> no-replace publish (try
// renameat2(RENAME_NOREPLACE) first; if the running kernel doesn't support
// it -- ENOSYS -- fall back to linkat+unlinkat, which has the identical
// no-replace guarantee on POSIX) -> fsync(dir_fd) for parent-directory
// durability. If the final name already exists: read it back and compare
// bytes -- identical content is idempotent (DurablyPublished,
// FoundPreExisting); any difference is NotPublished (a real conflict,
// never silently overwritten).
inline PublishResult write_validated_no_replace(int dir_fd, const ValidatedArtifactName& name,
                                                  std::span<const std::byte> bytes) noexcept {
    PublishResult result{};
    const std::string_view final_name = name.relative_name();
    const std::string tmp_name = make_unique_tmp_name(final_name);

    const int tmp_fd = ::openat(dir_fd, tmp_name.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (tmp_fd < 0) {
        result.state = PublishCommitState::NotPublished;
        result.ec = std::error_code(errno, std::generic_category());
        return result;
    }

    bool write_ok = true;
    std::size_t total = 0;
    while (write_ok && total < bytes.size()) {
        const ssize_t written = posix_retry_on_eintr(
            [&] { return ::write(tmp_fd, bytes.data() + total, bytes.size() - total); });
        if (written <= 0) {
            write_ok = false;
            break;
        }
        total += static_cast<std::size_t>(written);
    }
    write_ok = write_ok && (posix_retry_on_eintr([&] { return ::fsync(tmp_fd); }) == 0);
    const int fsync_errno = errno;
    ::close(tmp_fd);
    if (!write_ok) {
        ::unlinkat(dir_fd, tmp_name.c_str(), 0);  // best-effort cleanup of our own tmp
        result.state = PublishCommitState::NotPublished;
        result.ec = std::error_code(fsync_errno, std::generic_category());
        return result;
    }

    std::string final_name_str(final_name);
#ifdef SYS_renameat2
    long rc = ::syscall(SYS_renameat2, dir_fd, tmp_name.c_str(), dir_fd, final_name_str.c_str(),
                         static_cast<unsigned int>(1u /* RENAME_NOREPLACE */));
    bool renamed = (rc == 0);
    bool collision = !renamed && (errno == EEXIST);
    bool syscall_unsupported = !renamed && (errno == ENOSYS || errno == EINVAL);
#else
    bool renamed = false;
    bool collision = false;
    bool syscall_unsupported = true;
#endif
    if (syscall_unsupported) {
        // Fallback: linkat (fails EEXIST if target exists, same no-replace
        // guarantee) + unlink the tmp name.
        const int link_rc = ::linkat(dir_fd, tmp_name.c_str(), dir_fd, final_name_str.c_str(), 0);
        renamed = (link_rc == 0);
        collision = !renamed && (errno == EEXIST);
        if (renamed) ::unlinkat(dir_fd, tmp_name.c_str(), 0);
    }

    if (!renamed) {
        ::unlinkat(dir_fd, tmp_name.c_str(), 0);  // best-effort cleanup regardless of outcome
        if (!collision) {
            result.state = PublishCommitState::NotPublished;
            result.ec = std::error_code(errno, std::generic_category());
            return result;
        }
        // Target already exists -- read it back and compare bytes.
        const int existing_fd = ::openat(dir_fd, final_name_str.c_str(), O_RDONLY);
        if (existing_fd < 0) {
            result.state = PublishCommitState::NotPublished;
            result.ec = std::error_code(errno, std::generic_category());
            return result;
        }
        std::vector<std::byte> existing(bytes.size() + 1);  // +1 to detect "longer than expected"
        std::size_t read_total = 0;
        bool read_ok = true;
        while (read_ok && read_total < existing.size()) {
            const ssize_t n = posix_retry_on_eintr(
                [&] { return ::read(existing_fd, existing.data() + read_total, existing.size() - read_total); });
            if (n < 0) {
                read_ok = false;
                break;
            }
            if (n == 0) break;
            read_total += static_cast<std::size_t>(n);
        }
        ::close(existing_fd);
        if (!read_ok) {
            result.state = PublishCommitState::NotPublished;
            result.ec = std::error_code(errno, std::generic_category());
            return result;
        }
        const bool byte_equal =
            read_total == bytes.size() && std::memcmp(existing.data(), bytes.data(), bytes.size()) == 0;
        if (byte_equal) {
            result.state = PublishCommitState::DurablyPublished;
            result.provenance = PublishProvenance::FoundPreExisting;
            return result;
        }
        result.state = PublishCommitState::NotPublished;  // real conflict -- never overwritten
        return result;
    }

    // Parent-directory fsync -- this is what makes the rename's directory-
    // entry change crash-durable, not just the file content.
    const bool dir_ok = (posix_retry_on_eintr([&] { return ::fsync(dir_fd); }) == 0);
    if (!dir_ok) {
        // POSIX fsync(dir_fd) failing is a real, definite failure (unlike
        // Windows' FlushFileBuffers-on-a-directory-handle, which can be
        // legitimately unsupported on some filesystems) -- fail closed,
        // do not report this as merely "uncertain."
        result.state = PublishCommitState::NotPublished;
        result.ec = std::error_code(errno, std::generic_category());
        return result;
    }

    result.state = PublishCommitState::DurablyPublished;
    result.provenance = PublishProvenance::CreatedThisCallDurable;
    return result;
}

// Handle-relative exact-size read: rejects short/long reads, symlinks, and
// non-regular files. Never resizes a buffer based on an attacker-influenced
// file size -- `out` is caller-sized and fixed.
inline ReadFixedResult read_validated_exact(int dir_fd, const ValidatedArtifactName& name,
                                              std::span<std::byte> out) noexcept {
    ReadFixedResult result{};
    const std::string name_str(name.relative_name());

    const int fd = ::openat(dir_fd, name_str.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        result.status = (errno == ENOENT) ? ReadFixedStatus::NotFound : ReadFixedStatus::IoError;
        result.ec = std::error_code(errno, std::generic_category());
        return result;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        result.status = ReadFixedStatus::IoError;
        result.ec = std::error_code(e, std::generic_category());
        return result;
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        result.status = ReadFixedStatus::NotRegularFile;
        return result;
    }

    std::size_t total = 0;
    bool read_ok = true;
    // Read exactly out.size(), then attempt ONE more byte to detect a
    // too-long file without ever growing a buffer based on file size.
    std::array<std::byte, 1> probe{};
    while (read_ok && total < out.size()) {
        const ssize_t n =
            posix_retry_on_eintr([&] { return ::read(fd, out.data() + total, out.size() - total); });
        if (n < 0) {
            read_ok = false;
            break;
        }
        if (n == 0) break;  // short file -- caught by the size check below
        total += static_cast<std::size_t>(n);
    }
    bool longer_than_expected = false;
    if (read_ok && total == out.size()) {
        const ssize_t extra = posix_retry_on_eintr([&] { return ::read(fd, probe.data(), 1); });
        longer_than_expected = extra > 0;
    }
    ::close(fd);
    if (!read_ok) {
        result.status = ReadFixedStatus::IoError;
        result.ec = std::error_code(errno, std::generic_category());
        return result;
    }
    if (total != out.size() || longer_than_expected) {
        result.status = ReadFixedStatus::WrongSize;
        return result;
    }
    result.status = ReadFixedStatus::Ok;
    return result;
}

}  // namespace hy::compaction_detail

#endif  // !_WIN32
