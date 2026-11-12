#include "gpu_grass.hpp"
#include "pbr_material.hpp"
#include "subsurface.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Checks {
    unsigned failures = 0U;

    void expect(const bool condition, const char* const message) noexcept {
        if (!condition) {
            ++failures;
            std::fprintf(stderr, "FAILED: %s\n", message);
        }
    }
};

[[nodiscard]] bool validate_range(
    const std::span<const vcr::CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const std::uint64_t first_index,
    const std::uint64_t index_count,
    std::string& error) {
    error.clear();
    return vcr::validate_gpu_grass_index_range(
        vertices, indices, first_index, index_count, error);
}

[[nodiscard]] bool validate_submesh(
    const std::span<const vcr::CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const vcr::CookedModelSubmesh& submesh,
    std::string& error) {
    error.clear();
    return vcr::validate_gpu_grass_submesh(vertices, indices, submesh, error);
}

[[nodiscard]] vcr::CookedModelVertex make_reference_vertex(
    const std::uint32_t blade_index,
    const vcr::GpuGrassCorner corner,
    const float height_t,
    const float wind_seconds = 0.0F) noexcept {
    const float side_01 = static_cast<float>(static_cast<std::uint32_t>(corner)) * 0.5F;
    const vcr::GpuGrassBladeSample sample =
        vcr::evaluate_gpu_grass_blade(blade_index, side_01, height_t, wind_seconds);
    return vcr::CookedModelVertex{
        .position = {sample.position.x, sample.position.y, sample.position.z},
        .normal = {sample.normal.x, sample.normal.y, sample.normal.z},
        .texcoord = vcr::encode_gpu_grass_uv(blade_index, corner, height_t),
        .tangent = {sample.tangent.x, sample.tangent.y, sample.tangent.z, sample.tangent.w},
    };
}

void append_blade_strip(
    std::vector<vcr::CookedModelVertex>& vertices,
    std::vector<std::uint32_t>& indices,
    const std::uint32_t blade_index,
    const std::uint32_t segments,
    const float wind_seconds = 0.0F) {
    const auto base = static_cast<std::uint32_t>(vertices.size());
    for (std::uint32_t row = 0U; row < segments; ++row) {
        const float t = static_cast<float>(row) / static_cast<float>(segments);
        vertices.push_back(
            make_reference_vertex(blade_index, vcr::GpuGrassCorner::left_edge, t, wind_seconds));
        vertices.push_back(
            make_reference_vertex(blade_index, vcr::GpuGrassCorner::right_edge, t, wind_seconds));
    }
    const std::uint32_t tip_index = base + 2U * segments;
    vertices.push_back(
        make_reference_vertex(blade_index, vcr::GpuGrassCorner::center_tip, 1.0F, wind_seconds));

    for (std::uint32_t seg = 0U; seg + 1U < segments; ++seg) {
        const std::uint32_t l0 = base + 2U * seg;
        const std::uint32_t r0 = l0 + 1U;
        const std::uint32_t l1 = l0 + 2U;
        const std::uint32_t r1 = l0 + 3U;
        indices.insert(indices.end(), {l0, l1, r0, r0, l1, r1});
    }
    const std::uint32_t last_left = base + 2U * (segments - 1U);
    indices.insert(indices.end(), {last_left, tip_index, last_left + 1U});
}

