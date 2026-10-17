// SPDX-License-Identifier: proprietary
// alloc_probe.hpp -- a counting replacement for the global allocation functions, for tests that prove "this path calls operator new
// zero times".
//
// INCLUDE THIS HEADER FROM EXACTLY ONE TRANSLATION UNIT OF A TEST EXECUTABLE. It DEFINES the replaceable global operator new / delete
// (they may be defined once per program and may not be inline), so a second including TU of the same executable is a link error: loud,
// never silent. Every executable that includes it counts every operator new in the whole process, simdjson's and the standard library's
// included, but only while a Window is open.
//
// How a test uses it (see test_binance_json_parser_allocation.cpp):
//   * build every input BEFORE the window opens; inside it there is no gtest macro, no container growth and no string construction
//     (they allocate), only the call under test and plain stores into storage that already exists;
//   * read window.calls() / window.bytes() inside, assert on them AFTER the window is closed;
//   * run selfcheck() once (the positive control): if the counting functions were not the ones the executable links, or the window never
//     opened, a "0 allocations" result would be true for the wrong reason.
// What is counted is operator new / new[] in every form (plain, nothrow, aligned), which is how this codebase defines "heap" on a hot path
// (CLAUDE.md, Engineering Mandate 1). A raw malloc() called from inside a library would not be seen.
//
// Governance: test-only.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace alloc_probe {

inline std::atomic<bool> g_open{false};
inline std::atomic<std::size_t> g_calls{0};
inline std::atomic<std::size_t> g_bytes{0};

inline void note(std::size_t n) noexcept {
    if (g_open.load(std::memory_order_relaxed)) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        g_bytes.fetch_add(n, std::memory_order_relaxed);
    }
}

inline void* plain_alloc(std::size_t n) noexcept { return std::malloc(n != 0 ? n : 1); }

#if defined(_MSC_VER)
inline void* aligned_alloc_impl(std::size_t n, std::size_t align) noexcept { return _aligned_malloc(n != 0 ? n : 1, align); }
inline void aligned_free_impl(void* p) noexcept { _aligned_free(p); }
#else
inline void* aligned_alloc_impl(std::size_t n, std::size_t align) noexcept {
    void* p = nullptr;
    return ::posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, n != 0 ? n : 1) == 0 ? p : nullptr;
}
inline void aligned_free_impl(void* p) noexcept { std::free(p); }
#endif

// While a Window exists, every operator new is counted (calls and bytes since the window opened). Windows do not nest.
class Window {
public:
    Window() noexcept {
        g_calls.store(0, std::memory_order_relaxed);
        g_bytes.store(0, std::memory_order_relaxed);
        g_open.store(true, std::memory_order_relaxed);
    }
    ~Window() { g_open.store(false, std::memory_order_relaxed); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    [[nodiscard]] std::size_t calls() const noexcept { return g_calls.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t bytes() const noexcept { return g_bytes.load(std::memory_order_relaxed); }
};

// volatile: the compiler may not drop the block it holds (a new/delete pair nobody observes may legally be elided).
inline volatile char* volatile g_sink = nullptr;

// The positive control: the forms simdjson and the standard library allocate through must each be seen, and nothing outside a window may be.
struct SelfCheck {
    std::size_t after_array_new{0};          // expect 1: new char[100]
    std::size_t after_nothrow_array_new{0};  // expect 2: new (std::nothrow) char[100] -- simdjson's form
    std::size_t after_string_growth{0};      // expect 3: std::string growing past its small buffer
    std::size_t outside_window{0};           // expect 0: the same allocations with no window open
};

inline SelfCheck selfcheck() {
    SelfCheck c;
    {
        const Window window;
        g_sink = new char[100];
        g_sink[0] = 1;
        c.after_array_new = window.calls();
        delete[] const_cast<char*>(g_sink);
        g_sink = new (std::nothrow) char[100];
        g_sink[0] = 1;
        c.after_nothrow_array_new = window.calls();
        delete[] const_cast<char*>(g_sink);
        std::string grown;
        grown.resize(1000, 'x');
        g_sink = &grown[0];
        c.after_string_growth = window.calls();
        g_sink = nullptr;
    }
    g_calls.store(0, std::memory_order_relaxed);
    g_sink = new char[100];
    g_sink[0] = 1;
    delete[] const_cast<char*>(g_sink);
    g_sink = nullptr;
    c.outside_window = g_calls.load(std::memory_order_relaxed);
    return c;
}

}  // namespace alloc_probe

// GCC 14's -Wmismatched-new-delete cannot see that BOTH halves of this pair are replaced: when a replacement operator delete is inlined into
// code whose pointer came from operator new, it reports the std::free() inside as a mismatch. So the replacements are never inlined (a
// standalone delete body sees a pointer of unknown origin), and the block carries a diagnostic fence as well.
#if defined(_MSC_VER)
#define HY_PROBE_NOINLINE
#else
#define HY_PROBE_NOINLINE __attribute__((noinline))
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

HY_PROBE_NOINLINE void* operator new(std::size_t n) {
    alloc_probe::note(n);
    if (void* p = alloc_probe::plain_alloc(n)) return p;
    throw std::bad_alloc();
}
HY_PROBE_NOINLINE void* operator new[](std::size_t n) { return ::operator new(n); }
HY_PROBE_NOINLINE void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    alloc_probe::note(n);
    return alloc_probe::plain_alloc(n);
}
HY_PROBE_NOINLINE void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
HY_PROBE_NOINLINE void* operator new(std::size_t n, std::align_val_t align) {
    alloc_probe::note(n);
    if (void* p = alloc_probe::aligned_alloc_impl(n, static_cast<std::size_t>(align))) return p;
    throw std::bad_alloc();
}
HY_PROBE_NOINLINE void* operator new[](std::size_t n, std::align_val_t align) { return ::operator new(n, align); }
HY_PROBE_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete(void* p, std::align_val_t) noexcept { alloc_probe::aligned_free_impl(p); }
HY_PROBE_NOINLINE void operator delete[](void* p, std::align_val_t) noexcept { alloc_probe::aligned_free_impl(p); }
HY_PROBE_NOINLINE void operator delete(void* p, std::size_t, std::align_val_t) noexcept { alloc_probe::aligned_free_impl(p); }
HY_PROBE_NOINLINE void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { alloc_probe::aligned_free_impl(p); }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
