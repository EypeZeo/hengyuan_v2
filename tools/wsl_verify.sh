#!/usr/bin/env bash
# Local mirror of what .github/workflows/ci-native.yml and
# ci-native-sanitizers.yml actually run, executed against the WSL2-native copy
# produced by tools/wsl_sync.sh. Each mode below reproduces one CI job's steps
# verbatim rather than inventing a parallel verification path -- if this script
# and the CI YAML ever disagree, the YAML is authoritative and this should be
# fixed to match it, not the other way around.
#
# Usage (from inside WSL2):
#   bash tools/wsl_verify.sh none      # mirrors ci-native.yml
#   bash tools/wsl_verify.sh address   # mirrors ci-native-sanitizers.yml's asan-ubsan-full job
#   bash tools/wsl_verify.sh thread    # mirrors ci-native-sanitizers.yml's tsan-concurrency job
#   bash tools/wsl_verify.sh all       # all three in sequence
#
# Parallel-worktree usage: REPO below is hardcoded to the single shared
# ~/repos/hengyuan_v2 default for backward compatibility -- it does NOT infer
# from your current directory, so running this from inside a different
# worktree's WSL copy (e.g. ~/repos/hengyuan_v2_slice2a) silently verifies the
# WRONG tree unless overridden. Set HY_VERIFY_REPO to match whatever DST you
# gave tools/wsl_sync.sh for that worktree:
#   HY_VERIFY_REPO=~/repos/hengyuan_v2_slice2a bash tools/wsl_verify.sh none
#
# Round D's real libFuzzer harness (fuzz_compaction_intent_codec, Clang-only,
# -DHY_BUILD_FUZZ=ON) is NOT covered by any mode above -- this toolchain is
# GCC-14, and the fuzz target's CMake registration itself hard-errors if the
# active compiler isn't Clang (see native/CMakeLists.txt). The
# GCC/MSVC-buildable corpus runner (test_compaction_intent_codec_corpus_
# runner) that exercises the same checked-in seeds through the same target
# function IS part of the default `ctest` run in every mode above -- that is
# this script's actual coverage of that codec's fuzz corpus. A real Clang
# fuzzing run is `cmake -DCMAKE_CXX_COMPILER=clang++ -DHY_BUILD_FUZZ=ON ...`
# followed by `ctest -L fuzz` (selects only the bounded smoke test) or running
# the fuzz_compaction_intent_codec binary directly for an extended campaign --
# neither belongs in this script's default/no-flag `ctest` invocations, to
# keep this script's own tail latency unaffected by a workload it can't even
# build with the toolchain it targets.

set -euo pipefail

REPO="${HY_VERIFY_REPO:-${HOME}/repos/hengyuan_v2}"
cd "${REPO}/native"

MODE="${1:-}"
if [[ -z "${MODE}" ]]; then
    echo "usage: $0 none|address|thread|all" >&2
    exit 2
fi

