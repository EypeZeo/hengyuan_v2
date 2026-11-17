cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PROBE OR "${PROBE}" STREQUAL "")
    message(FATAL_ERROR "PROBE must be supplied")
endif()
if(NOT EXISTS "${PROBE}" OR IS_DIRECTORY "${PROBE}")
    message(FATAL_ERROR "PROBE does not name an existing helper executable: ${PROBE}")
endif()
file(REAL_PATH "${PROBE}" probe_real)

if(NOT DEFINED SCRATCH_ROOT OR "${SCRATCH_ROOT}" STREQUAL "")
    message(FATAL_ERROR "SCRATCH_ROOT must be supplied")
endif()
if(NOT IS_DIRECTORY "${SCRATCH_ROOT}")
    message(FATAL_ERROR "SCRATCH_ROOT is not an existing directory: ${SCRATCH_ROOT}")
endif()
file(REAL_PATH "${SCRATCH_ROOT}" scratch_root_real)

set(recovery_timeout_seconds 30)

function(create_owned_fixture label out_dir_var out_token_var)
    set(fixture_dir "")
    set(owner_token "")
    foreach(attempt RANGE 1 32)
        string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef candidate_name)
        string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef candidate_token)
        set(candidate_dir "${scratch_root_real}/s7-process-recovery-${label}-${candidate_name}")
        set(candidate_owner_marker "${candidate_dir}.owner")
        if(NOT EXISTS "${candidate_dir}" AND NOT EXISTS "${candidate_owner_marker}")
            file(MAKE_DIRECTORY "${candidate_dir}")
            if(IS_DIRECTORY "${candidate_dir}")
                file(REAL_PATH "${candidate_dir}" candidate_real)
                cmake_path(IS_PREFIX scratch_root_real "${candidate_real}" NORMALIZE candidate_is_child)
                if(NOT candidate_is_child OR candidate_real STREQUAL scratch_root_real)
                    message(FATAL_ERROR "Generated fixture directory escaped SCRATCH_ROOT: ${candidate_real}")
                endif()
                set(fixture_dir "${candidate_real}")
                set(owner_token "${candidate_token}")
                break()
            endif()
        endif()
    endforeach()

    if(fixture_dir STREQUAL "")
        message(FATAL_ERROR "Could not create a fresh ${label} fixture under ${scratch_root_real}")
    endif()

    set(owner_marker "${fixture_dir}.owner")
    file(WRITE "${owner_marker}" "${owner_token}")
    set(${out_dir_var} "${fixture_dir}" PARENT_SCOPE)
    set(${out_token_var} "${owner_token}" PARENT_SCOPE)
endfunction()

function(run_phase fixture_dir action expected_code)
    execute_process(
        COMMAND "${probe_real}" "${action}" "${fixture_dir}"
        WORKING_DIRECTORY "${fixture_dir}"
        RESULT_VARIABLE phase_result
        OUTPUT_VARIABLE phase_stdout
        ERROR_VARIABLE phase_stderr
        TIMEOUT ${recovery_timeout_seconds}
    )
    if(NOT "${phase_result}" STREQUAL "${expected_code}")
        set(timeout_note "")
        if("${phase_result}" MATCHES "[Tt]imeout")
            set(timeout_note " The helper exceeded the ${recovery_timeout_seconds}-second timeout.")
        endif()
        message(FATAL_ERROR
            "S7 recovery action '${action}' returned '${phase_result}', expected exit code ${expected_code}.${timeout_note}\n"
            "Fixture retained at '${fixture_dir}'.\n"
            "stdout:\n${phase_stdout}\n"
            "stderr:\n${phase_stderr}")
    endif()
endfunction()

function(verify_owned_fixture fixture_dir owner_token out_real_var)
    file(REAL_PATH "${fixture_dir}" cleanup_real)
    cmake_path(IS_PREFIX scratch_root_real "${cleanup_real}" NORMALIZE cleanup_is_child)
    if(NOT cleanup_is_child OR cleanup_real STREQUAL scratch_root_real OR NOT cleanup_real STREQUAL fixture_dir)
        message(FATAL_ERROR "Fixture identity or location changed; it was retained at '${cleanup_real}'")
    endif()

    set(owner_marker "${fixture_dir}.owner")
    if(NOT EXISTS "${owner_marker}" OR IS_DIRECTORY "${owner_marker}")
        message(FATAL_ERROR "Fixture ownership marker is missing; fixture retained at '${cleanup_real}'")
    endif()
    file(REAL_PATH "${owner_marker}" owner_marker_real)
    cmake_path(IS_PREFIX scratch_root_real "${owner_marker_real}" NORMALIZE owner_marker_is_child)
    if(NOT owner_marker_is_child OR NOT owner_marker_real STREQUAL owner_marker)
        message(FATAL_ERROR "Fixture ownership marker changed location; fixture retained at '${cleanup_real}'")
    endif()

    file(READ "${owner_marker}" current_owner_token)
    if(NOT current_owner_token STREQUAL owner_token)
        message(FATAL_ERROR "Fixture ownership marker changed; fixture retained at '${cleanup_real}'")
    endif()

    set(${out_real_var} "${cleanup_real}" PARENT_SCOPE)
endfunction()

create_owned_fixture("partial" partial_fixture partial_owner_token)
create_owned_fixture("stale-anchor" stale_anchor_fixture stale_anchor_owner_token)

run_phase("${partial_fixture}" "seed" 0)
run_phase("${partial_fixture}" "crash-partial" 73)
run_phase("${partial_fixture}" "recover-partial" 0)

run_phase("${stale_anchor_fixture}" "seed" 0)
run_phase("${stale_anchor_fixture}" "crash-stale-anchor" 73)
run_phase("${stale_anchor_fixture}" "recover-stale-anchor" 0)

verify_owned_fixture("${partial_fixture}" "${partial_owner_token}" cleanup_partial_fixture)
verify_owned_fixture("${stale_anchor_fixture}" "${stale_anchor_owner_token}" cleanup_stale_anchor_fixture)

file(REMOVE_RECURSE "${cleanup_partial_fixture}")
file(REMOVE_RECURSE "${cleanup_stale_anchor_fixture}")
if(EXISTS "${cleanup_partial_fixture}" OR EXISTS "${cleanup_stale_anchor_fixture}")
    message(FATAL_ERROR "Could not remove the ownership-verified S7 recovery fixtures; ownership markers were retained")
endif()
file(REMOVE "${partial_fixture}.owner")
file(REMOVE "${stale_anchor_fixture}.owner")
if(EXISTS "${partial_fixture}.owner" OR EXISTS "${stale_anchor_fixture}.owner")
    message(FATAL_ERROR "Could not remove the S7 recovery fixture ownership markers")
endif()

message(STATUS "S7 process-recovery crash and recovery scenarios passed")
