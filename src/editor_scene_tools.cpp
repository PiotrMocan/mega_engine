#include "editor_scene_tools.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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

[[nodiscard]] bool finite_matrix(const glm::mat4& matrix) noexcept {
    for (glm::length_t column = 0; column < 4; ++column) {
        for (glm::length_t row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::optional<SceneTransform> decompose_transform(
    const glm::mat4& matrix) noexcept {
    if (!finite_matrix(matrix)) {
        return std::nullopt;
    }
    constexpr float affine_epsilon = 1.0e-4F;
    if (std::abs(matrix[0][3]) > affine_epsilon ||
        std::abs(matrix[1][3]) > affine_epsilon ||
        std::abs(matrix[2][3]) > affine_epsilon ||
        std::abs(matrix[3][3] - 1.0F) > affine_epsilon) {
        return std::nullopt;
    }
    const glm::vec3 column_x(matrix[0]);
    const glm::vec3 column_y(matrix[1]);
    const glm::vec3 column_z(matrix[2]);
    glm::vec3 scale{
        glm::length(column_x),
        glm::length(column_y),
        glm::length(column_z),
    };
    constexpr float minimum_scale = 0.0001F;
    if (scale.x < minimum_scale || scale.y < minimum_scale ||
        scale.z < minimum_scale) {
        return std::nullopt;
    }
    const glm::vec3 axis_x = column_x / scale.x;
    const glm::vec3 raw_y = column_y / scale.y;
    const glm::vec3 orthogonal_y = raw_y - axis_x * glm::dot(axis_x, raw_y);
    const float orthogonal_y_length = glm::length(orthogonal_y);
    if (!std::isfinite(orthogonal_y_length) ||
        orthogonal_y_length <= affine_epsilon) {
        return std::nullopt;
    }
    const glm::vec3 axis_y = orthogonal_y / orthogonal_y_length;
    glm::vec3 axis_z = glm::cross(axis_x, axis_y);
    const float axis_z_length = glm::length(axis_z);
    if (!std::isfinite(axis_z_length) ||
        axis_z_length <= affine_epsilon) {
        return std::nullopt;
    }
    axis_z /= axis_z_length;
    if (glm::dot(axis_z, column_z) < 0.0F) {
        return std::nullopt;
    }
    const glm::mat3 rotation(axis_x, axis_y, axis_z);
    const glm::quat orientation = glm::normalize(glm::quat_cast(rotation));
    SceneTransform transform{
        .translation = glm::vec3(matrix[3]),
        .rotation_degrees = glm::degrees(glm::eulerAngles(orientation)),
        .scale = scale,
    };
    transform.rotation_degrees.x = std::remainder(
        transform.rotation_degrees.x, 360.0F);
    transform.rotation_degrees.y = std::remainder(
        transform.rotation_degrees.y, 360.0F);
    transform.rotation_degrees.z = std::remainder(
        transform.rotation_degrees.z, 360.0F);
    if (!finite_transform(transform) ||
        transform.scale.x < minimum_scale ||
        transform.scale.y < minimum_scale ||
        transform.scale.z < minimum_scale) {
        return std::nullopt;
    }
    return transform;
}

} // namespace

glm::mat4 scene_transform_matrix(
    const SceneTransform& transform) noexcept {
    const glm::quat rotation = glm::normalize(
        glm::quat(glm::radians(transform.rotation_degrees)));
    return glm::translate(
               glm::mat4(1.0F), transform.translation) *
        glm::mat4_cast(rotation) *
        glm::scale(glm::mat4(1.0F), transform.scale);
}

std::optional<glm::mat4> scene_world_matrix(
    const SceneDocument& scene,
    SceneNodeId id) noexcept {
    if (!scene.valid(id)) {
        return std::nullopt;
    }
    glm::mat4 world(1.0F);
    std::uint32_t traversed = 0U;
    for (; id && traversed < max_scene_nodes;
         id = scene.parent(id), ++traversed) {
        const auto node = scene.node(id);
        if (!node || !finite_transform(node->get().transform)) {
            return std::nullopt;
        }
        world = scene_transform_matrix(node->get().transform) * world;
    }
    if (id || !finite_matrix(world)) {
        return std::nullopt;
    }
    return world;
}

std::optional<SceneTransform> scene_local_transform_from_world(
    const SceneDocument& scene,
    const SceneNodeId id,
    const glm::mat4& world_matrix) noexcept {
    if (!scene.valid(id) || !finite_matrix(world_matrix)) {
        return std::nullopt;
    }
    glm::mat4 local = world_matrix;
    const SceneNodeId parent = scene.parent(id);
    if (parent) {
        const auto parent_world = scene_world_matrix(scene, parent);
        if (!parent_world) {
            return std::nullopt;
        }
        const float determinant = glm::determinant(*parent_world);
        if (!std::isfinite(determinant) ||
            std::abs(determinant) <= std::numeric_limits<float>::epsilon()) {
            return std::nullopt;
        }
        local = glm::inverse(*parent_world) * world_matrix;
    }
    return decompose_transform(local);
}

SceneNodeId duplicate_scene_subtree(
    SceneDocument& scene,
    const SceneNodeId source,
    std::uint32_t& duplicated_nodes,
    std::string& error) {
    duplicated_nodes = 0U;
    error.clear();
    if (!scene.valid(source)) {
        error = "The source node handle is stale or invalid.";
        return {};
    }

    std::vector<SceneNodeRecord> records;
    std::vector<SceneHierarchyRow> rows;
    scene.build_node_records(records);
    scene.build_hierarchy_rows(rows, {}, SceneHierarchyScope::all);
    if (records.size() != rows.size()) {
        error = "Scene hierarchy records are inconsistent.";
        return {};
    }
    const auto source_iterator = std::find_if(
        rows.begin(), rows.end(),
        [source](const SceneHierarchyRow& row) {
            return row.id == source;
        });
    if (source_iterator == rows.end()) {
        error = "The source node is absent from the scene hierarchy.";
        return {};
    }
    const std::size_t begin = static_cast<std::size_t>(
        source_iterator - rows.begin());
    std::size_t end = begin + 1U;
    while (end < rows.size() && rows[end].depth > rows[begin].depth) {
        ++end;
    }
    const std::size_t subtree_size = end - begin;
    if (subtree_size > max_scene_nodes - scene.size()) {
        error = "The scene has no capacity for the duplicated subtree.";
        return {};
    }

    std::vector<SceneNodeId> copies(subtree_size);
    SceneNodeId duplicate_root{};
    for (std::size_t record_index = begin;
         record_index < end;
         ++record_index) {
        const SceneNodeRecord& record = records[record_index];
        SceneNodeId parent{};
        if (record_index == begin) {
            parent = scene.parent(source);
        } else {
            if (record.parent_record_index < begin ||
                record.parent_record_index >= record_index) {
                error = "The source subtree contains an invalid parent record.";
                static_cast<void>(scene.destroy_subtree(duplicate_root));
                return {};
            }
            parent = copies[record.parent_record_index - begin];
        }

        std::string name = record.value.name;
        if (record_index == begin &&
            name.size() <= max_scene_node_name_bytes - 5U) {
            name += " Copy";
        }
        const SceneNodeId copy = scene.create_node(
            name, record.value.kind, parent);
        if (!copy) {
            error = "Unable to allocate the complete duplicated subtree.";
            if (duplicate_root) {
                static_cast<void>(scene.destroy_subtree(duplicate_root));
            }
            return {};
        }
        if (!duplicate_root) {
            duplicate_root = copy;
        }
        copies[record_index - begin] = copy;
        const auto copy_node = scene.node(copy);
        if (!copy_node) {
            error = "A duplicated node became invalid.";
            static_cast<void>(scene.destroy_subtree(duplicate_root));
            return {};
        }
        copy_node->get() = record.value;
        copy_node->get().name = std::move(name);
    }
    duplicated_nodes = static_cast<std::uint32_t>(subtree_size);
    return duplicate_root;
}

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
