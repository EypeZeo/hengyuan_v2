// SPDX-License-Identifier: proprietary
// windows_junction.hpp -- test-only: install an NTFS directory junction (Windows only).
//
// Shared by test_windows_native_io.cpp (the primitive layer: open_directory) and test_compaction_lease.cpp (the lease
// layer: CandidateLease::acquire), so the byte-exact reparse buffer exists once. NOT production code; no production
// header includes this.
#pragma once

#ifdef _WIN32

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>  // FSCTL_SET_REPARSE_POINT -- not always pulled in by windows.h alone

namespace hy::test_helpers {

// Builds and installs an NTFS directory junction at `link`, pointing at `target` (both must be absolute). Byte-exact
// manual REPARSE_DATA_BUFFER construction (that type is WDK-only; FSCTL_SET_REPARSE_POINT itself is not, see
// windows_native_io.hpp's own header comment on the same "stable constant, WDK-only struct" split) -- offsets computed
// directly rather than via sizeof(struct) to avoid any compiler-padding ambiguity. Junctions, unlike symlinks, need no
// elevated privilege / Developer Mode on Windows. `link` must not exist yet: it is created as an empty directory first.
inline bool create_directory_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    if (!std::filesystem::create_directory(link)) return false;

    HANDLE h = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const std::wstring substitute = L"\\??\\" + target.wstring();
    const std::wstring print_name = target.wstring();
    const auto sub_len_bytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    const auto print_len_bytes = static_cast<USHORT>(print_name.size() * sizeof(wchar_t));
    const std::size_t total_bytes = 16 + sub_len_bytes + 2 + print_len_bytes + 2;

    std::vector<std::byte> buf(total_bytes);
    const auto write_u32 = [&](std::size_t off, ULONG v) { std::memcpy(buf.data() + off, &v, 4); };
    const auto write_u16 = [&](std::size_t off, USHORT v) { std::memcpy(buf.data() + off, &v, 2); };
    write_u32(0, 0xA0000003UL);  // IO_REPARSE_TAG_MOUNT_POINT
    write_u16(4, static_cast<USHORT>(8 + sub_len_bytes + 2 + print_len_bytes + 2));  // ReparseDataLength
    write_u16(6, 0);                                        // Reserved
    write_u16(8, 0);                                        // SubstituteNameOffset
    write_u16(10, sub_len_bytes);                           // SubstituteNameLength
    write_u16(12, static_cast<USHORT>(sub_len_bytes + 2));  // PrintNameOffset
    write_u16(14, print_len_bytes);                         // PrintNameLength
    std::memcpy(buf.data() + 16, substitute.data(), sub_len_bytes);
    buf[16 + sub_len_bytes] = std::byte{0};
    buf[16 + sub_len_bytes + 1] = std::byte{0};
    std::memcpy(buf.data() + 16 + sub_len_bytes + 2, print_name.data(), print_len_bytes);
    buf[16 + sub_len_bytes + 2 + print_len_bytes] = std::byte{0};
    buf[16 + sub_len_bytes + 2 + print_len_bytes + 1] = std::byte{0};

    DWORD bytes_returned = 0;
    const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0,
                                      &bytes_returned, nullptr);
    ::CloseHandle(h);
    return ok == TRUE;
}

}  // namespace hy::test_helpers

#endif  // _WIN32
