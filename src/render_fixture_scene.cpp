#include "render_fixture_scene.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>

namespace vcr {
namespace {

[[nodiscard]] std::uint32_t fixture_hash(std::uint32_t value) noexcept {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] float hash_unit(const std::uint32_t value) noexcept {
    return static_cast<float>(fixture_hash(value) >> 8U) * (1.0F / 16'777'215.0F);
}

void add_fixture_cube(
    SceneDocument& scene, std::string name, const glm::vec3 position,
    const glm::vec3 half_extent, const glm::vec3 color, const float roughness) {
    const auto id = scene.create_node(std::move(name), SceneNodeKind::cube);
    const auto node = scene.node(id);
    if (!node) throw std::runtime_error("Render fixture exceeded scene capacity");
    auto& value = node->get();
    value.transform.translation = position;
    value.transform.scale = half_extent;
    value.material.base_color = glm::vec4(color, 1.0F);
    value.material.roughness = roughness;
}

void add_aa_box(
    SceneDocument& scene,
    std::string name,
    const glm::vec3 camera_position,
    const float distance,
    const glm::vec2 screen_position,
    const glm::vec2 half_extent,
    const float angle_degrees,
    const glm::vec3 color) {
    constexpr float pitch = 0.20943951024F; // The default camera's 12 degree downward pitch.
    const glm::vec3 forward{0.0F, -std::sin(pitch), -std::cos(pitch)};
    const glm::vec3 up{0.0F, std::cos(pitch), -std::sin(pitch)};
    const glm::vec3 right{1.0F, 0.0F, 0.0F};
    const auto id = scene.create_node(std::move(name), SceneNodeKind::cube);
    const auto node = scene.node(id);
    if (!node) throw std::runtime_error("AA fixture exceeded scene capacity");
    auto& value = node->get();
    value.transform.translation = camera_position + distance * forward +
        screen_position.x * right + screen_position.y * up;
    value.transform.rotation_degrees = {-12.0F, 0.0F, angle_degrees};
    value.transform.scale = {half_extent.x, half_extent.y, 0.008F};
    value.material.base_color = glm::vec4(color, 1.0F);
    value.material.roughness = 1.0F;
    value.material.model = MaterialModel::unlit;
}

} // namespace

SceneDocument make_cascade_cache_fixture(float camera_z) {
    assert(std::isfinite(camera_z));
    SceneDocument scene;
    const auto cube = [&](std::string name, glm::vec3 position, glm::vec3 half_extent,
                          glm::vec3 color) {
        const auto id = scene.create_node(std::move(name), SceneNodeKind::cube);
        const auto node = scene.node(id);
        if (!node) throw std::runtime_error("Cascade fixture exceeded scene capacity");
        auto &value = node->get();
        value.transform.translation = position;
        value.transform.scale = half_extent;
        value.material.base_color = glm::vec4(color, 1.0F);
        value.material.roughness = 0.85F;
    };
    cube("Shadow receivers", {0, -0.25F, camera_z - 100}, {100, 0.25F, 112}, glm::vec3(0.65F));
    // Rows straddle the 12/32/80 metre splits and the final coverage boundary.
    // Lateral offsets prevent every far caster from hiding behind the near row.
    constexpr std::array distances{6.0F, 10.8F, 12.0F, 13.2F, 28.8F, 32.0F,
                                    35.2F, 72.0F, 80.0F, 88.0F, 170.0F, 188.0F};
    for (std::size_t row = 0; row < distances.size(); ++row) {
        const float distance = distances[row];
        const float height = 0.65F + distance * 0.012F;
        for (int column = -1; column <= 1; ++column) {
            const float stagger = (row % 2 == 0 ? -1.0F : 1.0F) * height;
            cube("Cascade caster " + std::to_string(row) + ":" + std::to_string(column),
                 {static_cast<float>(column) * distance * 0.32F + stagger, height, camera_z - distance},
                 {height * 0.4F, height, height * 0.4F}, {0.45F, 0.48F, 0.5F});
        }
    }
    // Small contact features expose the D16 precision/bias tradeoff near camera.
    for (unsigned pole = 0; pole < 4; ++pole) {
        cube("Thin contact pole " + std::to_string(pole),
             {-3.0F + static_cast<float>(pole) * 0.4F, 0.5F, camera_z - 8.0F},
             {0.035F, 0.5F, 0.035F}, {0.7F, 0.3F, 0.15F});
    }
    cube("Tall off-centre caster", {25, 12, camera_z - 28}, {0.6F, 12, 0.6F}, glm::vec3(0.4F));
    static_cast<void>(scene.create_node("Sun", SceneNodeKind::directional_light));
    static_cast<void>(scene.create_node("Sky", SceneNodeKind::environment));
    return scene;
}

SceneDocument make_overdraw_fixture(float camera_z, unsigned layers) {
    assert(std::isfinite(camera_z) && (layers == 1 || layers == 4 || layers == 16));
    SceneDocument scene;
    constexpr float pitch = 0.20943951024F; // The default camera's 12 degree downward pitch.
    const glm::vec3 forward{0, -std::sin(pitch), -std::cos(pitch)};
    const glm::vec3 up{0, std::cos(pitch), -std::sin(pitch)};
    // Deliberately submit far layers first. Proportional positions/extents keep
    // their projected footprints aligned, without coplanar depth ties.
    for (unsigned layer = layers; layer > 0; --layer) {
        const float distance = 4.0F + static_cast<float>(layer - 1) * 0.35F;
        for (unsigned row = 0; row < 8; ++row) {
            for (unsigned column = 0; column < 16; ++column) {
                const auto id = scene.create_node("Overdraw " + std::to_string(layer) + ":" +
                    std::to_string(row) + ":" + std::to_string(column), SceneNodeKind::cube);
                const auto node = scene.node(id);
                if (!node) throw std::runtime_error("Overdraw fixture exceeded scene capacity");
                auto &value = node->get();
                const float x = (static_cast<float>(column) - 7.5F) * 0.13F;
                const float y = (static_cast<float>(row) - 3.5F) * 0.14F;
                value.transform.translation = glm::vec3(0, 4, camera_z) +
                    distance * (forward + up * y + glm::vec3(x, 0, 0));
                value.transform.rotation_degrees.x = -12.0F;
                value.transform.scale = distance * glm::vec3(0.064F, 0.069F, 0.006F);
                value.material.base_color = glm::vec4(0.2F + 0.025F * static_cast<float>(column),
                    0.25F + 0.05F * static_cast<float>(row), 0.45F, 1);
                value.material.roughness = 0.15F + 0.05F * static_cast<float>(row);
            }
        }
    }
    static_cast<void>(scene.create_node("Sun", SceneNodeKind::directional_light));
    static_cast<void>(scene.create_node("Sky", SceneNodeKind::environment));
    return scene;
}

SceneDocument make_light_swarm_fixture(
    const float camera_z,
    const std::uint32_t light_count,
    const LightSwarmLayout layout) {
    assert(std::isfinite(camera_z) && light_count <= maximum_local_lights);
    SceneDocument scene;
    const float receiver_z = camera_z - 18.0F;
    add_fixture_cube(
        scene, "Light receiver floor", {0.0F, -0.3F, receiver_z - 20.0F},
        {42.0F, 0.3F, 44.0F}, {0.32F, 0.34F, 0.38F}, 0.72F);
    add_fixture_cube(
        scene, "Central receiver", {0.0F, 1.5F, receiver_z},
        {3.5F, 1.5F, 3.5F}, {0.62F, 0.58F, 0.48F}, 0.34F);
    add_fixture_cube(
        scene, "Glossy receiver", {-8.0F, 1.0F, receiver_z - 9.0F},
        {2.0F, 1.0F, 2.0F}, {0.16F, 0.42F, 0.72F}, 0.12F);
    add_fixture_cube(
        scene, "Rough receiver", {8.0F, 2.0F, receiver_z - 16.0F},
        {2.5F, 2.0F, 2.5F}, {0.65F, 0.22F, 0.12F}, 0.88F);

    constexpr std::array colors{
        glm::vec3{1.0F, 0.22F, 0.12F},
        glm::vec3{0.18F, 0.48F, 1.0F},
        glm::vec3{0.24F, 1.0F, 0.38F},
        glm::vec3{1.0F, 0.72F, 0.18F},
    };
    constexpr float full_turn = 6.2831853071795864769F;
    const glm::vec3 overlap_target{0.0F, 1.5F, receiver_z};
    const float overlap_intensity = light_count == 0U
        ? 0.0F
        : 240.0F / static_cast<float>(light_count);
    for (std::uint32_t index = 0; index < light_count; ++index) {
        const bool spot = (index & 1U) != 0U;
        const auto id = scene.create_node(
            "Light " + std::to_string(index),
            spot ? SceneNodeKind::spot_light : SceneNodeKind::point_light);
        const auto node = scene.node(id);
        if (!node) throw std::runtime_error("Light swarm exceeded scene capacity");
        auto& value = node->get();
        value.light.color = colors[index % colors.size()];
        if (layout == LightSwarmLayout::overlap) {
            const float angle = full_turn * hash_unit(index * 6U + 1U);
            const float radius = 1.6F + 1.4F * hash_unit(index * 6U + 2U);
            value.transform.translation = overlap_target + glm::vec3{
                std::cos(angle) * radius, 0.0F, std::sin(angle) * radius};
            value.light.intensity_candela = overlap_intensity;
            value.light.range = 10.0F;
            if (spot) {
                const glm::vec3 direction = glm::normalize(
                    overlap_target - value.transform.translation);
                value.transform.rotation_degrees.y = glm::degrees(
                    std::atan2(-direction.x, -direction.z));
                value.light.inner_cone_degrees = 32.0F;
                value.light.outer_cone_degrees = 48.0F;
            }
        } else {
            value.transform.translation = {
                (hash_unit(index * 6U + 1U) * 2.0F - 1.0F) * 38.0F,
                1.5F + hash_unit(index * 6U + 2U) * 6.0F,
                camera_z - 6.0F - hash_unit(index * 6U + 3U) * 76.0F,
            };
            value.light.intensity_candela = 85.0F;
            value.light.range = 3.5F;
            if (spot) {
                value.transform.rotation_degrees.x = -90.0F;
                value.light.inner_cone_degrees = 24.0F;
                value.light.outer_cone_degrees = 38.0F;
            }
        }
    }
    static_cast<void>(scene.create_node("Sun", SceneNodeKind::directional_light));
    static_cast<void>(scene.create_node("Sky", SceneNodeKind::environment));
    return scene;
}

SceneDocument make_aa_wires_fixture(const float camera_z) {
    assert(std::isfinite(camera_z));
    SceneDocument scene;
    const glm::vec3 camera_position{0.0F, 4.0F, camera_z};

    // A dark, camera-facing backdrop isolates geometric edge coverage from
    // lighting and sky variation. All foreground patches use the same unlit path.
    add_aa_box(scene, "AA dark background", camera_position, 18.0F, {0.0F, 0.0F},
               {12.0F, 7.2F}, 0.0F, {0.018F, 0.022F, 0.032F});

    constexpr std::array angles{7.0F, 23.0F, 45.0F, 67.0F};
    constexpr std::array half_widths{0.0025F, 0.005F, 0.010F, 0.025F};
    for (std::size_t index = 0; index < angles.size(); ++index) {
        const float x = -4.5F + static_cast<float>(index) * 3.0F;
        add_aa_box(scene, "AA wire " + std::to_string(index), camera_position,
                   9.0F + static_cast<float>(index) * 0.35F, {x, 2.15F},
                   {1.35F, half_widths[index]}, angles[index],
                   {0.94F, 0.96F, 1.0F});
        add_aa_box(scene, "AA inverse wire " + std::to_string(index), camera_position,
                   9.2F + static_cast<float>(index) * 0.35F, {x, 0.75F},
                   {1.1F, half_widths[index]}, -angles[index],
                   {0.30F, 0.78F, 1.0F});
    }

    // Two touching strips create an almost isoluminant diagonal material
    // boundary (Rec.709 Y ~= 0.2288 on both sides). Color edge detection must
    // retain it, while the luma detector is expected to miss it.
    constexpr float boundary_angle = 31.0F;
    constexpr float boundary_offset = 0.72F;
    const float boundary_radians = glm::radians(boundary_angle);
    const glm::vec2 boundary_normal{-std::sin(boundary_radians), std::cos(boundary_radians)};
    const glm::vec2 boundary_center{3.6F, -1.65F};
    add_aa_box(scene, "AA red boundary", camera_position, 10.5F,
               boundary_center - boundary_normal * boundary_offset,
               {1.85F, boundary_offset}, boundary_angle, {0.95F, 0.035F, 0.025F});
    add_aa_box(scene, "AA green boundary", camera_position, 10.5F,
               boundary_center + boundary_normal * boundary_offset,
               {1.85F, boundary_offset}, boundary_angle, {0.025F, 0.3044F, 0.08F});

    // A near white cross must remain distinct from the far blue patch behind it.
    add_aa_box(scene, "AA far blue panel", camera_position, 12.0F, {-4.2F, -2.0F},
               {2.15F, 1.45F}, -9.0F, {0.025F, 0.12F, 0.82F});
    add_aa_box(scene, "AA near cross horizontal", camera_position, 8.0F,
               {-2.8F, -1.3333334F}, {1.25F, 0.030F}, 13.0F,
               {1.0F, 0.96F, 0.78F});
    add_aa_box(scene, "AA near cross vertical", camera_position, 8.0F,
               {-2.8F, -1.3333334F}, {1.25F, 0.030F}, 103.0F,
               {1.0F, 0.96F, 0.78F});

    // Large interiors provide controls for detecting blur or color drift away
    // from edges in screenshot-based quality comparisons.
    add_aa_box(scene, "AA black interior control", camera_position, 9.5F,
               {-0.8F, -2.4F}, {0.58F, 0.58F}, 0.0F, {0.015F, 0.015F, 0.015F});
    add_aa_box(scene, "AA grey interior control", camera_position, 9.5F,
               {0.65F, -2.4F}, {0.58F, 0.58F}, 0.0F, {0.38F, 0.38F, 0.38F});
    add_aa_box(scene, "AA white interior control", camera_position, 9.5F,
               {2.1F, -2.4F}, {0.58F, 0.58F}, 0.0F, {0.95F, 0.95F, 0.95F});
    return scene;
}

SceneDocument make_motion_bars_fixture(const float camera_z) {
    assert(std::isfinite(camera_z));
    SceneDocument scene;
    const glm::vec3 camera_position{0.0F, 4.0F, camera_z};

    add_aa_box(scene, "Motion dark background", camera_position, 18.0F,
               {0.0F, 0.0F}, {12.0F, 7.2F}, 0.0F,
               {0.012F, 0.016F, 0.025F});
    // These are intentionally the next two cube records: Application animates
    // them in opposite directions to exercise foreground disocclusion and
    // colliding velocities through the ordinary runtime transform path.
    add_aa_box(scene, "Motion horizontal bar", camera_position, 8.0F,
               {-2.6F, 1.35F}, {2.3F, 0.16F}, 11.0F,
               {1.0F, 0.18F, 0.06F});
    add_aa_box(scene, "Motion diagonal bar", camera_position, 10.5F,
               {2.8F, -0.2F}, {2.0F, 0.12F}, 58.0F,
               {0.05F, 0.72F, 1.0F});
    add_aa_box(scene, "Motion thin foreground pole", camera_position, 6.5F,
               {0.0F, -0.1F}, {0.035F, 2.8F}, 0.0F,
               {0.96F, 0.96F, 0.82F});
    add_aa_box(scene, "Motion near depth marker", camera_position, 7.0F,
               {-4.8F, -2.3F}, {0.65F, 0.65F}, 0.0F,
               {0.14F, 0.95F, 0.28F});
    add_aa_box(scene, "Motion far depth marker", camera_position, 14.0F,
               {4.8F, -2.3F}, {1.3F, 1.3F}, 0.0F,
               {0.78F, 0.16F, 0.92F});
    return scene;
}
}