struct AffineRows {
    glm::vec4 row0{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec4 row1{0.0F, 1.0F, 0.0F, 0.0F};
    glm::vec4 row2{0.0F, 0.0F, 1.0F, 0.0F};
};

[[nodiscard]] glm::vec3 transform_point(const AffineRows& m, const glm::vec3& p) noexcept {
    const glm::vec4 h{p.x, p.y, p.z, 1.0F};
    return {glm::dot(m.row0, h), glm::dot(m.row1, h), glm::dot(m.row2, h)};
}

[[nodiscard]] vcr::Bounds world_aabb_from_corners(
    const vcr::Bounds& local,
    const AffineRows& m) noexcept {
    glm::vec3 lo{std::numeric_limits<float>::infinity()};
    glm::vec3 hi{-std::numeric_limits<float>::infinity()};
    for (unsigned corner = 0U; corner < 8U; ++corner) {
        const glm::vec3 signs{
            (corner & 1U) != 0U ? 1.0F : -1.0F,
            (corner & 2U) != 0U ? 1.0F : -1.0F,
            (corner & 4U) != 0U ? 1.0F : -1.0F,
        };
        const glm::vec3 wp = transform_point(m, local.center + local.extent * signs);
        lo = glm::min(lo, wp);
        hi = glm::max(hi, wp);
    }
    return vcr::Bounds{
        .center = (lo + hi) * 0.5F,
        .extent = (hi - lo) * 0.5F,
    };
}

void test_material_model_tokens(Checks& check) {
    static_assert(static_cast<std::uint32_t>(vcr::MaterialModel::gpu_grass) == 3U);
    static_assert(vcr::to_token(vcr::MaterialModel::gpu_grass) == "gpu_grass");
    static_assert(vcr::parse_material_model("gpu_grass") == vcr::MaterialModel::gpu_grass);

    for (const vcr::MaterialModel model : {
             vcr::MaterialModel::standard,
             vcr::MaterialModel::unlit,
             vcr::MaterialModel::subsurface,
             vcr::MaterialModel::gpu_grass,
         }) {
        const std::string_view token = vcr::to_token(model);
        check.expect(
            vcr::parse_material_model(token) == model,
            "every MaterialModel token round-trips exactly");
    }

    constexpr std::array<std::string_view, 9U> invalid_tokens{
        std::string_view{},
        "GPU_GRASS",
        "Gpu_Grass",
        "gpu-grass",
        "grass",
        "gpu_grass ",
        " gpu_grass",
        "foliage",
        std::string_view{"gpu_grass\0extra", 15U},
    };
    for (const std::string_view invalid : invalid_tokens) {
        check.expect(
            !vcr::parse_material_model(invalid).has_value(),
            "non-exact material model spellings are rejected");
    }

    vcr::PbrMaterial grass_material{};
    grass_material.model = vcr::MaterialModel::gpu_grass;
    grass_material.subsurface_profile = vcr::SubsurfaceProfile::foliage;
    grass_material.subsurface_strength = vcr::gpu_grass_default_subsurface_strength;
    const vcr::PbrMaterial sanitized = vcr::sanitize_pbr_material(grass_material);
    check.expect(
        sanitized.model == vcr::MaterialModel::gpu_grass &&
            sanitized.subsurface_profile == vcr::SubsurfaceProfile::foliage &&
            std::abs(sanitized.subsurface_strength - vcr::gpu_grass_default_subsurface_strength) <=
                1.0e-6F,
        "sanitize_pbr_material preserves gpu_grass model and foliage subsurface parameters");

    vcr::PbrMaterial invalid_enum_material{};
    invalid_enum_material.model = static_cast<vcr::MaterialModel>(99U);
    check.expect(
        vcr::sanitize_pbr_material(invalid_enum_material).model == vcr::MaterialModel::standard,
        "sanitize_pbr_material resets out-of-range MaterialModel values to standard");
}

void test_uv_encoding_and_rejection(Checks& check) {
    for (const std::uint32_t blade :
         {0U, 1U, 31U, 127U, vcr::max_gpu_grass_blades_per_patch - 1U}) {
        for (const float height_t : {0.0F, 0.25F, 0.5F, 1.0F}) {
            for (const vcr::GpuGrassCorner corner : {
                     vcr::GpuGrassCorner::left_edge,
                     vcr::GpuGrassCorner::right_edge,
                 }) {
                const auto uv = vcr::encode_gpu_grass_uv(blade, corner, height_t);
                check.expect(
                    uv[1U] >= -2.0F && uv[1U] <= -1.0F,
                    "encoded grass V stays inside [-2, -1] outside ordinary [0, 1] UVs");
                const auto decoded = vcr::decode_gpu_grass_uv(uv);
                const float expected_side =
                    corner == vcr::GpuGrassCorner::left_edge ? 0.0F : 1.0F;
                check.expect(
                    decoded.has_value() && decoded->blade_index == blade &&
                        decoded->corner == corner &&
                        std::abs(decoded->side_01 - expected_side) <= 1.0e-6F &&
                        std::abs(decoded->height_t - height_t) <= 1.0e-6F,
                    "encoded edge grass UV decodes losslessly");
            }
        }
        const auto tip_uv =
            vcr::encode_gpu_grass_uv(blade, vcr::GpuGrassCorner::center_tip, 1.0F);
        const auto decoded_tip = vcr::decode_gpu_grass_uv(tip_uv);
        check.expect(
            decoded_tip.has_value() && decoded_tip->blade_index == blade &&
                decoded_tip->corner == vcr::GpuGrassCorner::center_tip &&
                std::abs(decoded_tip->side_01 - 0.5F) <= 1.0e-6F &&
                std::abs(decoded_tip->height_t - 1.0F) <= 1.0e-6F,
            "encoded center_tip grass UV at height_t = 1 decodes losslessly");
    }

    const auto clamped_low =
        vcr::decode_gpu_grass_uv(vcr::encode_gpu_grass_uv(3U, vcr::GpuGrassCorner::left_edge, -2.0F));
    const auto clamped_high =
        vcr::decode_gpu_grass_uv(vcr::encode_gpu_grass_uv(3U, vcr::GpuGrassCorner::right_edge, 4.0F));
    check.expect(
        clamped_low.has_value() && clamped_low->height_t == 0.0F && clamped_high.has_value() &&
            clamped_high->height_t == 1.0F,
        "encode_gpu_grass_uv clamps out-of-range height_t into [0, 1]");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float overflow_u = static_cast<float>(vcr::max_gpu_grass_blades_per_patch * 4U);
    const std::array<std::array<float, 2U>, 16U> invalid_uvs{{
        {0.0F, 0.0F},
        {0.5F, 0.5F},
        {1.0F, 1.0F},
        {0.0F, -2.01F},
        {0.0F, -0.99F},
        {-1.0F, -1.5F},
        {0.25F, -1.5F},
        {0.5F, -1.5F},
        {3.0F, -1.5F},
        {31.0F, -1.0F},
        {overflow_u, -1.5F},
        {1.0F, -2.0F},
        {1.0F, -1.5F},
        {5.0F, -1.01F},
        {nan, -1.5F},
        {0.0F, inf},
    }};
    for (const auto& uv : invalid_uvs) {
        check.expect(
            !vcr::decode_gpu_grass_uv(uv).has_value(),
            "invalid, ordinary [0, 1], non-tip center, reserved-corner, or non-finite UVs fail");
    }
    check.expect(
        !vcr::decode_gpu_grass_uv({-inf, -1.5F}).has_value(),
        "negative infinity U is rejected");
}

void test_reference_geometry_and_submesh_validation(Checks& check) {
    // Independent geometric invariants across blades without copying evaluator formulas.
    for (const std::uint32_t blade : {0U, 1U, 17U, 99U, 512U, 4095U}) {
        const auto base_l = vcr::evaluate_gpu_grass_blade(blade, 0.0F, 0.0F, 0.0F);
        const auto base_r = vcr::evaluate_gpu_grass_blade(blade, 1.0F, 0.0F, 0.0F);
        const auto mid_l = vcr::evaluate_gpu_grass_blade(blade, 0.0F, 0.5F, 0.0F);
        const auto mid_r = vcr::evaluate_gpu_grass_blade(blade, 1.0F, 0.5F, 0.0F);
        const auto high_l = vcr::evaluate_gpu_grass_blade(blade, 0.0F, 0.85F, 0.0F);
        const auto high_r = vcr::evaluate_gpu_grass_blade(blade, 1.0F, 0.85F, 0.0F);
        const auto tip_l = vcr::evaluate_gpu_grass_blade(blade, 0.0F, 1.0F, 0.0F);
        const auto tip_c = vcr::evaluate_gpu_grass_blade(blade, 0.5F, 1.0F, 0.0F);
        const auto tip_r = vcr::evaluate_gpu_grass_blade(blade, 1.0F, 1.0F, 0.0F);

        const float width_base = glm::length(base_r.position - base_l.position);
        const float width_mid = glm::length(mid_r.position - mid_l.position);
        const float width_high = glm::length(high_r.position - high_l.position);
        check.expect(
            std::abs(base_l.position.y) <= 1.0e-6F && std::abs(base_r.position.y) <= 1.0e-6F,
            "blade base vertices sit on the local y = 0 patch plane");
        check.expect(
            width_base >= 2.0F * vcr::gpu_grass_min_blade_half_width - 1.0e-5F &&
                width_base <= 2.0F * vcr::gpu_grass_max_blade_half_width + 1.0e-5F &&
                width_base > width_mid && width_mid > width_high,
            "blade width stays within configured range and tapers monotonically toward tip");
        check.expect(
            glm::length(tip_l.position - tip_c.position) <= 1.0e-6F &&
                glm::length(tip_r.position - tip_c.position) <= 1.0e-6F,
            "blade width collapses cleanly to a single point at height_t = 1");
        check.expect(
            0.0F < mid_l.position.y && mid_l.position.y < high_l.position.y &&
                high_l.position.y < tip_c.position.y &&
                tip_c.position.y <= vcr::gpu_grass_max_blade_height + 1.0e-5F,
            "blade height increases strictly along height_t within max_blade_height");

        const glm::vec3 tangent_xyz{base_l.tangent.x, base_l.tangent.y, base_l.tangent.z};
        check.expect(
            std::abs(glm::length(base_l.normal) - 1.0F) <= 1.0e-4F &&
                std::abs(glm::length(base_r.normal) - 1.0F) <= 1.0e-4F &&
                std::abs(glm::length(tangent_xyz) - 1.0F) <= 1.0e-4F &&
                std::abs(base_l.tangent.y) <= 1.0e-6F && base_l.tangent.w == 1.0F,
            "reference normals and horizontal tangents are unit length with positive handedness");
        check.expect(
            glm::dot(base_r.position - base_l.position, tangent_xyz) > 0.0F &&
                glm::dot(base_r.normal - base_l.normal, tangent_xyz) > 0.2F,
            "across-blade vector and curved shading normals splay along +tangent");
        check.expect(
            glm::length(base_l.surface_uv - glm::vec2(0.0F, 1.0F)) <= 1.0e-6F &&
                glm::length(tip_c.surface_uv - glm::vec2(0.5F, 0.0F)) <= 1.0e-6F,
            "surface_uv maps blade edges and tip into [0, 1]^2");
    }

    // Nested prefixes must cover all four quadrants of the patch footprint.
    for (const std::uint32_t prefix_count : {16U, 32U, 64U, 128U}) {
        unsigned quadrant_mask = 0U;
        bool inside_patch = true;
        for (std::uint32_t b = 0U; b < prefix_count; ++b) {
            const glm::vec3 root = vcr::evaluate_gpu_grass_blade(b, 0.5F, 0.0F, 0.0F).position;
            if (std::abs(root.x) > vcr::gpu_grass_patch_root_half_extent + 1.0e-4F ||
                std::abs(root.z) > vcr::gpu_grass_patch_root_half_extent + 1.0e-4F) {
                inside_patch = false;
            }
            const unsigned qx = root.x >= 0.0F ? 1U : 0U;
            const unsigned qz = root.z >= 0.0F ? 2U : 0U;
            quadrant_mask |= (1U << (qx | qz));
        }
        check.expect(
            inside_patch && quadrant_mask == 0xFU,
            "every nested blade prefix distributes roots across all four patch quadrants");
    }

    // Multi-LOD submesh validation pass.
    std::vector<vcr::CookedModelVertex> vertices{};
    std::vector<std::uint32_t> indices{};
    for (std::uint32_t b = 0U; b < 8U; ++b) {
        append_blade_strip(vertices, indices, b, 2U);
    }
    const auto lod0_count = static_cast<std::uint32_t>(indices.size());
    const auto lod1_first = lod0_count;
    for (std::uint32_t b = 0U; b < 4U; ++b) {
        append_blade_strip(vertices, indices, b, 1U);
    }
    const auto lod1_count = static_cast<std::uint32_t>(indices.size()) - lod1_first;

    vcr::CookedModelSubmesh submesh{
        .first_index = 0U,
        .index_count = lod0_count,
        .material_index = 0U,
        .name = "GrassBlades",
        .lod_first = {0U, lod1_first, 0U, 0U},
        .lod_count = {lod0_count, lod1_count, 0U, 0U},
        .lod_error = {0.0F, 0.625F, 0.75F, 0.875F},
    };
    std::string error{};
    check.expect(
        validate_submesh(vertices, indices, submesh, error) && error.empty(),
        "valid multi-LOD reference grass submesh passes validation");

    // Small tolerance perturbation passes; excessive offset fails.
    auto perturbed_vertices = vertices;
    perturbed_vertices[0U].position[0U] += 1.0e-3F;
    check.expect(
        validate_submesh(perturbed_vertices, indices, submesh, error),
        "position jitter inside gpu_grass_reference_position_tolerance is accepted");

    perturbed_vertices[0U].position[0U] += 4.0e-3F;
    check.expect(
        !validate_submesh(perturbed_vertices, indices, submesh, error) && !error.empty(),
        "position offset exceeding gpu_grass_reference_position_tolerance is rejected");

    // Baking wind-deformed vertices into the cooked reference mesh must fail.
    std::vector<vcr::CookedModelVertex> wind_baked_vertices{};
    std::vector<std::uint32_t> wind_baked_indices{};
    append_blade_strip(wind_baked_vertices, wind_baked_indices, 0U, 2U, 11.0F);
    check.expect(
        !validate_range(
            wind_baked_vertices,
            wind_baked_indices,
            0U,
            wind_baked_indices.size(),
            error) &&
            !error.empty(),
        "cooked vertices baked at non-zero wind time are rejected");

    // Index range bounds and non-multiple-of-3 rejections.
    check.expect(
        !validate_range(vertices, indices, 0U, 0U, error) && !error.empty(),
        "empty index range is rejected");
    check.expect(
        !validate_range(vertices, indices, 0U, 2U, error) && !error.empty(),
        "sub-triangle index count is rejected");
    check.expect(
        !validate_range(vertices, indices, 0U, 4U, error) && !error.empty(),
        "non-multiple-of-3 index count is rejected");
    check.expect(
        !validate_range(vertices, indices, indices.size(), 3U, error) && !error.empty(),
        "out-of-bounds first_index is rejected");
    check.expect(
        !validate_range(vertices, indices, indices.size() - 2U, 3U, error) && !error.empty(),
        "index range extending past buffer end is rejected");
    check.expect(
        !validate_range(
            vertices,
            indices,
            std::numeric_limits<std::uint64_t>::max() - 1ULL,
            3ULL,
            error) &&
            !error.empty(),
        "overflowing uint64 first_index + index_count is rejected safely");

    // Out-of-range vertex index.
    auto bad_indices = indices;
    bad_indices[0U] = static_cast<std::uint32_t>(vertices.size());
    check.expect(
        !validate_range(vertices, bad_indices, 0U, 3U, error) && !error.empty(),
        "out-of-range vertex index is rejected");

    // Ordinary [0, 1] UV mesh assigned to gpu_grass.
    auto ordinary_uv_vertices = vertices;
    ordinary_uv_vertices[0U].texcoord = {0.5F, 0.5F};
    check.expect(
        !validate_range(ordinary_uv_vertices, indices, 0U, 3U, error) && !error.empty(),
        "ordinary [0, 1] UV vertex is rejected");

    // Non-finite vertex position.
    auto nan_pos_vertices = vertices;
    nan_pos_vertices[0U].position[1U] = std::numeric_limits<float>::quiet_NaN();
    check.expect(
        !validate_range(nan_pos_vertices, indices, 0U, 3U, error) && !error.empty(),
        "non-finite vertex position is rejected");

    // Cross-blade triangle connecting blade 0 and blade 1.
    std::vector<vcr::CookedModelVertex> cross_verts{
        make_reference_vertex(0U, vcr::GpuGrassCorner::left_edge, 0.0F),
        make_reference_vertex(1U, vcr::GpuGrassCorner::center_tip, 1.0F),
        make_reference_vertex(0U, vcr::GpuGrassCorner::right_edge, 0.0F),
    };
    const std::array<std::uint32_t, 3U> single_tri{0U, 1U, 2U};
    check.expect(
        !validate_range(cross_verts, single_tri, 0U, 3U, error) && !error.empty(),
        "triangle connecting different blade indices is rejected");

    // Degenerate single-corner triangle (all three vertices left_edge).
    std::vector<vcr::CookedModelVertex> single_edge_verts{
        make_reference_vertex(2U, vcr::GpuGrassCorner::left_edge, 0.0F),
        make_reference_vertex(2U, vcr::GpuGrassCorner::left_edge, 0.5F),
        make_reference_vertex(2U, vcr::GpuGrassCorner::left_edge, 1.0F),
    };
    check.expect(
        !validate_range(single_edge_verts, single_tri, 0U, 3U, error) && !error.empty(),
        "degenerate single-edge corner encoding is rejected");

    // Degenerate tiny height span (< 0.04).
    std::vector<vcr::CookedModelVertex> tiny_seg_verts{
        make_reference_vertex(2U, vcr::GpuGrassCorner::left_edge, 0.50F),
        make_reference_vertex(2U, vcr::GpuGrassCorner::left_edge, 0.52F),
        make_reference_vertex(2U, vcr::GpuGrassCorner::right_edge, 0.50F),
    };
    check.expect(
        !validate_range(tiny_seg_verts, single_tri, 0U, 3U, error) && !error.empty(),
        "degenerate triangle height span below 0.04 is rejected");

    // Secondary LOD corruption caught by validate_gpu_grass_submesh.
    const auto corrupt_lod_first = static_cast<std::uint32_t>(indices.size());
    auto submesh_with_bad_lod2 = submesh;
    auto extended_vertices = vertices;
    auto extended_indices = indices;
    const auto bad_v0 = static_cast<std::uint32_t>(extended_vertices.size());
    extended_vertices.insert(extended_vertices.end(), cross_verts.begin(), cross_verts.end());
    extended_indices.insert(extended_indices.end(), {bad_v0, bad_v0 + 1U, bad_v0 + 2U});
    submesh_with_bad_lod2.lod_first[2U] = corrupt_lod_first;
    submesh_with_bad_lod2.lod_count[2U] = 3U;
    check.expect(
        !validate_submesh(extended_vertices, extended_indices, submesh_with_bad_lod2, error) &&
            !error.empty(),
        "validate_gpu_grass_submesh validates non-empty secondary LOD index ranges");
}

void test_wind_root_anchoring_and_freeze(Checks& check) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    check.expect(
        vcr::wrap_gpu_grass_wind_seconds(0.0, false) == 0.0F &&
            vcr::wrap_gpu_grass_wind_seconds(-5.0, false) == 0.0F &&
            vcr::wrap_gpu_grass_wind_seconds(nan, false) == 0.0F &&
            vcr::wrap_gpu_grass_wind_seconds(inf, false) == 0.0F &&
            vcr::wrap_gpu_grass_wind_seconds(-inf, false) == 0.0F,
        "non-positive or non-finite elapsed wind times clamp to 0.0 reference pose");
    for (const double t : {0.1, 12.5, 256.0, 900.0}) {
        check.expect(
            vcr::wrap_gpu_grass_wind_seconds(t, true) == 0.0F,
            "frozen wind mode forces exact 0.0 reference pose for RT and voxel GI");
    }