# CI keeps the default (all available cores).  A constrained WSL VM can pass a
# smaller positive value, e.g. HY_BUILD_JOBS=4, without changing the validated
# CMake/test matrix or weakening any sanitizer/control assertion. If `free -h`
# shows swap growing under the default, HY_BUILD_JOBS=4 (or lower) is usually
# faster than the full core count -- a thrashing build is not a fast one.
BUILD_JOBS="${HY_BUILD_JOBS:-$(nproc)}"
if ! [[ "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "HY_BUILD_JOBS must be a positive integer (got: ${BUILD_JOBS})" >&2
    exit 2
fi

# Local iteration-speed levers, all opt-in-by-detection: none of these change
# the compiler, flags, or test matrix the CI YAML validates (the script's own
# header comment's "YAML is authoritative" contract still holds) -- they only
# change how fast repeat local runs get there. Each is a no-op if the tool
# isn't installed, so a fresh WSL2 setup (only g++-14/cmake/boost/openssl per
# this repo's CLAUDE.md one-time setup) still works unmodified.
EXTRA_CMAKE_ARGS=()
if command -v ccache >/dev/null 2>&1; then
    # AI-driven edit/build/review loops recompile the same unchanged headers
    # over and over; ccache turns most of those into a cache hit instead of a
    # real compile. `sudo apt install -y ccache` if missing.
    EXTRA_CMAKE_ARGS+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
if command -v ninja >/dev/null 2>&1; then
    # Ninja's incremental dependency scheduling is a better fit than Unix
    # Makefiles for this many translation units. `sudo apt install -y
    # ninja-build` if missing; falls back to the CMake default generator.
    EXTRA_CMAKE_ARGS+=(-G Ninja)
fi
if command -v mold >/dev/null 2>&1; then
    # Only touches link time, not compiled output -- safe to always prefer
    # over ld.bfd when present. `sudo apt install -y mold` if missing.
    EXTRA_CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=mold)
fi
if [[ ${#EXTRA_CMAKE_ARGS[@]} -gt 0 ]]; then
    echo "iteration-speed: ${EXTRA_CMAKE_ARGS[*]}"
fi

run_none() {
    echo "=== none: mirrors ci-native.yml ==="
    cmake -B build-linux-none \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DCMAKE_BUILD_TYPE=Release \
        -DHY_BUILD_DEMO=ON \
        "${EXTRA_CMAKE_ARGS[@]}"
    cmake --build build-linux-none -j"${BUILD_JOBS}"
    (cd build-linux-none && ctest --output-on-failure)
}

run_address() {
    echo "=== address: mirrors ci-native-sanitizers.yml asan-ubsan-full job ==="
    cmake -B build-linux-asan \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DHY_SANITIZER=address \
        -DHY_BUILD_DEMO=ON \
        "${EXTRA_CMAKE_ARGS[@]}"
    cmake --build build-linux-asan -j"${BUILD_JOBS}"
    (
        cd build-linux-asan
        export ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:abort_on_error=1"
        export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
        ctest --output-on-failure
    )
}

run_thread() {
    echo "=== thread: mirrors ci-native-sanitizers.yml tsan-concurrency job ==="
    cmake -B build-linux-tsan \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DHY_SANITIZER=thread \
        -DHY_BUILD_TSAN_CONTROL=ON \
        -DHY_BUILD_DEMO=OFF \
        "${EXTRA_CMAKE_ARGS[@]}"
    cmake --build build-linux-tsan -j"${BUILD_JOBS}" --target \
        test_spsc_concurrency test_reconcile_concurrency test_shm_heartbeat \
        test_snapshot_refresh_gate test_public_feed_supervisor_concurrency test_single_flight_fetch_gate \
        test_kline_feed_driver tsan_control_relaxed_ring \
        tsan_control_export_worker_dual_consumer \
        compaction_lease_holder test_compaction_lease test_compaction_intent_store \
        test_migrated_v2_started_publisher

    echo "--- concurrency tests (must pass) ---"
    (
        cd build-linux-tsan
        export TSAN_OPTIONS="history_size=7:halt_on_error=1:exitcode=66:second_deadlock_stack=1"
        ctest -L concurrency --output-on-failure
    )

    echo "--- control: tsan_control_relaxed_ring (must FAIL with a data race report) ---"
    (
        cd build-linux-tsan
        export TSAN_OPTIONS="history_size=7:halt_on_error=1:exitcode=66"
        set +e
        # setarch -R: WSL2's default ASLR layout can trip TSan's shadow-memory
        # setup ("FATAL: ThreadSanitizer: unexpected memory mapping") before any
        # user code runs. Same workaround CMakeLists.txt applies to
        # test_spsc_concurrency via CROSSCOMPILING_EMULATOR; this binary isn't
        # ctest-registered (a deliberate data race must never look like a suite
        # failure), so it's wrapped here instead. Harmless no-op on runners that
        # don't need it.
        setarch "$(uname -m)" -R ./tsan_control_relaxed_ring > /tmp/tsan_control.log 2>&1
        rc=$?
        set -e
        if [[ $rc -eq 0 ]]; then
            echo "ERROR: tsan_control_relaxed_ring exited 0. TSan did NOT detect the injected race on this machine. Every 'no race found' result above is unsubstantiated."
            tail -50 /tmp/tsan_control.log
            exit 1
        fi
        if ! grep -q "WARNING: ThreadSanitizer: data race" /tmp/tsan_control.log; then
            echo "ERROR: tsan_control_relaxed_ring failed, but NOT with a ThreadSanitizer data-race report. It may be erroring for an unrelated reason."
            tail -50 /tmp/tsan_control.log
            exit 1
        fi
        echo "OK: TSan reported the injected race as expected."
        grep -A5 "WARNING: ThreadSanitizer: data race" /tmp/tsan_control.log | head -20
    )

    echo "--- control: tsan_control_export_worker_dual_consumer (must FAIL with a data race report) ---"
    (
        cd build-linux-tsan
        export TSAN_OPTIONS="history_size=7:halt_on_error=1:exitcode=66"
        set +e
        setarch "$(uname -m)" -R ./tsan_control_export_worker_dual_consumer > /tmp/tsan_control_export_worker.log 2>&1
        rc=$?
        set -e
        if [[ $rc -eq 0 ]]; then
            echo "ERROR: tsan_control_export_worker_dual_consumer exited 0. TSan did NOT detect the injected race on this machine. Every 'no race found' result above is unsubstantiated."
            tail -50 /tmp/tsan_control_export_worker.log
            exit 1
        fi
        if ! grep -q "WARNING: ThreadSanitizer: data race" /tmp/tsan_control_export_worker.log; then
            echo "ERROR: tsan_control_export_worker_dual_consumer failed, but NOT with a ThreadSanitizer data-race report. It may be erroring for an unrelated reason."
            tail -50 /tmp/tsan_control_export_worker.log
            exit 1
        fi
        echo "OK: TSan reported the injected race as expected."
        grep -A5 "WARNING: ThreadSanitizer: data race" /tmp/tsan_control_export_worker.log | head -20
    )
}

case "${MODE}" in
    none)    run_none ;;
    address) run_address ;;
    thread)  run_thread ;;
    all)     run_none; run_address; run_thread ;;
    *) echo "unknown mode: ${MODE} (expected none|address|thread|all)" >&2; exit 2 ;;
esac
