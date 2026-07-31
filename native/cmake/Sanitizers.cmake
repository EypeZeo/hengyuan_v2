# Sanitizers.cmake — runtime instrumentation for the HengYuan native core.
# Usage: call hengyuan_enable_sanitizers() ONCE, near the top of CMakeLists.txt.
#
# WHY A SINGLE ENUM OPTION AND NOT TWO BOOLEANS
# ---------------------------------------------
# AddressSanitizer and ThreadSanitizer both require exclusive control of the
# process's shadow memory mapping and cannot coexist. Two independent booleans
# would let someone set both and get a confusing link-time or runtime failure.
# HY_SANITIZER is one value, so the mutual exclusion is unrepresentable.
#
#   -DHY_SANITIZER=none      (default) no instrumentation
#   -DHY_SANITIZER=address   ASan + UBSan  — memory safety and undefined behavior
#   -DHY_SANITIZER=thread    TSan          — data races and memory-ordering bugs
#
# WHAT EACH ONE DOES *NOT* COVER — read this before trusting a green run
# ---------------------------------------------------------------------
# ASan/UBSan say NOTHING WHATSOEVER about memory ordering. They will not notice
# an acquire/release pair downgraded to relaxed. Only TSan speaks to that, and
# only TSan's happens-before model — not the x86 hardware, which gives
# acquire/release semantics for free and will happily run incorrect code
# correctly. Do not read "we run sanitizers" as "the atomics are verified".
#
# Conversely TSan does not replace ASan: it does not detect heap overflows,
# use-after-free, or integer UB.
#
# ORDERING REQUIREMENT
# --------------------
# This MUST be invoked before include(FetchContent)/FetchContent_MakeAvailable,
# so GoogleTest and simdjson are compiled with the same instrumentation. An
# uninstrumented GoogleTest under TSan is actively harmful: it both produces
# false positives on gtest's own synchronization AND loses real happens-before
# edges, so genuine races go unreported.

set(HY_SANITIZER "none" CACHE STRING "Runtime sanitizer: none | address | thread")
set_property(CACHE HY_SANITIZER PROPERTY STRINGS none address thread)

function(hengyuan_enable_sanitizers)
    if(HY_SANITIZER STREQUAL "none")
        return()
    endif()

    if(MSVC)
        if(HY_SANITIZER STREQUAL "address")
            # MSVC ships ASan but not UBSan. Incremental linking is incompatible.
            add_compile_options(/fsanitize=address)
            add_link_options(/INCREMENTAL:NO)
            message(STATUS "HY_SANITIZER=address — MSVC AddressSanitizer enabled (no UBSan on MSVC)")
        else()
            # Fail loudly rather than silently producing an uninstrumented build
            # that reports green. A sanitizer job that isn't sanitizing is worse
            # than no job: it manufactures confidence.
            message(FATAL_ERROR
                "HY_SANITIZER=${HY_SANITIZER} is not available on MSVC. "
                "MSVC supports 'address' only; use GCC/Clang for 'thread'.")
        endif()
        return()
    endif()

    if(HY_SANITIZER STREQUAL "address")
        # -fno-sanitize-recover=all is NOT optional. By default UBSan prints a
        # diagnostic and CONTINUES, leaving the process exit code at 0 — so a CI
        # job would report success while UBSan was reporting undefined behavior
        # on every run. Without this flag the UBSan half is purely decorative.
        set(_hy_san_flags
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
            -fno-omit-frame-pointer)
        message(STATUS "HY_SANITIZER=address — ASan + UBSan enabled (halt-on-error)")
    elseif(HY_SANITIZER STREQUAL "thread")
        # -O0 is NOT a conservative default here -- it is load-bearing, found by
        # actually running the TSan negative control (tsan_control_relaxed_ring)
        # and watching it silently miss a real, deliberately-injected data race.
        # Root cause, isolated with a series of standalone repros: GCC 14's
        # optimizer at -O1 AND -O2 (RelWithDebInfo's default) can compile a
        # multi-word struct copy (e.g. the 32-byte Payload racing in that
        # control) into a form TSan's instrumentation pass does not fully see —
        # the exact same race is caught instantly at -O0 and missed at both -O1
        # and -O2 on this toolchain. A TSan job that silently stops detecting
        # races because someone "reasonably" changed the build type is worse
        # than no TSan job: it manufactures confidence while(no error) is
        # printed. -O0 costs real wall-clock time; that is the trade being made
        # on purpose. Appended after RelWithDebInfo's own -O2 so it wins (GCC
        # takes the last -O flag on the command line).
        set(_hy_san_flags -fsanitize=thread -fno-omit-frame-pointer -O0)
        message(STATUS "HY_SANITIZER=thread — ThreadSanitizer enabled (forcing -O0: -O1/-O2 measurably miss races on this toolchain, see comment)")
    else()
        message(FATAL_ERROR
            "HY_SANITIZER must be one of: none, address, thread (got '${HY_SANITIZER}')")
    endif()

    # Applied directory-wide (not per-target) and BEFORE FetchContent so the
    # fetched dependencies are instrumented too — see the ordering note above.
    # Link flags must match compile flags or the sanitizer runtime is not linked.
    add_compile_options(${_hy_san_flags})
    add_link_options(${_hy_san_flags})
endfunction()
