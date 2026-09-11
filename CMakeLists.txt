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

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
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

set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_USE_CCACHE OFF CACHE BOOL "" FORCE)
set(ASSIMP_IGNORE_GIT_HASH ON CACHE BOOL "" FORCE)

set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_DEVELOPER_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_OSS_FUZZ OFF CACHE BOOL "" FORCE)
set(OPENEXR_TEST_LIBRARIES OFF CACHE BOOL "" FORCE)
set(OPENEXR_TEST_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_IMATH ON CACHE BOOL "" FORCE)
set(IMATH_INSTALL OFF CACHE BOOL "" FORCE)
set(IMATH_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)

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
FetchContent_Declare(
    DearImGui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8-docking
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v6.0.5
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    OpenEXR
    GIT_REPOSITORY
        https://github.com/AcademySoftwareFoundation/openexr.git
    GIT_TAG v3.4.13
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
    assimp
    OpenEXR
)
FetchContent_GetProperties(DearImGui)
if(NOT dearimgui_POPULATED)
    FetchContent_Populate(DearImGui)
endif()

# v1.92.8 stores the first secondary viewport's VkFormat address in the
# long-lived backend InitInfo. That viewport can then be destroyed while the
# pointer is retained. Generate a reviewable pinned-source patch that moves
# the format into backend-owned storage. We also keep the renderer contract's
# IMMEDIATE -> MAILBOX -> FIFO preference for secondary windows.
set(
    VCR_IMGUI_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/imgui"
)
file(MAKE_DIRECTORY "${VCR_IMGUI_GENERATED_DIR}")
file(
    READ
    "${dearimgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
    VCR_IMGUI_VULKAN_SOURCE
)
set(
    VCR_IMGUI_PIPELINE_MEMBER_MARKER
    "    VkPipeline                  PipelineForViewports;   // pipeline for secondary viewports (created by backend)"
)
set(
    VCR_IMGUI_FORMAT_POINTER_MARKER
    "            pipeline_info->PipelineRenderingCreateInfo.pColorAttachmentFormats = &wd->SurfaceFormat.format;"
)
set(
    VCR_IMGUI_PRESENT_MODES_MARKER
    "    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };"
)
foreach(
    VCR_IMGUI_PATCH_MARKER IN ITEMS
    VCR_IMGUI_PIPELINE_MEMBER_MARKER
    VCR_IMGUI_FORMAT_POINTER_MARKER
    VCR_IMGUI_PRESENT_MODES_MARKER
)
    string(
        FIND
        "${VCR_IMGUI_VULKAN_SOURCE}"
        "${${VCR_IMGUI_PATCH_MARKER}}"
        VCR_IMGUI_PATCH_MARKER_POSITION
    )
    if(VCR_IMGUI_PATCH_MARKER_POSITION EQUAL -1)
        message(FATAL_ERROR
            "Pinned Dear ImGui Vulkan backend marker "
            "${VCR_IMGUI_PATCH_MARKER} was not found"
        )
    endif()
endforeach()
set(VCR_IMGUI_VULKAN_PATCHED "${VCR_IMGUI_VULKAN_SOURCE}")
string(
    REPLACE
    "${VCR_IMGUI_PIPELINE_MEMBER_MARKER}"
    "    VkPipeline                  PipelineForViewports;   // pipeline for secondary viewports (created by backend)\n    VkFormat                    PipelineForViewportsColorFormat; // VCR: lifetime owned by backend"
    VCR_IMGUI_VULKAN_PATCHED
    "${VCR_IMGUI_VULKAN_PATCHED}"
)
string(
    REPLACE
    "${VCR_IMGUI_FORMAT_POINTER_MARKER}"
    "            bd->PipelineForViewportsColorFormat = wd->SurfaceFormat.format;\n            pipeline_info->PipelineRenderingCreateInfo.pColorAttachmentFormats = &bd->PipelineForViewportsColorFormat;"
    VCR_IMGUI_VULKAN_PATCHED
    "${VCR_IMGUI_VULKAN_PATCHED}"
)
string(
    REPLACE
    "${VCR_IMGUI_PRESENT_MODES_MARKER}"
    "    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR };"
    VCR_IMGUI_VULKAN_PATCHED
    "${VCR_IMGUI_VULKAN_PATCHED}"
)
if(VCR_IMGUI_VULKAN_PATCHED STREQUAL VCR_IMGUI_VULKAN_SOURCE)
    message(FATAL_ERROR
        "Pinned Dear ImGui Vulkan backend patch markers were not found"
    )
