#include "scene_document.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vcr {
namespace {

[[nodiscard]] bool finite_transform(
    const SceneTransform& transform) noexcept {
    const auto finite = [](const glm::vec3& value) {
        return std::isfinite(value.x) &&
               std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    constexpr float minimum_scale = 0.0001F;
    return finite(transform.translation) &&
           finite(transform.rotation_degrees) &&
           finite(transform.scale) &&
           transform.scale.x >= minimum_scale &&
           transform.scale.y >= minimum_scale &&
           transform.scale.z >= minimum_scale;
}

[[nodiscard]] bool valid_utf8(
    const std::string_view text) noexcept {
    std::size_t index = 0U;
    while (index < text.size()) {
        const auto first =
            static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        std::uint32_t code_point = 0U;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1U;
            code_point = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2U;
            code_point = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (continuation_count > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U;
             offset <= continuation_count;
             ++offset) {
            const auto continuation =
                static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point =
                (code_point << 6U) |
                static_cast<std::uint32_t>(
                    continuation & 0x3FU);
        }
        const bool overlong =
            (continuation_count == 1U && code_point < 0x80U) ||
            (continuation_count == 2U && code_point < 0x800U) ||
            (continuation_count == 3U &&
             code_point < 0x10000U);
        if (overlong ||
            code_point > 0x10FFFFU ||
            (code_point >= 0xD800U &&
             code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] glm::quat rotation_quaternion(
    const glm::vec3& degrees) noexcept {
    return glm::normalize(glm::quat(glm::radians(degrees)));
}

[[nodiscard]] glm::mat4 local_matrix(
    const SceneTransform& transform) noexcept {
    return glm::translate(
               glm::mat4(1.0F), transform.translation) *
           glm::mat4_cast(
               rotation_quaternion(
                   transform.rotation_degrees)) *
           glm::scale(glm::mat4(1.0F), transform.scale);
}

[[nodiscard]] bool finite_matrix(
    const glm::mat4& matrix) noexcept {
    for (glm::length_t column = 0; column < 4; ++column) {
        for (glm::length_t row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] glm::vec4 matrix_row(
    const glm::mat4& matrix,
    const glm::length_t row) noexcept {
    return glm::vec4{
        matrix[0U][row],
        matrix[1U][row],
        matrix[2U][row],
        matrix[3U][row],
    };
}

} // namespace

SceneDocument::SceneDocument() {
    slots_.reserve(max_scene_nodes);
    free_indices_.reserve(max_scene_nodes);
    traversal_scratch_.reserve(max_scene_nodes);
}

SceneNodeId SceneDocument::create_node(
    std::string name,
    const SceneNodeKind kind,
    const SceneNodeId parent_id) {
    if (live_count_ >= max_scene_nodes ||
        name.size() > max_scene_node_name_bytes) {
        return {};
    }
    const std::uint32_t parent_index = parent_id
        ? (valid(parent_id)
            ? parent_id.index
            : invalid_scene_node_index)
        : invalid_scene_node_index;
    if (parent_id && parent_index == invalid_scene_node_index) {
        return {};
    }

    std::uint32_t index = invalid_scene_node_index;
    if (!free_indices_.empty()) {
        index = free_indices_.back();
        free_indices_.pop_back();
    } else {
        if (slots_.size() >= max_scene_nodes) {
            return {};
        }
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    }

    Slot& slot = slots_[index];
    slot.value = SceneNode{
        .name = std::move(name),
        .asset_path = {},
        .kind = kind,
    };
    slot.world_matrix = glm::mat4(1.0F);
    slot.world_revision = 0;
    slot.parent = invalid_scene_node_index;
    slot.first_child = invalid_scene_node_index;
    slot.last_child = invalid_scene_node_index;
    slot.previous_sibling = invalid_scene_node_index;
    slot.next_sibling = invalid_scene_node_index;
    slot.alive = true;
    slot.world_visible = true;
    link_last(index, parent_index);
    ++live_count_;
    ++structure_revision_;
    ++content_revision_;
    return id_for_index(index);
}

bool SceneDocument::destroy_subtree(const SceneNodeId id) {
    if (!valid(id)) {
        return false;
    }
    traversal_scratch_.clear();
    traversal_scratch_.push_back(id.index);
    for (std::size_t cursor = 0U;
         cursor < traversal_scratch_.size();
         ++cursor) {
        const std::uint32_t current = traversal_scratch_[cursor];
        for (std::uint32_t child = slots_[current].first_child;
             child != invalid_scene_node_index;
             child = slots_[child].next_sibling) {
            traversal_scratch_.push_back(child);
        }
    }

    unlink(id.index);
    for (auto iterator = traversal_scratch_.rbegin();
         iterator != traversal_scratch_.rend();
         ++iterator) {
        Slot& slot = slots_[*iterator];
        slot.value = {};
        slot.world_matrix = glm::mat4(1.0F);
        slot.parent = invalid_scene_node_index;
        slot.first_child = invalid_scene_node_index;
        slot.last_child = invalid_scene_node_index;
        slot.previous_sibling = invalid_scene_node_index;
        slot.next_sibling = invalid_scene_node_index;
        slot.alive = false;
        slot.world_visible = false;
        ++slot.generation;
        if (slot.generation == 0U) {
            // Generation zero is the invalid sentinel. Retire the slot instead
            // of making a stale handle valid again after wraparound.
            slot.generation = std::numeric_limits<std::uint32_t>::max();
        } else {
            free_indices_.push_back(*iterator);
        }
    }
    live_count_ -= static_cast<std::uint32_t>(
        traversal_scratch_.size());
    ++structure_revision_;
    ++content_revision_;
    return true;
}

bool SceneDocument::reparent(
    const SceneNodeId id,
    const SceneNodeId new_parent_id) {
    if (!valid(id)) {
        return false;
    }
    const std::uint32_t new_parent = new_parent_id
        ? (valid(new_parent_id)
            ? new_parent_id.index
            : invalid_scene_node_index)
        : invalid_scene_node_index;
    if (new_parent_id && new_parent == invalid_scene_node_index) {
        return false;
    }
    if (id.index == new_parent ||
        would_create_cycle(id.index, new_parent)) {
        return false;
    }
    if (slots_[id.index].parent == new_parent) {
        return true;
    }
    unlink(id.index);
    link_last(id.index, new_parent);
    ++structure_revision_;
    ++content_revision_;
    return true;
}

bool SceneDocument::rename_node(
    const SceneNodeId id,
    std::string name) {
    if (!valid(id) ||
        name.size() > max_scene_node_name_bytes ||
        slots_[id.index].value.name == name) {
        return false;
    }
    slots_[id.index].value.name = std::move(name);
    // Names affect hierarchy labels and filtering, but never GPU content.
    ++structure_revision_;
    return true;
}

bool SceneDocument::valid(const SceneNodeId id) const noexcept {
    return id.index < slots_.size() &&
           id.generation != 0U &&
           slots_[id.index].alive &&
           slots_[id.index].generation == id.generation;
}

std::optional<std::reference_wrapper<SceneNode>>
SceneDocument::node(const SceneNodeId id) noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return std::ref(slots_[id.index].value);
}

std::optional<std::reference_wrapper<const SceneNode>>
SceneDocument::node(const SceneNodeId id) const noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return std::cref(slots_[id.index].value);
}

SceneNodeId SceneDocument::parent(const SceneNodeId id) const noexcept {
    if (!valid(id)) {
        return {};
    }
    return id_for_index(slots_[id.index].parent);
}

void SceneDocument::mark_content_changed() noexcept {
    ++content_revision_;
}

void SceneDocument::set_expanded(
    const SceneNodeId id,
    const bool expanded) noexcept {
    if (!valid(id) || slots_[id.index].value.expanded == expanded) {
        return;
    }
    slots_[id.index].value.expanded = expanded;
    ++structure_revision_;
}

void SceneDocument::build_hierarchy_rows(
    std::vector<SceneHierarchyRow>& output,
    const std::string_view filter) const {
    output.clear();
    const bool filtering = !filter.empty();
    std::uint32_t index = first_root_;
    std::uint32_t depth = 0U;
    while (index != invalid_scene_node_index) {
        const Slot& slot = slots_[index];
        if (!filtering || name_matches(slot.value.name, filter)) {
            output.push_back(SceneHierarchyRow{
                .id = id_for_index(index),
                // Filtered results form a flat search view. Keeping the
                // original depth while omitting ancestors can place a deep
                // match thousands of indent levels outside the window.
                .depth = filtering ? 0U : depth,
                .has_children =
                    slot.first_child != invalid_scene_node_index,
            });
        }
        const bool descend = filtering || slot.value.expanded;
        index = traversal_next(index, depth, descend);
    }
}

void SceneDocument::build_node_records(
    std::vector<SceneNodeRecord>& output) const {
    output.clear();
    output.reserve(live_count_);
    std::vector<std::uint32_t> ancestor_records;
    ancestor_records.reserve(live_count_);

    std::uint32_t index = first_root_;
    std::uint32_t depth = 0U;
    while (index != invalid_scene_node_index) {
        const std::uint32_t record_index =
            static_cast<std::uint32_t>(output.size());
        const std::uint32_t parent_record =
            depth == 0U
            ? invalid_scene_node_index
            : ancestor_records[
                  static_cast<std::size_t>(depth - 1U)];
        output.push_back(SceneNodeRecord{
            .value = slots_[index].value,
            .parent_record_index = parent_record,
        });
        if (ancestor_records.size() <= depth) {
            ancestor_records.push_back(record_index);
        } else {
            ancestor_records[depth] = record_index;
        }
        index = traversal_next(index, depth, true);
    }
}

SceneRenderView SceneDocument::build_render_view(
    std::vector<SceneGpuInstance>& cube_output,
    std::vector<SceneModelInstance>& model_output) {
    cube_output.clear();
    last_transform_updates_ = 0;
    model_output.clear();
    render_cube_ids_.clear();
    render_cube_ids_.reserve(max_scene_nodes);
    SceneNodeId ground_id{};
    SceneGpuInstance ground{};
    bool has_ground = false;
    bool terrain_visible = false;
    bool sky_visible = false;
    bool sun_visible = false;
    std::uint32_t index = first_root_;
    std::uint32_t depth = 0U;
    while (index != invalid_scene_node_index) {
        Slot& slot = slots_[index];
        const SceneNodeId parent_id = id_for_index(slot.parent);
        const auto parent_revision = parent_id ? slots_[slot.parent].world_revision : 0;
        // The public editor API exposes mutable node values. Compare the local
        // value at this boundary; descendants inherit a parent's new revision.
        if (slot.world_revision == 0 || slot.cached_transform != slot.value.transform ||
            slot.cached_visible != slot.value.visible || slot.cached_parent != parent_id ||
            slot.cached_parent_revision != parent_revision) {
            bool world_visible = slot.value.visible;
            glm::mat4 world_matrix(1.0F);
            if (!finite_transform(slot.value.transform)) {
                world_visible = false;
            } else {
                world_matrix = local_matrix(slot.value.transform);
                if (slot.parent != invalid_scene_node_index) {
                    const Slot &parent_slot = slots_[slot.parent];
                    world_matrix = parent_slot.world_matrix * world_matrix;
                    world_visible = world_visible && parent_slot.world_visible;
                }
                if (!finite_matrix(world_matrix)) {
                    world_matrix = glm::mat4(1.0F);
                    world_visible = false;
                }
            }
            slot.world_matrix = world_matrix;
            slot.world_visible = world_visible;
            slot.cached_transform = slot.value.transform;
            slot.cached_visible = slot.value.visible;
            slot.cached_parent = parent_id;
            slot.cached_parent_revision = parent_revision;
            ++slot.world_revision;
            ++last_transform_updates_;
        }
        const auto &world_matrix = slot.world_matrix;
        const bool world_visible = slot.world_visible;

        if (world_visible) {
            terrain_visible =
                terrain_visible ||
                slot.value.kind == SceneNodeKind::terrain;
            sky_visible =
                sky_visible ||
                slot.value.kind == SceneNodeKind::environment;
            sun_visible =
                sun_visible ||
                slot.value.kind ==
                    SceneNodeKind::directional_light;
        }
        if (world_visible &&
            (slot.value.kind == SceneNodeKind::cube ||
             slot.value.kind == SceneNodeKind::ground)) {
            const SceneGpuInstance instance{
                .model_row_0 = matrix_row(world_matrix, 0),
                .model_row_1 = matrix_row(world_matrix, 1),
                .model_row_2 = matrix_row(world_matrix, 2),
                .material =
                    sanitize_pbr_material(slot.value.material),
            };
            if (slot.value.kind == SceneNodeKind::ground) {
                if (!has_ground) {
                    ground = instance;
                    ground_id = id_for_index(index);
                    has_ground = true;
                }
            } else if (cube_output.size() < max_scene_nodes) {
                cube_output.push_back(instance);
                render_cube_ids_.push_back(id_for_index(index));
            }
        } else if (world_visible &&
                   slot.value.kind == SceneNodeKind::model &&
                   valid_scene_asset_path(slot.value.asset_path) &&
                   model_output.size() < max_scene_nodes) {
            model_output.push_back(SceneModelInstance{
                .model_row_0 = matrix_row(world_matrix, 0),
                .model_row_1 = matrix_row(world_matrix, 1),
                .model_row_2 = matrix_row(world_matrix, 2),
                .asset_path = slot.value.asset_path,
                .id = id_for_index(index),
            });
        }
        index = traversal_next(index, depth, true);
    }
    return SceneRenderView{
        .ground = ground,
        .cubes = cube_output,
        .models = model_output,
        .revision = content_revision_,
        .has_ground = has_ground,
        .terrain_visible = terrain_visible,
        .sky_visible = sky_visible,
        .sun_visible = sun_visible,
        .ground_id = ground_id,
        .cube_ids = render_cube_ids_,
    };
}

SceneDocument SceneDocument::make_default() {
    SceneDocument scene;
    const SceneNodeId world = scene.create_node(
        "World", SceneNodeKind::folder);
    const SceneNodeId environment = scene.create_node(
        "Environment", SceneNodeKind::folder, world);
    static_cast<void>(scene.create_node(
        "Sky", SceneNodeKind::environment, environment));
    static_cast<void>(scene.create_node(
        "Sun", SceneNodeKind::directional_light, environment));
    static_cast<void>(scene.create_node(
        "Landscape", SceneNodeKind::terrain, world));

    const SceneNodeId geometry = scene.create_node(
        "Geometry", SceneNodeKind::folder, world);
    const SceneNodeId ground = scene.create_node(
        "Ground", SceneNodeKind::ground, geometry);
    if (auto value = scene.node(ground)) {
        value->get().transform.translation =
            glm::vec3(0.0F, -4.0F, -7.0F);
        value->get().transform.scale = glm::vec3(31.0F, 1.0F, 27.0F);
        value->get().material = PbrMaterial{
            .base_color =
                glm::vec4(0.13F, 0.16F, 0.08F, 1.0F),
            .metallic = 0.0F,
            .roughness = 0.92F,
            .ambient_occlusion = 1.0F,
        };
    }

    constexpr std::array<glm::vec3, 15> positions{{
        {-5.0F, -2.8F, -3.0F},
        {-2.5F, -2.4F, -5.0F},
        {0.0F, -2.0F, -7.0F},
        {2.5F, -1.6F, -9.0F},
        {5.0F, -1.2F, -11.0F},
        {-6.0F, 0.0F, -10.0F},
        {-3.0F, 0.4F, -12.0F},
        {0.0F, 0.8F, -14.0F},
        {3.0F, 1.2F, -16.0F},
        {6.0F, 1.6F, -18.0F},
        {-4.0F, 3.0F, -18.0F},
        {-2.0F, 3.4F, -20.0F},
        {0.0F, 3.8F, -22.0F},
        {2.0F, 4.2F, -24.0F},
        {4.0F, 4.6F, -26.0F},
    }};
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        const SceneNodeId cube = scene.create_node(
            "Cube " + std::to_string(index + 1U),
            SceneNodeKind::cube,
            geometry);
        if (auto value = scene.node(cube)) {
            value->get().transform.translation = positions[index];
            const float scale =
                0.45F + static_cast<float>(index % 4U) * 0.08F;
            value->get().transform.scale = glm::vec3(scale);
            value->get().transform.rotation_degrees =
                glm::vec3(0.0F, static_cast<float>(index) * 17.0F, 0.0F);
            value->get().material = PbrMaterial{
                .base_color = glm::vec4(
                    0.18F +
                        0.045F * static_cast<float>(index % 5U),
                    0.48F +
                        0.06F * static_cast<float>(index % 3U),
                    0.82F -
                        0.04F * static_cast<float>(index % 4U),
                    1.0F),
                .metallic =
                    index >= positions.size() / 2U ? 1.0F : 0.0F,
                .roughness =
                    0.08F +
                    0.21F * static_cast<float>(index % 5U),
                .ambient_occlusion = 1.0F,
            };
        }
    }
    static_cast<void>(scene.create_node(
        "Main Camera", SceneNodeKind::camera, world));
    scene.mark_content_changed();
    return scene;
}

SceneNodeId SceneDocument::id_for_index(
    const std::uint32_t index) const noexcept {
    if (index == invalid_scene_node_index ||
        index >= slots_.size() ||
        !slots_[index].alive) {
        return {};
    }
    return SceneNodeId{
        .index = index,
        .generation = slots_[index].generation,
    };
}

bool SceneDocument::would_create_cycle(
    const std::uint32_t child,
    std::uint32_t new_parent) const noexcept {
    std::uint32_t depth = 0U;
    while (new_parent != invalid_scene_node_index &&
           depth <= live_count_) {
        if (new_parent == child) {
            return true;
        }
        new_parent = slots_[new_parent].parent;
        ++depth;
    }
    return depth > live_count_;
}

void SceneDocument::unlink(const std::uint32_t index) noexcept {
    Slot& slot = slots_[index];
    std::uint32_t& first = slot.parent == invalid_scene_node_index
        ? first_root_
        : slots_[slot.parent].first_child;
    std::uint32_t& last = slot.parent == invalid_scene_node_index
        ? last_root_
        : slots_[slot.parent].last_child;
    if (slot.previous_sibling != invalid_scene_node_index) {
        slots_[slot.previous_sibling].next_sibling =
            slot.next_sibling;
    } else {
        first = slot.next_sibling;
    }
    if (slot.next_sibling != invalid_scene_node_index) {
        slots_[slot.next_sibling].previous_sibling =
            slot.previous_sibling;
    } else {
        last = slot.previous_sibling;
    }
    slot.parent = invalid_scene_node_index;
    slot.previous_sibling = invalid_scene_node_index;
    slot.next_sibling = invalid_scene_node_index;
}

void SceneDocument::link_last(
    const std::uint32_t index,
    const std::uint32_t parent_index) noexcept {
    Slot& slot = slots_[index];
    slot.parent = parent_index;
    std::uint32_t& first = parent_index == invalid_scene_node_index
        ? first_root_
        : slots_[parent_index].first_child;
    std::uint32_t& last = parent_index == invalid_scene_node_index
        ? last_root_
        : slots_[parent_index].last_child;
    slot.previous_sibling = last;
    slot.next_sibling = invalid_scene_node_index;
    if (last != invalid_scene_node_index) {
        slots_[last].next_sibling = index;
    } else {
        first = index;
    }
    last = index;
}

std::uint32_t SceneDocument::traversal_next(
    const std::uint32_t index,
    std::uint32_t& depth,
    const bool descend) const noexcept {
    if (descend &&
        slots_[index].first_child != invalid_scene_node_index) {
        ++depth;
        return slots_[index].first_child;
    }
    std::uint32_t current = index;
    while (current != invalid_scene_node_index) {
        if (slots_[current].next_sibling != invalid_scene_node_index) {
            return slots_[current].next_sibling;
        }
        current = slots_[current].parent;
        if (current != invalid_scene_node_index && depth > 0U) {
            --depth;
        }
    }
    return invalid_scene_node_index;
}

bool SceneDocument::name_matches(
    const std::string_view name,
    const std::string_view filter) const noexcept {
    if (filter.empty()) {
        return true;
    }
    const auto equal_folded = [](const char lhs, const char rhs) {
        const auto fold = [](const char value) {
            return value >= 'A' && value <= 'Z'
                ? static_cast<char>(value - 'A' + 'a')
                : value;
        };
        return fold(lhs) == fold(rhs);
    };
    return std::search(
               name.begin(),
               name.end(),
               filter.begin(),
               filter.end(),
               equal_folded) != name.end();
}

std::string_view to_string(const SceneNodeKind kind) noexcept {
    switch (kind) {
    case SceneNodeKind::folder: return "Folder";
    case SceneNodeKind::cube: return "Cube";
    case SceneNodeKind::model: return "Model";
    case SceneNodeKind::ground: return "Ground";
    case SceneNodeKind::terrain: return "Terrain";
    case SceneNodeKind::camera: return "Camera";
    case SceneNodeKind::directional_light: return "Directional light";
    case SceneNodeKind::environment: return "Environment";
    }
    return "Unknown";
}

bool valid_scene_asset_path(
    const std::string_view path) noexcept {
    constexpr std::string_view extension = ".vcrasset.json";
    if (path.empty() ||
        path.size() > max_scene_asset_path_bytes ||
        !path.ends_with(extension) ||
        !valid_utf8(path) ||
        path.front() == '/' ||
        path.front() == '\\') {
        return false;
    }

    std::size_t segment_begin = 0U;
    for (std::size_t index = 0U; index <= path.size(); ++index) {
        const bool at_end = index == path.size();
        if (!at_end) {
            const auto value =
                static_cast<unsigned char>(path[index]);
            if (value == 0U ||
                value < 0x20U ||
                path[index] == '\\' ||
                path[index] == ':') {
                return false;
            }
        }
        if (!at_end && path[index] != '/') {
            continue;
        }
        const std::string_view segment =
            path.substr(segment_begin, index - segment_begin);
        if (segment.empty() ||
            segment == "." ||
            segment == "..") {
            return false;
        }
        segment_begin = index + 1U;
    }
    return true;
}

} // namespace vcr