    const std::array<AffineRows, 3U> transforms{{
        {},
        {
            .row0 = {0.0F, 0.0F, 1.0F, 42.5F},
            .row1 = {0.0F, 1.0F, 0.0F, 0.0F},
            .row2 = {-1.0F, 0.0F, 0.0F, -19.25F},
        },
        {
            .row0 = {-0.8F, 0.0F, 0.6F, -120.0F},
            .row1 = {0.0F, 1.35F, 0.0F, 3.0F},
            .row2 = {-0.6F, 0.0F, -0.8F, 88.0F},
        },
    }};

    for (const auto& xf : transforms) {
        bool saw_tip_wind_motion = false;
        for (const std::uint32_t blade : {0U, 5U, 29U, 127U, 1023U}) {
            const auto ref_left =
                vcr::evaluate_gpu_grass_blade(blade, 0.0F, 0.0F, 0.0F, xf.row0, xf.row2);
            const auto ref_right =
                vcr::evaluate_gpu_grass_blade(blade, 1.0F, 0.0F, 0.0F, xf.row0, xf.row2);
            const auto ref_tip =
                vcr::evaluate_gpu_grass_blade(blade, 0.5F, 1.0F, 0.0F, xf.row0, xf.row2);

            // Near-zero wind_seconds must smoothly approach static reference pose.
            const auto near_zero_tip =
                vcr::evaluate_gpu_grass_blade(blade, 0.5F, 1.0F, 1.0e-3F, xf.row0, xf.row2);
            check.expect(
                glm::length(near_zero_tip.position - ref_tip.position) <= 1.0e-4F,
                "startup wind envelope smoothly meets static reference pose near t = 0");

            for (const float wind_s : {0.25F, 1.8F, 19.4F, 143.7F, 399.2F}) {
                const auto live_left =
                    vcr::evaluate_gpu_grass_blade(blade, 0.0F, 0.0F, wind_s, xf.row0, xf.row2);
                const auto live_right =
                    vcr::evaluate_gpu_grass_blade(blade, 1.0F, 0.0F, wind_s, xf.row0, xf.row2);
                check.expect(
                    glm::length(live_left.position - ref_left.position) <= 1.0e-6F &&
                        glm::length(live_right.position - ref_right.position) <= 1.0e-6F,
                    "blade root vertices at height_t = 0 remain anchored under wind and transform");

                const auto live_tip =
                    vcr::evaluate_gpu_grass_blade(blade, 0.5F, 1.0F, wind_s, xf.row0, xf.row2);
                const glm::vec2 delta_xz{
                    live_tip.position.x - ref_tip.position.x,
                    live_tip.position.z - ref_tip.position.z,
                };
                const float horiz_disp = glm::length(delta_xz);
                if (wind_s >= 1.0F && horiz_disp > 1.0e-2F) {
                    saw_tip_wind_motion = true;
                }
                check.expect(
                    horiz_disp <= vcr::gpu_grass_max_wind_displacement + 1.0e-4F &&
                        live_tip.position.y > 0.0F &&
                        live_tip.position.y <= vcr::gpu_grass_max_blade_height + 1.0e-5F,
                    "tip wind displacement stays bounded and preserves finite positive height");
            }
        }
        check.expect(saw_tip_wind_motion, "active wind displaces blade tips horizontally");
    }
}