endif()
file(
    WRITE
    "${VCR_IMGUI_GENERATED_DIR}/imgui_impl_vulkan_vcr.cpp"
    "${VCR_IMGUI_VULKAN_PATCHED}"
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

add_library(vcr_imgui STATIC
    "${dearimgui_SOURCE_DIR}/imgui.cpp"
    "${dearimgui_SOURCE_DIR}/imgui_draw.cpp"
    "${dearimgui_SOURCE_DIR}/imgui_tables.cpp"
    "${dearimgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${dearimgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    src/imgui_vulkan_backend.cpp
)
target_include_directories(vcr_imgui PUBLIC
    "${dearimgui_SOURCE_DIR}"
    "${dearimgui_SOURCE_DIR}/backends"
    "${volk_SOURCE_DIR}"
    "${vulkanheaders_SOURCE_DIR}/include"
)
target_include_directories(vcr_imgui PRIVATE
    "${VCR_IMGUI_GENERATED_DIR}"
)
target_compile_definitions(vcr_imgui PRIVATE
    VK_NO_PROTOTYPES
    IMGUI_IMPL_VULKAN_USE_VOLK
)
target_link_libraries(vcr_imgui PUBLIC
    SDL3::SDL3-static
    volk::volk
)

add_library(vcr_asset_import STATIC
    src/asset_importer.cpp
)
target_include_directories(vcr_asset_import PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_definitions(vcr_asset_import PRIVATE
    VCR_HAS_ASSIMP=1
)
target_link_libraries(vcr_asset_import PRIVATE
    assimp::assimp
    glm::glm
    nlohmann_json::nlohmann_json
)
if(MSVC)
    target_compile_options(vcr_asset_import PRIVATE
        /W4 /permissive- /Zc:preprocessor /GR- /EHsc
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_asset_import PRIVATE /WX)
    endif()
else()
    target_compile_options(vcr_asset_import PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-rtti
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_asset_import PRIVATE -Werror)
    endif()
endif()

add_library(vcr_cooked_model_asset STATIC
    src/cooked_model_asset.cpp
)
target_include_directories(vcr_cooked_model_asset PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(vcr_cooked_model_asset
    PUBLIC glm::glm
    PRIVATE nlohmann_json::nlohmann_json
)
if(MSVC)
    target_compile_options(vcr_cooked_model_asset PRIVATE
        /W4 /permissive- /Zc:preprocessor /GR- /EHsc
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_cooked_model_asset PRIVATE /WX)
    endif()
else()
    target_compile_options(vcr_cooked_model_asset PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-rtti
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_cooked_model_asset PRIVATE -Werror)
    endif()
endif()

add_library(vcr_cooked_texture_asset STATIC
    src/cooked_texture_asset.cpp
)
target_include_directories(vcr_cooked_texture_asset PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_include_directories(vcr_cooked_texture_asset SYSTEM PRIVATE
    "${assimp_SOURCE_DIR}/contrib/stb"
    "${openexr_SOURCE_DIR}/src/lib/OpenEXR"
    "${openexr_BINARY_DIR}/src/lib/OpenEXR"
    "${openexr_BINARY_DIR}/cmake"
)
target_link_libraries(vcr_cooked_texture_asset PRIVATE
    OpenEXR::OpenEXR
    nlohmann_json::nlohmann_json
)
if(MSVC)
    target_compile_options(vcr_cooked_texture_asset PRIVATE
        /W4 /permissive- /Zc:preprocessor /GR- /EHsc
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_cooked_texture_asset PRIVATE /WX)
    endif()
else()
    target_compile_options(vcr_cooked_texture_asset PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-rtti
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_cooked_texture_asset PRIVATE -Werror)
    endif()
endif()

set(VCR_RENDERER_SOURCES
    src/cli_options.cpp
    src/fly_camera.cpp
    src/fsr3_upscaler.cpp
    src/gpu_buffer.cpp
    src/gpu_texture.cpp
    src/performance_metrics.cpp
    src/png_encoder.cpp
    src/renderer.cpp
    src/runtime_assets.cpp
    src/swapchain.cpp
    src/vma.cpp
    src/vulkan_context.cpp
)

add_executable(vulkan_cube
    src/main.cpp
    src/application.cpp
    src/mcp_server.cpp
    src/scene_document.cpp
    src/scene_io.cpp
    ${VCR_RENDERER_SOURCES}
)

add_executable(vulkan_editor
    src/editor_main.cpp
    src/editor_application.cpp
    src/editor_ui.cpp
    src/editor_ui_kit.cpp
    src/asset_browser.cpp
    src/scene_document.cpp
    src/scene_io.cpp
    ${VCR_RENDERER_SOURCES}
)

find_package(Threads REQUIRED)

function(vcr_configure_executable target editor_enabled)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${vulkanmemoryallocator_SOURCE_DIR}/include"
        "${vulkanheaders_SOURCE_DIR}/include"
    )
    if(VCR_HAS_FSR3)
        target_include_directories(${target} PRIVATE
            "${fidelityfxsdk_SOURCE_DIR}/ffx-api/include"
        )
    endif()

    target_link_libraries(${target} PRIVATE
        SDL3::SDL3-static
        volk::volk
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    target_compile_definitions(${target} PRIVATE
        VK_NO_PROTOTYPES
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_FORCE_RADIANS
        "VCR_EDITOR_ENABLED=${editor_enabled}"
        "VCR_BUILD_TYPE=\"$<CONFIG>\""
        "VCR_VALIDATION_DEFAULT=$<IF:$<OR:$<BOOL:${VCR_ENABLE_VALIDATION}>,$<CONFIG:Debug>>,1,0>"
        "VCR_HAS_FSR3=$<IF:$<BOOL:${VCR_HAS_FSR3}>,1,0>"
    )

    if(MSVC)
        target_compile_options(
            ${target} PRIVATE
            /W4 /permissive- /Zc:preprocessor /GR- /EHsc
        )
        if(VCR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-rtti
            $<$<CONFIG:Release>:-O3>
        )
        if(VCR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    if(VCR_HAS_FSR3)
        add_dependencies(${target} vcr_fidelityfx_runtime)
    endif()
endfunction()

vcr_configure_executable(vulkan_cube 1)
vcr_configure_executable(vulkan_editor 1)
target_link_libraries(vulkan_cube PRIVATE vcr_imgui)
target_link_libraries(
    vulkan_cube PRIVATE
    vcr_cooked_model_asset
    vcr_cooked_texture_asset
)
target_link_libraries(vulkan_editor PRIVATE vcr_imgui)
target_link_libraries(
    vulkan_editor PRIVATE
    vcr_asset_import
    vcr_cooked_model_asset
    vcr_cooked_texture_asset
)

# Both executables share one output directory. A single dependency target
# prevents parallel post-build copy_directory commands from racing over the
# same shader and asset files.
set(VCR_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_custom_target(vcr_runtime_assets
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${VCR_RUNTIME_OUTPUT_DIRECTORY}/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv"
            "${VCR_RUNTIME_OUTPUT_DIRECTORY}/shaders"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${VCR_RUNTIME_OUTPUT_DIRECTORY}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "${VCR_RUNTIME_OUTPUT_DIRECTORY}/assets"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${VCR_RUNTIME_OUTPUT_DIRECTORY}/LICENSES"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES"
            "${VCR_RUNTIME_OUTPUT_DIRECTORY}/LICENSES"
    COMMENT "Copying shared SPIR-V shaders, assets, and licenses"
)
if(VCR_HAS_FSR3)
    add_dependencies(vcr_runtime_assets vcr_fidelityfx_runtime)
    add_custom_command(TARGET vcr_runtime_assets POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${VCR_FFX_RUNTIME_DLL}"
                "${VCR_RUNTIME_OUTPUT_DIRECTORY}/amd_fidelityfx_vk.dll"
        COMMENT "Copying the source-built FidelityFX Vulkan runtime"
    )
endif()
add_dependencies(vulkan_cube vcr_runtime_assets)
add_dependencies(vulkan_editor vcr_runtime_assets)

include(CTest)
if(BUILD_TESTING)
    add_executable(vcr_core_tests
        tests/core_tests.cpp
        src/asset_browser.cpp
        src/cli_options.cpp
        src/fly_camera.cpp
        src/performance_metrics.cpp
        src/png_encoder.cpp
        src/scene_document.cpp
        src/scene_io.cpp
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
        nlohmann_json::nlohmann_json
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

    add_executable(vcr_asset_import_tests
        tests/asset_import_tests.cpp
    )
    target_link_libraries(
        vcr_asset_import_tests PRIVATE
        vcr_asset_import
        nlohmann_json::nlohmann_json
    )
    if(MSVC)
        target_compile_options(
            vcr_asset_import_tests PRIVATE /W4 /permissive- /EHsc
        )
    else()
        target_compile_options(vcr_asset_import_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
    endif()
    add_test(
        NAME vcr_asset_import_tests
        COMMAND vcr_asset_import_tests
    )
    if(UNIX)
        # WSL can inherit Windows TEMP paths on DrvFs, whose renameat2
        # implementation does not support RENAME_NOREPLACE. Exercise the
        # Linux atomic publication path on the native temporary filesystem.
        set_tests_properties(vcr_asset_import_tests PROPERTIES
            ENVIRONMENT "TMPDIR=/tmp;TMP=/tmp;TEMP=/tmp"
        )
    endif()

    add_executable(vcr_cooked_model_asset_tests
        tests/cooked_model_asset_tests.cpp
    )
    target_link_libraries(
        vcr_cooked_model_asset_tests PRIVATE
        vcr_cooked_model_asset
        nlohmann_json::nlohmann_json
    )
    if(MSVC)
        target_compile_options(
            vcr_cooked_model_asset_tests PRIVATE
            /W4 /permissive- /EHsc
        )
    else()
        target_compile_options(
            vcr_cooked_model_asset_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
    endif()
    add_test(
        NAME vcr_cooked_model_asset_tests
        COMMAND vcr_cooked_model_asset_tests
    )
    if(UNIX)
        set_tests_properties(vcr_cooked_model_asset_tests PROPERTIES
            ENVIRONMENT "TMPDIR=/tmp;TMP=/tmp;TEMP=/tmp"
        )
    endif()

    add_executable(vcr_cooked_texture_asset_tests
        tests/cooked_texture_asset_tests.cpp
        src/png_encoder.cpp
    )
    target_include_directories(vcr_cooked_texture_asset_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_include_directories(
        vcr_cooked_texture_asset_tests SYSTEM PRIVATE
        "${openexr_SOURCE_DIR}/src/lib/OpenEXR"
        "${openexr_BINARY_DIR}/src/lib/OpenEXR"
        "${openexr_BINARY_DIR}/cmake"
    )
    target_link_libraries(
        vcr_cooked_texture_asset_tests PRIVATE
        vcr_cooked_texture_asset
        OpenEXR::OpenEXR
        nlohmann_json::nlohmann_json
    )
    if(MSVC)
        target_compile_options(
            vcr_cooked_texture_asset_tests PRIVATE
            /W4 /permissive- /EHsc
        )
    else()
        target_compile_options(
            vcr_cooked_texture_asset_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
    endif()
    add_test(
        NAME vcr_cooked_texture_asset_tests
        COMMAND vcr_cooked_texture_asset_tests
    )
    if(UNIX)
        set_tests_properties(vcr_cooked_texture_asset_tests PROPERTIES
            ENVIRONMENT "TMPDIR=/tmp;TMP=/tmp;TEMP=/tmp"
        )
    endif()
endif()
