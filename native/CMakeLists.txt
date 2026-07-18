cmake_minimum_required(VERSION 3.22)
project(hengyuan_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerWarnings.cmake)
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
    find_package(Threads REQUIRED)

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
