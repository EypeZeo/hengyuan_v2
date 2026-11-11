// SPDX-License-Identifier: proprietary
// secure_wipe.hpp — cross-platform secure memory wipe that resists compiler elision.
// Governance: L1 (utility primitive, no secret, no network).

#pragma once

#include <array>
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

// RAII companion of secure_wipe() for a stack buffer that holds key material: the buffer is wiped when the scope ends, on EVERY way out of it -- the early
// `return` of a failure branch as much as the normal end. The hand-written secure_wipe() calls this replaced had to be repeated before each return, and a
// return added later without one left a derived HMAC key in the dead stack frame (audit F-B-002, L-60). Declare it right after the buffer, before anything is
// written into it:
//
//     std::array<std::byte, kKeyBlockSize> key_block{};
//     ScopedSecureWipe wipe_key_block(key_block);
//
// native/tests/test_key_copy_wipe_guard.cpp pins that every local key block in the durable sinks and in the audit export has one. The cost is one 64-byte
// wipe per scope exit, which the Owner accepted for it (2026-11-10) and which sits next to the fsync of the same append.
template <std::size_t N>
class ScopedSecureWipe {
public:
    explicit ScopedSecureWipe(std::array<std::byte, N>& buffer) noexcept : data_(buffer.data()) {}
    ~ScopedSecureWipe() { secure_wipe(data_, N); }
    ScopedSecureWipe(const ScopedSecureWipe&) = delete;
    ScopedSecureWipe& operator=(const ScopedSecureWipe&) = delete;

private:
    std::byte* data_;
};

}  // namespace hy
