// SPDX-License-Identifier: proprietary
// durable_log_store.hpp — shared platform file-I/O primitives for durable,
// hash-chained logs (file lock, append+fsync, tip-anchor atomic replace).
//
// Governance: L2 (real file I/O). This is NOT a spec-pinned ABI type like
// DurableControlPlaneSink -- it is an internal implementation detail shared
// by two concrete sinks (DurableAuditSink, ControlPlaneLogSink) that each
// need the identical crash-consistency discipline (single-writer exclusive
// lock, fsync-before-Ack, atomic tip-anchor replace) but persist different
// record families. Extracted this round (docs/SPEC_INVARIANTS.md's "Phase 1"
// entry) specifically to avoid two independently-maintained copies of this
// platform code drifting apart over time -- DurableAuditSink's own copy of
// this logic (pre-existing, already tested by test_durable_audit_sink.cpp)
// is refactored to delegate here in the same round; that refactor is
// required to be behavior-preserving, verified by test_durable_audit_sink.cpp
// passing unmodified.
//
// This class knows NOTHING about frame formats, MACs, keys, or record types
// -- it only moves bytes between memory and disk with the crash-consistency
// properties the callers need. Encoding/decoding (including tip-anchor MAC
// computation) stays the caller's responsibility.
//
// THREAD OWNERSHIP: single-writer use from one owner thread, same as every
// other durable-log class in this codebase. No internal synchronization.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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
#include <sys/file.h>  // flock() -- see durable_audit_sink.hpp's header
                        // comment for why <sys/file.h> must be included
                        // alongside <fcntl.h> on GCC.
#include <sys/stat.h>   // fstat() for log_size()
#include <unistd.h>
#endif

namespace hy {

// Upper bound on a durable log this process is willing to load into memory.
//
// AUDIT REC-NOEXCEPT-006: read_whole_log() sizes its buffer straight from
// lseek/GetFileSizeEx with no bound, and the whole recovery path above it is
// noexcept -- so a log larger than available memory turned a designed-to-be
// fail-closed startup into std::terminate() via an escaping std::bad_alloc. Since
// this class deliberately has no compaction (see durable_audit_sink.hpp's scope
// note), the log grows without limit in normal operation, which makes that
// reachable by ordinary use rather than only by corruption.
//
// 1 GiB is roughly 4.2 million OrderEvent frames (255 bytes each). It is a
// diagnostic tripwire, not a design capacity: a log approaching it means
// compaction is genuinely needed, and failing closed with IoError says so, where
// terminate() said nothing at all.
inline constexpr std::uint64_t kMaxDurableLogBytes = 1024ull * 1024ull * 1024ull;

namespace detail {

// std::vector::resize can throw bad_alloc/length_error. Every caller in this file
// is noexcept and must fail closed rather than let the exception escape into a
// std::terminate() (audit REC-NOEXCEPT-006).
inline bool try_resize(std::vector<std::byte>& v, std::size_t n) noexcept {
    try {
        v.resize(n);
        return true;
    } catch (...) {
        return false;
    }
}

#if !defined(_WIN32)
// AUDIT IO-EINTR-021: read/write/fsync/pread can all return -1/EINTR when a signal
// is delivered without SA_RESTART, and every one of them here treated that as a
// hard failure. In this codebase a "hard failure" on the append path permanently
// FENCES the sink (durable_audit_sink.hpp), so a single stray signal could retire a
// perfectly healthy writer for the rest of the process's life. There is no signal
// handler in the library today, but watchdog_daemon.cpp installs SIGINT/SIGTERM
// handlers and any embedder may install more -- this is precisely the class of
// assumption that holds until it silently does not.
//
// Deliberately NOT applied to fsync's own retry semantics beyond EINTR: on Linux a
// failed fsync may have already dropped the dirty page, so retrying a NON-EINTR
// fsync failure would be unsound (the PostgreSQL fsyncgate problem). EINTR is the
// one case where the operation provably did not run.
template <typename Fn>
inline auto retry_on_eintr(Fn&& fn) noexcept -> decltype(fn()) {
    for (;;) {
        const auto rc = fn();
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
}
#endif

}  // namespace detail

class DurableLogStore {
public:
    // log_path: the durable log file itself. lock_path: OS-level exclusive
    // lock sidecar. tip_path: tip-anchor file, atomically replaced on every
    // write_tip_anchor() call. Callers derive these three paths however suits
    // them (e.g. DurableAuditSink uses path+".lock"/path+".tip";
    // ControlPlaneLogSink uses path+".cp.lock"/path+".cp.tip" so the two
    // sinks can coexist over sibling base paths without colliding).
    DurableLogStore(std::string log_path, std::string lock_path, std::string tip_path) noexcept
        : log_path_(std::move(log_path)), lock_path_(std::move(lock_path)), tip_path_(std::move(tip_path)) {}

