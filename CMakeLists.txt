cmake_minimum_required(VERSION 3.22)

project(vulkan_cube_renderer VERSION 0.2.0 LANGUAGES C CXX)

include(FetchContent)
include(ExternalProject)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(
    VCR_ENABLE_VALIDATION
    "Force Vulkan validation on by default in every build configuration"
    OFF
)
option(VCR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.3.296.0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.3.296.0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.12
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.2.1
    GIT_SHALLOW TRUE
)
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
if(WIN32 AND MSVC)
    FetchContent_Declare(
        FidelityFXSDK
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
        GIT_TAG v1.1.4
        GIT_SHALLOW TRUE
    )
endif()

FetchContent_MakeAvailable(
    VulkanHeaders
    SDL3
    VulkanMemoryAllocator
    glm
    nlohmann_json
)
FetchContent_GetProperties(volk)
if(NOT volk_POPULATED)
    FetchContent_Populate(volk)
endif()
set(VCR_HAS_FSR3 OFF)
if(WIN32 AND MSVC)
    FetchContent_GetProperties(FidelityFXSDK)
    if(NOT fidelityfxsdk_POPULATED)
        FetchContent_Populate(FidelityFXSDK)
    endif()

    find_program(VCR_MSVC_LIB_EXECUTABLE NAMES lib.exe lib REQUIRED)
    set(VCR_FFX_RUNTIME_PREFIX "${CMAKE_BINARY_DIR}/ffx-runtime")
    set(VCR_FFX_VULKAN_LIBRARY "${VCR_FFX_RUNTIME_PREFIX}/vulkan-1.lib")
    file(MAKE_DIRECTORY "${VCR_FFX_RUNTIME_PREFIX}")
    execute_process(
        COMMAND
            "${VCR_MSVC_LIB_EXECUTABLE}"
            /nologo
            /machine:x64
            "/def:${CMAKE_CURRENT_SOURCE_DIR}/cmake/vulkan-1.def"
            "/out:${VCR_FFX_VULKAN_LIBRARY}"
        RESULT_VARIABLE VCR_FFX_IMPORT_LIBRARY_RESULT
        OUTPUT_VARIABLE VCR_FFX_IMPORT_LIBRARY_OUTPUT
        ERROR_VARIABLE VCR_FFX_IMPORT_LIBRARY_ERROR
    )
    if(NOT VCR_FFX_IMPORT_LIBRARY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Failed to create the Vulkan loader import library required by "
            "FidelityFX:\n${VCR_FFX_IMPORT_LIBRARY_OUTPUT}"
            "${VCR_FFX_IMPORT_LIBRARY_ERROR}"
        )
    endif()

    set(
        VCR_FFX_RUNTIME_DLL
        "${fidelityfxsdk_SOURCE_DIR}/ffx-api/bin/amd_fidelityfx_vk.dll"
    )
    ExternalProject_Add(
        vcr_fidelityfx_runtime
        PREFIX "${VCR_FFX_RUNTIME_PREFIX}"
        SOURCE_DIR "${fidelityfxsdk_SOURCE_DIR}/ffx-api"
        BINARY_DIR "${VCR_FFX_RUNTIME_PREFIX}/build"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        PATCH_COMMAND
            "${CMAKE_COMMAND}"
            "-DFFX_SDK_SOURCE_DIR=${fidelityfxsdk_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_fidelityfx_fsr3.cmake"
        CMAKE_GENERATOR "Visual Studio 17 2022"
        CMAKE_GENERATOR_PLATFORM x64
        CMAKE_ARGS
            -DFFX_API_BACKEND=VK_X64
            "-DVulkan_INCLUDE_DIR:PATH=${vulkanheaders_SOURCE_DIR}/include"
            "-DVulkan_LIBRARY:FILEPATH=${VCR_FFX_VULKAN_LIBRARY}"
        BUILD_COMMAND
            "${CMAKE_COMMAND}" --build <BINARY_DIR>
            --config Release --parallel
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${VCR_FFX_RUNTIME_DLL}"
        USES_TERMINAL_BUILD TRUE
    )
    set(VCR_HAS_FSR3 ON)
