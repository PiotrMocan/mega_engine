#include "editor_scene_tools.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace vcr {
namespace {

[[nodiscard]] bool finite_positive(const float value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

[[nodiscard]] bool finite_transform(
    const SceneTransform& transform) noexcept {
    const auto finite_vec3 = [](const glm::vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    };
    return finite_vec3(transform.translation) &&
        finite_vec3(transform.rotation_degrees) &&
        finite_vec3(transform.scale) &&
        transform.scale.x > 0.0F && transform.scale.y > 0.0F &&
        transform.scale.z > 0.0F;
}

[[nodiscard]] float next_unit_float(std::uint32_t& state) noexcept {
    // A fixed integer generator keeps authored placement identical across
    // standard-library implementations and future editor sessions.
    state = state * 1'664'525U + 1'013'904'223U;
    return static_cast<float>(state >> 8U) *
        (1.0F / 16'777'216.0F);
}

} // namespace

SceneNodeId create_flat_ground(
    SceneDocument& scene,
    const FlatGroundOptions& options,
    std::string& error) {
    error.clear();
    if (options.name.empty() ||
        options.name.size() > max_scene_node_name_bytes ||
        options.name.find('\0') != std::string::npos) {
        error = "Flat terrain name must contain 1 through 255 UTF-8 bytes.";
        return {};
    }
    if ((options.parent && !scene.valid(options.parent)) ||
        !std::isfinite(options.center_x) ||
        !std::isfinite(options.height) ||
        !std::isfinite(options.center_z) ||
        !finite_positive(options.width) ||
        !finite_positive(options.depth)) {
        error = "Flat terrain parent and dimensions are invalid.";
        return {};
    }

    const SceneNodeId ground = scene.create_node(
        options.name, SceneNodeKind::ground, options.parent);
    if (!ground) {
        error = "Unable to create flat terrain; the scene capacity is exhausted.";
        return {};
    }
    const auto node = scene.node(ground);
    if (!node) {
        static_cast<void>(scene.destroy_subtree(ground));
        error = "The new flat terrain node became invalid.";
        return {};
    }
    node->get().transform.translation = glm::vec3(
        options.center_x, options.height, options.center_z);
    // The renderer's authored ground primitive spans [-1, 1] on X and Z.
    node->get().transform.scale = glm::vec3(
        options.width * 0.5F, 1.0F, options.depth * 0.5F);
    node->get().material = sanitize_pbr_material(options.material);
    scene.mark_content_changed();
    return ground;
}

bool scatter_model_instances(
    SceneDocument& scene,
    const ModelScatterOptions& options,
    std::vector<SceneNodeId>& created,
    std::string& error) {
    created.clear();
    error.clear();
    if (!valid_scene_asset_path(options.asset_path)) {
        error = "Scatter requires a relative .vcrasset.json model path.";
        return false;
    }
    if (options.name_prefix.empty() ||
        options.name_prefix.size() > max_scene_node_name_bytes - 11U ||
        options.name_prefix.find('\0') != std::string::npos) {
        error = "Scatter name prefix is empty or too long.";
        return false;
    }
    if (options.count == 0U ||
        options.count > maximum_scatter_instances ||
        options.count > max_scene_nodes - scene.size()) {
        error = "Scatter count exceeds the operation or scene capacity.";
        return false;
    }
    if ((options.parent && !scene.valid(options.parent)) ||
        !std::isfinite(options.center_x) ||
        !std::isfinite(options.height) ||
        !std::isfinite(options.center_z) ||
        !finite_positive(options.width) ||
        !finite_positive(options.depth) ||
        !finite_positive(options.minimum_scale) ||
        !finite_positive(options.maximum_scale) ||
        options.minimum_scale > options.maximum_scale ||
        !finite_transform(options.base_transform)) {
        error = "Scatter transform, parent, area, or scale range is invalid.";
        return false;
    }

    created.reserve(options.count);
    const auto rollback = [&]() noexcept {
        for (auto iterator = created.rbegin();
             iterator != created.rend(); ++iterator) {
            static_cast<void>(scene.destroy_subtree(*iterator));
        }
        created.clear();
    };
    std::uint32_t random_state = options.seed;
    for (std::uint32_t index = 0U; index < options.count; ++index) {
        const SceneNodeId id = scene.create_node(
            options.name_prefix + " " + std::to_string(index + 1U),
            SceneNodeKind::model,
            options.parent);
        if (!id) {
            rollback();
            error = "Unable to complete the scatter batch.";
            return false;
        }
        const auto node = scene.node(id);
        if (!node) {
            static_cast<void>(scene.destroy_subtree(id));
            rollback();
            error = "A scattered model node became invalid.";
            return false;
        }

        SceneNode& value = node->get();
        value.asset_path = options.asset_path;
        value.transform = options.base_transform;
        value.transform.translation += glm::vec3(
            options.center_x +
                (next_unit_float(random_state) - 0.5F) * options.width,
            options.height,
            options.center_z +
                (next_unit_float(random_state) - 0.5F) * options.depth);
        value.transform.rotation_degrees.y +=
            next_unit_float(random_state) * 360.0F;
        const float scale = options.minimum_scale +
            next_unit_float(random_state) *
                (options.maximum_scale - options.minimum_scale);
        value.transform.scale *= scale;
        if (!finite_transform(value.transform)) {
            static_cast<void>(scene.destroy_subtree(id));
            rollback();
            error = "Scatter parameters overflow the authored transform range.";
            return false;
        }
        created.push_back(id);
    }
    scene.mark_content_changed();
    return true;
}

} // namespace vcr
