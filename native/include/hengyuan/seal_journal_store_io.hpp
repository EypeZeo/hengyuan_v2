// SPDX-License-Identifier: proprietary
// seal_journal_store_io.hpp — internal implementation detail of
// seal_journal_store_lease.hpp (Round E breadcrumb L2 loaders, docs/
// SPEC_INVARIANTS.md's "Seal-journal Round E breadcrumb L2 loaders" entry).
// DO NOT include this file from anywhere except seal_journal_store_lease.hpp
// -- it is not part of the public API surface. Same discipline as
// compaction_breadcrumb_io.hpp; this file is a DELIBERATE duplicate of that
// file's read-side primitives rather than a shared/refactored dependency --
// compaction_breadcrumb_io.hpp's own header comment states "DO NOT include
// this file from anywhere except compaction_lease.hpp," and coupling two
// independently-reviewed trust boundaries together is a bigger risk than a
// second, small, read-only copy (same reasoning durable_log_store.hpp's
// posix_retry_on_eintr duplication already established in this codebase).
//
// Read-only subset only: this round's SealJournalStoreLease has no write
// method (see seal_journal_store_lease.hpp's own SCOPE comment), so unlike
// compaction_breadcrumb_io.hpp this file carries NO publish/write-side
// primitives at all -- nothing here can create, replace, or delete a file.
//
// This file has two parts:
//
// 1. Platform-independent (top of file, always compiled): ReadFixedStatus/
//    ReadFixedResult (identical vocabulary to compaction_breadcrumb_io.hpp's,
//    duplicated rather than shared for the same trust-boundary reason above)
//    and SealJournalArtifactName/is_traversal_safe_name -- the read-only
//    counterpart of ValidatedArtifactName. The functions that touch a
//    filesystem accept ONLY a SealJournalArtifactName, never a raw
//    std::string_view/std::filesystem::path -- there is no public
//    constructor that accepts caller-supplied text.
//
// 2. POSIX-only (bottom of file, `#ifndef _WIN32`): an openat wrapper, the
//    direct analogue of windows_native_io.hpp on the other platform.
//    seal_journal_store_lease.hpp picks whichever half applies at compile
//    time.

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <system_error>

namespace hy::seal_journal_store_detail {

// ===========================================================================
// Platform-independent: shared result vocabulary (read-only subset)
// ===========================================================================

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
// Platform-independent: SealJournalArtifactName
// ===========================================================================

inline constexpr std::size_t kMaxArtifactNameLen = 63;

// Fixed-width 16-hex-digit lowercase formatting, null-terminated (out[16] =
// '\0') -- never snprintf with a width specifier (which can silently
// truncate on some libc implementations for values wider than expected).
// The output alphabet is exactly [0-9a-f], so it is structurally incapable
// of producing a path separator or "..". Byte-identical to
// compaction_breadcrumb_io.hpp's format_hex16(); duplicated for the same
// trust-boundary reason stated at the top of this file.
inline void format_hex16(std::uint64_t v, std::array<char, 17>& out) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[v & 0xFu];
        v >>= 4;
    }
    out[16] = '\0';
}

// Rejects: empty, ".", "..", any '/' or '\\', embedded NUL, non-ASCII,
// overlong (> kMaxArtifactNameLen), and a leading '-'. The only validation
// logic for artifact names in this file -- every SealJournalArtifactName
// factory below routes through it.
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

// A filename guaranteed (by construction) to satisfy is_traversal_safe_name().
// No public constructor accepts an arbitrary string.
class SealJournalArtifactName {
public:
    // File: "<candidate_id_hex16>.jhw" (SealJournalCommitWatermark).
    static SealJournalArtifactName for_commit_watermark(std::uint64_t candidate_id) noexcept {
        std::array<char, 17> id_hex{};
        format_hex16(candidate_id, id_hex);
        std::array<char, kMaxArtifactNameLen> name{};
        constexpr std::string_view kSuffix = ".jhw";
        constexpr std::size_t kNameLen = 16 + kSuffix.size();
        static_assert(kNameLen < kMaxArtifactNameLen);
        std::memcpy(name.data(), id_hex.data(), 16);
        std::memcpy(name.data() + 16, kSuffix.data(), kSuffix.size());
        return SealJournalArtifactName(std::string_view(name.data(), kNameLen));
    }
    // File: "<candidate_id_hex16>-<journal_seq_hex16>.jts" (SealJournalTombstoneWire).
    static SealJournalArtifactName for_tombstone(std::uint64_t candidate_id, std::uint64_t journal_seq) noexcept {
        std::array<char, 17> id_hex{};
        std::array<char, 17> seq_hex{};
        format_hex16(candidate_id, id_hex);
        format_hex16(journal_seq, seq_hex);
        std::array<char, kMaxArtifactNameLen> name{};
        constexpr std::string_view kSuffix = ".jts";
        constexpr std::size_t kNameLen = 16 + 1 + 16 + kSuffix.size();
        static_assert(kNameLen < kMaxArtifactNameLen);
        std::memcpy(name.data(), id_hex.data(), 16);
        name[16] = '-';
        std::memcpy(name.data() + 17, seq_hex.data(), 16);
        std::memcpy(name.data() + 33, kSuffix.data(), kSuffix.size());
        return SealJournalArtifactName(std::string_view(name.data(), kNameLen));
    }

    std::string_view relative_name() const noexcept { return std::string_view(buf_.data(), len_); }

private:
    explicit SealJournalArtifactName(std::string_view name) noexcept {
        // Both factories' call sites format via format_hex16(), which is
        // structurally incapable of producing an unsafe character -- this
        // assert is a regression tripwire for that guarantee, not the
        // safety mechanism itself (same reasoning as
        // compaction_breadcrumb_io.hpp's ValidatedArtifactName).
        const bool valid =
            name.size() < kMaxArtifactNameLen && is_traversal_safe_name(name);
        assert(valid);
        if (!valid) return;
        len_ = static_cast<std::uint8_t>(name.size());
        std::memcpy(buf_.data(), name.data(), len_);
    }

    std::array<char, kMaxArtifactNameLen> buf_{};
    std::uint8_t len_{0};
};

}  // namespace hy::seal_journal_store_detail

// ===========================================================================
// POSIX-only: openat wrapper (read-only subset)
// ===========================================================================
#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hy::seal_journal_store_detail {

template <typename Fn>
inline auto posix_retry_on_eintr(Fn&& fn) noexcept -> decltype(fn()) {
    for (;;) {
        const auto rc = fn();
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
}

// Handle-relative (dirfd-relative) exact-size read: rejects short/long
// reads, symlinks, and non-regular files. Never resizes a buffer based on
// an attacker-influenced file size -- `out` is caller-sized and fixed.
// Byte-identical logic to compaction_breadcrumb_io.hpp's
// read_validated_exact(); duplicated for the trust-boundary reason stated
// at the top of this file.
inline ReadFixedResult read_validated_exact(int dir_fd, const SealJournalArtifactName& name,
                                              std::span<std::byte> out) noexcept {
    ReadFixedResult result{};
    const std::string_view relative_name = name.relative_name();

    // SealJournalArtifactName keeps one zero-filled byte beyond every
    // factory-produced name, so data() is a valid C string for openat().
    const int fd = ::openat(dir_fd, relative_name.data(), O_RDONLY | O_NOFOLLOW);
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

}  // namespace hy::seal_journal_store_detail

#endif  // !_WIN32
