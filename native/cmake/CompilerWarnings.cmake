# CompilerWarnings.cmake — strict warning policy for HengYuan native core.
# Usage: hengyuan_set_warnings(<target>)
#        hengyuan_allow_third_party_tsan_fences(<target>)   [Asio consumers only]

function(hengyuan_set_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
            -Wall -Wextra -Wpedantic -Werror
            -Wconversion -Wsign-conversion -Wshadow
            -Wnon-virtual-dtor -Wcast-align
            -Woverloaded-virtual
            -Wdouble-promotion -Wformat=2
        >
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4 /WX /permissive- /utf-8 /bigobj
            /wd4324
        >
    )
endfunction()

# AUDIT BUILD-TSAN-WS-SESSION-008.
#
# GCC's -Wtsan fires on std::atomic_thread_fence under -fsanitize=thread
# ("'atomic_thread_fence' is not supported with '-fsanitize=thread'"), because
# TSan's happens-before model cannot represent a standalone fence. Under this
# file's -Werror that is a hard compile error, so any translation unit that
# inlines such a fence simply cannot be built in thread mode.
#
# Boost.Asio's detail::std_fenced_block (boost/asio/detail/std_fenced_block.hpp)
# contains exactly that fence, and every Asio-using TU inlines it. Unlike
# shm_heartbeat.hpp -- which hit this same GCC limitation in OUR code and was
# rewritten to use per-load acquire instead of a fence, see its own comment --
# this is third-party code we do not control, so the "rewrite it" route is not
# available.
#
# What is deliberately NOT done: Asio offers BOOST_ASIO_DISABLE_FENCED_BLOCK,
# which selects null_fenced_block -- i.e. it removes the synchronisation
# entirely rather than making it visible to TSan. Trading a real memory barrier
# for a build fix is not a trade this codebase makes.
#
# -Wtsan is diagnostic-only: it changes no code generation. Suppressing it
# leaves the binary and TSan's behaviour identical -- what it costs is the
# WARNING, not the checking. The residual caveat it was warning about does
# remain, and matters when reading results: TSan cannot see the ordering Asio
# establishes through those fences, so for these targets it may report false
# positives, and a clean run is weaker evidence than a clean run elsewhere in
# this tree. (Empirically, as of the audit that added this,
# test_binance_ws_session runs 12/12 with zero reports.)
#
# Scoped per-target, never globally, and that scoping is the point: OUR code
# must never reintroduce atomic_thread_fence (shm_heartbeat.hpp records why,
# and warns that "just put the fence back" is the natural-looking cleanup that
# would break the TSan build again). Leaving -Wtsan live on every other target
# keeps the diagnostic doing that guarding job; only the handful of TUs that
# inline Asio's fence opt out.
function(hengyuan_allow_third_party_tsan_fences target)
    if(HY_SANITIZER STREQUAL "thread")
        # GNU-only: -Wtsan is a GCC diagnostic; Clang has no such warning.
        # Applied after hengyuan_set_warnings(), so it follows -Werror on the
        # command line and wins for this one diagnostic.
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-Wno-tsan>)
    endif()
endfunction()
