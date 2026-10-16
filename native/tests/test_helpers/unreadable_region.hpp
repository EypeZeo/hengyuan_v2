// SPDX-License-Identifier: proprietary
// unreadable_region.hpp -- address space that cannot be read or written, handed to a function as a std::string_view.
//
// For proving "refused BEFORE any byte of the input is touched": a function that checks the length first just returns; one that copies,
// scans or even peeks at the first byte faults (access violation on Windows, SIGSEGV elsewhere) -- deterministically, with no dependence
// on what happens to lie behind a buffer. (The alternative, a real over-long buffer, only shows the refusal by its result; a missing
// check then overflows a destination in silence.)
//
// The region is reserved, never committed (VirtualAlloc MEM_RESERVE / PAGE_NOACCESS, mmap PROT_NONE), so a 8 MiB view costs no memory.
//
// Governance: test-only.

#pragma once

#include <cstddef>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace hy_test {

class UnreadableRegion {
public:
    explicit UnreadableRegion(std::size_t size) noexcept : size_(size) {
#if defined(_WIN32)
        base_ = ::VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
#else
        void* p = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base_ = (p == MAP_FAILED) ? nullptr : p;
#endif
    }
    ~UnreadableRegion() {
#if defined(_WIN32)
        if (base_ != nullptr) ::VirtualFree(base_, 0, MEM_RELEASE);
#else
        if (base_ != nullptr) ::munmap(base_, size_);
#endif
    }
    UnreadableRegion(const UnreadableRegion&) = delete;
    UnreadableRegion& operator=(const UnreadableRegion&) = delete;

    [[nodiscard]] bool ok() const noexcept { return base_ != nullptr; }

    // The first n bytes of the region (n <= the size it was built with). Reading ANY of them faults.
    [[nodiscard]] std::string_view view(std::size_t n) const noexcept {
        return std::string_view(static_cast<const char*>(base_), n <= size_ ? n : size_);
    }

private:
    void* base_{nullptr};
    std::size_t size_;
};

}  // namespace hy_test
