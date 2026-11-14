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

function(vcr_apply_cxx_warning_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:preprocessor /GR- /EHsc
        )
        if(VCR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-rtti
        )
        if(VCR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(MSVC AND CMAKE_CXX_FLAGS MATCHES "/fsanitize=address")
    add_link_options(/INCREMENTAL:NO)
endif()

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
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_COLLADA_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_PLY_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_STL_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_3DS_IMPORTER ON CACHE BOOL "" FORCE)
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
    ImGuizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG 1.10
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
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG 6daea4695c48338363b08022d2fb15deaef6ac09
)
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
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
    meshoptimizer
)
FetchContent_GetProperties(DearImGui)
if(NOT dearimgui_POPULATED)
    FetchContent_Populate(DearImGui)
endif()
FetchContent_GetProperties(ImGuizmo)
if(NOT imguizmo_POPULATED)
    FetchContent_Populate(ImGuizmo)
endif()
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
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
    "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp"
    src/imgui_vulkan_backend.cpp
)
target_include_directories(vcr_imgui PUBLIC
    "${dearimgui_SOURCE_DIR}"
    "${dearimgui_SOURCE_DIR}/backends"
    "${imguizmo_SOURCE_DIR}/src"
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

find_package(Threads REQUIRED)

set(VCR_ENGINE_CORE_SOURCES
    src/miniaudio_impl.c
    src/cooked_audio_asset.cpp
    src/audio_engine.cpp
    src/sound_cue_io.cpp
    src/cooked_model_asset.cpp
    src/skeletal_animation.cpp
    src/cooked_texture_asset.cpp
    src/png_encoder.cpp
    src/icon_system.cpp
    src/model_lod.cpp
    src/asset_preparation.cpp
    src/content_manifest.cpp
    src/scene_uuid.cpp
    src/scene_document.cpp
    src/scene_io.cpp
    src/prefab_io.cpp
    src/punctual_light.cpp
    src/lighting.cpp
    src/scene_visibility.cpp
    src/bounds_hierarchy.cpp
    src/cinematic_camera.cpp
    src/cutscene_sequencer.cpp
    src/runtime_scene.cpp
    src/collision.cpp
    src/physics_world.cpp
    src/runtime_transform_registry.cpp
    src/fly_camera.cpp
    src/input_actions.cpp
    src/animated_character.cpp
    src/gameplay.cpp
    src/gameplay_framework.cpp
    src/gameplay_session.cpp
    src/game_overlay.cpp
    src/game_ui_runtime.cpp
    src/localization.cpp
    src/game_module_loader.cpp
    src/cli_options.cpp
    src/performance_metrics.cpp
    src/project_system.cpp
    src/project_shipping.cpp
    src/runtime_assets.cpp
    src/asset_browser.cpp
    src/editor_scene_tools.cpp
    src/render_fixture_scene.cpp
)

add_library(vcr_engine SHARED ${VCR_ENGINE_CORE_SOURCES})
add_library(vcr::engine ALIAS vcr_engine)
set_source_files_properties(src/miniaudio_impl.c PROPERTIES
    COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>"
)
set_target_properties(vcr_engine PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    OUTPUT_NAME "vcr_engine"
    DEBUG_POSTFIX "d"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
target_include_directories(vcr_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_include_directories(vcr_engine SYSTEM PRIVATE
    "${miniaudio_SOURCE_DIR}"
    "${assimp_SOURCE_DIR}/contrib/stb"
    "${openexr_SOURCE_DIR}/src/lib/OpenEXR"
    "${openexr_BINARY_DIR}/src/lib/OpenEXR"
    "${openexr_BINARY_DIR}/cmake"
)
target_compile_definitions(vcr_engine
    PUBLIC
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_FORCE_RADIANS
    PRIVATE
        "VCR_VALIDATION_DEFAULT=$<IF:$<OR:$<BOOL:${VCR_ENABLE_VALIDATION}>,$<CONFIG:Debug>>,1,0>"
)
target_link_libraries(vcr_engine
    PUBLIC
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
    PRIVATE
        OpenEXR::OpenEXR
        meshoptimizer
)
if(WIN32)
    target_link_libraries(vcr_engine PRIVATE shell32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(vcr_engine PRIVATE m dl)
endif()
if(MSVC)
    target_compile_options(vcr_engine PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/Zc:preprocessor;/GR-;/EHsc>
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_engine PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/WX>
        )
    endif()
else()
    target_compile_options(vcr_engine PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-fno-rtti>
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_engine PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Werror>
        )
    endif()
endif()

add_library(vcr_window_icon STATIC
    src/window_icon_sdl.cpp
)
target_link_libraries(vcr_window_icon PUBLIC
    vcr_engine
    SDL3::SDL3-static
)
if(MSVC)
    target_compile_options(vcr_window_icon PRIVATE
        /W4 /permissive- /Zc:preprocessor /GR- /EHsc
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_window_icon PRIVATE /WX)
    endif()
else()
    target_compile_options(vcr_window_icon PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-rtti
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_window_icon PRIVATE -Werror)
    endif()
endif()

add_library(vcr_asset_import STATIC
    src/asset_importer.cpp
)
target_include_directories(vcr_asset_import PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_definitions(vcr_asset_import PRIVATE
    VCR_HAS_ASSIMP=1
)
target_link_libraries(vcr_asset_import
    PUBLIC
        vcr_engine
    PRIVATE
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

# Explicit offline asset step. LOD meshes share materials and are compacted;
# ordinary scene loading reads the prepared four-level chain without simplifying.
add_executable(vcr_asset_lod_cooker EXCLUDE_FROM_ALL
    tools/asset_lod_cooker.cpp
)
target_link_libraries(vcr_asset_lod_cooker PRIVATE
    vcr_engine meshoptimizer nlohmann_json::nlohmann_json)
set_target_properties(vcr_asset_lod_cooker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
vcr_apply_cxx_warning_flags(vcr_asset_lod_cooker)

add_executable(vcr_content_cooker
    tools/vcr_content_cooker.cpp
)
target_link_libraries(vcr_content_cooker PRIVATE
    vcr_engine
    vcr_asset_import
    glm::glm
    nlohmann_json::nlohmann_json
    Threads::Threads
)
set_target_properties(vcr_content_cooker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(MSVC)
    target_compile_options(vcr_content_cooker PRIVATE
        /W4 /permissive- /Zc:preprocessor /GR- /EHsc
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_content_cooker PRIVATE /WX)
    endif()
else()
    target_compile_options(vcr_content_cooker PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-rtti
    )
    if(VCR_WARNINGS_AS_ERRORS)
        target_compile_options(vcr_content_cooker PRIVATE -Werror)
    endif()
endif()

set(VCR_VULKAN_RENDERER_SOURCES
    src/fsr3_upscaler.cpp
    src/gpu_buffer.cpp
    src/gpu_texture.cpp
    src/renderer.cpp
    src/renderer_scene.cpp
    src/renderer_lighting.cpp
    src/renderer_ray_tracing.cpp
    src/renderer_gpu_visibility.cpp
    src/upload_batch.cpp
    src/renderer_assets.cpp
    src/swapchain.cpp
    src/vma.cpp
    src/vulkan_context.cpp
)

add_executable(vulkan_cube
    src/main.cpp
    src/application.cpp
    src/mcp_server.cpp
    ${VCR_VULKAN_RENDERER_SOURCES}
)

add_executable(vulkan_editor
    src/editor_main.cpp
    src/editor_application.cpp
    src/editor_ui.cpp
    src/editor_ui_kit.cpp
    src/mcp_server.cpp
    ${VCR_VULKAN_RENDERER_SOURCES}
)

add_executable(vulkan_project_manager
    src/project_manager_main.cpp
    src/project_manager_application.cpp
    src/editor_ui_kit.cpp
    src/vulkan_context.cpp
    src/swapchain.cpp
    src/vma.cpp
    src/gpu_buffer.cpp
    src/gpu_texture.cpp
    src/ui_texture_preview.cpp
)

add_executable(vulkan_icon_tool
    src/icon_tool_main.cpp
    src/icon_tool_application.cpp
    src/editor_ui_kit.cpp
    src/vulkan_context.cpp
    src/swapchain.cpp
    src/vma.cpp
    src/gpu_buffer.cpp
    src/gpu_texture.cpp
    src/ui_texture_preview.cpp
)

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
        vcr_engine
        SDL3::SDL3-static
        volk::volk
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
        meshoptimizer
    )
    if(WIN32)
        target_link_libraries(${target} PRIVATE shell32)
    endif()
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
        # Dense-scene editor initialization plus Vulkan loader/validation calls
        # can exhaust the default 1 MiB main-thread stack. Reserve virtual space;
        # Windows commits stack pages only as they are touched.
        target_link_options(${target} PRIVATE /STACK:8388608)
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
vcr_configure_executable(vulkan_project_manager 1)
vcr_configure_executable(vulkan_icon_tool 1)
if(WIN32)
    target_sources(vulkan_cube PRIVATE resources/vulkan_cube.rc)
    target_sources(vulkan_editor PRIVATE resources/vulkan_editor.rc)
    target_sources(vulkan_project_manager PRIVATE resources/vulkan_project_manager.rc)
    target_sources(vulkan_icon_tool PRIVATE resources/vulkan_icon_tool.rc)
endif()

# Explicit GPU numerical gate; ordinary CPU CTest does not silently require a GPU.
add_executable(vcr_tone_mapping_gpu_tests EXCLUDE_FROM_ALL
    tests/tone_mapping_gpu_tests.cpp src/vulkan_context.cpp src/vma.cpp src/gpu_buffer.cpp)
vcr_configure_executable(vcr_tone_mapping_gpu_tests 0)
target_compile_definitions(vcr_tone_mapping_gpu_tests PRIVATE
    "VCR_TONE_MAPPING_TEST_SPV=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv/tone_mapping_reference.comp.spv\"")
# Explicit exposure histogram/adaptation numeric gate; ordinary CTest stays CPU-only.
add_executable(vcr_exposure_gpu_tests EXCLUDE_FROM_ALL
    tests/exposure_gpu_tests.cpp src/vulkan_context.cpp src/vma.cpp src/gpu_buffer.cpp)
vcr_configure_executable(vcr_exposure_gpu_tests 0)
target_compile_definitions(vcr_exposure_gpu_tests PRIVATE
    "VCR_EXPOSURE_TEST_SPV=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv/exposure_reference.comp.spv\"")
# Explicit material numeric/ABI gate; never part of the ordinary CPU CTest run.
add_executable(vcr_material_gpu_tests EXCLUDE_FROM_ALL
    tests/material_gpu_tests.cpp src/vulkan_context.cpp src/vma.cpp src/gpu_buffer.cpp)
vcr_configure_executable(vcr_material_gpu_tests 0)
target_compile_definitions(vcr_material_gpu_tests PRIVATE
    "VCR_MATERIAL_TEST_SPV=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv/material_reference.comp.spv\"")
add_executable(vcr_lod_fade_gpu_tests EXCLUDE_FROM_ALL
    tests/lod_fade_gpu_tests.cpp src/vulkan_context.cpp src/vma.cpp src/gpu_buffer.cpp)
vcr_configure_executable(vcr_lod_fade_gpu_tests 0)
target_compile_definitions(vcr_lod_fade_gpu_tests PRIVATE
    "VCR_LOD_FADE_TEST_SPV=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv/lod_fade_reference.comp.spv\"")
# Explicit 3D LUT interpolation gate; ordinary CPU CTest never requires a GPU.
add_executable(vcr_color_grading_gpu_tests EXCLUDE_FROM_ALL
    tests/color_grading_gpu_tests.cpp src/vulkan_context.cpp src/vma.cpp
    src/gpu_buffer.cpp src/gpu_texture.cpp)
vcr_configure_executable(vcr_color_grading_gpu_tests 0)
target_compile_definitions(vcr_color_grading_gpu_tests PRIVATE
    "VCR_COLOR_GRADING_TEST_SPV=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv/color_grading_reference.comp.spv\"")
if(WIN32)
    add_custom_target(vcr_clang_tidy
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/analyze_windows.ps1"
            -Tool ClangTidy -BuildTree "${CMAKE_BINARY_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL VERBATIM)
    add_custom_target(vcr_msvc_analyze
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/analyze_windows.ps1"
            -Tool MSVC -BuildTree "${CMAKE_BINARY_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL VERBATIM)
endif()
target_link_libraries(
    vulkan_cube PRIVATE
    vcr_imgui
    vcr_window_icon
)
target_link_libraries(
    vulkan_editor PRIVATE
    vcr_imgui
    vcr_asset_import
    vcr_window_icon
)
target_link_libraries(
    vulkan_project_manager PRIVATE
    vcr_imgui
    vcr_window_icon
)
target_link_libraries(
    vulkan_icon_tool PRIVATE
    vcr_imgui
    vcr_window_icon
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
add_dependencies(vulkan_project_manager vcr_runtime_assets)
add_dependencies(vulkan_icon_tool vcr_runtime_assets)

include(CTest)
if(BUILD_TESTING)
    add_executable(vcr_gameplay_tests tests/gameplay_tests.cpp)
    target_link_libraries(vcr_gameplay_tests PRIVATE vcr_engine glm::glm)
    set_target_properties(vcr_gameplay_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_gameplay_tests)
    add_test(NAME vcr_gameplay_tests COMMAND vcr_gameplay_tests)

    add_executable(vcr_audio_input_tests tests/audio_input_tests.cpp)
    target_link_libraries(vcr_audio_input_tests PRIVATE
        vcr_engine
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(vcr_audio_input_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_audio_input_tests)
    add_test(NAME vcr_audio_input_tests COMMAND vcr_audio_input_tests)

    add_executable(vcr_fog_tests tests/fog_tests.cpp)
    target_include_directories(vcr_fog_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties(vcr_fog_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_fog_tests)
    add_test(NAME vcr_fog_tests COMMAND vcr_fog_tests)

    add_executable(vcr_packed_model_vertex_tests tests/packed_model_vertex_tests.cpp)
    target_include_directories(vcr_packed_model_vertex_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(vcr_packed_model_vertex_tests PRIVATE glm::glm)
    set_target_properties(vcr_packed_model_vertex_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_packed_model_vertex_tests)
    add_test(NAME vcr_packed_model_vertex_tests COMMAND vcr_packed_model_vertex_tests)

    add_executable(vcr_gpu_grass_tests tests/gpu_grass_tests.cpp)
    target_include_directories(vcr_gpu_grass_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(vcr_gpu_grass_tests PRIVATE glm::glm)
    set_target_properties(vcr_gpu_grass_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_gpu_grass_tests)
    add_test(NAME vcr_gpu_grass_tests COMMAND vcr_gpu_grass_tests)

    add_executable(vcr_exposure_tests tests/exposure_tests.cpp)
    target_include_directories(vcr_exposure_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties(vcr_exposure_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_exposure_tests)
    add_test(NAME vcr_exposure_tests COMMAND vcr_exposure_tests)

    add_executable(vcr_lod_transition_tests tests/lod_transition_tests.cpp)
    target_include_directories(vcr_lod_transition_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties(vcr_lod_transition_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_lod_transition_tests)
    add_test(NAME vcr_lod_transition_tests COMMAND vcr_lod_transition_tests)

    add_executable(vcr_lighting_tests tests/lighting_tests.cpp)
    target_link_libraries(vcr_lighting_tests PRIVATE vcr_engine glm::glm)
    set_target_properties(vcr_lighting_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_lighting_tests)
    add_test(NAME vcr_lighting_tests COMMAND vcr_lighting_tests)

    add_executable(vcr_runtime_tests tests/runtime_scene_tests.cpp)
    target_link_libraries(vcr_runtime_tests PRIVATE vcr_engine glm::glm meshoptimizer Threads::Threads)
    set_target_properties(vcr_runtime_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_runtime_tests)
    add_test(NAME vcr_runtime_tests COMMAND vcr_runtime_tests)

    add_executable(vcr_core_tests tests/core_tests.cpp)
    target_compile_definitions(vcr_core_tests PRIVATE VCR_VALIDATION_DEFAULT=0)
    target_link_libraries(
        vcr_core_tests PRIVATE
        vcr_engine
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(vcr_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_core_tests)
    add_test(NAME vcr_core_tests COMMAND vcr_core_tests)

    add_executable(vcr_asset_import_tests
        tests/asset_import_tests.cpp
    )
    target_link_libraries(
        vcr_asset_import_tests PRIVATE
        vcr_engine
        vcr_asset_import
        nlohmann_json::nlohmann_json
    )
    set_target_properties(vcr_asset_import_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_asset_import_tests)
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
        vcr_engine
        nlohmann_json::nlohmann_json
    )
    set_target_properties(vcr_cooked_model_asset_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_cooked_model_asset_tests)
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
    )
    target_include_directories(
        vcr_cooked_texture_asset_tests SYSTEM PRIVATE
        "${openexr_SOURCE_DIR}/src/lib/OpenEXR"
        "${openexr_BINARY_DIR}/src/lib/OpenEXR"
        "${openexr_BINARY_DIR}/cmake"
    )
    target_link_libraries(
        vcr_cooked_texture_asset_tests PRIVATE
        vcr_engine
        OpenEXR::OpenEXR
        nlohmann_json::nlohmann_json
    )
    set_target_properties(vcr_cooked_texture_asset_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_cooked_texture_asset_tests)
    add_test(
        NAME vcr_cooked_texture_asset_tests
        COMMAND vcr_cooked_texture_asset_tests
    )
    if(UNIX)
        set_tests_properties(vcr_cooked_texture_asset_tests PROPERTIES
            ENVIRONMENT "TMPDIR=/tmp;TMP=/tmp;TEMP=/tmp"
        )
    endif()

    add_executable(vcr_project_tests tests/project_tests.cpp)
    target_compile_definitions(vcr_project_tests PRIVATE VCR_VALIDATION_DEFAULT=0)
    target_link_libraries(
        vcr_project_tests PRIVATE
        vcr_engine
        vcr_window_icon
        SDL3::SDL3-static
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(vcr_project_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    if(WIN32)
        target_link_libraries(vcr_project_tests PRIVATE shell32)
    endif()
    vcr_apply_cxx_warning_flags(vcr_project_tests)
    add_test(NAME vcr_project_tests COMMAND vcr_project_tests)

    add_executable(vcr_animation_gameplay_tests tests/animation_gameplay_tests.cpp)
    target_link_libraries(vcr_animation_gameplay_tests PRIVATE
        vcr_engine
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(vcr_animation_gameplay_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_animation_gameplay_tests)
    add_test(NAME vcr_animation_gameplay_tests COMMAND vcr_animation_gameplay_tests)

    add_executable(vcr_sequencer_camera_tests tests/sequencer_camera_tests.cpp)
    target_link_libraries(vcr_sequencer_camera_tests PRIVATE
        vcr_engine
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(vcr_sequencer_camera_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    vcr_apply_cxx_warning_flags(vcr_sequencer_camera_tests)
    add_test(NAME vcr_sequencer_camera_tests COMMAND vcr_sequencer_camera_tests)

    add_executable(vcr_engine_gameplay_api_tests tests/engine_gameplay_api_tests.cpp)
    target_compile_definitions(vcr_engine_gameplay_api_tests PRIVATE
        VCR_EDITOR_ENABLED=0
        "VCR_CONTENT_COOKER_BIN=\"$<TARGET_FILE:vcr_content_cooker>\""
    )
    target_link_libraries(vcr_engine_gameplay_api_tests PRIVATE
        vcr_engine
        vcr_asset_import
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(vcr_engine_gameplay_api_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_dependencies(vcr_engine_gameplay_api_tests vcr_content_cooker)
    vcr_apply_cxx_warning_flags(vcr_engine_gameplay_api_tests)
    add_test(NAME vcr_engine_gameplay_api_tests COMMAND vcr_engine_gameplay_api_tests)

    add_executable(vcr_renderer_skinning_tests
        tests/renderer_skinning_tests.cpp
        ${VCR_VULKAN_RENDERER_SOURCES}
    )
    vcr_configure_executable(vcr_renderer_skinning_tests 1)
    target_link_libraries(vcr_renderer_skinning_tests PRIVATE
        vcr_imgui
        vcr_window_icon
    )
    add_dependencies(vcr_renderer_skinning_tests vcr_runtime_assets)
    add_test(NAME vcr_renderer_skinning_tests COMMAND vcr_renderer_skinning_tests)

    add_executable(vcr_game_ui_localization_tests
        tests/game_ui_localization_tests.cpp
    )
    target_compile_definitions(vcr_game_ui_localization_tests PRIVATE
        VCR_VALIDATION_DEFAULT=0
    )
    target_link_libraries(
        vcr_game_ui_localization_tests PRIVATE
        vcr_engine
        vcr_window_icon
        SDL3::SDL3-static
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(vcr_game_ui_localization_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    if(WIN32)
        target_link_libraries(vcr_game_ui_localization_tests PRIVATE shell32)
    endif()
    add_dependencies(vcr_game_ui_localization_tests vulkan_cube vcr_runtime_assets)
    vcr_apply_cxx_warning_flags(vcr_game_ui_localization_tests)
    add_test(NAME vcr_game_ui_localization_tests COMMAND vcr_game_ui_localization_tests)
endif()
