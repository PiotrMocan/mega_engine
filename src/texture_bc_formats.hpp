#pragma once
#include "cooked_texture_asset.hpp"
#include <vulkan/vulkan_core.h>

namespace vcr {
struct BcFormatEntry {
    CookedTexturePixelFormat format;
    VkFormat linear, srgb;
};
using BcPixel = CookedTexturePixelFormat;
inline constexpr BcFormatEntry bc_formats[]{
    {BcPixel::bc1_rgb, VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK},
    {BcPixel::bc1_rgba, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
    {BcPixel::bc2, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK},
    {BcPixel::bc3, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK},
    {BcPixel::bc4_unorm, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {BcPixel::bc4_snorm, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {BcPixel::bc5, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {BcPixel::bc5_snorm, VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_UNDEFINED},
    {BcPixel::bc6h_ufloat, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_UNDEFINED},
    {BcPixel::bc6h_sfloat, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_UNDEFINED},
    {BcPixel::bc7_unorm, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK},
};
inline VkFormat bc_vk_format(BcPixel pixel, CookedTextureColorSpace space) noexcept {
    for (const auto& entry : bc_formats) {
        if (entry.format == pixel)
            return space == CookedTextureColorSpace::srgb ? entry.srgb : entry.linear;
    }
    return VK_FORMAT_UNDEFINED;
}
} // namespace vcr
