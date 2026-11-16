#pragma once

#include "collision.hpp"
#include "physics_world.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_uuid.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t navmesh_file_version = 1U;
inline constexpr std::uint64_t maximum_navmesh_file_bytes =
    16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t max_nav_path_waypoints = 128U;
inline constexpr std::uint32_t invalid_nav_poly_id = UINT32_MAX;

struct NavMeshBakeSettings {
    float cell_size = 0.5F;
    float agent_radius = 0.4F;
    float agent_height = 1.8F;
    float max_slope_degrees = 44.0F;
    float max_step_height = 0.45F;
    bool include_terrain = true;
    bool use_custom_bounds = false;
    glm::vec3 custom_bounds_min{-64.0F, -16.0F, -64.0F};
    glm::vec3 custom_bounds_max{64.0F, 32.0F, 64.0F};
    std::uint32_t max_grid_cells_per_axis = 512U;

    friend bool operator==(
        const NavMeshBakeSettings&,
        const NavMeshBakeSettings&) = default;
};

static_assert(std::is_trivially_copyable_v<NavMeshBakeSettings>);

[[nodiscard]] bool valid_navmesh_bake_settings(
    const NavMeshBakeSettings& settings) noexcept;

[[nodiscard]] NavMeshBakeSettings sanitize_navmesh_bake_settings(
    NavMeshBakeSettings settings) noexcept;

struct NavMeshPolygon {
    std::uint32_t id = 0U;
    glm::vec2 min_xz{0.0F};
    glm::vec2 max_xz{0.0F};
    // Corner heights ordered (min_x,min_z), (max_x,min_z), (max_x,max_z), (min_x,max_z).
    std::array<float, 4U> corner_y{0.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 center{0.0F};
    float area = 0.0F;
    std::uint32_t first_portal = 0U;
    std::uint32_t portal_count = 0U;
    std::uint32_t first_off_mesh = 0U;
    std::uint32_t off_mesh_count = 0U;
    std::uint16_t area_flags = 1U;

    friend bool operator==(
        const NavMeshPolygon&,
        const NavMeshPolygon&) = default;
};

struct NavMeshPortalLink {
    std::uint32_t from_poly = invalid_nav_poly_id;
    std::uint32_t to_poly = invalid_nav_poly_id;
    glm::vec3 portal_left{0.0F};
    glm::vec3 portal_right{0.0F};
    float base_cost = 1.0F;

    friend bool operator==(
        const NavMeshPortalLink&,
        const NavMeshPortalLink&) = default;
};

struct NavMeshOffMeshConnection {
    SceneUuid source_node_uuid{};
    std::uint32_t from_poly = invalid_nav_poly_id;
    std::uint32_t to_poly = invalid_nav_poly_id;
    glm::vec3 start_pos{0.0F};
    glm::vec3 end_pos{0.0F};
    float radius = 0.75F;
    float cost_multiplier = 1.25F;
    float jump_height = 1.25F;
    float traversal_speed = 4.5F;
    OffMeshTraversalKind traversal_kind = OffMeshTraversalKind::jump;
    bool bidirectional = true;

    friend bool operator==(
        const NavMeshOffMeshConnection&,
        const NavMeshOffMeshConnection&) = default;
};

struct NavMeshData {
    std::uint32_t version = navmesh_file_version;
    NavMeshBakeSettings settings{};
    glm::vec3 bounds_min{0.0F};
    glm::vec3 bounds_max{0.0F};
    std::vector<NavMeshPolygon> polygons{};
    std::vector<NavMeshPortalLink> portals{};
    std::vector<NavMeshOffMeshConnection> off_mesh_links{};

    // Coarse 2D spatial acceleration index & bidirectional off-mesh adjacency
    // rebuilt automatically by rebuild_spatial_index().
    float spatial_cell_size = 2.0F;
    std::uint32_t spatial_cols = 0U;
    std::uint32_t spatial_rows = 0U;
    std::vector<std::uint32_t> spatial_cell_starts{};
    std::vector<std::uint32_t> spatial_cell_polys{};
    std::vector<std::uint32_t> poly_off_mesh_starts{};
    // Encodes (link_index << 1U) | (reverse_direction ? 1U : 0U).
    std::vector<std::uint32_t> poly_off_mesh_refs{};

