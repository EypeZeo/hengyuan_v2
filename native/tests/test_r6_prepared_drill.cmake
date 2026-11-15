cmake_minimum_required(VERSION 3.22)

foreach(required_var IN ITEMS DRILL RESOLVER SCRATCH_ROOT)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} must be supplied")
    endif()
endforeach()

foreach(tool_var IN ITEMS DRILL RESOLVER)
    if(NOT EXISTS "${${tool_var}}" OR IS_DIRECTORY "${${tool_var}}")
        message(FATAL_ERROR "${tool_var} does not name an executable file: ${${tool_var}}")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${SCRATCH_ROOT}")
    message(FATAL_ERROR "SCRATCH_ROOT is not an existing directory: ${SCRATCH_ROOT}")
endif()
file(REAL_PATH "${SCRATCH_ROOT}" scratch_root_real)

set(scratch_dir "")
set(owner_token "")
foreach(attempt RANGE 1 32)
    string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef candidate_name)
    string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef candidate_token)
    set(candidate_dir "${scratch_root_real}/r6-prepared-drill-${candidate_name}")
    if(NOT EXISTS "${candidate_dir}")
        file(MAKE_DIRECTORY "${candidate_dir}")
        if(IS_DIRECTORY "${candidate_dir}")
            set(scratch_dir "${candidate_dir}")
            set(owner_token "${candidate_token}")
            break()
        endif()
    endif()
endforeach()
if(scratch_dir STREQUAL "")
    message(FATAL_ERROR "Could not create a fresh R6 prepared-drill scratch directory under ${scratch_root_real}")
endif()

file(REAL_PATH "${scratch_dir}" scratch_real)
cmake_path(IS_PREFIX scratch_root_real "${scratch_real}" NORMALIZE scratch_is_child)
if(NOT scratch_is_child OR scratch_real STREQUAL scratch_root_real)
    message(FATAL_ERROR "The generated scratch directory is not a child of SCRATCH_ROOT: ${scratch_real}")
endif()
set(owner_marker "${scratch_real}/.r6-prepared-drill-owner")
file(WRITE "${owner_marker}" "${owner_token}")

set(test_uid "18446744073709551615")
set(drill_dir "${scratch_real}/generated-store")
set(invalid_uid_dir "${scratch_real}/invalid-uid-store")

execute_process(
    COMMAND "${DRILL}" --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_stdout
    ERROR_VARIABLE help_stderr
)
if(NOT help_result STREQUAL "0")
    message(FATAL_ERROR "hy_r6_prepare_drill --help returned ${help_result}. Scratch directory retained at ${scratch_real}.\n${help_stdout}\n${help_stderr}")
endif()

execute_process(
    COMMAND "${DRILL}" --output-dir "${drill_dir}" --uid "${test_uid}"
    RESULT_VARIABLE drill_result
    OUTPUT_VARIABLE drill_stdout
    ERROR_VARIABLE drill_stderr
)
if(NOT drill_result STREQUAL "0")
    message(FATAL_ERROR "Prepared-drill generation returned ${drill_result}. Scratch directory retained at ${scratch_real}.\n${drill_stdout}\n${drill_stderr}")
endif()

set(drill_output "${drill_stdout}\n${drill_stderr}")
string(REGEX MATCH "coid=([A-Za-z0-9_-]+)" coid_match "${drill_output}")
if(coid_match STREQUAL "")
    message(FATAL_ERROR "Prepared-drill success output did not contain coid=<ID>. Scratch directory retained at ${scratch_real}.\n${drill_output}")
endif()
set(drill_coid "${CMAKE_MATCH_1}")

if(NOT IS_DIRECTORY "${drill_dir}")
    message(FATAL_ERROR "The generator did not create its output directory. Scratch directory retained at ${scratch_real}")
endif()
foreach(expected_file IN ITEMS audit.log control.log operator.kek audit.keys r6-drill.txt)
    if(NOT EXISTS "${drill_dir}/${expected_file}" OR IS_DIRECTORY "${drill_dir}/${expected_file}")
        message(FATAL_ERROR "The generated store is missing ${expected_file}. Scratch directory retained at ${scratch_real}")
    endif()
endforeach()
file(SIZE "${drill_dir}/audit.log" audit_log_size)
if(NOT audit_log_size EQUAL 512)
    message(FATAL_ERROR "The generated audit log is ${audit_log_size} bytes; expected two 256-byte order frames (512 bytes). Scratch directory retained at ${scratch_real}")
endif()

function(run_list out_var err_var result_var)
    execute_process(
        COMMAND "${RESOLVER}"
            --audit-log "${drill_dir}/audit.log"
            --control-log "${drill_dir}/control.log"
            --kek-file "${drill_dir}/operator.kek"
            --key-file "${drill_dir}/audit.keys"
            --audit-key-id 1
            --control-key-id 1
            --list
        RESULT_VARIABLE list_result
        OUTPUT_VARIABLE list_stdout
        ERROR_VARIABLE list_stderr
    )
    set(${out_var} "${list_stdout}" PARENT_SCOPE)
    set(${err_var} "${list_stderr}" PARENT_SCOPE)
    set(${result_var} "${list_result}" PARENT_SCOPE)
