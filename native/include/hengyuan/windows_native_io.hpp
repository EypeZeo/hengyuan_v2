// SPDX-License-Identifier: proprietary
// windows_native_io.hpp — minimal NT native API wrapper for handle-relative
// file I/O on Windows (Round D, docs/SPEC_INVARIANTS.md's "Seal-journal
// Round D" entry).
//
// WHY THIS EXISTS: Win32's CreateFileW/MoveFileExW/CreateHardLinkW only
// accept full or drive-relative paths, never "relative to an already-open
// directory HANDLE" -- there is no Win32-level equivalent of POSIX's
// openat()/renameat2(). Without it, every file operation under a candidate
// directory would have to re-resolve the path from scratch, reopening the
// exact TOCTOU window (directory renamed/recreated between "check identity"
// and "open by path") that compaction_lease.hpp exists to close (Round D
// review v3/v4's P0). The NT native layer (ntdll.dll, always loaded in every
// Windows process) DOES support handle-relative opens and no-replace
// renames via OBJECT_ATTRIBUTES::RootDirectory -- this file is the minimal,
// hand-declared wrapper around exactly the two NT calls needed
// (NtCreateFile, NtSetInformationFile's FileRenameInformation class), not a
// general NT API binding.
//
// These are undocumented-but-extremely-stable "Native API" functions (used
// internally by kernel32.dll itself; the signatures and FILE_* constants
// below have been unchanged since Windows NT 4 / XP and are widely
// referenced in public Microsoft documentation, "Windows Internals," and
// countless shipped user-mode tools that need handle-relative I/O without
// requiring the WDK). Resolved via GetProcAddress on ntdll.dll at runtime,
// not linked against ntdll.lib -- ntdll.lib is a WDK component, not part of
// a default Windows SDK/MSVC install, and this repo's build should not
// require it.
//
// SCOPE: if a specific operation can't be done safely via this handle-
// relative layer, functions here return DirRelativeIoResult::Unsupported
// rather than falling back to a path-based Win32 call that would reopen the
// TOCTOU window -- see compaction_lease.hpp's callers, which must fail
// closed (UnsupportedAndFenced) on that result, never silently retry by
// path.
//
// POSIX builds never see this file at all (everything below is inside a
// `#ifdef _WIN32` guard) -- the POSIX equivalent (openat/linkat/renameat2)
// lives in compaction_breadcrumb_io.hpp.

#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>  // OBJECT_ATTRIBUTES, UNICODE_STRING, IO_STATUS_BLOCK,
                        // InitializeObjectAttributes -- standard Windows SDK,
                        // no WDK required.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace hy::win_native {

// --- Stable NT API surface, hand-declared (see file header for why) -------

using NTSTATUS = LONG;

inline constexpr NTSTATUS kStatusSuccess = 0x00000000L;
inline constexpr NTSTATUS kStatusObjectNameCollision = static_cast<NTSTATUS>(0xC0000035L);
inline constexpr NTSTATUS kStatusObjectNameNotFound = static_cast<NTSTATUS>(0xC0000034L);
inline constexpr NTSTATUS kStatusObjectPathNotFound = static_cast<NTSTATUS>(0xC000003AL);
inline constexpr NTSTATUS kStatusNotAReparsePoint = static_cast<NTSTATUS>(0xC0000275L);

inline bool nt_success(NTSTATUS status) noexcept { return status >= 0; }

// CreateDisposition values for NtCreateFile -- stable since NT4.
inline constexpr ULONG kFileCreate = 2;     // fail if exists (O_CREAT|O_EXCL equivalent)
inline constexpr ULONG kFileOpen = 1;       // fail if absent

// CreateOptions flags for NtCreateFile -- stable since NT4.
inline constexpr ULONG kFileDirectoryFile = 0x00000001;
inline constexpr ULONG kFileNonDirectoryFile = 0x00000040;
inline constexpr ULONG kFileSynchronousIoNonalert = 0x00000020;
inline constexpr ULONG kFileOpenReparsePoint = 0x00200000;
inline constexpr ULONG kFileOpenForBackupIntent = 0x00004000;  // required to open a
                                                                 // directory handle at all

