// SPDX-License-Identifier: proprietary
// env_loader.hpp — L3 secure .env file loader with ADR-019 D4 secret lifecycle gate.
//
// Governance: L3 — loads real credentials from .env file. Requires L3 review.
// Real secret never enters stdout / stderr / logs / SHM / subprocess / CLI args.
//
// ADR-019 D4 checklist enforced by this loader:
//   ✅ Variable-name allowlist (delegated to env_parser.hpp)
//   ✅ File permission check (Linux: must be 0600 or 0400, reject if group/other readable)
//   ✅ Anti-symlink (lstat → reject if S_ISLNK; Windows: GetFileAttributesW reject reparse point)
//   ✅ Fixed absolute path (compiled-in or caller-provided, never from env var or CLI arg)
//   ✅ Memory lock (mlock on Linux to prevent swap)
//   ✅ Secure wipe on destruction and on error
//   ✅ No SHM / no setenv / no subprocess propagation (by design: values stay in this object)
//
// NOT done here (belongs to operational runbook / future packet):
//   - Key rotation ceremony
//   - Revocation workflow
//   - Responsibility assignment (owner = human operator)

#pragma once

#include <hengyuan/env_parser.hpp>
#include <hengyuan/secure_wipe.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hy {

static constexpr std::size_t kMaxEnvFileSize = 4096;

enum class EnvLoadStatus : std::uint8_t {
    Ok = 0,
    FileNotFound = 1,
    PermissionTooWide = 2,
    IsSymlink = 3,
    FileTooLarge = 4,
    ReadError = 5,
    ParseError = 6,
    LockFailed = 7,
};

struct EnvLoadResult {
    EnvLoadStatus status{EnvLoadStatus::Ok};
    EnvParseResult parse_result{};
};

class SecureEnvLoader {
public:
    SecureEnvLoader() = default;

    ~SecureEnvLoader() { wipe(); }

    SecureEnvLoader(const SecureEnvLoader&) = delete;
    SecureEnvLoader& operator=(const SecureEnvLoader&) = delete;
    SecureEnvLoader(SecureEnvLoader&&) = delete;
    SecureEnvLoader& operator=(SecureEnvLoader&&) = delete;

    // Load .env from a fixed absolute path with full D4 gate enforcement.
    // The path must be a compile-time or caller-determined constant —
    // never sourced from getenv() or CLI argv (caller's responsibility).
    EnvLoadResult load(const char* path, const EnvAllowlist& allowlist) noexcept {
        wipe();

        EnvLoadResult result{};

#ifdef __linux__
        result = load_linux(path, allowlist);
#elif defined(_WIN32)
        result = load_windows(path, allowlist);
#else
        // Unsupported platform — fail-closed
        result.status = EnvLoadStatus::FileNotFound;
#endif

        if (result.status != EnvLoadStatus::Ok) {
            wipe();
        } else {
            loaded_ = true;
            parse_result_ = result.parse_result;
        }

        return result;
    }

    // Retrieve a parsed value by key. Returns empty if not loaded or key missing.
    std::string_view get(std::string_view key) const noexcept {
        if (!loaded_) return {};
        return parse_result_.get(key);
    }

    bool has(std::string_view key) const noexcept {
        if (!loaded_) return false;
        return parse_result_.has(key);
    }

    bool is_loaded() const noexcept { return loaded_; }

    // Explicitly wipe all secret material. Called automatically on destruction and error.
    void wipe() noexcept {
        secure_wipe(file_buf_.data(), file_buf_.size());
        parse_result_ = {};
        bytes_read_ = 0;
        loaded_ = false;

#ifdef __linux__
        if (memory_locked_) {
            munlock(file_buf_.data(), file_buf_.size());
            memory_locked_ = false;
        }
#elif defined(_WIN32)
        if (memory_locked_) {
            VirtualUnlock(file_buf_.data(), file_buf_.size());
            memory_locked_ = false;
        }
#endif
    }

private:
#ifdef __linux__
    EnvLoadResult load_linux(const char* path, const EnvAllowlist& allowlist) noexcept {
        EnvLoadResult result{};

        // Anti-symlink: use lstat (does NOT follow symlinks)
        struct stat st{};
        if (lstat(path, &st) != 0) {
            result.status = EnvLoadStatus::FileNotFound;
            return result;
        }

        if (S_ISLNK(st.st_mode)) {
            result.status = EnvLoadStatus::IsSymlink;
            return result;
        }

        if (!S_ISREG(st.st_mode)) {
            result.status = EnvLoadStatus::FileNotFound;
            return result;
        }

        // Permission check: must be 0600 or 0400 (owner-only)
        // Reject if group or other have any permission bits
        mode_t perm = st.st_mode & 0777;
        if (perm & 0077) {
            result.status = EnvLoadStatus::PermissionTooWide;
            return result;
        }

        // Size check
        if (static_cast<std::size_t>(st.st_size) > kMaxEnvFileSize) {
            result.status = EnvLoadStatus::FileTooLarge;
            return result;
        }

        // Lock buffer memory to prevent swap
        if (mlock(file_buf_.data(), file_buf_.size()) == 0) {
            memory_locked_ = true;
        } else {
            result.status = EnvLoadStatus::LockFailed;
            return result;
        }

        // Open with O_NOFOLLOW as additional symlink protection
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) {
            result.status = EnvLoadStatus::ReadError;
            return result;
        }

