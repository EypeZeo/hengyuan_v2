// SPDX-License-Identifier: proprietary
// kek_loader.hpp — L3 secure Key-Encryption-Key (KEK) file loader.
//
// Governance: L3 — loads real key material from a fixed-permission file.
// Real KEK bytes never enter stdout / stderr / logs / SHM / subprocess / CLI
// args. See docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md:1231 (§6.1.1.2):
// derived HMAC keys are stored "only wrapped under a process
// Key-Encryption-Key (KEK) -- sourced from a platform secret store / DPAPI /
// file mode 0600 operator-provisioned KEK distinct from the Binance API
// secret." This file implements the 0600-file option (owner decision,
// docs/SPEC_INVARIANTS.md's "Phase 0" entry).
//
// Deliberately NOT a reuse of SecureEnvLoader (env_loader.hpp): that class
// parses .env key=value text via env_parser.hpp's allowlist; a KEK is a
// fixed-size 32-byte binary blob with no key=value structure at all. Reusing
// SecureEnvLoader here would tangle two unrelated concerns (permission/
// anti-symlink checking vs. payload parsing) together. What IS reused,
// deliberately identically, is the security discipline itself: the same
// 0600/0400 permission check, anti-symlink defense, mlock, and secure wipe
// on destruction/error that env_loader.hpp already established and this
// codebase already trusts for the Binance API secret.
//
// The KEK file's path must be distinct from the Binance API secret's .env
// path (docs/SPEC_INVARIANTS.md's Phase 0 entry) -- caller's responsibility,
// same as env_loader.hpp's own "fixed absolute path, never from env var or
// CLI arg" rule.
//
// NOT done here (belongs to KeyRing, key_ring.hpp):
//   - Wrapping/unwrapping HMAC keys under this KEK
//   - Key rotation ceremony, retirement, or destruction of wrapped blobs

#pragma once

#include <hengyuan/secure_wipe.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

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

// HMAC-SHA256's key size is unconstrained by the algorithm itself, but this
// KEK is sized to match a single SHA-256 digest -- large enough that brute
// force is not the weak point, and large enough to key HMAC-SHA256 directly
// without any additional stretching.
inline constexpr std::size_t kKekSize = 32;

enum class KekLoadStatus : std::uint8_t {
    Ok = 0,
    FileNotFound = 1,
    PermissionTooWide = 2,
    IsSymlink = 3,
    SizeMismatch = 4,  // file is not exactly kKekSize bytes -- a KEK is never
                        // silently zero-padded or truncated to fit; a wrong
                        // size is a provisioning error, fail closed
    ReadError = 5,
    LockFailed = 6,
};

class KekLoader {
public:
    KekLoader() = default;

    ~KekLoader() { wipe(); }

    KekLoader(const KekLoader&) = delete;
    KekLoader& operator=(const KekLoader&) = delete;
    KekLoader(KekLoader&&) = delete;
    KekLoader& operator=(KekLoader&&) = delete;

    // Load the KEK from a fixed absolute path with full permission/anti-
    // symlink enforcement. The path must be a compile-time or caller-
    // determined constant -- never sourced from getenv() or CLI argv.
    KekLoadStatus load(const char* path) noexcept {
        wipe();

        KekLoadStatus status;
#ifdef __linux__
        status = load_linux(path);
#elif defined(_WIN32)
        status = load_windows(path);
#else
        status = KekLoadStatus::FileNotFound;  // unsupported platform, fail-closed
#endif

        if (status == KekLoadStatus::Ok) {
            loaded_ = true;
        } else {
            wipe();
        }
        return status;
    }

    // Valid only when is_loaded() -- caller must check first.
    std::span<const std::byte, kKekSize> get() const noexcept {
        return std::span<const std::byte, kKekSize>(kek_.data(), kek_.size());
    }

    bool is_loaded() const noexcept { return loaded_; }

    void wipe() noexcept {
        secure_wipe(kek_.data(), kek_.size());
        loaded_ = false;

#ifdef __linux__
        if (memory_locked_) {
            munlock(kek_.data(), kek_.size());
            memory_locked_ = false;
        }
#elif defined(_WIN32)
        if (memory_locked_) {
            VirtualUnlock(kek_.data(), kek_.size());
            memory_locked_ = false;
        }
#endif
    }

private:
#ifdef __linux__
    KekLoadStatus load_linux(const char* path) noexcept {
        struct stat st{};
        if (lstat(path, &st) != 0) return KekLoadStatus::FileNotFound;
        if (S_ISLNK(st.st_mode)) return KekLoadStatus::IsSymlink;
        if (!S_ISREG(st.st_mode)) return KekLoadStatus::FileNotFound;

        // Owner-only: reject if group or other have any permission bits.
        const mode_t perm = st.st_mode & 0777;
        if (perm & 0077) return KekLoadStatus::PermissionTooWide;

        if (static_cast<std::size_t>(st.st_size) != kKekSize) {
            return KekLoadStatus::SizeMismatch;
        }

        if (mlock(kek_.data(), kek_.size()) == 0) {
            memory_locked_ = true;
        } else {
            return KekLoadStatus::LockFailed;
        }

        const int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) return KekLoadStatus::ReadError;

        const ssize_t n = read(fd, kek_.data(), kek_.size());
        close(fd);

        if (n < 0 || static_cast<std::size_t>(n) != kKekSize) {
            return KekLoadStatus::ReadError;
        }
        return KekLoadStatus::Ok;
    }
#endif

#ifdef _WIN32
    KekLoadStatus load_windows(const char* path) noexcept {
        const DWORD attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) return KekLoadStatus::FileNotFound;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return KekLoadStatus::IsSymlink;
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) return KekLoadStatus::FileNotFound;

        if (VirtualLock(kek_.data(), kek_.size())) {
            memory_locked_ = true;
        }
        // VirtualLock failure is non-fatal here (requires elevated
        // privileges), matching env_loader.hpp's own precedent -- proceed
        // regardless, the buffer is ephemeral either way.

        HANDLE hFile = CreateFileA(
            path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return KekLoadStatus::ReadError;

        // AUDIT CRED-REPARSE-FAILOPEN-037: the `&&` short-circuited on query
        // failure, silently skipping the reparse re-check. Same fix and same
        // reasoning as env_loader.hpp's copy of this block.
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(hFile, &info)) {
            CloseHandle(hFile);
            return KekLoadStatus::ReadError;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            CloseHandle(hFile);
            return KekLoadStatus::IsSymlink;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(hFile, &fileSize) ||
            static_cast<std::size_t>(fileSize.QuadPart) != kKekSize) {
            CloseHandle(hFile);
            return KekLoadStatus::SizeMismatch;
        }

        DWORD bytesRead = 0;
        const BOOL readOk = ReadFile(hFile, kek_.data(), static_cast<DWORD>(kKekSize),
                                      &bytesRead, nullptr);
        CloseHandle(hFile);

        if (!readOk || bytesRead != kKekSize) return KekLoadStatus::ReadError;
        return KekLoadStatus::Ok;
    }
#endif

    bool loaded_{false};
    bool memory_locked_{false};
    std::array<std::byte, kKekSize> kek_{};
};

}  // namespace hy