void test_wind_phase_wrap_continuity(Checks& check) {
    constexpr double period = static_cast<double>(vcr::gpu_grass_wind_period_seconds);
    for (const double elapsed : {256.0, 256.001, 511.999, 512.0, 512.001, 768.0, 86400.0}) {
        const float wrapped = vcr::wrap_gpu_grass_wind_seconds(elapsed, false);
        check.expect(
            wrapped >= vcr::gpu_grass_wind_period_seconds &&
                wrapped <= 2.0F * vcr::gpu_grass_wind_period_seconds,
            "wrapped active wind time stays in [T, 2T] away from startup envelope");
    }

    const AffineRows rotated{
        .row0 = {0.6F, 0.0F, -0.8F, 17.0F},
        .row1 = {0.0F, 1.0F, 0.0F, 0.0F},
        .row2 = {0.8F, 0.0F, 0.6F, -31.0F},
    };

    // Exact harmonic periodicity once past the 0.5 s startup ramp.
    for (const double base_t : {2.25, 67.5, 199.125, 255.75, 310.5}) {
        for (const int multiple : {1, 2, 7, 32}) {
            const float w0 = vcr::wrap_gpu_grass_wind_seconds(base_t, false);
            const float w1 = vcr::wrap_gpu_grass_wind_seconds(
                base_t + static_cast<double>(multiple) * period, false);
            for (const std::uint32_t blade : {0U, 13U, 88U, 511U}) {
                const auto s0 = vcr::evaluate_gpu_grass_blade(
                    blade, 0.5F, 1.0F, w0, rotated.row0, rotated.row2);
                const auto s1 = vcr::evaluate_gpu_grass_blade(
                    blade, 0.5F, 1.0F, w1, rotated.row0, rotated.row2);
                check.expect(
                    glm::length(s1.position - s0.position) <= 2.5e-4F &&
                        glm::length(s1.normal - s0.normal) <= 5.0e-4F,
                    "wrapped wind evaluation is periodic across integer multiples of 256 s");
            }
        }
    }

    // Continuity across wrap seams k * 256 s (no snap in position or velocity).
    constexpr double dt = 1.0e-3;
    for (const int k : {1, 2, 3, 15}) {
        const double seam = static_cast<double>(k) * period;
        const float t_before = vcr::wrap_gpu_grass_wind_seconds(seam - dt, false);
        const float t_at = vcr::wrap_gpu_grass_wind_seconds(seam, false);
        const float t_after = vcr::wrap_gpu_grass_wind_seconds(seam + dt, false);
        for (const std::uint32_t blade : {0U, 21U, 127U, 2048U}) {
            const auto before = vcr::evaluate_gpu_grass_blade(
                blade, 0.5F, 1.0F, t_before, rotated.row0, rotated.row2);
            const auto at = vcr::evaluate_gpu_grass_blade(
                blade, 0.5F, 1.0F, t_at, rotated.row0, rotated.row2);
            const auto after = vcr::evaluate_gpu_grass_blade(
                blade, 0.5F, 1.0F, t_after, rotated.row0, rotated.row2);
            const glm::vec3 step_in = at.position - before.position;
            const glm::vec3 step_out = after.position - at.position;
            check.expect(
                glm::length(step_in) <= 3.0e-3F && glm::length(step_out) <= 3.0e-3F &&
                    glm::length(step_out - step_in) <= 2.5e-4F,
                "position and finite-difference motion remain continuous across wind wrap seams");
        }
    }
}

