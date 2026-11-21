cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PROBE OR NOT EXISTS "${PROBE}" OR IS_DIRECTORY "${PROBE}")
    message(FATAL_ERROR "PROBE must name the child process executable")
endif()
if(NOT DEFINED SCRATCH_ROOT OR NOT IS_DIRECTORY "${SCRATCH_ROOT}")
    message(FATAL_ERROR "SCRATCH_ROOT must name the build directory")
endif()

file(REAL_PATH "${PROBE}" probe_path)
file(REAL_PATH "${SCRATCH_ROOT}" scratch_root)
string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef run_id)

function(run_phase phase log_path expected_exit)
    execute_process(
        COMMAND "${probe_path}" "${phase}" "${log_path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 30)
    if(NOT "${result}" STREQUAL "${expected_exit}")
        message(FATAL_ERROR
            "${phase} returned '${result}', expected ${expected_exit}.\n"
            "Files retained at ${log_path}.*.\n${stdout}${stderr}")
    endif()
endfunction()

function(check_recovery label exit_phase recovery_phase)
    set(log_path "${scratch_root}/safe04-${label}-${run_id}")
    set(sidecars "${log_path}.kill-switch.lock" "${log_path}.kill-switch.tip" "${log_path}.kill-switch.tip.tmp")
    foreach(path IN LISTS sidecars)
        if(EXISTS "${path}" OR IS_SYMLINK "${path}")
            message(FATAL_ERROR "Scratch file already exists: ${path}")
        endif()
    endforeach()

    run_phase("${exit_phase}" "${log_path}" 73)
    run_phase("${recovery_phase}" "${log_path}" 0)
    file(REMOVE ${sidecars})
    message(STATUS "SAFE-04 recovery passed: ${label}")
endfunction()

check_recovery(persisted exit-after-persist recover-persisted)
check_recovery(interrupted exit-before-persist recover-interrupted)
