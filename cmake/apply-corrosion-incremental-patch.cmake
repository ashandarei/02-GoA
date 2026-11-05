cmake_minimum_required(VERSION 3.22)

foreach(required_variable IN ITEMS
        CORROSION_SOURCE_DIR
        CORROSION_PATCH_FILE
        CORROSION_GIT_EXECUTABLE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_variable}")
    endif()
endforeach()

if(NOT EXISTS "${CORROSION_SOURCE_DIR}/cmake/Corrosion.cmake")
    message(FATAL_ERROR "Invalid Corrosion source directory: ${CORROSION_SOURCE_DIR}")
endif()
if(NOT EXISTS "${CORROSION_PATCH_FILE}")
    message(FATAL_ERROR "Missing Corrosion incremental-build patch: ${CORROSION_PATCH_FILE}")
endif()

execute_process(
    COMMAND "${CORROSION_GIT_EXECUTABLE}" apply --check --whitespace=nowarn "${CORROSION_PATCH_FILE}"
    WORKING_DIRECTORY "${CORROSION_SOURCE_DIR}"
    RESULT_VARIABLE patch_check_result
    OUTPUT_QUIET
    ERROR_QUIET)

if(patch_check_result EQUAL 0)
    execute_process(
        COMMAND "${CORROSION_GIT_EXECUTABLE}" apply --whitespace=nowarn "${CORROSION_PATCH_FILE}"
        WORKING_DIRECTORY "${CORROSION_SOURCE_DIR}"
        RESULT_VARIABLE patch_result)
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply the Corrosion incremental-build patch")
    endif()
    message(STATUS "Applied Corrosion incremental Cargo target patch")
else()
    execute_process(
        COMMAND "${CORROSION_GIT_EXECUTABLE}" apply --reverse --check "${CORROSION_PATCH_FILE}"
        WORKING_DIRECTORY "${CORROSION_SOURCE_DIR}"
        RESULT_VARIABLE reverse_check_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT reverse_check_result EQUAL 0)
        message(FATAL_ERROR
            "Corrosion source does not match the pinned incremental-build patch revision")
    endif()
    message(STATUS "Corrosion incremental Cargo target patch already applied")
endif()