void test_analytic_bounds_containment(Checks& check) {
    // Seed reference bounds from a single static blade to ensure enlarge_gpu_grass_local_bounds
    // conservatively covers all other blades, heights, wind states, and patch orientations.
    glm::vec3 seed_min{std::numeric_limits<float>::infinity()};
    glm::vec3 seed_max{-std::numeric_limits<float>::infinity()};
    for (const float side : {0.0F, 0.5F, 1.0F}) {
        for (const float t : {0.0F, 0.5F, 1.0F}) {
            const glm::vec3 p = vcr::evaluate_gpu_grass_blade(0U, side, t, 0.0F).position;
            seed_min = glm::min(seed_min, p);
            seed_max = glm::max(seed_max, p);
        }
    }
    const vcr::Bounds reference_bounds{
        .center = (seed_min + seed_max) * 0.5F,
        .extent = (seed_max - seed_min) * 0.5F,
    };
    const vcr::Bounds local_bounds = vcr::enlarge_gpu_grass_local_bounds(reference_bounds);
    const glm::vec3 local_min = local_bounds.center - local_bounds.extent;
    const glm::vec3 local_max = local_bounds.center + local_bounds.extent;

    check.expect(
        local_bounds.extent.x > reference_bounds.extent.x &&
            local_bounds.extent.z > reference_bounds.extent.z && local_min.y < 0.0F &&
            local_max.y > vcr::gpu_grass_max_blade_height,
        "enlarge_gpu_grass_local_bounds expands horizontal and vertical extents conservatively");

    const std::array<AffineRows, 5U> transforms{{
        {},
        {
            .row0 = {1.0F, 0.0F, 0.0F, 240.0F},
            .row1 = {0.0F, 1.0F, 0.0F, -7.5F},
            .row2 = {0.0F, 0.0F, 1.0F, -510.0F},
        },
        {
            .row0 = {-0.5F, 0.0F, 0.8660254F, -14.0F},
            .row1 = {0.0F, 1.0F, 0.0F, 0.0F},
            .row2 = {-0.8660254F, 0.0F, -0.5F, 23.0F},
        },
        {
            .row0 = {1.45F, -0.18F, 0.52F, 8.0F},
            .row1 = {0.15F, 0.92F, 0.22F, 1.5F},
            .row2 = {-0.48F, -0.25F, 1.18F, -6.0F},
        },
        // Degenerate zero XZ basis exercises the local_wind_len fallback branch.
        {
            .row0 = {0.0F, 0.0F, 0.0F, 5.0F},
            .row1 = {0.0F, 1.0F, 0.0F, 0.0F},
            .row2 = {0.0F, 0.0F, 0.0F, -3.0F},
        },
    }};

    bool all_local_contained = true;
    bool all_world_contained = true;
    constexpr float eps = 1.0e-5F;

    for (const auto& xf : transforms) {
        const vcr::Bounds world_bounds = world_aabb_from_corners(local_bounds, xf);
        const glm::vec3 world_min = world_bounds.center - world_bounds.extent;
        const glm::vec3 world_max = world_bounds.center + world_bounds.extent;

        for (const std::uint32_t blade :
             {0U, 1U, 7U, 31U, 64U, 127U, 511U, 2047U, vcr::max_gpu_grass_blades_per_patch - 1U}) {
            for (const float wind_s : {0.0F, 0.2F, 1.7F, 29.4F, 113.8F, 256.0F, 419.6F, 511.95F}) {
                for (const float t : {0.0F, 0.35F, 0.7F, 1.0F}) {
                    for (const float side : {0.0F, 0.5F, 1.0F}) {
                        const auto sample = vcr::evaluate_gpu_grass_blade(
                            blade, side, t, wind_s, xf.row0, xf.row2);
                        if (sample.position.x < local_min.x - eps ||
                            sample.position.y < local_min.y - eps ||
                            sample.position.z < local_min.z - eps ||
                            sample.position.x > local_max.x + eps ||
                            sample.position.y > local_max.y + eps ||
                            sample.position.z > local_max.z + eps) {
                            all_local_contained = false;
                        }
                        const glm::vec3 wp = transform_point(xf, sample.position);
                        if (wp.x < world_min.x - eps || wp.y < world_min.y - eps ||
                            wp.z < world_min.z - eps || wp.x > world_max.x + eps ||
                            wp.y > world_max.y + eps || wp.z > world_max.z + eps) {
                            all_world_contained = false;
                        }
                    }
                }
            }
        }
    }

    check.expect(
        all_local_contained,
        "enlarge_gpu_grass_local_bounds contains all sampled blades across time and wind directions");
    check.expect(
        all_world_contained,
        "transformed grass bounds contain all world-space animated blade vertices");
}

} // namespace

int main() {
    Checks check;
    test_material_model_tokens(check);
    test_uv_encoding_and_rejection(check);
    test_reference_geometry_and_submesh_validation(check);
    test_wind_root_anchoring_and_freeze(check);
    test_wind_phase_wrap_continuity(check);
    test_analytic_bounds_containment(check);
    if (check.failures != 0U) {
        return 1;
    }
    std::puts("GPU grass material, UV contract, validator, wind continuity, and bounds tests passed.");
    return 0;
}
