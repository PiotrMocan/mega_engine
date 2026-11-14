include_guard(GLOBAL)

cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED VCR_ENGINE_ROOT OR "${VCR_ENGINE_ROOT}" STREQUAL "")
    if(DEFINED ENV{VCR_ENGINE_ROOT} AND NOT "$ENV{VCR_ENGINE_ROOT}" STREQUAL "")
        set(_vcr_candidate_root "$ENV{VCR_ENGINE_ROOT}")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/../../CMakeLists.txt" AND
           EXISTS "${CMAKE_SOURCE_DIR}/../../src/game_module_api.hpp")
        get_filename_component(_vcr_candidate_root "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
    else()
        get_filename_component(_vcr_candidate_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
        if(EXISTS "${_vcr_candidate_root}/../src/game_module_api.hpp")
            get_filename_component(_vcr_candidate_root "${_vcr_candidate_root}/.." ABSOLUTE)
        endif()
    endif()
    set(VCR_ENGINE_ROOT "${_vcr_candidate_root}" CACHE PATH "Root directory of the VCR engine repository")
endif()

set(VCR_ENGINE_SDK_BUILD_DIR "" CACHE PATH "Optional explicit engine build directory (e.g. build/windows-asan)")

string(TOUPPER "${CMAKE_BUILD_TYPE}" _vcr_build_type_upper)
if(DEFINED CMAKE_CONFIGURATION_TYPES AND NOT "${CMAKE_CONFIGURATION_TYPES}" STREQUAL "")
    set(_vcr_is_multi_config TRUE)
else()
    set(_vcr_is_multi_config FALSE)
endif()
if("${_vcr_build_type_upper}" STREQUAL "DEBUG")
    set(_vcr_is_debug TRUE)
else()
    set(_vcr_is_debug FALSE)
endif()
set(_vcr_is_asan FALSE)
if(CMAKE_CXX_FLAGS MATCHES "/fsanitize=address")
    set(_vcr_is_asan TRUE)
elseif(NOT "${_vcr_build_type_upper}" STREQUAL "" AND
       CMAKE_CXX_FLAGS_${_vcr_build_type_upper} MATCHES "/fsanitize=address")
    set(_vcr_is_asan TRUE)
endif()
if(_vcr_is_debug)
    set(_vcr_expected_stem "vcr_engined")
else()
    set(_vcr_expected_stem "vcr_engine")
endif()

function(_vcr_locate_sdk_pair out_dll out_lib base_dir)
    foreach(_vcr_stem IN LISTS ARGN)
        set(_vcr_dll_candidate "${base_dir}/bin/${_vcr_stem}.dll")
        set(_vcr_lib_candidate "${base_dir}/lib/${_vcr_stem}.lib")
        if(NOT EXISTS "${_vcr_lib_candidate}")
            set(_vcr_lib_candidate "${base_dir}/bin/${_vcr_stem}.lib")
        endif()
        if(EXISTS "${_vcr_dll_candidate}" AND EXISTS "${_vcr_lib_candidate}")
            set(${out_dll} "${_vcr_dll_candidate}" PARENT_SCOPE)
            set(${out_lib} "${_vcr_lib_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

function(_vcr_locate_project_sdk_pair out_dll out_lib proj_dir)
    foreach(_vcr_stem IN LISTS ARGN)
        set(_vcr_dll_candidate "${proj_dir}/${_vcr_stem}.dll")
        set(_vcr_lib_candidate "${proj_dir}/engine/lib/${_vcr_stem}.lib")
        if(EXISTS "${_vcr_dll_candidate}" AND EXISTS "${_vcr_lib_candidate}")
            set(${out_dll} "${_vcr_dll_candidate}" PARENT_SCOPE)
            set(${out_lib} "${_vcr_lib_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

unset(VCR_ENGINE_DLL)
unset(VCR_ENGINE_IMP_LIB)
set(_vcr_release_dll "")
set(_vcr_release_lib "")
set(_vcr_debug_dll "")
set(_vcr_debug_lib "")
set(_vcr_sdk_deps_dir "")

if(NOT "${VCR_ENGINE_SDK_BUILD_DIR}" STREQUAL "")
    set(_vcr_sdk_deps_dir "${VCR_ENGINE_SDK_BUILD_DIR}/_deps")
    if(_vcr_is_multi_config)
        _vcr_locate_sdk_pair(_vcr_release_dll _vcr_release_lib "${VCR_ENGINE_SDK_BUILD_DIR}" vcr_engine)
        _vcr_locate_sdk_pair(_vcr_debug_dll _vcr_debug_lib "${VCR_ENGINE_SDK_BUILD_DIR}" vcr_engined vcr_engine)
        if(NOT "${_vcr_release_dll}" STREQUAL "" AND NOT "${_vcr_release_lib}" STREQUAL "")
            set(VCR_ENGINE_DLL "${_vcr_release_dll}")
            set(VCR_ENGINE_IMP_LIB "${_vcr_release_lib}")
        else()
            set(VCR_ENGINE_DLL "${_vcr_debug_dll}")
            set(VCR_ENGINE_IMP_LIB "${_vcr_debug_lib}")
        endif()
    elseif(_vcr_is_debug)
        _vcr_locate_sdk_pair(VCR_ENGINE_DLL VCR_ENGINE_IMP_LIB "${VCR_ENGINE_SDK_BUILD_DIR}" vcr_engined vcr_engine)
    elseif("${_vcr_build_type_upper}" STREQUAL "")
        _vcr_locate_sdk_pair(VCR_ENGINE_DLL VCR_ENGINE_IMP_LIB "${VCR_ENGINE_SDK_BUILD_DIR}" vcr_engine vcr_engined)
    else()
        _vcr_locate_sdk_pair(VCR_ENGINE_DLL VCR_ENGINE_IMP_LIB "${VCR_ENGINE_SDK_BUILD_DIR}" vcr_engine)
    endif()
else()
    _vcr_locate_project_sdk_pair(_vcr_release_dll _vcr_release_lib "${CMAKE_SOURCE_DIR}" vcr_engine)
    if("${_vcr_release_dll}" STREQUAL "" OR "${_vcr_release_lib}" STREQUAL "")
        foreach(_vcr_cfg IN ITEMS windows-release windows-asan)
            _vcr_locate_sdk_pair(_vcr_release_dll _vcr_release_lib "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}" vcr_engine)
            if(NOT "${_vcr_release_dll}" STREQUAL "" AND NOT "${_vcr_release_lib}" STREQUAL "")
                break()
            endif()
        endforeach()
    endif()

    _vcr_locate_project_sdk_pair(_vcr_debug_dll _vcr_debug_lib "${CMAKE_SOURCE_DIR}" vcr_engined)
    if("${_vcr_debug_dll}" STREQUAL "" OR "${_vcr_debug_lib}" STREQUAL "")
        _vcr_locate_sdk_pair(_vcr_debug_dll _vcr_debug_lib "${VCR_ENGINE_ROOT}/build/windows-debug" vcr_engined vcr_engine)
    endif()

    if(_vcr_is_multi_config)
        if(NOT "${_vcr_release_dll}" STREQUAL "" AND NOT "${_vcr_release_lib}" STREQUAL "")
            set(VCR_ENGINE_DLL "${_vcr_release_dll}")
            set(VCR_ENGINE_IMP_LIB "${_vcr_release_lib}")
        else()
            set(VCR_ENGINE_DLL "${_vcr_debug_dll}")
            set(VCR_ENGINE_IMP_LIB "${_vcr_debug_lib}")
        endif()
    elseif(_vcr_is_debug)
        set(VCR_ENGINE_DLL "${_vcr_debug_dll}")
        set(VCR_ENGINE_IMP_LIB "${_vcr_debug_lib}")
        set(_vcr_sdk_deps_dir "${VCR_ENGINE_ROOT}/build/windows-debug/_deps")
    elseif(_vcr_is_asan)
        _vcr_locate_sdk_pair(VCR_ENGINE_DLL VCR_ENGINE_IMP_LIB "${VCR_ENGINE_ROOT}/build/windows-asan" vcr_engine)
        if(NOT "${VCR_ENGINE_DLL}" STREQUAL "" AND NOT "${VCR_ENGINE_IMP_LIB}" STREQUAL "")
            set(_vcr_sdk_deps_dir "${VCR_ENGINE_ROOT}/build/windows-asan/_deps")
        else()
            set(VCR_ENGINE_DLL "${_vcr_release_dll}")
            set(VCR_ENGINE_IMP_LIB "${_vcr_release_lib}")
        endif()
    else()
        set(VCR_ENGINE_DLL "${_vcr_release_dll}")
        set(VCR_ENGINE_IMP_LIB "${_vcr_release_lib}")
        if((NOT DEFINED VCR_ENGINE_DLL OR "${VCR_ENGINE_DLL}" STREQUAL "") AND
           "${_vcr_build_type_upper}" STREQUAL "")
            set(VCR_ENGINE_DLL "${_vcr_debug_dll}")
            set(VCR_ENGINE_IMP_LIB "${_vcr_debug_lib}")
        endif()
    endif()
endif()

if("${_vcr_sdk_deps_dir}" STREQUAL "" OR
   NOT EXISTS "${_vcr_sdk_deps_dir}/glm-src/glm/glm.hpp" OR
   NOT EXISTS "${_vcr_sdk_deps_dir}/nlohmann_json-src/include/nlohmann/json.hpp")
    set(_vcr_sdk_deps_dir "")
    foreach(_vcr_cfg IN ITEMS windows-release windows-debug windows-asan)
        if(EXISTS "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/_deps/glm-src/glm/glm.hpp" AND
           EXISTS "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/_deps/nlohmann_json-src/include/nlohmann/json.hpp")
            set(_vcr_sdk_deps_dir "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/_deps")
            break()
        endif()
    endforeach()
endif()

if(NOT DEFINED VCR_ENGINE_DLL OR "${VCR_ENGINE_DLL}" STREQUAL "" OR NOT EXISTS "${VCR_ENGINE_DLL}")
    message(FATAL_ERROR
        "Unable to locate ${_vcr_expected_stem}.dll for build type '${CMAKE_BUILD_TYPE}'. Build the matching engine configuration first or set VCR_ENGINE_SDK_BUILD_DIR.")
endif()
if(NOT DEFINED VCR_ENGINE_IMP_LIB OR "${VCR_ENGINE_IMP_LIB}" STREQUAL "" OR NOT EXISTS "${VCR_ENGINE_IMP_LIB}")
    message(FATAL_ERROR
        "Unable to locate ${_vcr_expected_stem}.lib for build type '${CMAKE_BUILD_TYPE}'. Build the matching engine configuration first or set VCR_ENGINE_SDK_BUILD_DIR.")
endif()

get_filename_component(VCR_ENGINE_DLL_NAME "${VCR_ENGINE_DLL}" NAME)

set(_vcr_engine_include_dirs "${VCR_ENGINE_ROOT}/src")
if(EXISTS "${CMAKE_SOURCE_DIR}/engine/include")
    list(PREPEND _vcr_engine_include_dirs "${CMAKE_SOURCE_DIR}/engine/include")
endif()

if(NOT "${_vcr_sdk_deps_dir}" STREQUAL "" AND
   EXISTS "${_vcr_sdk_deps_dir}/glm-src/glm/glm.hpp" AND
   EXISTS "${_vcr_sdk_deps_dir}/nlohmann_json-src/include/nlohmann/json.hpp")
    list(APPEND _vcr_engine_include_dirs
        "${_vcr_sdk_deps_dir}/glm-src"
        "${_vcr_sdk_deps_dir}/nlohmann_json-src/include"
    )
else()
    include(FetchContent)
    FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 1.0.1
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glm nlohmann_json)
    list(APPEND _vcr_engine_include_dirs
        "${glm_SOURCE_DIR}"
        "${nlohmann_json_SOURCE_DIR}/include"
    )
endif()

if(MSVC AND _vcr_is_asan)
    add_link_options(/INCREMENTAL:NO)
endif()

if(NOT TARGET vcr::engine)
    add_library(vcr::engine SHARED IMPORTED GLOBAL)
    set_target_properties(vcr::engine PROPERTIES
        IMPORTED_LOCATION "${VCR_ENGINE_DLL}"
        IMPORTED_IMPLIB "${VCR_ENGINE_IMP_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${_vcr_engine_include_dirs}"
        INTERFACE_COMPILE_DEFINITIONS "GLM_FORCE_DEPTH_ZERO_TO_ONE;GLM_FORCE_RADIANS"
        INTERFACE_COMPILE_FEATURES cxx_std_23
    )
    if(_vcr_is_multi_config)
        if(NOT "${_vcr_debug_dll}" STREQUAL "" AND NOT "${_vcr_debug_lib}" STREQUAL "")
            set_target_properties(vcr::engine PROPERTIES
                IMPORTED_LOCATION_DEBUG "${_vcr_debug_dll}"
                IMPORTED_IMPLIB_DEBUG "${_vcr_debug_lib}"
            )
        endif()
        if(NOT "${_vcr_release_dll}" STREQUAL "" AND NOT "${_vcr_release_lib}" STREQUAL "")
            set_target_properties(vcr::engine PROPERTIES
                IMPORTED_LOCATION_RELEASE "${_vcr_release_dll}"
                IMPORTED_IMPLIB_RELEASE "${_vcr_release_lib}"
                IMPORTED_LOCATION_RELWITHDEBINFO "${_vcr_release_dll}"
                IMPORTED_IMPLIB_RELWITHDEBINFO "${_vcr_release_lib}"
                IMPORTED_LOCATION_MINSIZEREL "${_vcr_release_dll}"
                IMPORTED_IMPLIB_MINSIZEREL "${_vcr_release_lib}"
            )
        endif()
    endif()
endif()

if("${VCR_ENGINE_SDK_BUILD_DIR}" STREQUAL "" AND NOT _vcr_is_debug AND NOT _vcr_is_asan)
    set(_vcr_default_stage_module ON)
else()
    set(_vcr_default_stage_module OFF)
endif()
option(
    VCR_STAGE_MODULE_TO_PROJECT_BIN
    "Stage compiled game module DLL into <project>/bin after build"
    ${_vcr_default_stage_module}
)

function(vcr_add_game_module target_name)
    cmake_parse_arguments(VCR_MOD "" "OUTPUT_NAME" "SOURCES" ${ARGN})
    if(NOT VCR_MOD_SOURCES)
        message(FATAL_ERROR "vcr_add_game_module(${target_name}) requires SOURCES")
    endif()
    set(_out_name "${VCR_MOD_OUTPUT_NAME}")
    if("${_out_name}" STREQUAL "")
        set(_out_name "${target_name}")
    endif()

    add_library(${target_name} SHARED ${VCR_MOD_SOURCES})
    target_link_libraries(${target_name} PUBLIC vcr::engine)
    target_compile_features(${target_name} PUBLIC cxx_std_23)
    set_target_properties(${target_name} PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        OUTPUT_NAME "${_out_name}"
        DEBUG_POSTFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4 /permissive- /Zc:preprocessor /GR- /EHsc
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-rtti
        )
    endif()
    if(WIN32)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:vcr::engine>"
                "$<TARGET_FILE_DIR:${target_name}>/$<TARGET_FILE_NAME:vcr::engine>"
            COMMENT "Staging $<TARGET_FILE_NAME:vcr::engine> beside $<TARGET_FILE_NAME:${target_name}>"
        )
    endif()
    if(VCR_STAGE_MODULE_TO_PROJECT_BIN)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_SOURCE_DIR}/bin"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${target_name}>"
                "${CMAKE_SOURCE_DIR}/bin/$<TARGET_FILE_NAME:${target_name}>"
            COMMENT "Staging $<TARGET_FILE_NAME:${target_name}> into project bin/"
        )
    endif()
endfunction()
