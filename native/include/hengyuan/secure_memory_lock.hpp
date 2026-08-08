// SPDX-License-Identifier: proprietary
// secure_memory_lock.hpp — best-effort "keep this buffer out of swap" primitive.
//
// Governance: L1 (utility primitive, no secret of its own, no network).
//
// AUDIT SEC-KEKCOPY-019: kek_loader.hpp mlocks the KEK it reads, and env_loader.hpp
// mlocks the .env buffer -- ADR-019 D4's checklist lists "Memory lock (mlock) to
// prevent swap" as a requirement. But KeyRing and LastRemoteAckedTipStore each keep
// their OWN copy of that same KEK in an ordinary member array, which the page
// allocator is free to write to the swap file. The protection on the original was
// therefore undone by the copies, and both live for the whole process lifetime.
//
// Deliberately split out of secure_wipe.hpp rather than added to it: this needs
// <windows.h> / <sys/mman.h>, and secure_wipe.hpp's own header comment records the
// decision to keep those out of a header included nearly everywhere.
//
// BEST EFFORT, and callers must treat it that way. mlock is bounded by
// RLIMIT_MEMLOCK and VirtualLock by the process working-set quota, so a failure is
// an environment/provisioning fact rather than a code defect. Unlike
// kek_loader.hpp -- where a lock failure is fatal because the caller is still able
// to abort the whole load -- these call sites are constructors with no error
// channel, so they record the outcome (see the memory_locked() accessors) instead
// of failing construction. The secure_wipe on destruction happens either way.

#pragma once

#include <cstddef>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#endif

namespace hy {

// Returns true iff the range is now locked into physical memory. Locking is
// page-granular on every supported platform, so this may lock more than [ptr, len).
inline bool try_lock_memory(void* ptr, std::size_t len) noexcept {
    if (!ptr || len == 0) return false;
#if defined(_WIN32)
    return VirtualLock(ptr, len) != 0;
#elif defined(__linux__)
    return ::mlock(ptr, len) == 0;
#else
    return false;  // unsupported platform: report honestly rather than claim success
#endif
}

// Must only be called with a range try_lock_memory() returned true for.
inline void unlock_memory(void* ptr, std::size_t len) noexcept {
    if (!ptr || len == 0) return;
#if defined(_WIN32)
    VirtualUnlock(ptr, len);
#elif defined(__linux__)
    ::munlock(ptr, len);
#else
    (void)ptr;
    (void)len;
#endif
}

}  // namespace hy