endif()

add_library(vcr_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(vcr_volk PUBLIC
    "${volk_SOURCE_DIR}"
    "${vulkanheaders_SOURCE_DIR}/include"
)
target_link_libraries(vcr_volk PUBLIC ${CMAKE_DL_LIBS})
add_library(volk::volk ALIAS vcr_volk)

add_executable(vulkan_cube
    src/main.cpp
    src/application.cpp
    src/cli_options.cpp
    src/fly_camera.cpp
    src/fsr3_upscaler.cpp
    src/gpu_buffer.cpp
    src/gpu_texture.cpp
    src/mcp_server.cpp
    src/performance_metrics.cpp
    src/png_encoder.cpp
    src/renderer.cpp
    src/swapchain.cpp
    src/vma.cpp
    src/vulkan_context.cpp
)

target_include_directories(vulkan_cube PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${vulkanmemoryallocator_SOURCE_DIR}/include"
    "${vulkanheaders_SOURCE_DIR}/include"
)
if(VCR_HAS_FSR3)
    target_include_directories(vulkan_cube PRIVATE
        "${fidelityfxsdk_SOURCE_DIR}/ffx-api/include"
    )
endif()

find_package(Threads REQUIRED)

target_link_libraries(vulkan_cube PRIVATE
    SDL3::SDL3-static
    volk::volk
    glm::glm
    nlohmann_json::nlohmann_json
    Threads::Threads
)

target_compile_definitions(vulkan_cube PRIVATE
    VK_NO_PROTOTYPES
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    "VCR_VALIDATION_DEFAULT=$<IF:$<OR:$<BOOL:${VCR_ENABLE_VALIDATION}>,$<CONFIG:Debug>>,1,0>"
    "VCR_HAS_FSR3=$<IF:$<BOOL:${VCR_HAS_FSR3}>,1,0>"
)

if(MSVC)
    target_compile_options(
        vulkan_cube PRIVATE /W4 /permissive- /Zc:preprocessor /GR- /EHsc
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vulkan_cube PRIVATE /WX)
    endif()
else()
    target_compile_options(vulkan_cube PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-rtti
        $<$<CONFIG:Release>:-O3>
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vulkan_cube PRIVATE -Werror)
    endif()
endif()

set_target_properties(vulkan_cube PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET vulkan_cube POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:vulkan_cube>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv"
            "$<TARGET_FILE_DIR:vulkan_cube>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:vulkan_cube>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:vulkan_cube>/assets"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:vulkan_cube>/LICENSES"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES"
            "$<TARGET_FILE_DIR:vulkan_cube>/LICENSES"
    COMMENT "Copying precompiled SPIR-V shaders and runtime assets"
)
if(VCR_HAS_FSR3)
    add_dependencies(vulkan_cube vcr_fidelityfx_runtime)
    add_custom_command(TARGET vulkan_cube POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${VCR_FFX_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:vulkan_cube>/amd_fidelityfx_vk.dll"
        COMMENT "Copying the source-built FidelityFX Vulkan runtime"
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(vcr_core_tests
        tests/core_tests.cpp
        src/cli_options.cpp
        src/fly_camera.cpp
        src/performance_metrics.cpp
        src/png_encoder.cpp
    )
    target_include_directories(vcr_core_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_definitions(vcr_core_tests PRIVATE
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_FORCE_RADIANS
        VCR_VALIDATION_DEFAULT=0
    )
    target_link_libraries(
        vcr_core_tests PRIVATE
        glm::glm
        Threads::Threads
    )
    if(MSVC)
        target_compile_options(
            vcr_core_tests PRIVATE /W4 /permissive- /EHsc
        )
    else()
        target_compile_options(vcr_core_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
    endif()
    add_test(NAME vcr_core_tests COMMAND vcr_core_tests)
endif()
