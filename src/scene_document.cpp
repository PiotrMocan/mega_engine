#include "scene_document.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace vcr {
namespace {

[[nodiscard]] std::size_t hash_scene_uuid(const SceneUuid uuid) noexcept {
    std::uint64_t mixed =
        uuid.high ^ (uuid.low * 0x9E3779B97F4A7C15ULL);
    mixed ^= mixed >> 33U;
    mixed *= 0xFF51AFD7ED558CCDULL;
    mixed ^= mixed >> 33U;
    return static_cast<std::size_t>(mixed);
}

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

std::optional<SceneCameraPose> SceneDocument::startup_camera_pose() const noexcept {
    if (!valid(startup_camera) || node(startup_camera)->get().kind != SceneNodeKind::camera)
        return std::nullopt;
    glm::mat4 world(1.0F);
    for (auto id = startup_camera; id; id = parent(id)) {
        const auto &value = node(id)->get();
        if (!finite_transform(value.transform))
            return std::nullopt;
        world = local_matrix(value.transform) * world;
    }
    if (!finite_matrix(world))
        return std::nullopt;
    // Double precision avoids overflow while normalizing large valid scene transforms.
    const glm::dvec3 direction = -glm::dvec3(world[2]);
    const double length = glm::length(direction);
    if (!std::isfinite(length) || length <= 0)
        return std::nullopt;
    const auto forward = direction / length;
    return SceneCameraPose{
        glm::vec3(world[3]),
        static_cast<float>(std::atan2(forward.z, forward.x)),
        static_cast<float>(std::asin(std::clamp(forward.y, -1.0, 1.0)))};
}

SceneDocument::SceneDocument() {
    slots_.reserve(max_scene_nodes);
    free_indices_.reserve(max_scene_nodes);
    traversal_scratch_.reserve(max_scene_nodes);
}

SceneNodeId SceneDocument::create_node(
    std::string name,
    const SceneNodeKind kind,
    const SceneNodeId parent_id) {
    SceneUuid generated = generate_scene_uuid();
    for (std::uint32_t attempt = 0U;
         find_by_uuid(generated) && attempt < 64U;
         ++attempt) {
        generated = generate_scene_uuid();
    }
    return create_node_with_uuid(
        std::move(name), kind, generated, parent_id);
}

SceneNodeId SceneDocument::create_node_with_uuid(
    std::string name,
    const SceneNodeKind kind,
    const SceneUuid node_uuid,
    const SceneNodeId parent_id) {
    if (!node_uuid ||
        live_count_ >= max_scene_nodes ||
        name.size() > max_scene_node_name_bytes ||
        find_by_uuid(node_uuid)) {
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

    ensure_uuid_table_capacity();

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
    slot.uuid = node_uuid;
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
    insert_uuid_bucket(node_uuid, index);
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
        erase_uuid_bucket(slot.uuid, *iterator);
        colliders_.remove_slot(*iterator);
        rigid_bodies_.remove_slot(*iterator);
        prefab_instances_.remove_slot(*iterator);
        audio_sources_.remove_slot(*iterator);
        audio_listeners_.remove_slot(*iterator);
        reverb_zones_.remove_slot(*iterator);
        slot.uuid = {};
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

std::optional<SceneUuid> SceneDocument::uuid(
    const SceneNodeId id) const noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return slots_[id.index].uuid;
}

SceneNodeId SceneDocument::find_by_uuid(
    const SceneUuid target) const noexcept {
    if (!target || uuid_buckets_.empty()) {
        return {};
    }
    const std::size_t mask = uuid_buckets_.size() - 1U;
    std::size_t pos = hash_scene_uuid(target) & mask;
    for (std::size_t step = 0U; step < uuid_buckets_.size(); ++step) {
        const std::uint32_t entry = uuid_buckets_[pos];
        if (entry == uuid_bucket_empty) {
            return {};
        }
        if (entry != uuid_bucket_tombstone &&
            entry < slots_.size() &&
            slots_[entry].alive &&
            slots_[entry].uuid == target) {
            return id_for_index(entry);
        }
        pos = (pos + 1U) & mask;
    }
    return {};
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

bool SceneDocument::set_collider(
    const SceneNodeId id,
    const ColliderComponent& collider_value) {
    if (!valid(id) || !valid_collider_component(collider_value)) {
        return false;
    }
    if (const auto existing = colliders_.get(id);
        existing && existing->get() == collider_value) {
        return true;
    }
    if (!colliders_.upsert(id, collider_value)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::remove_collider(const SceneNodeId id) noexcept {
    if (!valid(id) || !colliders_.remove(id)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::has_collider(const SceneNodeId id) const noexcept {
    return valid(id) && colliders_.contains(id);
}

std::optional<std::reference_wrapper<const ColliderComponent>>
SceneDocument::collider(const SceneNodeId id) const noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return colliders_.get(id);
}

std::optional<std::reference_wrapper<ColliderComponent>>
SceneDocument::collider_mut(const SceneNodeId id) noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return colliders_.get_mut(id);
}

bool SceneDocument::set_rigid_body(
    const SceneNodeId id,
    const RigidBodyComponent& rigid_body_value) {
    if (!valid(id) || !valid_rigid_body_component(rigid_body_value)) {
        return false;
    }
    if (const auto existing = rigid_bodies_.get(id);
        existing && existing->get() == rigid_body_value) {
        return true;
    }
    if (!rigid_bodies_.upsert(id, rigid_body_value)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::remove_rigid_body(const SceneNodeId id) noexcept {
    if (!valid(id) || !rigid_bodies_.remove(id)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::has_rigid_body(const SceneNodeId id) const noexcept {
    return valid(id) && rigid_bodies_.contains(id);
}

std::optional<std::reference_wrapper<const RigidBodyComponent>>
SceneDocument::rigid_body(const SceneNodeId id) const noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return rigid_bodies_.get(id);
}

std::optional<std::reference_wrapper<RigidBodyComponent>>
SceneDocument::rigid_body_mut(const SceneNodeId id) noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return rigid_bodies_.get_mut(id);
}

bool SceneDocument::set_prefab_instance(
    const SceneNodeId id,
    const PrefabInstanceComponent& prefab_value) {
    if (!valid(id) || !valid_prefab_instance_component(prefab_value)) {
        return false;
    }
    if (const auto existing = prefab_instances_.get(id);
        existing && existing->get() == prefab_value) {
        return true;
    }
    if (!prefab_instances_.upsert(id, prefab_value)) {
        return false;
    }
    ++structure_revision_;
    return true;
}

bool SceneDocument::remove_prefab_instance(const SceneNodeId id) noexcept {
    if (!valid(id) || !prefab_instances_.remove(id)) {
        return false;
    }
    ++structure_revision_;
    return true;
}

bool SceneDocument::has_prefab_instance(const SceneNodeId id) const noexcept {
    return valid(id) && prefab_instances_.contains(id);
}

std::optional<std::reference_wrapper<const PrefabInstanceComponent>>
SceneDocument::prefab_instance(const SceneNodeId id) const noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return prefab_instances_.get(id);
}

SceneNodeId SceneDocument::enclosing_prefab_root(
    SceneNodeId current) const noexcept {
    if (!valid(current)) {
        return {};
    }
    SceneNodeId fallback{};
    std::uint32_t steps = 0U;
    while (current && steps < max_scene_nodes) {
        if (const auto inst = prefab_instances_.get(current)) {
            if (inst->get().is_prefab_root) {
                return current;
            }
            if (!fallback) {
                fallback = current;
            }
        }
        current = parent(current);
        ++steps;
    }
    return fallback;
}

std::uint32_t SceneDocument::unpack_prefab_instance(const SceneNodeId id) {
    const SceneNodeId root = enclosing_prefab_root(id);
    if (!root) {
        return 0U;
    }
    traversal_scratch_.clear();
    traversal_scratch_.push_back(root.index);
    for (std::size_t cursor = 0U;
         cursor < traversal_scratch_.size();
         ++cursor) {
        const std::uint32_t current = traversal_scratch_[cursor];
        for (std::uint32_t child = slots_[current].first_child;
             child != invalid_scene_node_index;
             child = slots_[child].next_sibling) {
            const SceneNodeId child_id = id_for_index(child);
            if (const auto child_inst = prefab_instances_.get(child_id);
                child_inst && child_inst->get().is_prefab_root) {
                continue;
            }
            traversal_scratch_.push_back(child);
        }
    }
    std::uint32_t removed = 0U;
    for (const std::uint32_t slot_index : traversal_scratch_) {
        const SceneNodeId target_id = id_for_index(slot_index);
        if (prefab_instances_.remove(target_id)) {
            ++removed;
        }
    }
    if (removed > 0U) {
        ++structure_revision_;
    }
    return removed;
}

bool SceneDocument::set_audio_source(
    const SceneNodeId id,
    const AudioSourceComponent& audio_source_value) {
    if (!valid(id) || !valid_audio_source_component(audio_source_value)) {
        return false;
    }
    if (const auto existing = audio_sources_.get(id);
        existing && existing->get() == audio_source_value) {
        return true;
    }
    if (!audio_sources_.upsert(id, audio_source_value)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::remove_audio_source(const SceneNodeId id) noexcept {
    if (!valid(id) || !audio_sources_.remove(id)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::has_audio_source(const SceneNodeId id) const noexcept {
    return valid(id) && audio_sources_.contains(id);
}

std::optional<std::reference_wrapper<const AudioSourceComponent>>
SceneDocument::audio_source(const SceneNodeId id) const noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return audio_sources_.get(id);
}

std::optional<std::reference_wrapper<AudioSourceComponent>>
SceneDocument::audio_source_mut(const SceneNodeId id) noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return audio_sources_.get_mut(id);
}

bool SceneDocument::set_audio_listener(
    const SceneNodeId id,
    const AudioListenerComponent& listener_value) {
    if (!valid(id) || !valid_audio_listener_component(listener_value)) {
        return false;
    }
    if (const auto existing = audio_listeners_.get(id);
        existing && existing->get() == listener_value) {
        return true;
    }
    if (!audio_listeners_.upsert(id, listener_value)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::remove_audio_listener(const SceneNodeId id) noexcept {
    if (!valid(id) || !audio_listeners_.remove(id)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::has_audio_listener(const SceneNodeId id) const noexcept {
    return valid(id) && audio_listeners_.contains(id);
}

std::optional<std::reference_wrapper<const AudioListenerComponent>>
SceneDocument::audio_listener(const SceneNodeId id) const noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return audio_listeners_.get(id);
}

std::optional<std::reference_wrapper<AudioListenerComponent>>
SceneDocument::audio_listener_mut(const SceneNodeId id) noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return audio_listeners_.get_mut(id);
}

bool SceneDocument::set_reverb_zone(
    const SceneNodeId id,
    const AudioReverbZoneComponent& zone_value) {
    if (!valid(id) || !valid_audio_reverb_zone_component(zone_value)) {
        return false;
    }
    if (const auto existing = reverb_zones_.get(id);
        existing && existing->get() == zone_value) {
        return true;
    }
    if (!reverb_zones_.upsert(id, zone_value)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::remove_reverb_zone(const SceneNodeId id) noexcept {
    if (!valid(id) || !reverb_zones_.remove(id)) {
        return false;
    }
    ++content_revision_;
    return true;
}

bool SceneDocument::has_reverb_zone(const SceneNodeId id) const noexcept {
    return valid(id) && reverb_zones_.contains(id);
}

std::optional<std::reference_wrapper<const AudioReverbZoneComponent>>
SceneDocument::reverb_zone(const SceneNodeId id) const noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return reverb_zones_.get(id);
}

std::optional<std::reference_wrapper<AudioReverbZoneComponent>>
SceneDocument::reverb_zone_mut(const SceneNodeId id) noexcept {
    if (!valid(id)) {
        return std::nullopt;
    }
    return reverb_zones_.get_mut(id);
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
    const std::string_view filter,
    const SceneHierarchyScope scope) const {
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
        const bool descend = filtering || scope == SceneHierarchyScope::all || slot.value.expanded;
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
        const SceneNodeId current_id = id_for_index(index);
        const std::uint32_t record_index =
            static_cast<std::uint32_t>(output.size());
        const std::uint32_t parent_record =
            depth == 0U
            ? invalid_scene_node_index
            : ancestor_records[
                  static_cast<std::size_t>(depth - 1U)];
        std::optional<ColliderComponent> node_collider{};
        if (const auto attached = colliders_.get(current_id)) {
            node_collider = attached->get();
        }
        std::optional<RigidBodyComponent> node_rigid_body{};
        if (const auto attached_rb = rigid_bodies_.get(current_id)) {
            node_rigid_body = attached_rb->get();
        }
        std::optional<PrefabInstanceComponent> node_prefab{};
        if (const auto attached_pf = prefab_instances_.get(current_id)) {
            node_prefab = attached_pf->get();
        }
        std::optional<AudioSourceComponent> node_audio_src{};
        if (const auto attached_as = audio_sources_.get(current_id)) {
            node_audio_src = attached_as->get();
        }
        std::optional<AudioListenerComponent> node_audio_lst{};
        if (const auto attached_al = audio_listeners_.get(current_id)) {
            node_audio_lst = attached_al->get();
        }
        std::optional<AudioReverbZoneComponent> node_reverb{};
        if (const auto attached_rz = reverb_zones_.get(current_id)) {
            node_reverb = attached_rz->get();
        }
        output.push_back(SceneNodeRecord{
            .id = current_id,
            .uuid = slots_[index].uuid,
            .value = slots_[index].value,
            .collider = node_collider,
            .rigid_body = node_rigid_body,
            .prefab_instance = node_prefab,
            .audio_source = node_audio_src,
            .audio_listener = node_audio_lst,
            .reverb_zone = node_reverb,
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

void SceneDocument::build_world_nodes(
    std::vector<SceneWorldNode>& output,
    std::vector<std::uint32_t>& ancestor_scratch) const {
    output.clear();
    ancestor_scratch.clear();
    if (output.capacity() < live_count_) {
        output.reserve(live_count_);
    }
    if (ancestor_scratch.capacity() < live_count_) {
        ancestor_scratch.reserve(live_count_);
    }

    std::uint32_t index = first_root_;
    std::uint32_t depth = 0U;
    while (index != invalid_scene_node_index) {
        const Slot& slot = slots_[index];
        bool world_visible = slot.value.visible;
        glm::mat4 world_matrix(1.0F);
        if (!finite_transform(slot.value.transform)) {
            world_visible = false;
        } else {
            world_matrix = local_matrix(slot.value.transform);
            if (depth > 0U) {
                const SceneWorldNode& parent_world =
                    output[ancestor_scratch[depth - 1U]];
                world_matrix =
                    parent_world.world_matrix * world_matrix;
                world_visible =
                    world_visible && parent_world.world_visible;
            }
            if (!finite_matrix(world_matrix)) {
                world_matrix = glm::mat4(1.0F);
                world_visible = false;
            }
        }
        const auto record_index =
            static_cast<std::uint32_t>(output.size());
        output.push_back(SceneWorldNode{
            .id = id_for_index(index),
            .uuid = slot.uuid,
            .world_matrix = world_matrix,
            .kind = slot.value.kind,
            .world_visible = world_visible,
        });
        if (ancestor_scratch.size() <= depth) {
            ancestor_scratch.push_back(record_index);
        } else {
            ancestor_scratch[depth] = record_index;
        }
        index = traversal_next(index, depth, true);
    }
}

SceneRenderView SceneDocument::build_render_view(
    std::vector<SceneGpuInstance>& cube_output,
    std::vector<SceneModelInstance>& model_output,
    std::vector<ScenePunctualLight>& light_output) {
    cube_output.clear();
    last_transform_updates_ = 0;
    model_output.clear();
    light_output.clear();
    render_cube_ids_.clear();
    render_cube_ids_.reserve(max_scene_nodes);
    SceneNodeId ground_id{};
    SceneGpuInstance ground{};
    bool has_ground = false;
    bool terrain_visible = false;
    bool sky_visible = false;
    bool sun_visible = false;
    bool ocean_visible = false;
    float ocean_height = 0.0F;
    OceanSettings ocean{};
    bool local_lights_overflow = false;
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
            // One infinite ocean; the first visible, valid node owns its settings.
            if (slot.value.kind == SceneNodeKind::ocean && !ocean_visible &&
                valid_ocean_settings(slot.value.ocean)) {
                ocean_visible = true;
                ocean_height = world_matrix[3].y;
                ocean = slot.value.ocean;
            }
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
                   (slot.value.kind == SceneNodeKind::point_light ||
                    slot.value.kind == SceneNodeKind::spot_light)) {
            const PunctualLightType type =
                slot.value.kind == SceneNodeKind::spot_light
                    ? PunctualLightType::spot
                    : PunctualLightType::point;
            GpuPunctualLight gpu{};
            if (make_gpu_punctual_light(
                    slot.value.light, type, world_matrix, gpu)) {
                if (light_output.size() < maximum_local_lights) {
                    light_output.push_back(ScenePunctualLight{
                        .gpu = gpu,
                        .id = id_for_index(index),
                    });
                } else {
                    local_lights_overflow = true;
                }
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
        .local_lights = light_output,
        .revision = content_revision_,
        .has_ground = has_ground,
        .terrain_visible = terrain_visible,
        .sky_visible = sky_visible,
        .sun_visible = sun_visible,
        .ocean_visible = ocean_visible,
        .ocean_height = ocean_height,
        .ocean = ocean,
        .local_lights_overflow = local_lights_overflow,
        .ground_id = ground_id,
        .cube_ids = render_cube_ids_,
    };
}

std::uint32_t SceneDocument::local_light_count() const noexcept {
    return static_cast<std::uint32_t>(std::count_if(
        slots_.begin(), slots_.end(), [](const Slot& slot) {
            return slot.alive &&
                (slot.value.kind == SceneNodeKind::point_light ||
                 slot.value.kind == SceneNodeKind::spot_light);
        }));
}

SceneDocument SceneDocument::make_default() {
    constexpr std::uint64_t default_scene_seed =
        0x5643525F44464C54ULL;
    std::uint64_t next_uuid_seq = 1U;
    SceneDocument scene;
    const auto make_node = [&](std::string name,
                               const SceneNodeKind kind,
                               const SceneNodeId parent_node = {}) {
        return scene.create_node_with_uuid(
            std::move(name),
            kind,
            deterministic_scene_uuid(
                default_scene_seed, next_uuid_seq++),
            parent_node);
    };
    const SceneNodeId world = make_node(
        "World", SceneNodeKind::folder);
    const SceneNodeId environment = make_node(
        "Environment", SceneNodeKind::folder, world);
    static_cast<void>(make_node(
        "Sky", SceneNodeKind::environment, environment));
    static_cast<void>(make_node(
        "Sun", SceneNodeKind::directional_light, environment));
    static_cast<void>(make_node(
        "Landscape", SceneNodeKind::terrain, world));

    const SceneNodeId geometry = make_node(
        "Geometry", SceneNodeKind::folder, world);
    const SceneNodeId ground = make_node(
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
        const SceneNodeId cube = make_node(
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
    static_cast<void>(make_node(
        "Main Camera", SceneNodeKind::camera, world));
    scene.mark_content_changed();
    return scene;
}

void SceneDocument::ensure_uuid_table_capacity() {
    constexpr std::size_t minimum_buckets = 64U;
    if (uuid_buckets_.empty()) {
        rehash_uuid_table(minimum_buckets);
        return;
    }
    const std::size_t occupied =
        static_cast<std::size_t>(live_count_) + 1U + uuid_tombstones_;
    if (occupied * 10U < uuid_buckets_.size() * 7U) {
        return;
    }
    const std::size_t target_live =
        static_cast<std::size_t>(live_count_) + 1U;
    std::size_t next_capacity = uuid_buckets_.size();
    while (target_live * 10U >= next_capacity * 5U) {
        next_capacity *= 2U;
    }
    rehash_uuid_table(next_capacity);
}

void SceneDocument::rehash_uuid_table(const std::size_t new_capacity) {
    uuid_buckets_.assign(new_capacity, uuid_bucket_empty);
    uuid_tombstones_ = 0U;
    for (std::uint32_t index = 0U;
         index < static_cast<std::uint32_t>(slots_.size());
         ++index) {
        if (slots_[index].alive && slots_[index].uuid) {
            insert_uuid_bucket(slots_[index].uuid, index);
        }
    }
}

void SceneDocument::insert_uuid_bucket(
    const SceneUuid node_uuid,
    const std::uint32_t index) noexcept {
    if (!node_uuid || uuid_buckets_.empty()) {
        return;
    }
    const std::size_t mask = uuid_buckets_.size() - 1U;
    std::size_t pos = hash_scene_uuid(node_uuid) & mask;
    for (std::size_t step = 0U; step < uuid_buckets_.size(); ++step) {
        const std::uint32_t entry = uuid_buckets_[pos];
        if (entry == uuid_bucket_empty) {
            uuid_buckets_[pos] = index;
            return;
        }
        if (entry == uuid_bucket_tombstone) {
            uuid_buckets_[pos] = index;
            if (uuid_tombstones_ > 0U) {
                --uuid_tombstones_;
            }
            return;
        }
        pos = (pos + 1U) & mask;
    }
}

void SceneDocument::erase_uuid_bucket(
    const SceneUuid node_uuid,
    const std::uint32_t index) noexcept {
    if (!node_uuid || uuid_buckets_.empty()) {
        return;
    }
    const std::size_t mask = uuid_buckets_.size() - 1U;
    std::size_t pos = hash_scene_uuid(node_uuid) & mask;
    for (std::size_t step = 0U; step < uuid_buckets_.size(); ++step) {
        const std::uint32_t entry = uuid_buckets_[pos];
        if (entry == uuid_bucket_empty) {
            return;
        }
        if (entry == index) {
            uuid_buckets_[pos] = uuid_bucket_tombstone;
            ++uuid_tombstones_;
            return;
        }
        pos = (pos + 1U) & mask;
    }
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
    case SceneNodeKind::point_light: return "Point light";
    case SceneNodeKind::spot_light: return "Spot light";
    case SceneNodeKind::environment: return "Environment";
    case SceneNodeKind::ocean: return "Ocean";
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