    [[nodiscard]] bool empty() const noexcept { return polygons.empty(); }
    [[nodiscard]] bool valid() const noexcept;
    void clear() noexcept;
    void rebuild_spatial_index();
};

[[nodiscard]] inline bool navmesh_empty(const NavMeshData& navmesh) noexcept {
    return navmesh.polygons.empty();
}

[[nodiscard]] float sample_polygon_height(
    const NavMeshPolygon& poly,
    glm::vec2 xz) noexcept;

[[nodiscard]] bool bake_navmesh_from_scene(
    const SceneDocument& scene,
    const NavMeshBakeSettings& settings,
    NavMeshData& out_navmesh,
    std::string* out_error = nullptr,
    ModelBoundsProvider model_bounds = {},
    ModelCollisionProvider model_collision = {});

[[nodiscard]] std::string make_default_navmesh_relative_path(
    std::string_view scene_name_or_id);

[[nodiscard]] bool serialize_navmesh_json(
    const NavMeshData& navmesh,
    std::string& out_json,
    std::string& error);

[[nodiscard]] bool parse_navmesh_json(
    std::string_view json_text,
    NavMeshData& out_navmesh,
    std::string& error);

[[nodiscard]] bool save_navmesh_file(
    const NavMeshData& navmesh,
    const std::filesystem::path& asset_root,
    std::string_view relative_path,
    std::string& error);

[[nodiscard]] bool load_navmesh_file(
    const std::filesystem::path& asset_root,
    std::string_view relative_path,
    NavMeshData& out_navmesh,
    std::string& error);

struct DynamicNavBlocker {
    SceneNodeId node_id{};
    SceneUuid node_uuid{};
    NavObstacleShape shape = NavObstacleShape::box;
    glm::vec3 center{0.0F};
    glm::mat3 axes{1.0F};
    glm::vec3 half_extents{0.5F};
    float radius = 0.5F;
    float height = 1.0F;
    float margin = 0.25F;
    float cost_penalty = 8.0F;
    bool block_navigation = true;
    Bounds world_aabb{};

    friend bool operator==(
        const DynamicNavBlocker& lhs,
        const DynamicNavBlocker& rhs) noexcept {
        return lhs.node_id == rhs.node_id &&
               lhs.node_uuid == rhs.node_uuid &&
               lhs.shape == rhs.shape &&
               lhs.center == rhs.center &&
               lhs.axes == rhs.axes &&
               lhs.half_extents == rhs.half_extents &&
               lhs.radius == rhs.radius &&
               lhs.height == rhs.height &&
               lhs.margin == rhs.margin &&
               lhs.cost_penalty == rhs.cost_penalty &&
               lhs.block_navigation == rhs.block_navigation &&
               lhs.world_aabb.center == rhs.world_aabb.center &&
               lhs.world_aabb.extent == rhs.world_aabb.extent;
    }
};

struct NavPathWaypoint {
    glm::vec3 position{0.0F};
    std::uint32_t poly_id = invalid_nav_poly_id;
    std::uint32_t off_mesh_link_index = invalid_nav_poly_id;
    bool is_off_mesh_traversal = false;
    OffMeshTraversalKind traversal_kind = OffMeshTraversalKind::jump;
    glm::vec3 off_mesh_end{0.0F};
    float jump_height = 0.0F;
    float traversal_speed = 4.5F;
};

struct NavPath {
    std::array<NavPathWaypoint, max_nav_path_waypoints> waypoints{};
    std::uint32_t waypoint_count = 0U;
    float total_cost = 0.0F;
    bool complete = false;
    bool partial = false;
    bool used_off_mesh_link = false;

    void clear() noexcept {
        waypoint_count = 0U;
        total_cost = 0.0F;
        complete = false;
        partial = false;
        used_off_mesh_link = false;
    }

    [[nodiscard]] std::span<const NavPathWaypoint> points() const noexcept {
        return std::span<const NavPathWaypoint>(waypoints.data(), waypoint_count);
    }
};

struct NavPathQueryOptions {
    float agent_radius = 0.4F;
    float agent_height = 1.8F;
    float start_search_radius = 6.0F;
    float goal_search_radius = 8.0F;
    std::uint32_t max_expanded_nodes = 8'192U;
    bool allow_off_mesh_links = true;
    bool allow_partial_path = true;
    SceneNodeId ignored_node{};
};

class NavigationRuntime final {
public:
    NavigationRuntime();

    void clear() noexcept;
    void set_navmesh(NavMeshData navmesh);
    [[nodiscard]] const NavMeshData& navmesh() const noexcept {
        return navmesh_;
    }
    [[nodiscard]] bool has_navmesh() const noexcept {
        return !navmesh_.polygons.empty();
    }

    void sync_dynamic_obstacles(
        const SceneDocument& scene,
        const PhysicsWorld* physics_world = nullptr,
        std::span<const SceneWorldNode> prebuilt_world_nodes = {},
        std::span<const std::uint32_t> world_node_index_by_id = {}) noexcept;

