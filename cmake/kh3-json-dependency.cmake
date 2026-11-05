# The companion uses one pinned decoder for bridge requests and native workers,
# including complete JSON validation and Unicode surrogate pairs.
if(NOT TARGET kh3ap_json_headers)
    set(_kh3ap_json_source "${CMAKE_CURRENT_LIST_DIR}/../build-json-deps/nlohmann_json-src")
    if(NOT EXISTS "${_kh3ap_json_source}/include/nlohmann/json.hpp")
        include(FetchContent)
        FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
            SOURCE_DIR "${_kh3ap_json_source}")
        set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${_kh3ap_json_source}"
        RESULT_VARIABLE _json_revision_result
        OUTPUT_VARIABLE _json_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${_kh3ap_json_source}"
        RESULT_VARIABLE _json_status_result
        OUTPUT_VARIABLE _json_status
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _json_revision_result EQUAL 0 OR NOT _json_status_result EQUAL 0 OR
       NOT "${_json_revision}" STREQUAL "9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03" OR
       _json_status)
        message(FATAL_ERROR "JSON dependency must be the clean pinned nlohmann/json checkout")
    endif()
    add_library(kh3ap_json_headers INTERFACE)
    target_include_directories(kh3ap_json_headers SYSTEM INTERFACE "${_kh3ap_json_source}/include")
    target_compile_features(kh3ap_json_headers INTERFACE cxx_std_20)
endif()
