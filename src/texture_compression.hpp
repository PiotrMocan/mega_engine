#pragma once

#include "cooked_texture_asset.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace vcr {

inline constexpr std::size_t vcrtex_header_bytes = 40U;

/// Selects the GPU block-compressed format appropriate for a shipped texture's semantic.
[[nodiscard]] CookedTexturePixelFormat select_shipped_texture_format(
    CookedTextureSemantic semantic) noexcept;

/// Computes full mip count for a 2D texture with the given dimensions.
[[nodiscard]] std::uint32_t compute_full_mip_levels(
    std::uint32_t width,
    std::uint32_t height) noexcept;

/// Computes block-compressed mip byte offsets and total byte size with overflow checks.
[[nodiscard]] bool compute_bc_mip_layout(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t mip_levels,
    CookedTexturePixelFormat format,
    std::vector<std::size_t>& out_offsets,
    std::uint64_t& out_total_bytes);

// Visits each lower mip synchronously in descending size. Input is validated, tightly
// packed RGBA8. The texel span is valid only for the duration of the callback.
using TextureMipSink = std::function<void(std::uint32_t level, std::uint32_t width,
    std::uint32_t height, std::span<const std::uint8_t> texels)>;
void visit_filtered_texture_mips(const CookedTextureAsset& base, float alpha_cutoff,
    const TextureMipSink& write_mip);

/// Generates mipmaps and encodes an RGBA8 texture into a `.vcrtex` BC7/BC4 container.
[[nodiscard]] bool cook_texture_to_vcrtex(
    const CookedTextureAsset& rgba8_mip0,
    float alpha_cutoff,
    CookedTexturePixelFormat target_format,
    std::vector<std::uint8_t>& out_vcrtex_bytes,
    std::uint32_t& out_mip_levels,
    std::string& out_error);

[[nodiscard]] bool decode_ktx2_bytes(std::span<const std::uint8_t> bytes,
    const CookedTextureLoadOptions& options, CookedTextureAsset& asset, std::string& error);

/// Validates and parses a `.vcrtex` container into `asset` (setting dimensions,
/// mip offsets, average_linear, and compressed block payload).
[[nodiscard]] bool decode_vcrtex_bytes(
    std::span<const std::uint8_t> file_bytes,
    CookedTexturePixelFormat expected_format,
    const CookedTextureLoadOptions& options,
    CookedTextureAsset& asset,
    std::string& out_error);

/// Decodes mip 0 of a block-compressed `CookedTextureAsset` into tightly packed RGBA8 pixels.
[[nodiscard]] bool decode_bc_mip0_to_rgba8(
    const CookedTextureAsset& texture,
    std::vector<std::uint8_t>& out_rgba8,
    std::string& out_error);

} // namespace vcr