// FILE_INFORMATION_CLASS values used by NtSetInformationFile/
// NtQueryInformationFile below -- stable since NT4. Declared as plain
// constants (not enumerators of a WDK-declared FILE_INFORMATION_CLASS
// value set) because the standard Windows SDK's <winternl.h> declares the
// FILE_INFORMATION_CLASS *type* but not a complete set of its enumerators
// -- FileBasicInformation/FileRenameInformation specifically are WDK-only
// (<ntifs.h>) even though the type they belong to is SDK-available.
inline constexpr ULONG kFileBasicInformationClass = 4;
inline constexpr ULONG kFileRenameInformationClass = 10;

// Mirrors the stable, publicly-documented FILE_BASIC_INFORMATION layout
// (used by NtQueryInformationFile to read FileAttributes, specifically to
// check FILE_ATTRIBUTE_REPARSE_POINT). Declared under an "Nt" suffix to
// avoid any name collision with an SDK-declared type of the same base name.
struct FileBasicInformationNt {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG FileAttributes;
};

// Mirrors the stable FILE_RENAME_INFORMATION layout (NOT the newer
// _EX variant with POSIX-semantics flags -- deliberately using the older,
// universally-documented structure so this doesn't depend on a specific
// Windows 10+ feature level being present).
//
// Deliberately NOT #pragma pack(1) -- found by actually running this
// against a real NTFS volume: the kernel expects this struct at its
// natural alignment (BOOLEAN followed by 7 bytes of padding before the
// 8-byte-aligned HANDLE on x64), and forcing byte-packing shifts every
// field after ReplaceIfExists out of the position NtSetInformationFile
// expects, which surfaced as a generic STATUS_INVALID_PARAMETER (0xC000000D)
// with no indication the actual cause was struct layout.
struct FileRenameInformationNt {
    BOOLEAN ReplaceIfExists;
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];  // flexible array member idiom; real buffer is allocated
                         // larger and the tail bytes hold the actual name
};

using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                         PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtSetInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                                 FILE_INFORMATION_CLASS);
using NtQueryInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                                    FILE_INFORMATION_CLASS);

namespace detail {

// Resolved once, lazily, never re-resolved -- ntdll.dll is loaded for the
// entire process lifetime, its export table never changes underneath us.
struct NtProcTable {
    NtCreateFileFn nt_create_file{nullptr};
    NtSetInformationFileFn nt_set_information_file{nullptr};
    NtQueryInformationFileFn nt_query_information_file{nullptr};
    bool resolved{false};
    bool ok{false};
};

inline NtProcTable& proc_table() noexcept {
    static NtProcTable table = [] {
        NtProcTable t;
        t.resolved = true;
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) return t;
        t.nt_create_file = reinterpret_cast<NtCreateFileFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtCreateFile")));
        t.nt_set_information_file = reinterpret_cast<NtSetInformationFileFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtSetInformationFile")));
        t.nt_query_information_file = reinterpret_cast<NtQueryInformationFileFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtQueryInformationFile")));
        t.ok = t.nt_create_file != nullptr && t.nt_set_information_file != nullptr &&
               t.nt_query_information_file != nullptr;
        return t;
    }();
    return table;
}

}  // namespace detail

// True iff the three NT functions this file needs were successfully
// resolved from ntdll.dll -- checked once at the start of every public
// function below; if false, every operation returns Unsupported rather
// than risk calling through a null function pointer.
inline bool native_api_available() noexcept { return detail::proc_table().ok; }

// RAII wrapper for a native HANDLE -- move-only, closes via CloseHandle.
class RawHandle {
public:
    RawHandle() noexcept = default;
    explicit RawHandle(HANDLE h) noexcept : h_(h) {}
    ~RawHandle() { reset(); }
    RawHandle(const RawHandle&) = delete;
    RawHandle& operator=(const RawHandle&) = delete;
    RawHandle(RawHandle&& other) noexcept : h_(other.h_) { other.h_ = INVALID_HANDLE_VALUE; }
    RawHandle& operator=(RawHandle&& other) noexcept {
        if (this == &other) return *this;
        reset();
        h_ = other.h_;
        other.h_ = INVALID_HANDLE_VALUE;
        return *this;
    }

