cmake_minimum_required(VERSION 3.22)
project(ZeroTwoGoA VERSION 2.0.1 LANGUAGES C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/write-if-different.cmake")

set(KH3AP_PINNED_UE4SS_REPOSITORY "https://github.com/ashandarei/ue4ss-goa.git")
set(KH3AP_PINNED_UE4SS_COMMIT "06dd053d3e537ec2b477ee76337661b15642d012")
set(KH3AP_PINNED_CORROSION_REPOSITORY "https://github.com/corrosion-rs/corrosion.git")
set(KH3AP_PINNED_CORROSION_COMMIT "123be1e3d8170c86e121392e8bffa4def7dc3447")
set(UE4SS_SRC "" CACHE PATH "Path to a local RE-UE4SS source checkout")
option(
    KH3AP_ALLOW_UNPINNED_UE4SS
    "Allow a UE4SS checkout other than the reviewed ue4ss-goa revision"
    OFF)

get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Game__Shipping__Win64" CACHE STRING "" FORCE)
endif()
if(NOT _is_multi_config AND
   NOT CMAKE_BUILD_TYPE MATCHES "^(Game|CasePreserving)__(Debug|Shipping|Test)__Win64$")
    message(FATAL_ERROR
        "Unsupported UE4SS build type '${CMAKE_BUILD_TYPE}'. Generic Debug/Release types "
        "omit Unreal build definitions. Reconfigure with "
        "-DCMAKE_BUILD_TYPE=Game__Shipping__Win64 as described in README.md.")
endif()

if(NOT UE4SS_SRC)
    set(_default_ue4ss_src "${CMAKE_CURRENT_LIST_DIR}/vendor/ue4ss-goa")
    if(EXISTS "${_default_ue4ss_src}/CMakeLists.txt")
        set(UE4SS_SRC "${_default_ue4ss_src}" CACHE PATH "Path to a local RE-UE4SS source checkout" FORCE)
        message(STATUS "Using default pinned ue4ss-goa checkout at ${UE4SS_SRC}")
    else()
        message(FATAL_ERROR
            "Pinned ue4ss-goa checkout is missing. Clone ${KH3AP_PINNED_UE4SS_REPOSITORY} "
            "at ${KH3AP_PINNED_UE4SS_COMMIT} into ${_default_ue4ss_src}, or set UE4SS_SRC explicitly.")
    endif()
endif()

if(NOT EXISTS "${UE4SS_SRC}/CMakeLists.txt")
    message(FATAL_ERROR "UE4SS_SRC does not point to a valid RE-UE4SS checkout: ${UE4SS_SRC}")
endif()

find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${UE4SS_SRC}"
    RESULT_VARIABLE _ue4ss_revision_result
    OUTPUT_VARIABLE _ue4ss_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT _ue4ss_revision_result EQUAL 0)
    message(FATAL_ERROR "UE4SS_SRC is not a Git checkout, so its pinned revision cannot be verified")
endif()

if(NOT "${_ue4ss_revision}" STREQUAL "${KH3AP_PINNED_UE4SS_COMMIT}")
    if(KH3AP_ALLOW_UNPINNED_UE4SS)
        message(WARNING
            "Using unpinned UE4SS revision ${_ue4ss_revision}; "
            "release builds require ${KH3AP_PINNED_UE4SS_COMMIT}")
    else()
        message(FATAL_ERROR
            "UE4SS_SRC is at ${_ue4ss_revision}; expected reviewed ue4ss-goa revision "
            "${KH3AP_PINNED_UE4SS_COMMIT}. Set KH3AP_ALLOW_UNPINNED_UE4SS=ON only for local research.")
    endif()
else()
    message(STATUS
        "Verified ue4ss-goa revision ${KH3AP_PINNED_UE4SS_COMMIT} "
        "(${KH3AP_PINNED_UE4SS_REPOSITORY})")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
    WORKING_DIRECTORY "${UE4SS_SRC}"
    RESULT_VARIABLE _ue4ss_status_result
    OUTPUT_VARIABLE _ue4ss_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT _ue4ss_status_result EQUAL 0)
    message(FATAL_ERROR "Unable to inspect the ue4ss-goa dependency worktree")
