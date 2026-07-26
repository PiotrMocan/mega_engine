# FidelityFX SDK v1.1.4 creates the FSR3 luma-history resources as RGBA16F,
# but its Vulkan GLSL callback declares the corresponding storage image as
# rgba8. Newer SDK source keeps the RGBA16F resource, so the GLSL qualifier is
# the erroneous side of the mismatch. Vulkan validation correctly reports
# undefined image load/store values unless the qualifier is fixed.

if(NOT DEFINED FFX_SDK_SOURCE_DIR)
    message(FATAL_ERROR "FFX_SDK_SOURCE_DIR was not provided")
endif()

set(
    callback_file
    "${FFX_SDK_SOURCE_DIR}/sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h"
)
if(NOT EXISTS "${callback_file}")
    message(FATAL_ERROR "FidelityFX FSR3 GLSL callback was not found: ${callback_file}")
endif()

file(READ "${callback_file}" callback_source)
set(
    incorrect_declaration
    "layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8) uniform image2D  rw_luma_history;"
)
set(
    corrected_declaration
    "layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba16f) uniform image2D  rw_luma_history;"
)

string(FIND "${callback_source}" "${corrected_declaration}" corrected_position)
if(NOT corrected_position EQUAL -1)
    message(STATUS "FidelityFX FSR3 Vulkan luma-history declaration is already corrected")
    return()
endif()

string(FIND "${callback_source}" "${incorrect_declaration}" incorrect_position)
if(incorrect_position EQUAL -1)
    message(FATAL_ERROR "Unexpected FidelityFX FSR3 luma-history declaration; refusing an unsafe patch")
endif()

string(
    REPLACE
    "${incorrect_declaration}"
    "${corrected_declaration}"
    patched_source
    "${callback_source}"
)
file(WRITE "${callback_file}" "${patched_source}")
message(STATUS "Corrected FidelityFX FSR3 Vulkan luma-history storage format to rgba16f")
