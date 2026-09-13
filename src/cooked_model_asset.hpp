#pragma once

#include "pbr_material.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t cooked_model_schema_version = 1U;
inline constexpr std::uint32_t cooked_mesh_binary_version = 1U;
inline constexpr std::uint32_t cooked_mesh_vertex_stride = 48U;

struct CookedModelVertex {
    std::array<float, 3U> position{};
    std::array<float, 3U> normal{};
    std::array<float, 2U> texcoord{};
    std::array<float, 4U> tangent{};
};

static_assert(sizeof(CookedModelVertex) == cooked_mesh_vertex_stride);

struct CookedModelSubmesh {
    std::uint64_t first_index = 0U;
    std::uint64_t index_count = 0U;
    std::uint32_t material_index = 0U;
    std::string name{};
    // Runtime-prepared index ranges; the versioned cooked file stays LOD0.
    std::array<std::uint32_t, 3> lod_first{};
    std::array<std::uint32_t, 3> lod_count{};
    std::array<float, 3> lod_error{};
};

struct MaterialTessellation {
    // Model-local displacement = scale * (height - center).
    float displacement_scale = 0.05F;
    float displacement_center = 0.5F;
    float target_edge_pixels = 24.0F;
    float maximum_factor = 8.0F;
    // World-space Y threshold used to keep displacement off vertical faces.
    float normal_threshold = 0.55F;
    // Normalized UV distance over which displacement fades to zero at a mesh edge.
    float uv_edge_fade = 0.0F;
    std::uint32_t enabled_flag = 0U;
    std::uint32_t up_facing_only = 0U;

    [[nodiscard]] bool enabled() const noexcept {
        return enabled_flag != 0U &&
            displacement_scale > 0.0F && maximum_factor > 1.0F;
    }
};

static_assert(sizeof(MaterialTessellation) == 32U);

struct CookedModelMaterial {
    std::filesystem::path relative_path{};
    std::string name{};
    PbrMaterial factors{};
    std::optional<std::filesystem::path> base_color_texture{};
    std::optional<std::filesystem::path> normal_texture{};
    // Linear glTF packing: G roughness, B metallic; multiplied by factors.
    std::optional<std::filesystem::path> metallic_roughness_texture{};
    std::optional<std::filesystem::path> displacement_texture{};
    MaterialTessellation tessellation{};
};

struct CookedModelLodReference {
    // Relative to asset_root, unlike mesh/material references within a model.
    std::filesystem::path model{};
    // Conservative deviation from LOD0 in model-local units, including removals.
    float geometric_error = 0;
};

struct CookedModelAsset {
    std::filesystem::path metadata_relative_path{};
    std::filesystem::path mesh_relative_path{};
    std::vector<CookedModelVertex> vertices{};
    std::vector<std::uint32_t> indices{};
    std::vector<CookedModelSubmesh> submeshes{};
    std::vector<CookedModelMaterial> materials{};
    std::vector<CookedModelLodReference> lods{};
};

enum class CookedModelLoadErrorCode : std::uint8_t {
    invalid_asset_root,
    invalid_path,
    symbolic_link,
    file_not_found,
    file_too_large,
    io_error,
    invalid_json,
    unsupported_schema,
    limit_exceeded,
    invalid_mesh,
    invalid_material,
};

struct CookedModelLoadError {
    CookedModelLoadErrorCode code =
        CookedModelLoadErrorCode::io_error;
    std::string message{};
    std::filesystem::path path{};
};

class CookedModelLoadResult final {
public:
    explicit CookedModelLoadResult(CookedModelAsset asset)
        : asset_(std::move(asset)) {}
    explicit CookedModelLoadResult(CookedModelLoadError error)
        : error_(std::move(error)) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return asset_.has_value();
    }
    [[nodiscard]] CookedModelAsset& value() & {
        return *asset_;
    }
    [[nodiscard]] const CookedModelAsset& value() const& {
        return *asset_;
    }
    [[nodiscard]] CookedModelAsset&& value() && {
        return std::move(*asset_);
    }
    [[nodiscard]] CookedModelAsset* operator->() noexcept {
        return &value();
    }
    [[nodiscard]] const CookedModelAsset* operator->() const noexcept {
        return &value();
    }
    [[nodiscard]] CookedModelAsset& operator*() & {
        return value();
    }
    [[nodiscard]] const CookedModelAsset& operator*() const& {
        return value();
    }
    [[nodiscard]] const CookedModelLoadError& error() const& {
        return error_;
    }

private:
    std::optional<CookedModelAsset> asset_{};
    CookedModelLoadError error_{};
};

struct CookedModelLoadOptions {
    std::uintmax_t maximum_model_metadata_bytes =
        8ULL * 1024ULL * 1024ULL;
    std::uintmax_t maximum_material_metadata_bytes =
        1ULL * 1024ULL * 1024ULL;
    std::uintmax_t maximum_total_material_metadata_bytes =
        64ULL * 1024ULL * 1024ULL;
    std::uintmax_t maximum_mesh_file_bytes =
        2ULL * 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t maximum_vertices = 10'000'000ULL;
    std::uint64_t maximum_indices = 30'000'000ULL;
    std::uint32_t maximum_submeshes = 100'000U;
    std::uint32_t maximum_materials = 100'000U;
    std::size_t maximum_name_bytes = 255U;
    std::size_t maximum_reference_bytes = 4096U;
};

// Loads editor-cooked data into owned, contiguous CPU arrays. Every path in
// the model and material metadata is resolved beneath asset_root without
// following symbolic links.
[[nodiscard]] CookedModelLoadResult load_cooked_model_asset(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& model_metadata_relative_path,
    const CookedModelLoadOptions& options = {});

} // namespace vcr