endif()
if(_ue4ss_status)
    if(KH3AP_ALLOW_UNPINNED_UE4SS)
        message(WARNING "ue4ss-goa has tracked local changes; this is not a release build")
    else()
        message(FATAL_ERROR
            "ue4ss-goa has tracked local changes. Release builds require the clean pinned checkout; "
            "set KH3AP_ALLOW_UNPINNED_UE4SS=ON only for local research.")
    endif()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" submodule status --recursive
    WORKING_DIRECTORY "${UE4SS_SRC}"
    RESULT_VARIABLE _ue4ss_submodule_result
    OUTPUT_VARIABLE _ue4ss_submodule_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT _ue4ss_submodule_result EQUAL 0 OR _ue4ss_submodule_status MATCHES "(^|\n)[-+U]")
    message(FATAL_ERROR
        "ue4ss-goa submodules are missing or do not match the pinned revision. Run "
        "git -C \"${UE4SS_SRC}\" submodule update --init --recursive")
endif()

# The Corrosion revision pinned by ue4ss-goa models Cargo as an always-dirty
# custom target. Cargo itself exits quickly when nothing changed, but the
# permanently dirty edge adds noise and makes downstream native targets look
# dirty to Ninja. Predeclare the same revision (the first FetchContent
# declaration wins), patch it to create real target stamps, and include the
# sibling patternsleuth workspace as an input root for path dependencies.
include(FetchContent)
set(_corrosion_incremental_patch
    "${CMAKE_CURRENT_LIST_DIR}/cmake/patches/corrosion-incremental-cargo-target.patch")
set(_corrosion_patch_script
    "${CMAKE_CURRENT_LIST_DIR}/cmake/apply-corrosion-incremental-patch.cmake")
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY "${KH3AP_PINNED_CORROSION_REPOSITORY}"
    GIT_TAG "${KH3AP_PINNED_CORROSION_COMMIT}"
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DCORROSION_SOURCE_DIR=<SOURCE_DIR>"
        "-DCORROSION_PATCH_FILE=${_corrosion_incremental_patch}"
        "-DCORROSION_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -P "${_corrosion_patch_script}")

