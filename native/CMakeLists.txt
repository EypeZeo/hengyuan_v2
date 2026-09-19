cmake_minimum_required(VERSION 3.22)

# CMake 3.30+ defaults CMP0167 to NEW, which removes FindBoost.  Boost 1.91
# is supplied as a source tree on Windows, so retain the compatible module
# lookup across the complete configure, including vcpkg's find-package wrapper.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

project(hengyuan_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

# MUST come before FetchContent: sanitizer flags are applied directory-wide so
# that GoogleTest and simdjson are instrumented too. An uninstrumented GoogleTest
# under TSan both invents false positives and loses real happens-before edges.
hengyuan_enable_sanitizers()

# TSan's shadow-memory setup can fail with "FATAL: ThreadSanitizer: unexpected
# memory mapping" under WSL2, because WSL2's default ASLR layout doesn't match
# what TSan expects for its fixed shadow region. CROSSCOMPILING_EMULATOR is
# CMake's existing mechanism for prefixing every invocation of a test binary
# with a wrapper command (designed for running cross-compiled binaries under an
# emulator); `setarch -R` disables ASLR for just this process and needs no
# root. Setting CMAKE_CROSSCOMPILING_EMULATOR here (a variable, not a
# per-target property) makes CMake initialize the CROSSCOMPILING_EMULATOR
# property on EVERY target created after this point -- previously this was an
# opt-in set_target_properties() call repeated per threaded test target, and
# three such targets (test_binance_environment, test_binance_ws_session,
# test_seal_id_watermark_export_started_loader) were missing it, silently
# skipping TSan coverage with no visible failure (AUDIT BUILD-TSAN-GUARD-001).
# Applied unconditionally to all targets, not just threaded ones: harmless for
# single-threaded binaries, and removes the maintenance burden of tracking
# which targets need it. gtest_discover_tests() also runs each binary once at
# build time to enumerate test names, and that invocation hits the same fatal
# error if left unwrapped -- CMAKE_CROSSCOMPILING_EMULATOR is honored by both.
if(HY_SANITIZER STREQUAL "thread" AND UNIX AND NOT APPLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "setarch;${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
endif()

# Needed unconditionally by the concurrency tests below. Previously this lived
# inside if(HY_BUILD_DEMO), which meant a tests-only configure had no thread
# support at all.
find_package(Threads REQUIRED)

include(FetchContent)

# --- simdjson (IO-01: Binance JSON parser) ---
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.12.3
)
FetchContent_MakeAvailable(simdjson)

# Header-only core (MarketEvent + SpscRing + HotThread + EventRecorder)
add_library(hengyuan_core INTERFACE)
target_include_directories(hengyuan_core INTERFACE include)
target_compile_features(hengyuan_core INTERFACE cxx_std_20)
# std::fopen is standard C; silence MSVC's non-standard "use fopen_s" deprecation
target_compile_definitions(hengyuan_core INTERFACE $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
# BCryptGenRandom (export_worker.hpp/durable_audit_sink.hpp store-identity
# generation, Phase 5) lives in bcrypt.lib, not linked by default the way
# kernel32's CreateFileA/friends already used elsewhere in this codebase are.
target_link_libraries(hengyuan_core INTERFACE $<$<PLATFORM_ID:Windows>:bcrypt>)

# Static library: parser + orderbook + validator (IO-01 + OB-01)
add_library(hengyuan_dataplane STATIC
    src/binance_json_parser.cpp
    src/orderbook.cpp
    src/input_validator.cpp
)
target_link_libraries(hengyuan_dataplane PUBLIC hengyuan_core simdjson)
get_target_property(_simdjson_inc simdjson INTERFACE_INCLUDE_DIRECTORIES)
if(_simdjson_inc)
    target_include_directories(hengyuan_dataplane SYSTEM PUBLIC ${_simdjson_inc})
endif()
hengyuan_set_warnings(hengyuan_dataplane)

# --- Batch 5, 5a: StrategySpec TOML parser (docs/STRATEGY_SPEC.md). Deliberately its own
# static library, not folded into hengyuan_dataplane (parser/orderbook/validator, an unrelated
# concern) or hengyuan_core (header-only) -- no Boost/OpenSSL dependency, so it stays outside
# if(HY_BUILD_DEMO) and builds unconditionally, same posture as hengyuan_dataplane itself.
add_library(hengyuan_strategy_spec STATIC
    src/strategy_spec_toml_parser.cpp
)
target_link_libraries(hengyuan_strategy_spec PUBLIC hengyuan_core)
hengyuan_set_warnings(hengyuan_strategy_spec)

# --- D12-9 dry-run evidence harness (header-only, no Boost/OpenSSL needed) ---
add_executable(live_submit_evidence_harness src/live_submit_evidence_harness.cpp)
target_link_libraries(live_submit_evidence_harness PRIVATE hengyuan_core)
hengyuan_set_warnings(live_submit_evidence_harness)

# --- Read-only cross-file MUST-equal audit tool (diagnostic; deliberately NOT
# a production code path -- never acquires a lease, never writes breadcrumbs).
# Scans a constructed fixture breadcrumb directory and reports which
# MUST-equal cross-file rules are violated; feeds the IntentPhaseAdvancer
# step-2 field mapping table. Self-check mode runs the full per-rule injection
# matrix (registered as a ctest entry inside the HY_BUILD_TESTS block below).
add_executable(seal_journal_cross_file_audit src/seal_journal_cross_file_audit.cpp)
target_link_libraries(seal_journal_cross_file_audit PRIVATE hengyuan_core)
hengyuan_set_warnings(seal_journal_cross_file_audit)

# --- Per-store .jhw/.jts overview tool (diagnostic; deliberately NOT a
# production code path -- no lease acquisition, plain directory_iterator +
# ifstream). Lists every candidate's commit watermark + tombstones in one
# seal-journal/<store_uuid>/ directory. No --self-check mode, so no ctest
# registration.
add_executable(seal_journal_store_dump src/seal_journal_store_dump.cpp)
target_link_libraries(seal_journal_store_dump PRIVATE hengyuan_core)
hengyuan_set_warnings(seal_journal_store_dump)

# --- Binance feed demo (optional, requires Boost + OpenSSL) ---
option(HY_BUILD_DEMO "Build binance_feed_demo (requires Boost + OpenSSL)" OFF)

if(HY_BUILD_DEMO)
    # Boost.System is header-only as of Boost 1.69.  CMake's legacy
    # FindBoost module still treats `COMPONENTS system` as a request for a
    # prebuilt boost_system library, which makes a valid 1.91 source/install
    # tree appear incomplete.  Keep a project-local target named Boost::system
    # so all consumers retain one stable link dependency without requiring a
    # dummy archive or a version-specific code path.
    find_package(Boost REQUIRED)
    if(NOT TARGET Boost::system)
        add_library(hengyuan_boost_system INTERFACE)
        target_link_libraries(hengyuan_boost_system INTERFACE Boost::headers)
        add_library(Boost::system ALIAS hengyuan_boost_system)
    endif()
    find_package(OpenSSL REQUIRED)
    # vcpkg's static (x64-windows-static-md) OpenSSL::Crypto does not itself
    # carry Crypt32.lib/Ws2_32.lib as transitive link dependencies on
    # Windows, even though its Windows cert-store backend (e_capi.c /
    # winstore_store.c) and socket BIO backend (bss_sock.c / bio_sock*.c)
    # need them -- every target here that links OpenSSL::Crypto needs
    # Crypt32, or MSVC fails with ~40 unresolved __imp_Cert*/WSA* symbols
    # at link time. A target that does NOT also link Boost::system (which
    # itself pulls in Ws2_32 transitively) additionally needs Ws2_32
    # explicitly, or linking fails on __imp_recv/__imp_send/etc. instead.
    set(HY_OPENSSL_WIN32_LIBS "")
    set(HY_OPENSSL_WIN32_LIBS_NO_BOOST "")
    if(WIN32)
        set(HY_OPENSSL_WIN32_LIBS crypt32)
        set(HY_OPENSSL_WIN32_LIBS_NO_BOOST crypt32 ws2_32)
    endif()
    # Threads is found unconditionally at the top of this file (the concurrency
    # tests need it whether or not demos are built).

    add_executable(binance_feed_demo src/binance_feed_demo.cpp)
    target_link_libraries(binance_feed_demo PRIVATE
        hengyuan_dataplane
        Boost::system
        OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
        Threads::Threads
    )
    target_include_directories(binance_feed_demo SYSTEM PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    hengyuan_set_warnings(binance_feed_demo)
    hengyuan_allow_third_party_tsan_fences(binance_feed_demo)

    # TODO 1A.4 prerequisite: first process in native/src/ that actually runs
    # poll_once()/drain_reconcile_events() in a continuous loop (previously test-only),
    # alongside a real BinanceUserDataWsSession on a second thread. See the file's own
    # header comment for exactly what's real vs. simulated here.
    add_executable(live_submit_reconcile_harness_demo src/live_submit_reconcile_harness_demo.cpp)
    target_link_libraries(live_submit_reconcile_harness_demo PRIVATE
        hengyuan_dataplane
        Boost::system
        OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
        Threads::Threads
    )
    target_include_directories(live_submit_reconcile_harness_demo SYSTEM PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    hengyuan_set_warnings(live_submit_reconcile_harness_demo)
    hengyuan_allow_third_party_tsan_fences(live_submit_reconcile_harness_demo)

    # Batch H, H6: cold-start harness wiring H1-H5 against a real (owner-supplied,
    # testnet-only) credential -- same Boost/Beast/OpenSSL dependency shape as the mock-
    # credential harness above.
    add_executable(live_submit_real_credentials_demo src/live_submit_real_credentials_demo.cpp)
    target_link_libraries(live_submit_real_credentials_demo PRIVATE
        hengyuan_dataplane
        Boost::system
        OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
        Threads::Threads
    )
    target_include_directories(live_submit_real_credentials_demo SYSTEM PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    hengyuan_set_warnings(live_submit_real_credentials_demo)
    hengyuan_allow_third_party_tsan_fences(live_submit_real_credentials_demo)

    # 批次 6 6a-2: public kline feed -> StreamingEvaluator -> HoldingStateTracker, log only --
    # zero order-related code anywhere in this target. Needs hengyuan_strategy_spec (loads specs
    # via load_strategy_spec()) on top of the usual Boost/Beast/OpenSSL dependency shape.
    add_executable(live_bar_feed_signal_demo src/live_bar_feed_signal_demo.cpp)
    target_link_libraries(live_bar_feed_signal_demo PRIVATE
        hengyuan_dataplane
        hengyuan_strategy_spec
        Boost::system
        OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
        Threads::Threads
    )
    target_include_directories(live_bar_feed_signal_demo SYSTEM PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    hengyuan_set_warnings(live_bar_feed_signal_demo)
    hengyuan_allow_third_party_tsan_fences(live_bar_feed_signal_demo)

    # 批次 6 6b-1: H6's cold-start chain + 6a-1 kline stream + new depth stream + real
    # KillSwitch + dry-run evidence replay -- ctx.submit_port stays mock throughout (see the
    # file's own header comment), so this produces zero real network writes and needs no L5
    # review. Same dependency shape as live_submit_real_credentials_demo + live_bar_feed_signal_demo.
    add_executable(live_submit_preflight_harness src/live_submit_preflight_harness.cpp)
    target_link_libraries(live_submit_preflight_harness PRIVATE
        hengyuan_dataplane
        hengyuan_strategy_spec
        Boost::system
        OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
        Threads::Threads
    )
    target_include_directories(live_submit_preflight_harness SYSTEM PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    hengyuan_set_warnings(live_submit_preflight_harness)
    hengyuan_allow_third_party_tsan_fences(live_submit_preflight_harness)

    add_executable(binance_dry_run_demo src/binance_dry_run_demo.cpp)
    target_link_libraries(binance_dry_run_demo PRIVATE
        hengyuan_dataplane
        Boost::system
        OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
        Threads::Threads
    )
    target_include_directories(binance_dry_run_demo SYSTEM PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    hengyuan_set_warnings(binance_dry_run_demo)
    hengyuan_allow_third_party_tsan_fences(binance_dry_run_demo)

    # P2-MD-02 / Track C: manual "VPS manual verification = L4" connectivity smoke test --
    # see the file's own header comment. Not registered with ctest; run by hand.
    add_executable(binance_connectivity_smoke src/binance_connectivity_smoke.cpp)
    target_link_libraries(binance_connectivity_smoke PRIVATE
        hengyuan_dataplane
        Boost::system
        OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
        Threads::Threads
    )
    target_include_directories(binance_connectivity_smoke SYSTEM PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    hengyuan_set_warnings(binance_connectivity_smoke)
    hengyuan_allow_third_party_tsan_fences(binance_connectivity_smoke)

    add_executable(latency_bench src/latency_bench.cpp)
    target_link_libraries(latency_bench PRIVATE hengyuan_dataplane)
    hengyuan_set_warnings(latency_bench)

    # Watchdog daemon (Linux only — shm_open, prctl)
    if(UNIX AND NOT APPLE)
        add_executable(watchdog_daemon src/watchdog_daemon.cpp)
        target_link_libraries(watchdog_daemon PRIVATE hengyuan_core rt)
        hengyuan_set_warnings(watchdog_daemon)
    endif()

endif()

# --- Tests (GTest via FetchContent) ---
option(HY_BUILD_TESTS "Build native unit tests" ON)

if(HY_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)

    # gtest_discover_tests RUNS each test binary at build time to enumerate its
    # tests. The default 5s timeout is routinely exceeded under a sanitizer
    # runtime, and the failure surfaces as a confusing BUILD error rather than a
    # test failure. 120s is generous headroom for instrumented startup.
    set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_TIMEOUT 120)

    # Read-only cross-file MUST-equal audit tool -- self-check exercises the
    # full per-rule injection matrix (clean fixtures + one tampered fixture per
    # rule, asserting the violated-rule set matches expectation exactly).
    add_test(NAME seal_journal_cross_file_audit_selfcheck
             COMMAND seal_journal_cross_file_audit --self-check)

    add_executable(test_market_event_layout tests/test_market_event_layout.cpp)
    target_link_libraries(test_market_event_layout PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_market_event_layout)
    gtest_discover_tests(test_market_event_layout)

    add_executable(test_spsc_ring tests/test_spsc_ring.cpp)
    target_link_libraries(test_spsc_ring PRIVATE hengyuan_core GTest::gtest_main)
    target_include_directories(test_spsc_ring PRIVATE tests)
    hengyuan_set_warnings(test_spsc_ring)
    gtest_discover_tests(test_spsc_ring)

    add_executable(test_binance_json_parser tests/test_binance_json_parser.cpp)
    target_link_libraries(test_binance_json_parser PRIVATE hengyuan_dataplane GTest::gtest_main)
    hengyuan_set_warnings(test_binance_json_parser)
    gtest_discover_tests(test_binance_json_parser)

    add_executable(test_orderbook tests/test_orderbook.cpp)
    target_link_libraries(test_orderbook PRIVATE hengyuan_dataplane GTest::gtest_main)
    hengyuan_set_warnings(test_orderbook)
    gtest_discover_tests(test_orderbook)

    add_executable(test_input_validator tests/test_input_validator.cpp)
    target_link_libraries(test_input_validator PRIVATE hengyuan_dataplane GTest::gtest_main)
    hengyuan_set_warnings(test_input_validator)
    gtest_discover_tests(test_input_validator)

    add_executable(test_hot_thread tests/test_hot_thread.cpp)
    target_link_libraries(test_hot_thread PRIVATE hengyuan_dataplane GTest::gtest_main)
    hengyuan_set_warnings(test_hot_thread)
    gtest_discover_tests(test_hot_thread)

    add_executable(test_event_recorder tests/test_event_recorder.cpp)
    target_link_libraries(test_event_recorder PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_event_recorder)
    gtest_discover_tests(test_event_recorder)

    # P2-EXEC-SIM-01: dry-run simulation (header-only)
    add_executable(test_fixed_point tests/test_fixed_point.cpp)
    target_link_libraries(test_fixed_point PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_fixed_point)
    gtest_discover_tests(test_fixed_point)

    add_executable(test_kill_switch tests/test_kill_switch.cpp)
    target_link_libraries(test_kill_switch PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_kill_switch)
    gtest_discover_tests(test_kill_switch)

    add_executable(test_risk_gate tests/test_risk_gate.cpp)
    target_link_libraries(test_risk_gate PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_risk_gate)
    gtest_discover_tests(test_risk_gate)

    add_executable(test_sim_executor tests/test_sim_executor.cpp)
    target_link_libraries(test_sim_executor PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_sim_executor)
    gtest_discover_tests(test_sim_executor)

    # D3-LIVE preflight gate
    add_executable(test_preflight_gate tests/test_preflight_gate.cpp)
    target_link_libraries(test_preflight_gate PRIVATE hengyuan_dataplane GTest::gtest_main)
    hengyuan_set_warnings(test_preflight_gate)
    gtest_discover_tests(test_preflight_gate)

    # P2-INFRA-HB-01: SHM heartbeat (portable tests, no shm_open needed).
    # Threads::Threads + the concurrency label + the setarch wrapper (audit
    # PERF-SHM-004): ShmControlBlock's seqlock is a CROSS-PROCESS protocol, so the
    # only way to put it under TSan at all is a two-thread in-process test of the
    # same protocol. Without the label the TSan job -- which builds a hand-picked
    # target list, not the whole suite -- would never run it, which is how the
    # original "documented as relaxed atomics, implemented with none" state
    # survived.
    add_executable(test_shm_heartbeat tests/test_shm_heartbeat.cpp)
    target_link_libraries(test_shm_heartbeat PRIVATE hengyuan_core GTest::gtest_main Threads::Threads)
    hengyuan_set_warnings(test_shm_heartbeat)

    gtest_discover_tests(test_shm_heartbeat PROPERTIES LABELS concurrency)

    # P2-EXEC-SIM-04: intent channel
    add_executable(test_intent_channel tests/test_intent_channel.cpp)
    target_link_libraries(test_intent_channel PRIVATE hengyuan_dataplane GTest::gtest_main)
    hengyuan_set_warnings(test_intent_channel)
    gtest_discover_tests(test_intent_channel)

    # P2-CORE-OB-02: depth snapshot bootstrap
    add_executable(test_depth_manager tests/test_depth_manager.cpp)
    target_link_libraries(test_depth_manager PRIVATE hengyuan_dataplane GTest::gtest_main)
    hengyuan_set_warnings(test_depth_manager)
    gtest_discover_tests(test_depth_manager)

    # P2-EXEC-SIM-02: dry-run pipeline integration (HotThread + SimExecutor)
    add_executable(test_dry_run_pipeline tests/test_dry_run_pipeline.cpp)
    target_link_libraries(test_dry_run_pipeline PRIVATE hengyuan_dataplane GTest::gtest_main)
    hengyuan_set_warnings(test_dry_run_pipeline)
    gtest_discover_tests(test_dry_run_pipeline)

    # P2-EXEC-LIVE-01 D12-1: env parser (L1, header-only, no OpenSSL)
    add_executable(test_env_parser tests/test_env_parser.cpp)
    target_link_libraries(test_env_parser PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_env_parser)
    gtest_discover_tests(test_env_parser)

    # 轨道 A 第四项: vendored SHA-256/HMAC-SHA256 (L1, no OpenSSL -- see
    # sha256.hpp's own header comment for why). Foundation for the durable
    # audit log's frame checksum; independently testable/verifiable first.
    add_executable(test_sha256_hmac tests/test_sha256_hmac.cpp)
    target_link_libraries(test_sha256_hmac PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_sha256_hmac)
    gtest_discover_tests(test_sha256_hmac)

    # Phase 0 (密钥轮换共享基础设施): KEK 文件加载器 (L3, 镜像 env_loader.hpp
    # 的权限检查/反符号链接/mlock/安全擦除模式，见 kek_loader.hpp)
    add_executable(test_kek_loader tests/test_kek_loader.cpp)
    target_link_libraries(test_kek_loader PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_kek_loader)
    gtest_discover_tests(test_kek_loader)

    # Phase 0: HY-KEKWRAP-v1 key wrap/unwrap (L1, 纯内存, 无文件 I/O -- 见
    # key_ring.hpp 头部注释，这不是标准 AEAD，正确性靠往返+篡改测试)
    add_executable(test_key_ring tests/test_key_ring.cpp)
    target_link_libraries(test_key_ring PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_key_ring)
    gtest_discover_tests(test_key_ring)

    # Phase 1: DurableLogStore 共享平台 I/O 层 (L2, 真实文件 I/O -- 跨平台锁/
    # fsync/tip-anchor 原子替换/流式 read_chunk。DurableAuditSink 与
    # ControlPlaneLogSink 共用同一份实现，见 durable_log_store.hpp 头部注释)
    add_executable(test_durable_log_store tests/test_durable_log_store.cpp)
    target_link_libraries(test_durable_log_store PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_log_store)
    gtest_discover_tests(test_durable_log_store)

    # 轨道 A 第四项: durable 日志帧编解码 (L1, 纯内存, 无文件 I/O -- 真正的文件
    # 落盘在 durable_audit_sink.hpp，这里只验证帧格式本身)
    add_executable(test_durable_frame_codec tests/test_durable_frame_codec.cpp)
    target_link_libraries(test_durable_frame_codec PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_frame_codec)
    gtest_discover_tests(test_durable_frame_codec)

    # Round D (docs/SPEC_INVARIANTS.md "Seal-journal Round D"): CompactionIntent
    # codec (L1, pure in-memory, no file I/O -- real file I/O is
    # compaction_lease.hpp, see test_compaction_lease.cpp).
    add_executable(test_compaction_intent_codec tests/test_compaction_intent_codec.cpp)
    target_link_libraries(test_compaction_intent_codec PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_compaction_intent_codec)
    gtest_discover_tests(test_compaction_intent_codec)

    # Round D: deterministic runner over the checked-in fuzz seed corpus
    # (fuzz/corpus/compaction_intent/) -- GCC/MSVC-buildable, no libFuzzer/
    # Clang dependency, part of the default ctest run. The real extended
    # fuzzing loop (fuzz_compaction_intent_codec, Clang-only, HY_BUILD_FUZZ)
    # is registered separately below under LABELS "fuzz". Both drivers call
    # the exact same target function (fuzz/compaction_intent_codec_fuzz_
    # target.hpp) so they cannot drift into testing different things.
    # HY_COMPACTION_INTENT_CORPUS_DIR: same compile-definition pattern as
    # HY_TEST_FIXTURE_DIR elsewhere in this file.
    set(HY_COMPACTION_INTENT_CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/compaction_intent")
    add_executable(test_compaction_intent_codec_corpus_runner
        tests/test_compaction_intent_codec_corpus_runner.cpp)
    target_link_libraries(test_compaction_intent_codec_corpus_runner PRIVATE hengyuan_core GTest::gtest_main)
    target_compile_definitions(test_compaction_intent_codec_corpus_runner PRIVATE
        HY_COMPACTION_INTENT_CORPUS_DIR="${HY_COMPACTION_INTENT_CORPUS_DIR}")
    hengyuan_set_warnings(test_compaction_intent_codec_corpus_runner)
    gtest_discover_tests(test_compaction_intent_codec_corpus_runner)

    # Round D: real libFuzzer harness -- Clang-only (needs -fsanitize=fuzzer,
    # which GCC/MSVC don't implement), off by default, same
    # "option-gated, not ctest-label-gated" shape as HY_BUILD_TSAN_CONTROL
    # above: a normal MSVC/GCC-14 build never configures this target at all,
    # so plain `ctest` on those toolchains is structurally unaffected by it
    # regardless of any label filter. Unlike tsan_control_relaxed_ring, this
    # IS ctest-registered (under LABELS "fuzz") -- `ctest -L fuzz` is the
    # explicit, separate invocation a Clang-specific CI job uses to run it;
    # a plain `ctest` inside that same job would still pick it up too, which
    # is intended there (see native/README or ci-native.yml for the actual
    # job wiring once one exists).
    option(HY_BUILD_FUZZ "Build the libFuzzer harness for compaction_intent_codec (Clang-only)" OFF)
    if(HY_BUILD_FUZZ)
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR "HY_BUILD_FUZZ requires Clang (libFuzzer/-fsanitize=fuzzer); "
                                 "compiler is ${CMAKE_CXX_COMPILER_ID}")
        endif()
        add_executable(fuzz_compaction_intent_codec fuzz/fuzz_compaction_intent_codec.cpp)
        target_link_libraries(fuzz_compaction_intent_codec PRIVATE hengyuan_core)
        target_compile_options(fuzz_compaction_intent_codec PRIVATE -fsanitize=fuzzer)
        target_link_options(fuzz_compaction_intent_codec PRIVATE -fsanitize=fuzzer)
        hengyuan_set_warnings(fuzz_compaction_intent_codec)

        # Explicit resource ceiling -- the structural fix this round's review
        # required (see docs/SPEC_INVARIANTS.md's Round D entry): an unbounded
        # fuzz invocation must never be the only option. Values are
        # deliberately modest for a "smoke" run; a real extended fuzzing
        # campaign uses this binary directly with its own -runs/-max_total_time.
        add_test(NAME fuzz_compaction_intent_codec_smoke
                 COMMAND fuzz_compaction_intent_codec
                         "${HY_COMPACTION_INTENT_CORPUS_DIR}"
                         -runs=100000 -timeout=60 -rss_limit_mb=2048 -seed=1)
        set_tests_properties(fuzz_compaction_intent_codec_smoke PROPERTIES LABELS "fuzz")
    endif()

    # Round D: compaction_breadcrumb_io.hpp's platform-independent surface
    # (ValidatedArtifactName / is_traversal_safe_name) -- the POSIX openat/
    # linkat/renameat2 half and the Windows NT-native half
    # (windows_native_io.hpp) are exercised for real by test_compaction_lease
    # (needs an actual directory), not here.
    add_executable(test_compaction_breadcrumb_io_detail tests/test_compaction_breadcrumb_io_detail.cpp)
    target_link_libraries(test_compaction_breadcrumb_io_detail PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_compaction_breadcrumb_io_detail)
    gtest_discover_tests(test_compaction_breadcrumb_io_detail)

    # Round D: standalone subprocess for test_compaction_lease's
    # CrossProcessMutualExclusion test -- NOT a GTest binary, argv-driven,
    # spawned as a child process. Deliberately not linked against GTest.
    add_executable(compaction_lease_holder tests/helpers/compaction_lease_holder.cpp)
    target_link_libraries(compaction_lease_holder PRIVATE hengyuan_core)
    hengyuan_set_warnings(compaction_lease_holder)

    # Round D: CandidateLease's own lifecycle surface (acquire/release/held/
    # fenced/move) against a real temp directory -- L2, real file I/O.
    # Genesis write/read coverage (via IntentStore, the only friend) lives in
    # test_compaction_intent_store.cpp. Threading test (NonOwnerRelease...)
    # needs Threads -- same pattern as other concurrency-relevant targets.
    # HY_COMPACTION_LEASE_HOLDER_EXE is the built path of the helper above,
    # for the CrossProcessMutualExclusion test's real subprocess spawn --
    # same "compile-definition, not an assumed relative path" pattern as
    # HY_TEST_FIXTURE_DIR elsewhere in this file.
    add_executable(test_compaction_lease tests/test_compaction_lease.cpp)
    target_link_libraries(test_compaction_lease PRIVATE hengyuan_core GTest::gtest_main Threads::Threads)
    target_compile_definitions(test_compaction_lease PRIVATE
        HY_COMPACTION_LEASE_HOLDER_EXE="$<TARGET_FILE:compaction_lease_holder>")
    hengyuan_set_warnings(test_compaction_lease)

    gtest_discover_tests(test_compaction_lease PROPERTIES LABELS concurrency)

    # Round E breadcrumb L2 loaders (docs/SPEC_INVARIANTS.md's "Seal-journal
    # Round E breadcrumb L2 loaders" entry): SealJournalStoreLease's own
    # lifecycle surface + the two friend-only read methods, against a real
    # temp directory -- L2, real file I/O. Threading tests need Threads,
    # same pattern as test_compaction_lease above.
    add_executable(test_seal_journal_store_lease tests/test_seal_journal_store_lease.cpp)
    target_link_libraries(test_seal_journal_store_lease PRIVATE hengyuan_core GTest::gtest_main Threads::Threads)
    hengyuan_set_warnings(test_seal_journal_store_lease)

    gtest_discover_tests(test_seal_journal_store_lease PROPERTIES LABELS concurrency)

    # Round D: IntentStore -- genesis-only write/read/match/inspect via the
    # CandidateLease friend relationship (L2, 真实文件 I/O).
    add_executable(test_compaction_intent_store tests/test_compaction_intent_store.cpp)
    target_link_libraries(test_compaction_intent_store PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_compaction_intent_store)

    gtest_discover_tests(test_compaction_intent_store PROPERTIES LABELS concurrency)

    # Round E receipt + raise_intent_phase() (Building->Reserved) (docs/
    # SPEC_INVARIANTS.md's "Seal-journal Round E receipt + raise_intent_phase()
    # (Building->Reserved)" entry): IntentPhaseAdvancer, a second
    # CandidateLease-friend write path alongside IntentStore -- test-only
    # scope this round, see intent_phase_advancer.hpp's own header comment.
    add_executable(test_intent_phase_advancer tests/test_intent_phase_advancer.cpp)
    target_link_libraries(test_intent_phase_advancer PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_intent_phase_advancer)

    gtest_discover_tests(test_intent_phase_advancer PROPERTIES LABELS concurrency)

    # Round D: windows_native_io.hpp is #ifdef _WIN32-only (no POSIX
    # equivalent, see its own file header) -- this coverage only makes sense
    # on Windows, and is gated the same way the header itself is.
    if(WIN32)
        add_executable(test_windows_native_io tests/test_windows_native_io.cpp)
        target_link_libraries(test_windows_native_io PRIVATE hengyuan_core GTest::gtest_main)
        hengyuan_set_warnings(test_windows_native_io)
        gtest_discover_tests(test_windows_native_io)
    endif()

    # Phase 1: control-plane 11 种 payload 的帧编解码 (L1, 纯内存, 无文件 I/O --
    # 真正的文件落盘在 control_plane_log_sink.hpp，这里只验证帧格式本身，见
    # control_plane_frame_codec.hpp 头部注释)
    add_executable(test_control_plane_frame_codec tests/test_control_plane_frame_codec.cpp)
    target_link_libraries(test_control_plane_frame_codec PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_control_plane_frame_codec)
    gtest_discover_tests(test_control_plane_frame_codec)

    # Phase 1: ControlPlaneLogSink -- 首个真实 DurableControlPlaneSink 实现
    # (L2, 真实文件 I/O，基于共享 DurableLogStore，见 control_plane_log_sink.hpp
    # 头部注释：机制先行 + 两条安全关键 recovery fold 规则)
    add_executable(test_control_plane_log_sink tests/test_control_plane_log_sink.cpp)
    target_link_libraries(test_control_plane_log_sink PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_control_plane_log_sink)
    gtest_discover_tests(test_control_plane_log_sink)

    # 轨道 A 第四项: DurableAuditSink 核心 (L2, 真实文件 I/O -- 跨平台锁/fsync/
    # tip anchor 原子替换/recovery_scan()). 真实文件测试见
    # test_durable_audit_sink.cpp；纯内存帧格式测试见上面的 test_durable_frame_codec。
    add_executable(test_durable_audit_sink tests/test_durable_audit_sink.cpp)
    target_link_libraries(test_durable_audit_sink PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_audit_sink)
    gtest_discover_tests(test_durable_audit_sink)

    # Phase 5 (docs/SPEC_INVARIANTS.md): export-worker best-effort telemetry
    # consumer for ExportOutboxRing (L2, real file I/O for LastRemoteAckedTip
    # persistence; ExternalAnchorClient stays mock-only via FakeExternalAnchorClient,
    # same precedent as SubmitPort).
    add_executable(test_export_worker tests/test_export_worker.cpp)
    target_link_libraries(test_export_worker PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_export_worker)
    gtest_discover_tests(test_export_worker)

    # P2-EXEC-LIVE-01 D12-9: live submit orchestrator (L1, mock submit port)
    add_executable(test_live_submit_orchestrator tests/test_live_submit_orchestrator.cpp)
    target_link_libraries(test_live_submit_orchestrator PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_live_submit_orchestrator)
    gtest_discover_tests(test_live_submit_orchestrator)

    # 批次 6 6b-0c: target-position planner -- pure logic; only account_truth.hpp (hengyuan_core).
    add_executable(test_target_position_planner tests/test_target_position_planner.cpp)
    target_link_libraries(test_target_position_planner PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_target_position_planner)
    gtest_discover_tests(test_target_position_planner)

    # 批次 6 6b-0a: feed validity gate -- pure logic (header needs only <cstdint>), no Boost.
    add_executable(test_feed_validity_gate tests/test_feed_validity_gate.cpp)
    target_link_libraries(test_feed_validity_gate PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_feed_validity_gate)
    gtest_discover_tests(test_feed_validity_gate)

    # 批次 6 6b-1: proves orchestrate_submit() reaches exactly OperatorNotConfirmed given a
    # fully gate-satisfying context with confirmation never bound -- same no-real-network,
    # hengyuan_core-only dependency shape as live_submit_evidence_harness.cpp's own Fixture.
    add_executable(test_live_submit_preflight_harness tests/test_live_submit_preflight_harness.cpp)
    target_link_libraries(test_live_submit_preflight_harness PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_live_submit_preflight_harness)
    gtest_discover_tests(test_live_submit_preflight_harness)

    # P2-EXEC-LIVE-01 D12-5: order lifecycle state machine + idempotency (L1)
    add_executable(test_order_lifecycle tests/test_order_lifecycle.cpp)
    target_link_libraries(test_order_lifecycle PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_order_lifecycle)
    gtest_discover_tests(test_order_lifecycle)

    # P2-EXEC-LIVE-01 D12-6: exit-safety / kill-switch contract (L1)
    add_executable(test_exit_safety tests/test_exit_safety.cpp)
    target_link_libraries(test_exit_safety PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_exit_safety)
    gtest_discover_tests(test_exit_safety)

    # P2-EXEC-LIVE-01 D12-7: audit trail (L1, append-only)
    add_executable(test_audit_trail tests/test_audit_trail.cpp)
    target_link_libraries(test_audit_trail PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_audit_trail)
    gtest_discover_tests(test_audit_trail)

    # P2-EXEC-LIVE-01 D12-8: dry-run-before-live evidence chain (L1)
    add_executable(test_dry_run_evidence tests/test_dry_run_evidence.cpp)
    target_link_libraries(test_dry_run_evidence PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_dry_run_evidence)
    gtest_discover_tests(test_dry_run_evidence)

    # P2-EXEC-LIVE-01 D12-4: account truth + pre-trade validation (L1, no network)
    add_executable(test_account_truth tests/test_account_truth.cpp)
    target_link_libraries(test_account_truth PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_account_truth)
    gtest_discover_tests(test_account_truth)

    # L4 §5.1 (PR 5b): derive_scale_from_decimal_string()/parse_decimal_to_ticks_with_scale()
    # (L1, no network -- binance_decimal.hpp is deliberately independent of the
    # Boost/OpenSSL network stack test_binance_private_rest.cpp requires)
    add_executable(test_binance_decimal tests/test_binance_decimal.cpp)
    target_link_libraries(test_binance_decimal PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_binance_decimal)
    gtest_discover_tests(test_binance_decimal)

    # L4 §5.3/§5.3.1 (PR 5b): SymbolRegistry plain unit tests (no network, no concurrency --
    # the concurrency test is a separate binary immediately below, same
    # test_spsc_ring/test_spsc_concurrency split-by-label pattern used elsewhere in this file)
    add_executable(test_symbol_registry tests/test_symbol_registry.cpp)
    target_link_libraries(test_symbol_registry PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_symbol_registry)
    gtest_discover_tests(test_symbol_registry)

    # L4 §5.3.1's core invariant under real concurrent load: std::shared_mutex is the first
    # shared-state concurrency primitive this L4 branch has introduced. Same "concurrency"
    # label as test_reconcile_concurrency/test_spsc_concurrency above, for the same reasons --
    # WSL2 TSan is required, not optional, before trusting this design (see the test file's
    # own header comment).
    add_executable(test_symbol_registry_concurrency tests/test_symbol_registry_concurrency.cpp)
    target_link_libraries(test_symbol_registry_concurrency PRIVATE hengyuan_core GTest::gtest_main Threads::Threads)
    hengyuan_set_warnings(test_symbol_registry_concurrency)
    gtest_discover_tests(test_symbol_registry_concurrency PROPERTIES LABELS concurrency)

    # P2-EXEC-LIVE-01 D12-3: transport policy (L4, no network, policy validation only)
    add_executable(test_transport_policy tests/test_transport_policy.cpp)
    target_link_libraries(test_transport_policy PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_transport_policy)
    gtest_discover_tests(test_transport_policy)

    # Batch 5, 5a: StrategySpec TOML parser -- fail-closed load/validation tests, no
    # Boost/OpenSSL needed (hengyuan_strategy_spec has none).
    add_executable(test_strategy_spec_toml_parser tests/test_strategy_spec_toml_parser.cpp)
    target_link_libraries(test_strategy_spec_toml_parser PRIVATE hengyuan_strategy_spec GTest::gtest_main)
    hengyuan_set_warnings(test_strategy_spec_toml_parser)
    gtest_discover_tests(test_strategy_spec_toml_parser)

    # Batch 5, 5b: streaming operators (header-only, hengyuan_core only) -- per-operator unit
    # tests against hand-computed sequences.
    add_executable(test_strategy_spec_operators tests/test_strategy_spec_operators.cpp)
    target_link_libraries(test_strategy_spec_operators PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_strategy_spec_operators)
    gtest_discover_tests(test_strategy_spec_operators)

    # Batch 5, 5b: StreamingEvaluator -- DAG-level warm-up/signal-mode/capacity-gate tests.
    # Needs hengyuan_strategy_spec too (loads specs via load_strategy_spec()).
    add_executable(test_strategy_spec_evaluator tests/test_strategy_spec_evaluator.cpp)
    target_link_libraries(test_strategy_spec_evaluator PRIVATE hengyuan_strategy_spec GTest::gtest_main)
    hengyuan_set_warnings(test_strategy_spec_evaluator)
    gtest_discover_tests(test_strategy_spec_evaluator)

    # Batch 5, 5c: cross-language consistency test -- reads the checked-in golden fixtures
    # under tests/fixtures/strategy_spec/ (produced by
    # py_core/tools/generate_strategy_spec_fixture.py) via simdjson's DOM API. Links simdjson
    # directly (not via hengyuan_dataplane, which would drag in Boost/OpenSSL for no reason
    # here) -- same SYSTEM-include-dir workaround hengyuan_dataplane itself uses above, so
    # simdjson's own headers don't trip this project's /W4 /WX.
    add_executable(test_strategy_spec_consistency tests/test_strategy_spec_consistency.cpp)
    target_link_libraries(test_strategy_spec_consistency PRIVATE hengyuan_strategy_spec simdjson GTest::gtest_main)
    if(_simdjson_inc)
        target_include_directories(test_strategy_spec_consistency SYSTEM PRIVATE ${_simdjson_inc})
    endif()
    # Dedicated macro (not the unrelated, Boost-gated HY_TEST_FIXTURE_DIR set much later in
    # this file for the TLS-fixture-using tests) -- this target has no Boost/OpenSSL
    # dependency and must not be coupled to that variable's file-order-dependent availability.
    target_compile_definitions(test_strategy_spec_consistency PRIVATE
        HY_STRATEGY_SPEC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/strategy_spec")
    hengyuan_set_warnings(test_strategy_spec_consistency)
    gtest_discover_tests(test_strategy_spec_consistency)

    # 批次 6 6a-2: HoldingStateTracker -- pure value-type state machine, header-only
    # (hengyuan_core only, no Boost/OpenSSL/strategy_spec dependency of its own).
    add_executable(test_holding_state_tracker tests/test_holding_state_tracker.cpp)
    target_link_libraries(test_holding_state_tracker PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_holding_state_tracker)
    gtest_discover_tests(test_holding_state_tracker)

    # TODO 1A.2: spot rate-limit budget (REQUEST_WEIGHT/RAW_REQUESTS/ORDERS, 80/10/10 lanes)
    add_executable(test_spot_rate_limit_budget tests/test_spot_rate_limit_budget.cpp)
    target_link_libraries(test_spot_rate_limit_budget PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_spot_rate_limit_budget)
    gtest_discover_tests(test_spot_rate_limit_budget)

    # TODO 1A.3 follow-up: minimal net-quantity PositionTruth
    add_executable(test_position_truth tests/test_position_truth.cpp)
    target_link_libraries(test_position_truth PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_position_truth)
    gtest_discover_tests(test_position_truth)

    # TODO 1A.4: user-data-stream event struct/ring/drain function -- deliberately Boost-free
    # (see binance_user_data_event.hpp's own header comment), same tier as test_position_truth.
    add_executable(test_binance_user_data_event tests/test_binance_user_data_event.cpp)
    target_link_libraries(test_binance_user_data_event PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_binance_user_data_event)
    gtest_discover_tests(test_binance_user_data_event)

    # TODO 1A.4 batch 2: dedup-safe per-in-flight-order WS-fill baseline, shared by all three
    # fill-application paths -- same Boost-free tier as the two targets above.
    add_executable(test_order_fill_context tests/test_order_fill_context.cpp)
    target_link_libraries(test_order_fill_context PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_order_fill_context)
    gtest_discover_tests(test_order_fill_context)

    # P2-EXEC-LIVE-01 D12-2: secure env loader (L3, file I/O + permission check)
    add_executable(test_env_loader tests/test_env_loader.cpp)
    target_link_libraries(test_env_loader PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_env_loader)
    gtest_discover_tests(test_env_loader)

    # Batch 4 round 1: Binance L4 §2.2/§7.1.2 clock offset tracking -- offline
    # pure logic only, no OpenSSL/network dependency.
    add_executable(test_binance_clock_sync tests/test_binance_clock_sync.cpp)
    target_link_libraries(test_binance_clock_sync PRIVATE hengyuan_core GTest::gtest_main)
    target_include_directories(test_binance_clock_sync PRIVATE tests)
    hengyuan_set_warnings(test_binance_clock_sync)
    gtest_discover_tests(test_binance_clock_sync)

    # TODO 1A.4: listenKey cross-thread publisher -- offline pure logic only, same
    # no-OpenSSL/no-network dependency shape as test_binance_clock_sync above.
    add_executable(test_listen_key_publisher tests/test_listen_key_publisher.cpp)
    target_link_libraries(test_listen_key_publisher PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_listen_key_publisher)
    gtest_discover_tests(test_listen_key_publisher)

    # Layer 2 of docs/SPEC_INVARIANTS.md: durable_control_plane.hpp ABI surface
    add_executable(test_durable_control_plane_abi tests/test_durable_control_plane_abi.cpp)
    target_link_libraries(test_durable_control_plane_abi PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_control_plane_abi)
    gtest_discover_tests(test_durable_control_plane_abi)

    # 轨道 C: L4 §10 freeze-episode 垂直切片 (ABI-only, 见 durable_control_plane.hpp
    # 里对应的 SPEC-STRUCT 注释块)
    add_executable(test_durable_control_plane_freeze_abi tests/test_durable_control_plane_freeze_abi.cpp)
    target_link_libraries(test_durable_control_plane_freeze_abi PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_control_plane_freeze_abi)
    gtest_discover_tests(test_durable_control_plane_freeze_abi)

    # 轨道 C: L4 §10 DurableControlPlaneSink 接口族收尾 (3 个抽象接口 + 7 个依赖
    # 结构体, ABI-only, 见 durable_control_plane.hpp 里对应的 SPEC-CLASS/SPEC-METHOD
    # 注释块)
    add_executable(test_durable_control_plane_sink_interface_abi tests/test_durable_control_plane_sink_interface_abi.cpp)
    target_link_libraries(test_durable_control_plane_sink_interface_abi PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_control_plane_sink_interface_abi)
    gtest_discover_tests(test_durable_control_plane_sink_interface_abi)

    # 轨道 C: L4 §10 seal-journal Round A（SealIdWatermark 家族，ABI-only；
    # 见 durable_control_plane.hpp 顶部注释与 SPEC_INVARIANTS.md）
    add_executable(test_durable_control_plane_seal_journal_abi tests/test_durable_control_plane_seal_journal_abi.cpp)
    target_link_libraries(test_durable_control_plane_seal_journal_abi PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_control_plane_seal_journal_abi)
    gtest_discover_tests(test_durable_control_plane_seal_journal_abi)

    # 轨道 C: L4 §10 seal-journal Round B（Started 五元组，ABI-only）
    add_executable(test_durable_control_plane_seal_journal_started_abi tests/test_durable_control_plane_seal_journal_started_abi.cpp)
    target_link_libraries(test_durable_control_plane_seal_journal_started_abi PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_control_plane_seal_journal_started_abi)
    gtest_discover_tests(test_durable_control_plane_seal_journal_started_abi)

    # Round E Slice 1 (docs\SPEC_INVARIANTS.md "Seal-journal Round E Slice 1"):
    # SealIdWatermark/SealExportStartedWire codec (L1, pure in-memory, no file
    # I/O -- this codec has no I/O layer at all yet).
    add_executable(test_seal_journal_precondition_codec tests/test_seal_journal_precondition_codec.cpp)
    target_link_libraries(test_seal_journal_precondition_codec PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_seal_journal_precondition_codec)
    gtest_discover_tests(test_seal_journal_precondition_codec)

    # Round E breadcrumb L2: fixed-name, read-only CandidateLease loaders for
    # SealIdWatermark and legacy/V2 SealExportStarted records.
    add_executable(test_seal_id_watermark_export_started_loader
        tests/test_seal_id_watermark_export_started_loader.cpp)
    target_link_libraries(test_seal_id_watermark_export_started_loader PRIVATE
        hengyuan_core GTest::gtest_main Threads::Threads)
    hengyuan_set_warnings(test_seal_id_watermark_export_started_loader)
    gtest_discover_tests(test_seal_id_watermark_export_started_loader)

    # Round E Slice 2a (docs\SPEC_INVARIANTS.md "Seal-journal Round E Slice 2a"):
    # SealJournalCommitWatermark/SealJournalTombstoneWire codec (L1, pure
    # in-memory, no file I/O -- this codec has no I/O layer at all yet).
    add_executable(test_seal_journal_commit_tombstone_codec tests/test_seal_journal_commit_tombstone_codec.cpp)
    target_link_libraries(test_seal_journal_commit_tombstone_codec PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_seal_journal_commit_tombstone_codec)
    gtest_discover_tests(test_seal_journal_commit_tombstone_codec)

    # Round E breadcrumb L2: per-store, read-only commit-watermark and
    # tombstone loaders, including sparse 1..watermark enumeration.
    add_executable(test_seal_journal_commit_tombstone_loader
        tests/test_seal_journal_commit_tombstone_loader.cpp)
    target_link_libraries(test_seal_journal_commit_tombstone_loader PRIVATE
        hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_seal_journal_commit_tombstone_loader)

    gtest_discover_tests(test_seal_journal_commit_tombstone_loader)

    # Round E Slice 2b: SealExportStartedMigrationWire/SealStartedCleanupTombstoneWire/
    # SealStartedAbandonWire codec (seal_export_migration_cleanup_abandon_codec.hpp).
    add_executable(test_seal_export_migration_cleanup_abandon_codec tests/test_seal_export_migration_cleanup_abandon_codec.cpp)
    target_link_libraries(test_seal_export_migration_cleanup_abandon_codec PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_seal_export_migration_cleanup_abandon_codec)
    gtest_discover_tests(test_seal_export_migration_cleanup_abandon_codec)

    # Round E breadcrumb L2 loaders (seal_started_migration_cleanup_loader.hpp):
    # read-only loaders for seal-export-started.mig / seal-export-started.clr,
    # unit-tested against a duck-typed lease mock (no real file I/O needed --
    # the real CandidateLease read methods are friend-only, coordinator-side).
    add_executable(test_seal_started_migration_cleanup_loader tests/test_seal_started_migration_cleanup_loader.cpp)
    target_link_libraries(test_seal_started_migration_cleanup_loader PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_seal_started_migration_cleanup_loader)
    gtest_discover_tests(test_seal_started_migration_cleanup_loader)

    # Round E breadcrumb L2: SealStartedAbandonLoader (read-only .abd loader).
    add_executable(test_seal_started_abandon_loader tests/test_seal_started_abandon_loader.cpp)
    target_link_libraries(test_seal_started_abandon_loader PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_seal_started_abandon_loader)
    gtest_discover_tests(test_seal_started_abandon_loader)

    # Seal-journal Round E `.xgc` decode + loader: CompactionIntentGcAuthorizedLoader
    # (read-only .xgc loader; encode/decode themselves already landed in Round D).
    add_executable(test_compaction_intent_gc_authorized_loader tests/test_compaction_intent_gc_authorized_loader.cpp)
    target_link_libraries(test_compaction_intent_gc_authorized_loader PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_compaction_intent_gc_authorized_loader)
    gtest_discover_tests(test_compaction_intent_gc_authorized_loader)

    # Seal-journal Round E MigratedV2Started (V+M companion write path):
    # MigratedV2StartedPublisher -- new CandidateLease friend + write method,
    # same concurrency label/TSan wrapper discipline as test_compaction_lease.
    add_executable(test_migrated_v2_started_publisher tests/test_migrated_v2_started_publisher.cpp)
    target_link_libraries(test_migrated_v2_started_publisher PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_migrated_v2_started_publisher)

    gtest_discover_tests(test_migrated_v2_started_publisher PROPERTIES LABELS concurrency)

    # Round E breadcrumb L2 loaders: coordinator-only composition of Modules
    # 1/3/4's loaders into one call (load_all_breadcrumb_preconditions()) --
    # written after all three landed, see the header's own SCOPE comment.
    add_executable(test_seal_journal_breadcrumb_precondition_aggregate
        tests/test_seal_journal_breadcrumb_precondition_aggregate.cpp)
    target_link_libraries(test_seal_journal_breadcrumb_precondition_aggregate PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_seal_journal_breadcrumb_precondition_aggregate)
    gtest_discover_tests(test_seal_journal_breadcrumb_precondition_aggregate)

    # 轨道 C: L4 §10 seal-journal Round C（CompactionCandidateIntent GC 家族，最后
    # 一轮，ABI-only、声明层——不是 .x1/.xgc 的持久化/GC/恢复实现，见
    # durable_control_plane.hpp 顶部注释与 SPEC_INVARIANTS.md）
    add_executable(test_durable_control_plane_compaction_intent_abi tests/test_durable_control_plane_compaction_intent_abi.cpp)
    target_link_libraries(test_durable_control_plane_compaction_intent_abi PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_control_plane_compaction_intent_abi)
    gtest_discover_tests(test_durable_control_plane_compaction_intent_abi)

    # 轨道 A 第三项: reconcile/poll loop (order_tracker.hpp). Single-threaded here
    # (poll_once()/drain_reconcile_events() are plain functions) -- the real
    # cross-thread claim is verified separately by test_reconcile_concurrency.
    add_executable(test_order_tracker tests/test_order_tracker.cpp)
    target_link_libraries(test_order_tracker PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_order_tracker)
    gtest_discover_tests(test_order_tracker)

    # Real two-thread stress for the lock-free SPSC primitives. Labelled
    # "concurrency" so the TSan CI job can select just these (`ctest -L
    # concurrency`) — building all 27 test binaries under TSan triples the job
    # for zero added signal, since every other test in the tree is single-threaded
    # and TSan finds nothing in a single-threaded program.
    add_executable(test_spsc_concurrency tests/test_spsc_concurrency.cpp)
    target_link_libraries(test_spsc_concurrency PRIVATE hengyuan_core GTest::gtest_main Threads::Threads)
    hengyuan_set_warnings(test_spsc_concurrency)

    # TSan/WSL2 ASLR wrapping (setarch -R) is applied globally via
    # CMAKE_CROSSCOMPILING_EMULATOR near the top of this file — see that
    # comment for the full rationale. No per-target property needed here.

    gtest_discover_tests(test_spsc_concurrency PROPERTIES LABELS concurrency)

    # Real two-thread stress for the reconcile/poll loop (order_tracker.hpp +
    # live_submit_orchestrator.hpp's drain_reconcile_events() wiring) -- the
    # cross-thread claim test_order_tracker.cpp deliberately does NOT make
    # (that file is single-threaded by design, see its own header). Same
    # "concurrency" label as test_spsc_concurrency above, for the same reasons.
    add_executable(test_reconcile_concurrency tests/test_reconcile_concurrency.cpp)
    target_link_libraries(test_reconcile_concurrency PRIVATE hengyuan_core GTest::gtest_main Threads::Threads)
    hengyuan_set_warnings(test_reconcile_concurrency)

    gtest_discover_tests(test_reconcile_concurrency PROPERTIES LABELS concurrency)

    # TODO 1A.4: real two-thread stress for the two NEW cross-thread boundaries this batch
    # introduces (UserDataWsEventRing, ListenKeyPublisher) -- the first new cross-thread
    # boundary in this codebase since order_tracker.hpp's own SpscRing pair. Deliberately
    # Boost-free (binance_listen_key_publisher.hpp/binance_user_data_event.hpp both are), same
    # "concurrency" label/reasoning as test_spsc_concurrency/test_reconcile_concurrency above.
    add_executable(test_user_data_ws_concurrency tests/test_user_data_ws_concurrency.cpp)
    target_link_libraries(test_user_data_ws_concurrency PRIVATE hengyuan_core GTest::gtest_main Threads::Threads)
    hengyuan_set_warnings(test_user_data_ws_concurrency)

    gtest_discover_tests(test_user_data_ws_concurrency PROPERTIES LABELS concurrency)

    # Negative control for the TSan gate: a deliberately-racy ring that TSan MUST
    # report. Built only on demand — it is NOT registered with ctest, because a
    # deliberate data race must never look like a suite failure. CI runs the
    # binary directly and inverts the exit code (see ci-native-sanitizers.yml).
    option(HY_BUILD_TSAN_CONTROL "Build the deliberately-racy TSan negative control" OFF)
    if(HY_BUILD_TSAN_CONTROL)
        add_executable(tsan_control_relaxed_ring tests/tsan_control_relaxed_ring.cpp)
        target_link_libraries(tsan_control_relaxed_ring PRIVATE hengyuan_core Threads::Threads)
        hengyuan_set_warnings(tsan_control_relaxed_ring)

        # Phase 5: negative control proving ExportOutboxRing's documented
        # single-consumer contract is a real, TSan-detectable requirement --
        # see the file's own header comment for why it drives the ring
        # in-memory directly instead of through run_export_worker_once().
        add_executable(tsan_control_export_worker_dual_consumer
            tests/tsan_control_export_worker_dual_consumer.cpp)
        target_link_libraries(tsan_control_export_worker_dual_consumer PRIVATE hengyuan_core Threads::Threads)
        hengyuan_set_warnings(tsan_control_export_worker_dual_consumer)
    endif()

    # P2-EXEC-LIVE-01: HMAC signer (requires OpenSSL)
    if(HY_BUILD_DEMO)
        add_executable(test_binance_signer tests/test_binance_signer.cpp)
        target_link_libraries(test_binance_signer PRIVATE
            hengyuan_core OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS_NO_BOOST}
            GTest::gtest_main)
        target_include_directories(test_binance_signer SYSTEM PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_signer PRIVATE tests)
        hengyuan_set_warnings(test_binance_signer)
        gtest_discover_tests(test_binance_signer)

        # Batch 4 round 1: EnvironmentBinding + BoundHmacCredentials (depends
        # on BinanceSigner, hence OpenSSL) and §2.1 query signing.
        add_executable(test_binance_environment tests/test_binance_environment.cpp)
        target_link_libraries(test_binance_environment PRIVATE
            hengyuan_core OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS_NO_BOOST}
            GTest::gtest_main)
        target_include_directories(test_binance_environment SYSTEM PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_environment PRIVATE tests)
        hengyuan_set_warnings(test_binance_environment)
        gtest_discover_tests(test_binance_environment)

        add_executable(test_binance_query_signing tests/test_binance_query_signing.cpp)
        target_link_libraries(test_binance_query_signing PRIVATE
            hengyuan_core OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS_NO_BOOST}
            GTest::gtest_main)
        target_include_directories(test_binance_query_signing SYSTEM PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_query_signing PRIVATE tests)
        hengyuan_set_warnings(test_binance_query_signing)
        gtest_discover_tests(test_binance_query_signing)

        # P2-MD-02 / Track C: REST/WS/Gate connectivity tests -- fully offline (local
        # blackhole/TLS fixtures, see tests/test_helpers/blackhole_acceptor.hpp), but they need
        # Boost/OpenSSL to build at all, so they're gated the same as test_binance_signer above.
        # HY_TEST_FIXTURE_DIR points at the checked-in test CA/leaf cert used by the
        # wrong-SAN/read-timeout fixtures -- passed as a compile definition rather than assumed
        # relative to ctest's working directory.
        set(HY_TEST_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")

        add_executable(test_binance_rest_snapshot tests/test_binance_rest_snapshot.cpp)
        target_link_libraries(test_binance_rest_snapshot PRIVATE
            hengyuan_dataplane Boost::system OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
            Threads::Threads GTest::gtest_main)
        target_include_directories(test_binance_rest_snapshot SYSTEM PRIVATE
            ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_rest_snapshot PRIVATE tests)
        target_compile_definitions(test_binance_rest_snapshot PRIVATE
            HY_TEST_FIXTURE_DIR="${HY_TEST_FIXTURE_DIR}")
        hengyuan_set_warnings(test_binance_rest_snapshot)
        hengyuan_allow_third_party_tsan_fences(test_binance_rest_snapshot)
        gtest_discover_tests(test_binance_rest_snapshot)

        # L4 §3/§4: read-only private REST client -- first real network consumer of the
        # signer/environment/query-signing/clock-sync foundation above, so it needs both that
        # stack (OpenSSL) and the REST transport stack (Boost/Beast + simdjson via
        # hengyuan_dataplane, same as test_binance_rest_snapshot above).
        add_executable(test_binance_private_rest tests/test_binance_private_rest.cpp)
        target_link_libraries(test_binance_private_rest PRIVATE
            hengyuan_dataplane Boost::system OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
            Threads::Threads GTest::gtest_main)
        target_include_directories(test_binance_private_rest SYSTEM PRIVATE
            ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_private_rest PRIVATE tests)
        target_compile_definitions(test_binance_private_rest PRIVATE
            HY_TEST_FIXTURE_DIR="${HY_TEST_FIXTURE_DIR}")
        hengyuan_set_warnings(test_binance_private_rest)
        hengyuan_allow_third_party_tsan_fences(test_binance_private_rest)
        gtest_discover_tests(test_binance_private_rest)

        add_executable(test_binance_ws_session tests/test_binance_ws_session.cpp)
        target_link_libraries(test_binance_ws_session PRIVATE
            hengyuan_dataplane Boost::system OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
            Threads::Threads GTest::gtest_main)
        target_include_directories(test_binance_ws_session SYSTEM PRIVATE
            ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_ws_session PRIVATE tests)
        hengyuan_set_warnings(test_binance_ws_session)
        hengyuan_allow_third_party_tsan_fences(test_binance_ws_session)
        gtest_discover_tests(test_binance_ws_session)

        # TODO 1A.4: user-data-stream WS session -- same Boost/Beast/OpenSSL/simdjson
        # dependency shape as test_binance_ws_session above, same blackhole-acceptor
        # no-real-network fixture pattern.
        add_executable(test_binance_user_data_ws_session tests/test_binance_user_data_ws_session.cpp)
        target_link_libraries(test_binance_user_data_ws_session PRIVATE
            hengyuan_dataplane Boost::system OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
            Threads::Threads GTest::gtest_main)
        target_include_directories(test_binance_user_data_ws_session SYSTEM PRIVATE
            ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_user_data_ws_session PRIVATE tests)
        hengyuan_set_warnings(test_binance_user_data_ws_session)
        hengyuan_allow_third_party_tsan_fences(test_binance_user_data_ws_session)
        gtest_discover_tests(test_binance_user_data_ws_session)

        # 批次 6 6a-1: public kline/candlestick WS session -- same Boost/Beast/OpenSSL/simdjson
        # dependency shape and blackhole-acceptor no-real-network fixture pattern as
        # test_binance_user_data_ws_session above.
        add_executable(test_binance_kline_ws_session tests/test_binance_kline_ws_session.cpp)
        target_link_libraries(test_binance_kline_ws_session PRIVATE
            hengyuan_dataplane Boost::system OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
            Threads::Threads GTest::gtest_main)
        target_include_directories(test_binance_kline_ws_session SYSTEM PRIVATE
            ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_kline_ws_session PRIVATE tests)
        hengyuan_set_warnings(test_binance_kline_ws_session)
        hengyuan_allow_third_party_tsan_fences(test_binance_kline_ws_session)
        gtest_discover_tests(test_binance_kline_ws_session)

        # TODO 1A.4 batch 2: the reconnect supervisor -- constructs real
        # BinanceUserDataWsSession instances (via the same blackhole-acceptor no-real-network
        # fixture pattern as test_binance_user_data_ws_session above), so it needs the same
        # Boost/Beast/OpenSSL/simdjson dependency shape. Single-threaded (poll() is called
        # directly, not from a real io_context-driving thread, for most of this file's tests) --
        # not labelled concurrency.
        add_executable(test_binance_user_data_ws_supervisor tests/test_binance_user_data_ws_supervisor.cpp)
        target_link_libraries(test_binance_user_data_ws_supervisor PRIVATE
            hengyuan_dataplane Boost::system OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
            Threads::Threads GTest::gtest_main)
        target_include_directories(test_binance_user_data_ws_supervisor SYSTEM PRIVATE
            ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_user_data_ws_supervisor PRIVATE tests)
        hengyuan_set_warnings(test_binance_user_data_ws_supervisor)
        hengyuan_allow_third_party_tsan_fences(test_binance_user_data_ws_supervisor)
        gtest_discover_tests(test_binance_user_data_ws_supervisor)

        # Batch H, H3: listenKey keepalive/expiry scheduler -- constructs a real
        # BinancePrivateRestClient against a local TlsResponseAcceptor (test_binance_private_rest
        # above's own no-real-network fixture pattern), so it needs the same Boost/Beast/
        # OpenSSL/simdjson dependency shape and the same HY_TEST_FIXTURE_DIR compile definition
        # (set once above, reused here).
        add_executable(test_binance_listen_key_keepalive tests/test_binance_listen_key_keepalive.cpp)
        target_link_libraries(test_binance_listen_key_keepalive PRIVATE
            hengyuan_dataplane Boost::system OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
            Threads::Threads GTest::gtest_main)
        target_include_directories(test_binance_listen_key_keepalive SYSTEM PRIVATE
            ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_listen_key_keepalive PRIVATE tests)
        target_compile_definitions(test_binance_listen_key_keepalive PRIVATE
            HY_TEST_FIXTURE_DIR="${HY_TEST_FIXTURE_DIR}")
        hengyuan_set_warnings(test_binance_listen_key_keepalive)
        hengyuan_allow_third_party_tsan_fences(test_binance_listen_key_keepalive)
        gtest_discover_tests(test_binance_listen_key_keepalive)

        # Batch H, H5: binance_submit_adapter.hpp -- composite SubmitPort wiring combining
        # BinancePrivateRestClient (Boost/Beast/OpenSSL, same as test_binance_private_rest
        # above) with SymbolRegistry (hengyuan_core only). Registered in this Boost-linked
        # group, not test_symbol_registry's lightweight one, since it needs the union of both
        # dependency sets. Same TlsResponseAcceptor/HY_TEST_FIXTURE_DIR fixture pattern as
        # test_binance_private_rest/test_binance_listen_key_keepalive above.
        add_executable(test_binance_submit_adapter tests/test_binance_submit_adapter.cpp)
        target_link_libraries(test_binance_submit_adapter PRIVATE
            hengyuan_dataplane Boost::system OpenSSL::SSL OpenSSL::Crypto ${HY_OPENSSL_WIN32_LIBS}
            Threads::Threads GTest::gtest_main)
        target_include_directories(test_binance_submit_adapter SYSTEM PRIVATE
            ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
        target_include_directories(test_binance_submit_adapter PRIVATE tests)
        target_compile_definitions(test_binance_submit_adapter PRIVATE
            HY_TEST_FIXTURE_DIR="${HY_TEST_FIXTURE_DIR}")
        hengyuan_set_warnings(test_binance_submit_adapter)
        hengyuan_allow_third_party_tsan_fences(test_binance_submit_adapter)
        gtest_discover_tests(test_binance_submit_adapter)

    endif()

    # AUDIT VERIF-TSAN-016: SnapshotRefreshGate spawns a real std::thread and hands a
    # mailbox across it, but its tests used to sit behind HY_BUILD_DEMO (the header
    # named the production fetcher as a constructor default, dragging in Boost/OpenSSL)
    # -- and the TSan job builds HY_BUILD_DEMO=OFF with a hand-picked target list. The
    # one class here doing real cross-thread publication had never run under TSan.
    # snapshot_refresh_gate.hpp is Boost-free now and this binary is labelled
    # `concurrency`, with the same setarch wrapper the other TSan targets need under
    # WSL2. The one gate test that genuinely needs a real fetch moved to
    # test_binance_rest_snapshot.cpp above.
    add_executable(test_snapshot_refresh_gate tests/test_snapshot_refresh_gate.cpp)
    target_link_libraries(test_snapshot_refresh_gate PRIVATE
        hengyuan_core GTest::gtest_main Threads::Threads)
    hengyuan_set_warnings(test_snapshot_refresh_gate)

    gtest_discover_tests(test_snapshot_refresh_gate PROPERTIES LABELS concurrency)

    # P2-MD-02 / Track C: depth snapshot codec -- deliberately does NOT depend on
    # HY_BUILD_DEMO/Boost/OpenSSL (see binance_depth_snapshot_codec.hpp's own header comment),
    # so it builds and runs under the default HY_BUILD_TESTS configuration.
    add_executable(test_binance_depth_snapshot_codec tests/test_binance_depth_snapshot_codec.cpp)
    target_link_libraries(test_binance_depth_snapshot_codec PRIVATE
        hengyuan_dataplane GTest::gtest_main)
    hengyuan_set_warnings(test_binance_depth_snapshot_codec)
    gtest_discover_tests(test_binance_depth_snapshot_codec)
endif()