    [[nodiscard]] std::span<const DynamicNavBlocker> dynamic_blockers()
        const noexcept {
        return blockers_;
    }
    [[nodiscard]] std::uint64_t obstacles_revision() const noexcept {
        return obstacles_revision_;
    }
    [[nodiscard]] std::uint64_t navmesh_revision() const noexcept {
        return navmesh_revision_;
    }
    [[nodiscard]] std::size_t scratch_total_capacity() const noexcept {
        return blockers_.capacity() +
            blockers_scratch_.capacity() +
            world_nodes_scratch_.capacity() +
            world_node_index_scratch_.capacity() +
            ancestor_scratch_.capacity() +
            scratch_.g_score.capacity() +
            scratch_.f_score.capacity() +
            scratch_.parent_poly.capacity() +
            scratch_.parent_edge.capacity() +
            scratch_.parent_edge_flags.capacity() +
            scratch_.open_stamp.capacity() +
            scratch_.closed_stamp.capacity() +
            scratch_.open_heap.capacity() +
            scratch_.corridor.capacity() +
            scratch_.portal_lefts.capacity() +
            scratch_.portal_rights.capacity() +
            scratch_.portal_polys.capacity();
    }

    [[nodiscard]] bool is_point_blocked(
        glm::vec3 point,
        float agent_radius,
        float agent_height = 1.8F,
        SceneNodeId ignored_node = {}) const noexcept;

    [[nodiscard]] bool is_segment_blocked(
        glm::vec3 a,
        glm::vec3 b,
        float agent_radius,
        float agent_height = 1.8F,
        SceneNodeId ignored_node = {}) const noexcept;

    [[nodiscard]] bool find_nearest_point(
        glm::vec3 query_pos,
        float search_radius,
        glm::vec3& out_snapped,
        std::uint32_t* out_poly_id = nullptr,
        float agent_radius = 0.0F,
        float agent_height = 1.8F,
        SceneNodeId ignored_node = {}) const noexcept;

    [[nodiscard]] bool find_path(
        glm::vec3 start,
        glm::vec3 goal,
        NavPath& out_path,
        const NavPathQueryOptions& options = {}) noexcept;

    [[nodiscard]] glm::vec3 compute_local_steering(
        glm::vec3 position,
        glm::vec3 desired_velocity,
        float agent_radius,
        float max_speed,
        std::span<const glm::vec3> neighbor_positions = {},
        SceneNodeId ignored_node = {}) const noexcept;

private:
    struct AStarOpenEntry {
        std::uint32_t poly_id = 0U;
        float f_score = 0.0F;
    };

    struct CorridorStep {
        std::uint32_t poly_id = invalid_nav_poly_id;
        std::uint32_t portal_index = invalid_nav_poly_id;
        std::uint32_t off_mesh_index = invalid_nav_poly_id;
        bool is_off_mesh = false;
        bool off_mesh_reverse = false;
    };

    struct NavQueryScratch {
        std::uint32_t query_stamp = 1U;
        std::vector<float> g_score{};
        std::vector<float> f_score{};
        std::vector<std::uint32_t> parent_poly{};
        std::vector<std::uint32_t> parent_edge{};
        // bit 0 = off_mesh, bit 1 = off_mesh_reverse
        std::vector<std::uint8_t> parent_edge_flags{};
        std::vector<std::uint32_t> open_stamp{};
        std::vector<std::uint32_t> closed_stamp{};
        std::vector<AStarOpenEntry> open_heap{};
        std::vector<CorridorStep> corridor{};
        std::vector<glm::vec3> portal_lefts{};
        std::vector<glm::vec3> portal_rights{};
        std::vector<std::uint32_t> portal_polys{};
    };

    [[nodiscard]] float evaluate_polygon_blocker_penalty(
        std::uint32_t poly_id,
        float agent_radius,
        float agent_height,
        SceneNodeId ignored_node,
        bool& out_hard_blocked) const noexcept;

    [[nodiscard]] float evaluate_portal_blocker_penalty(
        const NavMeshPortalLink& portal,
        float agent_radius,
        float agent_height,
        SceneNodeId ignored_node,
        bool& out_hard_blocked) const noexcept;

    NavMeshData navmesh_{};
    std::vector<DynamicNavBlocker> blockers_{};
    std::vector<DynamicNavBlocker> blockers_scratch_{};
    std::vector<SceneWorldNode> world_nodes_scratch_{};
    std::vector<std::uint32_t> world_node_index_scratch_{};
    std::vector<std::uint32_t> ancestor_scratch_{};
    NavQueryScratch scratch_{};
    std::uint64_t navmesh_revision_ = 1U;
    std::uint64_t obstacles_revision_ = 1U;
};

} // namespace vcr