# Existing build trees may already have populated Corrosion before this patch
# declaration was added. Patch those trees idempotently before ue4ss-goa calls
# FetchContent_MakeAvailable(); fresh trees use PATCH_COMMAND above.
if(DEFINED Corrosion_SOURCE_DIR AND
   EXISTS "${Corrosion_SOURCE_DIR}/cmake/Corrosion.cmake")
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DCORROSION_SOURCE_DIR=${Corrosion_SOURCE_DIR}"
            "-DCORROSION_PATCH_FILE=${_corrosion_incremental_patch}"
            "-DCORROSION_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
            -P "${_corrosion_patch_script}"
        RESULT_VARIABLE _corrosion_patch_result)
    if(NOT _corrosion_patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to prepare the cached Corrosion source")
    endif()
endif()

set(CORROSION_ADDITIONAL_BUILD_INPUT_ROOTS
    "${UE4SS_SRC}/deps/first/patternsleuth")

add_subdirectory("${UE4SS_SRC}" "${CMAKE_BINARY_DIR}/RE-UE4SS")

# KH3's UE4.17 fork retains the cache-line-padded lock-free UObject free list.
# The generic Derived417 layout in the pinned UE4SS revision models that member
# as one pointer, which places its create/delete listener arrays at 0x50/0x60.
# KH3 accesses those arrays at 0x150/0x160. Registering a listener through the
# generic layout therefore overwrites the free-list bookkeeping and can make
# two concurrent UObject allocations claim the same occupied index (the engine
# line-177 fatal). Generate a KH3-only header override with the padded layout,
# while leaving the reviewed dependency checkout clean. Exact source matching
# and static offset assertions make this deviation fail closed.
set(_ue4ss_override_dir
    "${CMAKE_CURRENT_BINARY_DIR}/generated/kh3ap_ue4ss_overrides")
set(_ue4ss_derived417_source
    "${UE4SS_SRC}/deps/first/Unreal/include/Unreal/VersionedContainer/Derived417.hpp")
if(NOT EXISTS "${_ue4ss_derived417_source}")
    message(FATAL_ERROR
        "Unable to locate UE4SS Derived417.hpp for KH3 UObject-array hardening")
endif()
file(READ "${_ue4ss_derived417_source}" _ue4ss_derived417_contents)
set(_ue4ss_derived417_free_list_declaration
    "            uint8* ObjAvailableList;")
string(REGEX MATCHALL
    "uint8\\* ObjAvailableList"
    _ue4ss_derived417_free_list_matches
    "${_ue4ss_derived417_contents}")
list(LENGTH _ue4ss_derived417_free_list_matches
    _ue4ss_derived417_free_list_match_count)
if(NOT _ue4ss_derived417_free_list_match_count EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one unpadded Derived417 ObjAvailableList declaration; found ${_ue4ss_derived417_free_list_match_count}")
endif()
string(REPLACE
    "${_ue4ss_derived417_free_list_declaration}"
    "            uint8 ObjAvailableList[PLATFORM_CACHE_LINE_SIZE + 0x8 + PLATFORM_CACHE_LINE_SIZE];"
    _ue4ss_derived417_patched
    "${_ue4ss_derived417_contents}")
set(_ue4ss_derived417_stale_comment
    "            // In 4.17, this is a pointer NOT surrounded by padding.")
set(_ue4ss_derived417_kh3_comment
    "            // KH3 retains the two cache-line pads around this lock-free list.")
string(FIND
    "${_ue4ss_derived417_patched}"
    "${_ue4ss_derived417_stale_comment}"
    _ue4ss_derived417_comment_index)
if(_ue4ss_derived417_comment_index EQUAL -1)
    message(FATAL_ERROR
        "Expected the reviewed Derived417 ObjAvailableList layout comment")
endif()
string(REPLACE
    "${_ue4ss_derived417_stale_comment}"
    "${_ue4ss_derived417_kh3_comment}"
    _ue4ss_derived417_patched
    "${_ue4ss_derived417_patched}")
set(_ue4ss_derived417_override_dir
    "${_ue4ss_override_dir}/include/Unreal/VersionedContainer")
file(MAKE_DIRECTORY "${_ue4ss_derived417_override_dir}")
set(_ue4ss_derived417_override
    "${_ue4ss_derived417_override_dir}/Derived417.hpp")
kh3ap_write_if_different("${_ue4ss_derived417_override}" "${_ue4ss_derived417_patched}")
target_include_directories(Unreal BEFORE PUBLIC
    "${_ue4ss_override_dir}/include")
set(_ue4ss_derived417_layout_assert
    "${_ue4ss_override_dir}/Derived417LayoutAssert.cpp")
string(CONCAT _ue4ss_derived417_layout_assert_contents
    "#include <cstddef>\n"
    "#include <Unreal/VersionedContainer/Derived417.hpp>\n\n"
    "static_assert(offsetof(RC::Unreal::Derived417::FUObjectArray, UObjectCreateListeners) == 0x150,\n"
    "              \"KH3 UObject create-listener offset must be 0x150\");\n"
    "static_assert(offsetof(RC::Unreal::Derived417::FUObjectArray, UObjectDeleteListeners) == 0x160,\n"
    "              \"KH3 UObject delete-listener offset must be 0x160\");\n")
kh3ap_write_if_different("${_ue4ss_derived417_layout_assert}"
    "${_ue4ss_derived417_layout_assert_contents}")
target_sources(Unreal PRIVATE "${_ue4ss_derived417_layout_assert}")
message(STATUS
    "Configured KH3 UE4SS Derived417 listener offsets at 0x150/0x160")

# KH3's UE4.17 async package loader can construct exports concurrently. The
# pinned UE4SS StaticConstructObject detour runs a global post-hook/search-pool
# path for every one of those exports; live KH3 minidumps show the detour in
# both line-177 FUObjectArray concurrency failures. Build a generated override
# of UnrealInitializer.cpp that leaves every other UE4SS hook intact but does
# not install that one detour. Generating the one-line override keeps the pinned
# dependency checkout clean and makes the reviewed deviation fail closed if
# upstream source changes around the expected call site.
set(_ue4ss_unreal_initializer_source
    "${UE4SS_SRC}/deps/first/Unreal/src/UnrealInitializer.cpp")
if(NOT EXISTS "${_ue4ss_unreal_initializer_source}")
    message(FATAL_ERROR
        "Unable to locate UE4SS UnrealInitializer.cpp for KH3 async-loading hardening")
endif()
file(READ "${_ue4ss_unreal_initializer_source}"
    _ue4ss_unreal_initializer_contents)
set(_ue4ss_static_construct_hook_call "        HookStaticConstructObject();")
string(REGEX MATCHALL
    "HookStaticConstructObject\\(\\)"
    _ue4ss_static_construct_hook_matches
    "${_ue4ss_unreal_initializer_contents}")
list(LENGTH _ue4ss_static_construct_hook_matches
    _ue4ss_static_construct_hook_match_count)
if(NOT _ue4ss_static_construct_hook_match_count EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one UE4SS HookStaticConstructObject call; found ${_ue4ss_static_construct_hook_match_count}")
endif()
set(_ue4ss_static_construct_hook_disabled
    "        Output::send(STR(\"[KH3AP] UE4SS StaticConstructObject detour disabled for KH3 async-loading stability\\n\"));")
string(REPLACE
    "${_ue4ss_static_construct_hook_call}"
    "${_ue4ss_static_construct_hook_disabled}"
    _ue4ss_unreal_initializer_patched
    "${_ue4ss_unreal_initializer_contents}")
file(MAKE_DIRECTORY "${_ue4ss_override_dir}")
set(_ue4ss_unreal_initializer_override
    "${_ue4ss_override_dir}/UnrealInitializer.cpp")
kh3ap_write_if_different("${_ue4ss_unreal_initializer_override}"
    "${_ue4ss_unreal_initializer_patched}")
set_property(
    SOURCE "${_ue4ss_unreal_initializer_source}"
    TARGET_DIRECTORY Unreal
    PROPERTY HEADER_FILE_ONLY TRUE)
target_sources(Unreal PRIVATE "${_ue4ss_unreal_initializer_override}")
message(STATUS
    "Configured KH3 UE4SS without the global StaticConstructObject detour")

# Keep UE4SS's upstream linker defaults instead of forcing /OPT:REF on its
# shipping DLL. Runtime size optimization can be revisited after Steam parity.

# The pinned PolyHook revision allocates executable trampolines from new[],
# then changes protection on the surrounding ordinary heap page. Apply the
# KH3 Win64 hardening patch to the fetched source before it is compiled so each
# trampoline owns a VirtualAlloc page and transitions from RW to RX explicitly.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_polyhook_hardening_patch
        "${CMAKE_CURRENT_LIST_DIR}/cmake/patches/polyhook-virtualalloc-trampoline.patch")
    if(NOT PolyHook_2_SOURCE_DIR OR NOT EXISTS "${PolyHook_2_SOURCE_DIR}/sources/x64Detour.cpp")
        message(FATAL_ERROR "Unable to locate fetched PolyHook source for KH3 trampoline hardening")
    endif()

    execute_process(
        COMMAND git apply --check "${_polyhook_hardening_patch}"
        WORKING_DIRECTORY "${PolyHook_2_SOURCE_DIR}"
        RESULT_VARIABLE _polyhook_patch_check
        OUTPUT_QUIET
        ERROR_QUIET)
    if(_polyhook_patch_check EQUAL 0)
        execute_process(
            COMMAND git apply --whitespace=nowarn "${_polyhook_hardening_patch}"
            WORKING_DIRECTORY "${PolyHook_2_SOURCE_DIR}"
            RESULT_VARIABLE _polyhook_patch_result)
        if(NOT _polyhook_patch_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply KH3 PolyHook trampoline hardening patch")
        endif()
        message(STATUS "Applied KH3 PolyHook VirtualAlloc trampoline hardening")
    else()
        execute_process(
            COMMAND git apply --reverse --check "${_polyhook_hardening_patch}"
            WORKING_DIRECTORY "${PolyHook_2_SOURCE_DIR}"
            RESULT_VARIABLE _polyhook_reverse_check
            OUTPUT_QUIET
            ERROR_QUIET)
        if(NOT _polyhook_reverse_check EQUAL 0)
            message(FATAL_ERROR "PolyHook source does not match the expected hardening patch revision")
        endif()
        message(STATUS "KH3 PolyHook VirtualAlloc trampoline hardening already applied")
    endif()
endif()


include(cmake/kh3-json-dependency.cmake)
add_subdirectory(src/probe)