endfunction()

run_list(list_before list_before_stderr list_before_result)
if(NOT list_before_result STREQUAL "0")
    message(FATAL_ERROR "hy_operator_resolve --list returned ${list_before_result}. Scratch directory retained at ${scratch_real}.\n${list_before}\n${list_before_stderr}")
endif()

string(REGEX MATCH "unresolved orders: 1[^0-9]" unresolved_count_line "${list_before}")
if(unresolved_count_line STREQUAL "")
    message(FATAL_ERROR "The resolver did not list exactly one unresolved order. Scratch directory retained at ${scratch_real}.\n${list_before}")
endif()
string(REGEX MATCHALL "\\[[0-9]+\\] ${drill_coid}  state Ambiguous  Live  BTCUSDT" matching_rows "${list_before}")
list(LENGTH matching_rows matching_row_count)
if(NOT matching_row_count EQUAL 1)
    message(FATAL_ERROR "The resolver did not list exactly one matching Ambiguous BTCUSDT order (${drill_coid}). Scratch directory retained at ${scratch_real}.\n${list_before}")
endif()
string(REGEX MATCH "store binding: bound to TESTNET, account uid ${test_uid} \\(bound at [0-9]+\\)" binding_line "${list_before}")
if(binding_line STREQUAL "")
    message(FATAL_ERROR "The resolver did not show the Testnet binding for uid ${test_uid}. Scratch directory retained at ${scratch_real}.\n${list_before}")
endif()
string(FIND "${list_before}" "intent-only orders (nothing was ever sent for them; not listed): 0" no_intent_orphans_at)
if(no_intent_orphans_at EQUAL -1)
    message(FATAL_ERROR "The resolver did not report zero Intent-only orders. Scratch directory retained at ${scratch_real}.\n${list_before}")
endif()
string(FIND "${list_before}" "-> can be resolved" resolvable_at)
if(resolvable_at EQUAL -1)
    message(FATAL_ERROR "The generated order was not shown as resolvable. Scratch directory retained at ${scratch_real}.\n${list_before}")
endif()

execute_process(
    COMMAND "${DRILL}" --output-dir "${drill_dir}" --uid "${test_uid}"
    RESULT_VARIABLE reuse_result
    OUTPUT_VARIABLE reuse_stdout
    ERROR_VARIABLE reuse_stderr
)
if(reuse_result STREQUAL "0")
    message(FATAL_ERROR "The generator accepted a reused output directory. Scratch directory retained at ${scratch_real}.\n${reuse_stdout}\n${reuse_stderr}")
endif()

run_list(list_after_reuse list_after_reuse_stderr list_after_reuse_result)
if(NOT list_after_reuse_result STREQUAL "0")
    message(FATAL_ERROR "hy_operator_resolve --list after output-directory reuse returned ${list_after_reuse_result}. Scratch directory retained at ${scratch_real}.\n${list_after_reuse}\n${list_after_reuse_stderr}")
endif()
if(NOT list_after_reuse STREQUAL list_before)
    message(FATAL_ERROR "The listed store changed after the generator refused a reused directory. Scratch directory retained at ${scratch_real}.\nBefore:\n${list_before}\nAfter:\n${list_after_reuse}")
endif()

execute_process(
    COMMAND "${DRILL}" --output-dir "${invalid_uid_dir}" --uid 0
    RESULT_VARIABLE invalid_uid_result
    OUTPUT_VARIABLE invalid_uid_stdout
    ERROR_VARIABLE invalid_uid_stderr
)
if(invalid_uid_result STREQUAL "0")
    message(FATAL_ERROR "The generator accepted uid 0. Scratch directory retained at ${scratch_real}.\n${invalid_uid_stdout}\n${invalid_uid_stderr}")
endif()
if(EXISTS "${invalid_uid_dir}")
    message(FATAL_ERROR "The generator created output for invalid uid 0. Scratch directory retained at ${scratch_real}")
endif()

file(REAL_PATH "${scratch_dir}" cleanup_real)
cmake_path(IS_PREFIX scratch_root_real "${cleanup_real}" NORMALIZE cleanup_is_child)
if(NOT cleanup_is_child OR cleanup_real STREQUAL scratch_root_real OR NOT cleanup_real STREQUAL scratch_real)
    message(FATAL_ERROR "The scratch directory changed identity or location; it was retained at ${cleanup_real}")
endif()
if(NOT EXISTS "${owner_marker}")
    message(FATAL_ERROR "The scratch ownership marker is missing; scratch directory retained at ${scratch_real}")
endif()
file(READ "${owner_marker}" current_owner_token)
if(NOT current_owner_token STREQUAL owner_token)
    message(FATAL_ERROR "The scratch ownership marker changed; scratch directory retained at ${scratch_real}")
endif()
file(REMOVE_RECURSE "${cleanup_real}")

message(STATUS "R6 prepared-drill offline CLI checks passed for ${drill_coid}")
