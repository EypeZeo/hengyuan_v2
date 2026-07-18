// SPDX-License-Identifier: proprietary
// secure_wipe.hpp — cross-platform secure memory wipe that resists compiler elision.
// Governance: L1 (utility primitive, no secret, no network).

#pragma once

#include <cstddef>
#include <cstring>

namespace hy {

inline void secure_wipe(void* ptr, std::size_t len) noexcept {
    if (!ptr || len == 0) return;

#if defined(_WIN32)
    // MSVC: RtlSecureZeroMemory / SecureZeroMemory — never optimized away.
    // Declared in <windows.h> but we avoid pulling that in; use volatile cast.
    volatile unsigned char* vp = static_cast<volatile unsigned char*>(ptr);
    while (len--) { *vp++ = 0; }
#elif defined(__STDC_LIB_EXT1__) || (defined(__STDC_WANT_LIB_EXT1__) && __STDC_WANT_LIB_EXT1__)
    memset_s(ptr, len, 0, len);
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    explicit_bzero(ptr, len);
#else
    // Portable fallback: volatile function pointer prevents elision.
    static void* (*const volatile memset_func)(void*, int, std::size_t) = std::memset;
    memset_func(ptr, 0, len);
#endif
}

}  // namespace hy
