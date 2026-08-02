cmake_minimum_required(VERSION 3.22)
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

# --- D12-9 dry-run evidence harness (header-only, no Boost/OpenSSL needed) ---
add_executable(live_submit_evidence_harness src/live_submit_evidence_harness.cpp)
target_link_libraries(live_submit_evidence_harness PRIVATE hengyuan_core)
hengyuan_set_warnings(live_submit_evidence_harness)

# --- Binance feed demo (optional, requires Boost + OpenSSL) ---
option(HY_BUILD_DEMO "Build binance_feed_demo (requires Boost + OpenSSL)" OFF)

if(HY_BUILD_DEMO)
    find_package(Boost REQUIRED COMPONENTS system)
    find_package(OpenSSL REQUIRED)
    # Threads is found unconditionally at the top of this file (the concurrency
    # tests need it whether or not demos are built).

    add_executable(binance_feed_demo src/binance_feed_demo.cpp)
    target_link_libraries(binance_feed_demo PRIVATE
        hengyuan_dataplane
        Boost::system
        OpenSSL::SSL OpenSSL::Crypto
        Threads::Threads
    )
    target_include_directories(binance_feed_demo SYSTEM PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    hengyuan_set_warnings(binance_feed_demo)

    add_executable(binance_dry_run_demo src/binance_dry_run_demo.cpp)
    target_link_libraries(binance_dry_run_demo PRIVATE
        hengyuan_dataplane
        Boost::system
        OpenSSL::SSL OpenSSL::Crypto
        Threads::Threads
    )
    target_include_directories(binance_dry_run_demo SYSTEM PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    hengyuan_set_warnings(binance_dry_run_demo)

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

    add_executable(test_market_event_layout tests/test_market_event_layout.cpp)
    target_link_libraries(test_market_event_layout PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_market_event_layout)
    gtest_discover_tests(test_market_event_layout)

    add_executable(test_spsc_ring tests/test_spsc_ring.cpp)
    target_link_libraries(test_spsc_ring PRIVATE hengyuan_core GTest::gtest_main)
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

    # P2-INFRA-HB-01: SHM heartbeat (portable tests, no shm_open needed)
    add_executable(test_shm_heartbeat tests/test_shm_heartbeat.cpp)
    target_link_libraries(test_shm_heartbeat PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_shm_heartbeat)
    gtest_discover_tests(test_shm_heartbeat)

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

    # 轨道 A 第四项: durable 日志帧编解码 (L1, 纯内存, 无文件 I/O -- 真正的文件
    # 落盘在 durable_audit_sink.hpp，这里只验证帧格式本身)
    add_executable(test_durable_frame_codec tests/test_durable_frame_codec.cpp)
    target_link_libraries(test_durable_frame_codec PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_frame_codec)
    gtest_discover_tests(test_durable_frame_codec)

    # 轨道 A 第四项: DurableAuditSink 核心 (L2, 真实文件 I/O -- 跨平台锁/fsync/
    # tip anchor 原子替换/recovery_scan()). 真实文件测试见
    # test_durable_audit_sink.cpp；纯内存帧格式测试见上面的 test_durable_frame_codec。
    add_executable(test_durable_audit_sink tests/test_durable_audit_sink.cpp)
    target_link_libraries(test_durable_audit_sink PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_audit_sink)
    gtest_discover_tests(test_durable_audit_sink)

    # P2-EXEC-LIVE-01 D12-9: live submit orchestrator (L1, mock submit port)
    add_executable(test_live_submit_orchestrator tests/test_live_submit_orchestrator.cpp)
    target_link_libraries(test_live_submit_orchestrator PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_live_submit_orchestrator)
    gtest_discover_tests(test_live_submit_orchestrator)

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

    # P2-EXEC-LIVE-01 D12-3: transport policy (L4, no network, policy validation only)
    add_executable(test_transport_policy tests/test_transport_policy.cpp)
    target_link_libraries(test_transport_policy PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_transport_policy)
    gtest_discover_tests(test_transport_policy)

    # P2-EXEC-LIVE-01 D12-2: secure env loader (L3, file I/O + permission check)
    add_executable(test_env_loader tests/test_env_loader.cpp)
    target_link_libraries(test_env_loader PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_env_loader)
    gtest_discover_tests(test_env_loader)

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
    # Round C=CompactionCandidateIntent GC 家族 未做，见 durable_control_plane.hpp
    # 顶部注释与 SPEC_INVARIANTS.md）
    add_executable(test_durable_control_plane_seal_journal_abi tests/test_durable_control_plane_seal_journal_abi.cpp)
    target_link_libraries(test_durable_control_plane_seal_journal_abi PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_control_plane_seal_journal_abi)
    gtest_discover_tests(test_durable_control_plane_seal_journal_abi)

    # 轨道 C: L4 §10 seal-journal Round B（Started 五元组，ABI-only；Round C 仍未做）
    add_executable(test_durable_control_plane_seal_journal_started_abi tests/test_durable_control_plane_seal_journal_started_abi.cpp)
    target_link_libraries(test_durable_control_plane_seal_journal_started_abi PRIVATE hengyuan_core GTest::gtest_main)
    hengyuan_set_warnings(test_durable_control_plane_seal_journal_started_abi)
    gtest_discover_tests(test_durable_control_plane_seal_journal_started_abi)

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

    # TSan's shadow-memory setup can fail with "FATAL: ThreadSanitizer:
    # unexpected memory mapping" under WSL2, because WSL2's default ASLR
    # layout doesn't match what TSan expects for its fixed shadow region.
    # CROSSCOMPILING_EMULATOR is CMake's existing mechanism for prefixing
    # every invocation of a test binary with a wrapper command (designed for
    # running cross-compiled binaries under an emulator); `setarch -R`
    # disables ASLR for just this process and needs no root. It is applied
    # here, not only in the CI/local verify script, because
    # gtest_discover_tests() ALSO runs the binary once at build time to
    # enumerate test names, and that invocation hits the same fatal error if
    # left unwrapped — CROSSCOMPILING_EMULATOR is honored by both.
    if(HY_SANITIZER STREQUAL "thread" AND UNIX AND NOT APPLE)
        set_target_properties(test_spsc_concurrency PROPERTIES
            CROSSCOMPILING_EMULATOR "setarch;${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
    endif()

    gtest_discover_tests(test_spsc_concurrency PROPERTIES LABELS concurrency)

    # Real two-thread stress for the reconcile/poll loop (order_tracker.hpp +
    # live_submit_orchestrator.hpp's drain_reconcile_events() wiring) -- the
    # cross-thread claim test_order_tracker.cpp deliberately does NOT make
    # (that file is single-threaded by design, see its own header). Same
    # "concurrency" label/CROSSCOMPILING_EMULATOR treatment as
    # test_spsc_concurrency above, for the same reasons.
    add_executable(test_reconcile_concurrency tests/test_reconcile_concurrency.cpp)
    target_link_libraries(test_reconcile_concurrency PRIVATE hengyuan_core GTest::gtest_main Threads::Threads)
    hengyuan_set_warnings(test_reconcile_concurrency)

    if(HY_SANITIZER STREQUAL "thread" AND UNIX AND NOT APPLE)
        set_target_properties(test_reconcile_concurrency PROPERTIES
            CROSSCOMPILING_EMULATOR "setarch;${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
    endif()

    gtest_discover_tests(test_reconcile_concurrency PROPERTIES LABELS concurrency)

    # Negative control for the TSan gate: a deliberately-racy ring that TSan MUST
    # report. Built only on demand — it is NOT registered with ctest, because a
    # deliberate data race must never look like a suite failure. CI runs the
    # binary directly and inverts the exit code (see ci-native-sanitizers.yml).
    option(HY_BUILD_TSAN_CONTROL "Build the deliberately-racy TSan negative control" OFF)
    if(HY_BUILD_TSAN_CONTROL)
        add_executable(tsan_control_relaxed_ring tests/tsan_control_relaxed_ring.cpp)
        target_link_libraries(tsan_control_relaxed_ring PRIVATE hengyuan_core Threads::Threads)
        hengyuan_set_warnings(tsan_control_relaxed_ring)
    endif()

    # P2-EXEC-LIVE-01: HMAC signer (requires OpenSSL)
    if(HY_BUILD_DEMO)
        add_executable(test_binance_signer tests/test_binance_signer.cpp)
        target_link_libraries(test_binance_signer PRIVATE
            hengyuan_core OpenSSL::SSL OpenSSL::Crypto GTest::gtest_main)
        target_include_directories(test_binance_signer SYSTEM PRIVATE ${OPENSSL_INCLUDE_DIR})
        hengyuan_set_warnings(test_binance_signer)
        gtest_discover_tests(test_binance_signer)
    endif()
endif()