    HANDLE get() const noexcept { return h_; }
    bool valid() const noexcept { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    void reset(HANDLE new_h = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) ::CloseHandle(h_);
        h_ = new_h;
    }
    // Releases ownership without closing -- used when a rename operation
    // logically "consumes" the handle's identity (it doesn't, actually; NT
    // rename keeps the same handle valid under the new name, so this is
    // rarely needed, kept for completeness/symmetry with the RAII idiom
    // used elsewhere in this codebase, e.g. secure_memory_lock.hpp).
    HANDLE release() noexcept {
        HANDLE h = h_;
        h_ = INVALID_HANDLE_VALUE;
        return h;
    }

private:
    HANDLE h_{INVALID_HANDLE_VALUE};
};

enum class DirOpenResult : std::uint8_t {
    Opened,
    RejectedReparsePoint,  // FILE_OPEN_REPARSE_POINT was NOT set and the target
                            // turned out to be a reparse point -- NT native
                            // open without that flag transparently follows
                            // reparse points, so this file always opens with
                            // it set and explicitly checks/rejects.
    NotFound,
    Unsupported,
    Failed,
};

// Opens `path` as a directory handle suitable for use as OBJECT_ATTRIBUTES::
// RootDirectory in later handle-relative calls. Rejects reparse points
// (symlink/junction) rather than silently following them -- see
// compaction_lease.hpp's identity-verification requirements.
inline DirOpenResult open_directory(const std::wstring& path, RawHandle& out,
                                     NTSTATUS* out_query_status = nullptr) noexcept {
    if (!native_api_available()) return DirOpenResult::Unsupported;
    auto& t = detail::proc_table();

    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(path.size() * sizeof(WCHAR));
    name.MaximumLength = name.Length;
    name.Buffer = const_cast<PWSTR>(path.c_str());

    // NT native paths need the "\??\" DOS-device prefix to be interpreted
    // as a Win32-style path (drive letter or \\?\ form) rather than as an
    // object-manager-namespace path. RtlDosPathNameToNtPathName_U is the
    // documented way to do this conversion, but it allocates via
    // RtlFreeHeap semantics that are awkward to wrap safely here; the
    // simpler, equally-documented alternative is prefixing "\??\" by hand,
    // which works for any path already in extended-length (\\?\...) or
    // drive-absolute (C:\...) form -- both of which std::filesystem::path
    // produces when given an absolute path, which is what
    // compaction_lease.hpp always passes.
    std::wstring nt_path = L"\\??\\" + path;
    UNICODE_STRING nt_name{};
    nt_name.Length = static_cast<USHORT>(nt_path.size() * sizeof(WCHAR));
    nt_name.MaximumLength = nt_name.Length;
    nt_name.Buffer = const_cast<PWSTR>(nt_path.c_str());

    OBJECT_ATTRIBUTES attrs{};
    InitializeObjectAttributes(&attrs, &nt_name, 0 /* no OBJ_CASE_INSENSITIVE by default */, nullptr,
                                nullptr);

    IO_STATUS_BLOCK iosb{};
    HANDLE h = INVALID_HANDLE_VALUE;
    const NTSTATUS status = t.nt_create_file(
        // FILE_READ_ATTRIBUTES is required for the FileBasicInformation query
        // below (found by testing against a real directory: without it,
        // NtQueryInformationFile returns STATUS_ACCESS_DENIED, 0xC0000022).
        &h,
        static_cast<ACCESS_MASK>(FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE),
        &attrs, &iosb,
        nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        kFileOpen,
        kFileDirectoryFile | kFileSynchronousIoNonalert | kFileOpenForBackupIntent | kFileOpenReparsePoint,
        nullptr, 0);
    if (!nt_success(status)) {
        if (status == kStatusObjectNameNotFound || status == kStatusObjectPathNotFound) {
            return DirOpenResult::NotFound;
        }
        return DirOpenResult::Failed;
    }

    RawHandle handle(h);
    // Reject if it turned out to be a reparse point -- we opened with
    // kFileOpenReparsePoint specifically so the open itself doesn't
    // transparently traverse it; now explicitly check the attribute.
    // Fail-closed if the query itself fails: an unconfirmed "not a reparse
    // point" must not be treated the same as a confirmed one (an earlier
    // version of this function did exactly that -- silently proceeded on
    // query failure -- found to be wrong by testing against a real
    // directory symlink, see the RejectedReparsePoint test below).
    FileBasicInformationNt basic{};
    const NTSTATUS query_status = t.nt_query_information_file(
        handle.get(), &iosb, &basic, sizeof(basic), static_cast<FILE_INFORMATION_CLASS>(kFileBasicInformationClass));
    if (out_query_status != nullptr) *out_query_status = query_status;
    if (!nt_success(query_status)) return DirOpenResult::Failed;
    if ((basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return DirOpenResult::RejectedReparsePoint;
    }

    out = std::move(handle);
    return DirOpenResult::Opened;
}

enum class RelativeCreateResult : std::uint8_t {
    Created,
    AlreadyExists,  // STATUS_OBJECT_NAME_COLLISION -- caller decides what to do
                    // (byte-compare for idempotency, etc.)
    Unsupported,
    Failed,
};

// Creates a new file named `relative_name` inside the directory referenced
// by `dir`, failing (not overwriting) if it already exists -- the Windows
// analogue of POSIX O_CREAT|O_EXCL, and always handle-relative (no
// pathname re-resolution from the filesystem root, which is the whole
// point: this cannot be redirected by a directory rename/recreate that
// happens after `dir` was opened).
inline RelativeCreateResult create_new_relative(const RawHandle& dir, const std::wstring& relative_name,
                                                  RawHandle& out) noexcept {
    if (!native_api_available()) return RelativeCreateResult::Unsupported;
    auto& t = detail::proc_table();

    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(relative_name.size() * sizeof(WCHAR));
    name.MaximumLength = name.Length;
    name.Buffer = const_cast<PWSTR>(relative_name.c_str());

    OBJECT_ATTRIBUTES attrs{};
    InitializeObjectAttributes(&attrs, &name, 0, dir.get(), nullptr);

    IO_STATUS_BLOCK iosb{};
    HANDLE h = INVALID_HANDLE_VALUE;
    // DELETE is required here, not just GENERIC_WRITE/GENERIC_READ -- Windows
    // rename (whether via Win32 SetFileInformationByHandle(FileRenameInfo)
    // or the NT-native FileRenameInformation class used by
    // rename_no_replace() below) requires the handle being renamed to carry
    // DELETE access; NTFS treats rename as delete-and-relink under the hood.
    // Found by actually running this against a real NTFS volume during
    // development, not by reading documentation alone -- the first version
    // of this function omitted DELETE and rename_no_replace failed with a
    // generic (non-STATUS_OBJECT_NAME_COLLISION) error on every call.
    const NTSTATUS status = t.nt_create_file(
        &h, static_cast<ACCESS_MASK>(GENERIC_WRITE | GENERIC_READ | DELETE | SYNCHRONIZE), &attrs, &iosb,
        nullptr, FILE_ATTRIBUTE_NORMAL, 0 /* no sharing while we write it */, kFileCreate,
        kFileNonDirectoryFile | kFileSynchronousIoNonalert, nullptr, 0);
    if (!nt_success(status)) {
        if (status == kStatusObjectNameCollision) return RelativeCreateResult::AlreadyExists;
        return RelativeCreateResult::Failed;
    }
    out = RawHandle(h);
    return RelativeCreateResult::Created;
}

enum class RelativeOpenResult : std::uint8_t {
    Opened,
    NotFound,
    RejectedReparsePoint,
    Unsupported,
    Failed,
};

// Opens an EXISTING file named `relative_name` inside `dir` for reading,
// rejecting reparse points. Handle-relative, same TOCTOU-closing property
// as create_new_relative.
inline RelativeOpenResult open_existing_relative(const RawHandle& dir, const std::wstring& relative_name,
                                                   RawHandle& out, NTSTATUS* out_status = nullptr) noexcept {
    if (!native_api_available()) return RelativeOpenResult::Unsupported;
    auto& t = detail::proc_table();

    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(relative_name.size() * sizeof(WCHAR));
    name.MaximumLength = name.Length;
    name.Buffer = const_cast<PWSTR>(relative_name.c_str());

    OBJECT_ATTRIBUTES attrs{};
    InitializeObjectAttributes(&attrs, &name, 0, dir.get(), nullptr);

    IO_STATUS_BLOCK iosb{};
    HANDLE h = INVALID_HANDLE_VALUE;
    const NTSTATUS status = t.nt_create_file(
        &h, static_cast<ACCESS_MASK>(GENERIC_READ | SYNCHRONIZE), &attrs, &iosb, nullptr,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, kFileOpen,
        kFileNonDirectoryFile | kFileSynchronousIoNonalert | kFileOpenReparsePoint, nullptr, 0);
    if (out_status != nullptr) *out_status = status;
    if (!nt_success(status)) {
        if (status == kStatusObjectNameNotFound) return RelativeOpenResult::NotFound;
        return RelativeOpenResult::Failed;
    }
    RawHandle handle(h);
    FileBasicInformationNt basic{};
    const NTSTATUS query_status = t.nt_query_information_file(
        handle.get(), &iosb, &basic, sizeof(basic), static_cast<FILE_INFORMATION_CLASS>(kFileBasicInformationClass));
    // Fail-closed on query failure -- see open_directory()'s identical fix
    // and comment for why "unconfirmed" must not be treated as "confirmed
    // not a reparse point."
    if (!nt_success(query_status)) return RelativeOpenResult::Failed;
    if ((basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return RelativeOpenResult::RejectedReparsePoint;
    }
    out = std::move(handle);
    return RelativeOpenResult::Opened;
}

enum class RelativeRenameResult : std::uint8_t {
    Renamed,
    AlreadyExists,  // STATUS_OBJECT_NAME_COLLISION -- ReplaceIfExists=FALSE did its job
    Unsupported,
    Failed,
};

// Renames the already-open file `file` to `new_relative_name` inside `dir`,
// WITHOUT replacing an existing file of that name (the direct analogue of
// POSIX renameat2(..., RENAME_NOREPLACE)). Uses the stable (NT4-era)
// FILE_RENAME_INFORMATION structure, not the newer POSIX-semantics
// extended variant, so this doesn't depend on a specific Windows 10+
// feature level.
inline RelativeRenameResult rename_no_replace(const RawHandle& file, const RawHandle& dir,
                                                const std::wstring& new_relative_name,
                                                NTSTATUS* out_status = nullptr) noexcept {
    if (!native_api_available()) return RelativeRenameResult::Unsupported;
    auto& t = detail::proc_table();

    const std::size_t name_bytes = new_relative_name.size() * sizeof(WCHAR);
    const std::size_t total_bytes = sizeof(FileRenameInformationNt) + name_bytes;
    std::vector<std::byte> buf(total_bytes);
    auto* info = reinterpret_cast<FileRenameInformationNt*>(buf.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = dir.get();
    info->FileNameLength = static_cast<ULONG>(name_bytes);
    std::memcpy(info->FileName, new_relative_name.c_str(), name_bytes);

    IO_STATUS_BLOCK iosb{};
    const NTSTATUS status = t.nt_set_information_file(
        file.get(), &iosb, info, static_cast<ULONG>(total_bytes),
        static_cast<FILE_INFORMATION_CLASS>(kFileRenameInformationClass));
    if (out_status != nullptr) *out_status = status;
    if (!nt_success(status)) {
        if (status == kStatusObjectNameCollision) return RelativeRenameResult::AlreadyExists;
        return RelativeRenameResult::Failed;
    }
    return RelativeRenameResult::Renamed;
}

}  // namespace hy::win_native

#endif  // _WIN32
