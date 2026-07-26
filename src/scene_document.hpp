#pragma once

#include "pbr_material.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t max_scene_nodes = 100'000U;
inline constexpr std::size_t max_scene_node_name_bytes = 255U;
inline constexpr std::size_t max_scene_asset_path_bytes = 4'096U;
inline constexpr std::uint32_t invalid_scene_node_index =
    UINT32_MAX;

struct SceneNodeId {
    std::uint32_t index = invalid_scene_node_index;
    std::uint32_t generation = 0U;

    [[nodiscard]] explicit operator bool() const noexcept {
        return index != invalid_scene_node_index && generation != 0U;
    }

    friend bool operator==(
        const SceneNodeId&,
        const SceneNodeId&) = default;
};

enum class SceneNodeKind : std::uint8_t {
    folder,
    cube,
    ground,
    terrain,
    camera,
    directional_light,
    environment,
    model,
};

struct SceneTransform {
    glm::vec3 translation{0.0F};
    glm::vec3 rotation_degrees{0.0F};
    glm::vec3 scale{1.0F};
};

struct SceneNode {
    std::string name;
    // UTF-8 generic path to a .vcrasset.json file, relative to the
    // editor's assets root. Required only for model nodes.
    std::string asset_path;
    SceneTransform transform{};
    PbrMaterial material{};
    SceneNodeKind kind = SceneNodeKind::folder;
    bool visible = true;
    bool expanded = true;
};

struct SceneHierarchyRow {
    SceneNodeId id{};
    std::uint32_t depth = 0U;
    bool has_children = false;
};

struct SceneNodeRecord {
    SceneNode value{};
    std::uint32_t parent_record_index =
        invalid_scene_node_index;
};

struct alignas(16) SceneGpuInstance {
    glm::vec4 model_row_0{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec4 model_row_1{0.0F, 1.0F, 0.0F, 0.0F};
    glm::vec4 model_row_2{0.0F, 0.0F, 1.0F, 0.0F};
    PbrMaterial material{};
};

static_assert(sizeof(SceneGpuInstance) == 80U);
static_assert(offsetof(SceneGpuInstance, material) == 48U);

struct SceneModelInstance {
    glm::vec4 model_row_0{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec4 model_row_1{0.0F, 1.0F, 0.0F, 0.0F};
    glm::vec4 model_row_2{0.0F, 0.0F, 1.0F, 0.0F};
    // Refers to SceneNode::asset_path. Consume the render view before
    // mutating the document.
    std::string_view asset_path{};
};

// Non-owning view into caller-provided instance storage. Consume it before
// mutating either the SceneDocument or the output vectors used to build it.
struct SceneRenderView {
    SceneGpuInstance ground{};
    std::span<const SceneGpuInstance> cubes{};
    std::span<const SceneModelInstance> models{};
    std::uint64_t revision = 0U;
    bool has_ground = false;
    bool terrain_visible = false;
    bool sky_visible = false;
    bool sun_visible = false;
};

class SceneDocument final {
public:
    SceneDocument();

    [[nodiscard]] SceneNodeId create_node(
        std::string name,
        SceneNodeKind kind,
        SceneNodeId parent = {});
    [[nodiscard]] bool destroy_subtree(SceneNodeId id);
    [[nodiscard]] bool reparent(
        SceneNodeId id,
        SceneNodeId new_parent);
    [[nodiscard]] bool rename_node(
        SceneNodeId id,
        std::string name);

    [[nodiscard]] bool valid(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<std::reference_wrapper<SceneNode>>
        node(SceneNodeId id) noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<const SceneNode>>
        node(SceneNodeId id) const noexcept;
    [[nodiscard]] SceneNodeId parent(SceneNodeId id) const noexcept;

    void mark_content_changed() noexcept;
    void set_expanded(SceneNodeId id, bool expanded) noexcept;

    void build_hierarchy_rows(
        std::vector<SceneHierarchyRow>& output,
        std::string_view filter = {}) const;
    void build_node_records(
        std::vector<SceneNodeRecord>& output) const;
    [[nodiscard]] SceneRenderView build_render_view(
        std::vector<SceneGpuInstance>& cube_output,
        std::vector<SceneModelInstance>& model_output);

    [[nodiscard]] std::uint32_t size() const noexcept {
        return live_count_;
    }
    [[nodiscard]] std::uint64_t structure_revision() const noexcept {
        return structure_revision_;
    }
    [[nodiscard]] std::uint64_t content_revision() const noexcept {
        return content_revision_;
    }

    [[nodiscard]] static SceneDocument make_default();

private:
    struct Slot {
        SceneNode value{};
        glm::mat4 world_matrix{1.0F};
        std::uint32_t generation = 1U;
        std::uint32_t parent = invalid_scene_node_index;
        std::uint32_t first_child = invalid_scene_node_index;
        std::uint32_t last_child = invalid_scene_node_index;
        std::uint32_t previous_sibling = invalid_scene_node_index;
        std::uint32_t next_sibling = invalid_scene_node_index;
        bool alive = false;
        bool world_visible = true;
    };

    [[nodiscard]] SceneNodeId id_for_index(
        std::uint32_t index) const noexcept;
    [[nodiscard]] bool would_create_cycle(
        std::uint32_t child,
        std::uint32_t new_parent) const noexcept;
    void unlink(std::uint32_t index) noexcept;
    void link_last(
        std::uint32_t index,
        std::uint32_t parent) noexcept;
    [[nodiscard]] std::uint32_t traversal_next(
        std::uint32_t index,
        std::uint32_t& depth,
        bool descend) const noexcept;
    [[nodiscard]] bool name_matches(
        std::string_view name,
        std::string_view filter) const noexcept;

    std::vector<Slot> slots_{};
    std::vector<std::uint32_t> free_indices_{};
    std::vector<std::uint32_t> traversal_scratch_{};
    std::uint32_t first_root_ = invalid_scene_node_index;
    std::uint32_t last_root_ = invalid_scene_node_index;
    std::uint32_t live_count_ = 0U;
    std::uint64_t structure_revision_ = 1U;
    std::uint64_t content_revision_ = 1U;
};

[[nodiscard]] std::string_view to_string(
    SceneNodeKind kind) noexcept;

[[nodiscard]] bool valid_scene_asset_path(
    std::string_view path) noexcept;

} // namespace vcr
