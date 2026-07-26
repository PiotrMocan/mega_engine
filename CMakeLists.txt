cmake_minimum_required(VERSION 3.22)

project(vulkan_cube_renderer VERSION 0.2.0 LANGUAGES C CXX)

include(FetchContent)
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
    src/gpu_buffer.cpp
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
    COMMENT "Copying precompiled SPIR-V shaders"
)

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