        ssize_t n = read(fd, file_buf_.data(), file_buf_.size());
        close(fd);

        if (n < 0) {
            result.status = EnvLoadStatus::ReadError;
            return result;
        }

        bytes_read_ = static_cast<std::size_t>(n);
        return parse_buffer(allowlist);
    }
#endif

#ifdef _WIN32
    EnvLoadResult load_windows(const char* path, const EnvAllowlist& allowlist) noexcept {
        EnvLoadResult result{};

        // Anti-symlink / reparse point check
        DWORD attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            result.status = EnvLoadStatus::FileNotFound;
            return result;
        }

        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            result.status = EnvLoadStatus::IsSymlink;
            return result;
        }

        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            result.status = EnvLoadStatus::FileNotFound;
            return result;
        }

        // Lock buffer memory to prevent pagefile swap
        if (VirtualLock(file_buf_.data(), file_buf_.size())) {
            memory_locked_ = true;
        }
        // VirtualLock failure is non-fatal on Windows (requires elevated privileges)
        // but we still proceed — the file content is ephemeral in the buffer.

        // Open file — FILE_FLAG_OPEN_REPARSE_POINT prevents following symlinks
        HANDLE hFile = CreateFileA(
            path,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);

        if (hFile == INVALID_HANDLE_VALUE) {
            result.status = EnvLoadStatus::ReadError;
            return result;
        }

        // Re-check: if the opened handle is actually a reparse point, reject.
        //
        // AUDIT CRED-REPARSE-FAILOPEN-037: the query failing used to SKIP this
        // check and proceed. windows_native_io.hpp already records this exact
        // mistake and its fix ("an unconfirmed 'not a reparse point' must not
        // be treated the same as a confirmed one"); this file had the same
        // shape. Fail closed on query failure.
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(hFile, &info)) {
            CloseHandle(hFile);
            result.status = EnvLoadStatus::ReadError;
            return result;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            CloseHandle(hFile);
            result.status = EnvLoadStatus::IsSymlink;
            return result;
        }

        // Size check
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(hFile, &fileSize) ||
            static_cast<std::size_t>(fileSize.QuadPart) > kMaxEnvFileSize) {
            CloseHandle(hFile);
            result.status = EnvLoadStatus::FileTooLarge;
            return result;
        }

        DWORD bytesRead = 0;
        BOOL readOk = ReadFile(hFile, file_buf_.data(),
                               static_cast<DWORD>(fileSize.QuadPart),
                               &bytesRead, nullptr);
        CloseHandle(hFile);

        if (!readOk) {
            result.status = EnvLoadStatus::ReadError;
            return result;
        }

        bytes_read_ = static_cast<std::size_t>(bytesRead);
        return parse_buffer(allowlist);
    }
#endif

    EnvLoadResult parse_buffer(const EnvAllowlist& allowlist) noexcept {
        EnvLoadResult result{};
        std::string_view buf(file_buf_.data(), bytes_read_);
        result.parse_result = parse_env_buffer(buf, allowlist);

        if (result.parse_result.status != EnvParseStatus::Ok) {
            result.status = EnvLoadStatus::ParseError;
        }

        return result;
    }

    bool loaded_{false};
    bool memory_locked_{false};
    std::size_t bytes_read_{0};
    std::array<char, kMaxEnvFileSize> file_buf_{};
    EnvParseResult parse_result_{};
};

}  // namespace hy
