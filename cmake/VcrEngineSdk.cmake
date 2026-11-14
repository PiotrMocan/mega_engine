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

if(NOT "${VCR_ENGINE_SDK_BUILD_DIR}" STREQUAL "")
    set(VCR_ENGINE_DLL "${VCR_ENGINE_SDK_BUILD_DIR}/bin/vcr_engine.dll")
    set(VCR_ENGINE_IMP_LIB "${VCR_ENGINE_SDK_BUILD_DIR}/lib/vcr_engine.lib")
    set(_vcr_sdk_deps_dir "${VCR_ENGINE_SDK_BUILD_DIR}/_deps")
else()
    if(EXISTS "${CMAKE_SOURCE_DIR}/vcr_engine.dll" AND
       EXISTS "${CMAKE_SOURCE_DIR}/engine/lib/vcr_engine.lib")
        set(VCR_ENGINE_DLL "${CMAKE_SOURCE_DIR}/vcr_engine.dll")
        set(VCR_ENGINE_IMP_LIB "${CMAKE_SOURCE_DIR}/engine/lib/vcr_engine.lib")
    else()
        foreach(_vcr_cfg IN ITEMS windows-release windows-debug windows-asan)
            if(EXISTS "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/bin/vcr_engine.dll" AND
               EXISTS "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/lib/vcr_engine.lib")
                set(VCR_ENGINE_DLL "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/bin/vcr_engine.dll")
                set(VCR_ENGINE_IMP_LIB "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/lib/vcr_engine.lib")
                break()
            endif()
        endforeach()
    endif()
    set(_vcr_sdk_deps_dir "")
    foreach(_vcr_cfg IN ITEMS windows-release windows-debug windows-asan)
        if(EXISTS "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/_deps/glm-src/glm/glm.hpp" AND
           EXISTS "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/_deps/nlohmann_json-src/include/nlohmann/json.hpp")
            set(_vcr_sdk_deps_dir "${VCR_ENGINE_ROOT}/build/${_vcr_cfg}/_deps")
            break()
        endif()
    endforeach()
endif()

if(NOT DEFINED VCR_ENGINE_DLL OR NOT EXISTS "${VCR_ENGINE_DLL}")
    message(FATAL_ERROR
        "Unable to locate vcr_engine.dll. Build the engine first or run 'Update Engine (.exe + .dll)' for the project.")
endif()
if(NOT DEFINED VCR_ENGINE_IMP_LIB OR NOT EXISTS "${VCR_ENGINE_IMP_LIB}")
    message(FATAL_ERROR
        "Unable to locate vcr_engine.lib. Build the engine first or set VCR_ENGINE_SDK_BUILD_DIR.")
endif()

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

if(MSVC AND CMAKE_CXX_FLAGS MATCHES "/fsanitize=address")
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
endif()

if("${VCR_ENGINE_SDK_BUILD_DIR}" STREQUAL "")
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