    ~DurableLogStore() {
        close_log();
        release_lock();
    }

    DurableLogStore(const DurableLogStore&) = delete;
    DurableLogStore& operator=(const DurableLogStore&) = delete;
    DurableLogStore(DurableLogStore&&) = delete;
    DurableLogStore& operator=(DurableLogStore&&) = delete;

    bool is_log_open() const noexcept { return log_open_; }

    // --- Platform I/O (verbatim port of DurableAuditSink's pre-existing
    // platform branches, one file lower so ControlPlaneLogSink can share it
    // without duplicating the crash-consistency logic) ---
#if defined(_WIN32)
    bool acquire_lock() noexcept {
        // No-sharing CreateFileA is itself an OS-enforced exclusive lock: a
        // second handle opened anywhere for this path fails with a sharing
        // violation.
        lock_handle_ = CreateFileA(lock_path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return lock_handle_ != INVALID_HANDLE_VALUE;
    }
    void release_lock() noexcept {
        if (lock_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(lock_handle_);
            lock_handle_ = INVALID_HANDLE_VALUE;
        }
    }
    bool open_log() noexcept {
        log_handle_ = CreateFileA(log_path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        log_open_ = log_handle_ != INVALID_HANDLE_VALUE;
        return log_open_;
    }
    void close_log() noexcept {
        if (log_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(log_handle_);
            log_handle_ = INVALID_HANDLE_VALUE;
        }
        log_open_ = false;
    }
    bool read_whole_log(std::vector<std::byte>& out) noexcept {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(log_handle_, &size)) return false;
        if (static_cast<std::uint64_t>(size.QuadPart) > kMaxDurableLogBytes) return false;
        if (!detail::try_resize(out, static_cast<std::size_t>(size.QuadPart))) return false;
        if (out.empty()) return true;
        if (SetFilePointer(log_handle_, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return false;
        DWORD read_bytes = 0;
        BOOL ok = ReadFile(log_handle_, out.data(), static_cast<DWORD>(out.size()), &read_bytes, nullptr);
        return ok && static_cast<std::size_t>(read_bytes) == out.size();
    }
    bool append_and_fsync(std::span<const std::byte> bytes) noexcept {
        if (SetFilePointer(log_handle_, 0, nullptr, FILE_END) == INVALID_SET_FILE_POINTER) return false;
        DWORD written = 0;
        if (!WriteFile(log_handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            return false;
        }
        if (static_cast<std::size_t>(written) != bytes.size()) return false;
        return FlushFileBuffers(log_handle_) != 0;
    }
    std::uint64_t log_size() const noexcept {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(log_handle_, &size)) return 0;
        return static_cast<std::uint64_t>(size.QuadPart);
    }
    bool read_chunk(std::uint64_t offset, std::span<std::byte> buffer, std::size_t want,
                     std::size_t& out_read) noexcept {
        out_read = 0;
        const std::size_t effective_want = want < buffer.size() ? want : buffer.size();
        if (effective_want == 0) return true;
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(log_handle_, pos, nullptr, FILE_BEGIN)) return false;
        DWORD read_bytes = 0;
        if (!ReadFile(log_handle_, buffer.data(), static_cast<DWORD>(effective_want), &read_bytes, nullptr)) {
            return false;
        }
        out_read = static_cast<std::size_t>(read_bytes);
        return true;
    }
    bool write_tip_anchor(std::span<const std::byte> anchor_bytes) noexcept {
        const std::string tmp_path = tip_path_ + ".tmp";
        HANDLE h = CreateFileA(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(h, anchor_bytes.data(), static_cast<DWORD>(anchor_bytes.size()), &written, nullptr);
        ok = ok && (static_cast<std::size_t>(written) == anchor_bytes.size());
        ok = ok && FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok) return false;

        // MOVEFILE_WRITE_THROUGH: NTFS + FlushFileBuffers on the file handle
        // above is the honest contract here -- this is NOT POSIX directory-
        // fsync equivalence (no parent-directory metadata flush on Windows).
        return MoveFileExA(tmp_path.c_str(), tip_path_.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    bool read_tip_anchor(std::vector<std::byte>& out) noexcept {
        HANDLE h = CreateFileA(tip_path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size)) {
            CloseHandle(h);
            return false;
        }
        if (static_cast<std::uint64_t>(size.QuadPart) > kMaxDurableLogBytes ||
            !detail::try_resize(out, static_cast<std::size_t>(size.QuadPart))) {
            CloseHandle(h);
            return false;
        }
        bool ok = true;
        if (!out.empty()) {
            DWORD read_bytes = 0;
            ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read_bytes, nullptr) &&
                 static_cast<std::size_t>(read_bytes) == out.size();
        }
        CloseHandle(h);
        return ok;
    }

private:
    HANDLE lock_handle_{INVALID_HANDLE_VALUE};
    HANDLE log_handle_{INVALID_HANDLE_VALUE};

public:
#else
    bool acquire_lock() noexcept {
        lock_fd_ = ::open(lock_path_.c_str(), O_CREAT | O_RDWR, 0600);
        if (lock_fd_ < 0) return false;
        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            ::close(lock_fd_);
            lock_fd_ = -1;
            return false;
        }
        return true;
    }
    void release_lock() noexcept {
        if (lock_fd_ >= 0) {
            ::flock(lock_fd_, LOCK_UN);
            ::close(lock_fd_);
            lock_fd_ = -1;
        }
    }
    bool open_log() noexcept {
        log_fd_ = ::open(log_path_.c_str(), O_CREAT | O_RDWR, 0600);
        log_open_ = log_fd_ >= 0;
        return log_open_;
    }
    void close_log() noexcept {
        if (log_fd_ >= 0) {
            ::close(log_fd_);
            log_fd_ = -1;
        }
        log_open_ = false;
    }
    bool read_whole_log(std::vector<std::byte>& out) noexcept {
        const off_t size = ::lseek(log_fd_, 0, SEEK_END);
        if (size < 0) return false;
        if (static_cast<std::uint64_t>(size) > kMaxDurableLogBytes) return false;
        if (!detail::try_resize(out, static_cast<std::size_t>(size))) return false;
        if (out.empty()) return true;
        if (::lseek(log_fd_, 0, SEEK_SET) < 0) return false;
        std::size_t total = 0;
        while (total < out.size()) {
            const ssize_t n = detail::retry_on_eintr(
                [&] { return ::read(log_fd_, out.data() + total, out.size() - total); });
            if (n <= 0) return false;
            total += static_cast<std::size_t>(n);
        }
        return true;
    }
    bool append_and_fsync(std::span<const std::byte> bytes) noexcept {
        if (::lseek(log_fd_, 0, SEEK_END) < 0) return false;
        std::size_t total = 0;
        while (total < bytes.size()) {
            const ssize_t written = detail::retry_on_eintr(
                [&] { return ::write(log_fd_, bytes.data() + total, bytes.size() - total); });
            if (written <= 0) return false;
            total += static_cast<std::size_t>(written);
        }
        return detail::retry_on_eintr([&] { return ::fsync(log_fd_); }) == 0;
    }
    std::uint64_t log_size() const noexcept {
        struct stat st{};
        if (::fstat(log_fd_, &st) != 0) return 0;
        return static_cast<std::uint64_t>(st.st_size);
    }
    bool read_chunk(std::uint64_t offset, std::span<std::byte> buffer, std::size_t want,
                     std::size_t& out_read) noexcept {
        out_read = 0;
        const std::size_t effective_want = want < buffer.size() ? want : buffer.size();
        if (effective_want == 0) return true;
        std::size_t total = 0;
        while (total < effective_want) {
            const ssize_t n = detail::retry_on_eintr([&] {
                return ::pread(log_fd_, buffer.data() + total, effective_want - total,
                               static_cast<off_t>(offset + total));
            });
            if (n < 0) return false;
            if (n == 0) break;  // EOF -- short read is legal, caller decides
            total += static_cast<std::size_t>(n);
        }
        out_read = total;
        return true;
    }
    bool write_tip_anchor(std::span<const std::byte> anchor_bytes) noexcept {
        const std::string tmp_path = tip_path_ + ".tmp";
        int fd = ::open(tmp_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd < 0) return false;
        std::size_t total = 0;
        bool ok = true;
        while (ok && total < anchor_bytes.size()) {
            const ssize_t written = detail::retry_on_eintr([&] {
                return ::write(fd, anchor_bytes.data() + total, anchor_bytes.size() - total);
            });
            if (written <= 0) {
                ok = false;
                break;
            }
            total += static_cast<std::size_t>(written);
        }
        ok = ok && (detail::retry_on_eintr([&] { return ::fsync(fd); }) == 0);
        ::close(fd);
        if (!ok) return false;

        if (::rename(tmp_path.c_str(), tip_path_.c_str()) != 0) return false;

        // Directory-entry fsync: the rename itself needs the containing
        // directory's metadata flushed for crash-durability, not just the
        // file's own contents.
        const auto slash = tip_path_.find_last_of('/');
        const std::string dir = (slash == std::string::npos) ? "." : tip_path_.substr(0, slash);
        int dir_fd = ::open(dir.c_str(), O_RDONLY);
        if (dir_fd < 0) return false;
        const bool dir_ok = (detail::retry_on_eintr([&] { return ::fsync(dir_fd); }) == 0);
        ::close(dir_fd);
        return dir_ok;
    }
    bool read_tip_anchor(std::vector<std::byte>& out) noexcept {
        int fd = ::open(tip_path_.c_str(), O_RDONLY);
        if (fd < 0) return false;
        const off_t size = ::lseek(fd, 0, SEEK_END);
        if (size < 0) {
            ::close(fd);
            return false;
        }
        if (static_cast<std::uint64_t>(size) > kMaxDurableLogBytes ||
            !detail::try_resize(out, static_cast<std::size_t>(size))) {
            ::close(fd);
            return false;
        }
        bool ok = true;
        if (!out.empty()) {
            if (::lseek(fd, 0, SEEK_SET) < 0) {
                ok = false;
            } else {
                std::size_t total = 0;
                while (ok && total < out.size()) {
                    const ssize_t n = detail::retry_on_eintr(
                        [&] { return ::read(fd, out.data() + total, out.size() - total); });
                    if (n <= 0) {
                        ok = false;
                        break;
                    }
                    total += static_cast<std::size_t>(n);
                }
            }
        }
        ::close(fd);
        return ok;
    }

private:
    int lock_fd_{-1};
    int log_fd_{-1};
#endif

private:
    std::string log_path_;
    std::string lock_path_;
    std::string tip_path_;
    bool log_open_{false};
};

}  // namespace hy
