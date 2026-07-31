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

set -euo pipefail

REPO="${HOME}/repos/hengyuan_v2"
cd "${REPO}/native"

MODE="${1:-}"
if [[ -z "${MODE}" ]]; then
    echo "usage: $0 none|address|thread|all" >&2
    exit 2
fi

run_none() {
    echo "=== none: mirrors ci-native.yml ==="
    cmake -B build-linux-none \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DCMAKE_BUILD_TYPE=Release \
        -DHY_BUILD_DEMO=ON
    cmake --build build-linux-none -j"$(nproc)"
    (cd build-linux-none && ctest --output-on-failure)
}

run_address() {
    echo "=== address: mirrors ci-native-sanitizers.yml asan-ubsan-full job ==="
    cmake -B build-linux-asan \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DHY_SANITIZER=address \
        -DHY_BUILD_DEMO=ON
    cmake --build build-linux-asan -j"$(nproc)"
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
        -DHY_BUILD_DEMO=OFF
    cmake --build build-linux-tsan -j"$(nproc)" --target test_spsc_concurrency tsan_control_relaxed_ring

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
}

case "${MODE}" in
    none)    run_none ;;
    address) run_address ;;
    thread)  run_thread ;;
    all)     run_none; run_address; run_thread ;;
    *) echo "unknown mode: ${MODE} (expected none|address|thread|all)" >&2; exit 2 ;;
esac
