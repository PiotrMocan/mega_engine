#include "collision.hpp"
#include "editor_scene_tools.hpp"
#include "fly_camera.hpp"
#include "gameplay_framework.hpp"
#include "gameplay_session.hpp"
#include "navigation.hpp"
#include "npc_ai.hpp"
#include "physics_world.hpp"
#include "prefab_io.hpp"
#include "project_system.hpp"
#include "scene_component_json.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"
#include "scene_uuid.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void require_check(
    const bool condition,
    const char* const expression,
    const int line,
    const std::string_view detail = {}) {
    if (condition) {
        return;
    }
    std::cerr << "FAIL line " << line << ": " << expression;
    if (!detail.empty()) {
        std::cerr << " (" << detail << ")";
    }
    std::cerr << '\n';
    ++g_failures;
}

#define REQUIRE(expr) require_check(static_cast<bool>(expr), #expr, __LINE__)
#define REQUIRE_MSG(expr, msg)                                \
    do {                                                      \
        const bool vcr_req_ok_ = static_cast<bool>(expr);     \
        require_check(vcr_req_ok_, #expr, __LINE__, (msg));   \
    } while (false)

[[nodiscard]] bool close_to(
    const float a,
    const float b,
    const float eps = 1.0e-3F) noexcept {
    return std::abs(a - b) <= eps;
}

class ScopedTempDir final {
public:
    ScopedTempDir() {
        static std::atomic<std::uint64_t> counter{0U};
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto id = counter.fetch_add(1U, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
            ("vcr_nav_ai_test_" + std::to_string(tick) + "_" + std::to_string(id));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

void write_text_file(
    const std::filesystem::path& file_path,
    const std::string_view text) {
    std::filesystem::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
}

vcr::SceneNodeId add_box_node(
    vcr::SceneDocument& scene,
    const std::string& name,
    const glm::vec3 center,
    const glm::vec3 half_extents) {
    const vcr::SceneNodeId id = scene.create_node(name, vcr::SceneNodeKind::cube);
    if (auto nr = scene.node(id)) {
        nr->get().transform.translation = center;
        nr->get().transform.scale = half_extents;
    }
    return id;
}

// 1. NavMesh baking from static geometry + terrain & .vcrnav.json serialization.
void test_navmesh_bake_and_serialization() {
    vcr::SceneDocument scene;
    // Main flat floor spanning [-10, 10] in XZ with top surface y = 0.0.
    const vcr::SceneNodeId floor_id = add_box_node(
        scene, "floor", {0.0F, -0.5F, 0.0F}, {10.0F, 0.5F, 10.0F});
    REQUIRE(floor_id);
    // Low traversable step with top surface at y = 0.25 (< max_step_height = 0.5).
    const vcr::SceneNodeId step_id = add_box_node(
        scene, "step", {5.0F, -0.125F, 0.0F}, {2.0F, 0.375F, 3.0F});
    REQUIRE(step_id);
    // Tall unwalkable pillar whose top sits well above walkable floor + headroom.
    const vcr::SceneNodeId wall_id = add_box_node(
        scene, "wall", {0.0F, 1.5F, 0.0F}, {1.0F, 1.5F, 4.0F});
    REQUIRE(wall_id);

    vcr::NavMeshBakeSettings settings{};
    settings.cell_size = 0.5F;
    settings.agent_radius = 0.4F;
    settings.agent_height = 1.8F;
    settings.max_step_height = 0.5F;
    settings.include_terrain = false;

    vcr::NavMeshData baked{};
    std::string error;
    REQUIRE_MSG(
        vcr::bake_navmesh_from_scene(scene, settings, baked, &error), error);
    REQUIRE(baked.valid());
    REQUIRE(!baked.empty());
    REQUIRE(!baked.portals.empty());

    // Floor polygons exist and at least one polygon lies on the low step (y ~ 0.25).
    bool found_step_poly = false;
    for (const vcr::NavMeshPolygon& poly : baked.polygons) {
        if (poly.center.x >= 3.0F && poly.center.x <= 7.0F &&
            std::abs(poly.center.y - 0.25F) <= 0.1F) {
            found_step_poly = true;
            break;
        }
    }
    REQUIRE(found_step_poly);

    // Points under the tall wall footprint must not have walkable snapped floor inside the wall.
    vcr::NavigationRuntime runtime;
    runtime.set_navmesh(baked);
    glm::vec3 snapped{};
    if (runtime.find_nearest_point({0.0F, 0.0F, 0.0F}, 0.15F, snapped)) {
        // Any fallback snap within 0.15m of wall center at ground height shouldn't happen.
        REQUIRE(std::abs(snapped.y) > 0.5F || std::abs(snapped.x) > 0.5F);
    }

    // Round-trip save_navmesh_file / load_navmesh_file.
    const ScopedTempDir temp;
    const std::filesystem::path asset_root = temp.path() / "assets";
    std::filesystem::create_directories(asset_root);
    REQUIRE_MSG(
        vcr::save_navmesh_file(
            baked, asset_root, "navmesh/test_scene.vcrnav.json", error),
        error);

    vcr::NavMeshData loaded{};
    REQUIRE_MSG(
        vcr::load_navmesh_file(
            asset_root, "navmesh/test_scene.vcrnav.json", loaded, error),
        error);
    REQUIRE(loaded.valid());
    REQUIRE(loaded.polygons.size() == baked.polygons.size());
    REQUIRE(loaded.portals.size() == baked.portals.size());
    REQUIRE(glm::distance(loaded.bounds_min, baked.bounds_min) <= 1.0e-3F);
    REQUIRE(glm::distance(loaded.bounds_max, baked.bounds_max) <= 1.0e-3F);

    // Negative serialization checks: path traversal, truncated JSON, NaN/Inf, out-of-range indices.
    vcr::NavMeshData bad{};
    REQUIRE(!vcr::save_navmesh_file(
        baked, asset_root, "../escape.vcrnav.json", error));
    REQUIRE(!vcr::load_navmesh_file(
        asset_root, "../escape.vcrnav.json", bad, error));
    REQUIRE(!vcr::parse_navmesh_json("{\"format\":\"vulkan_cube_navmesh\"", bad, error));

    std::string json_text;
    REQUIRE_MSG(vcr::serialize_navmesh_json(baked, json_text, error), error);
    nlohmann::json parsed_doc = nlohmann::json::parse(json_text);
    nlohmann::json out_of_range = parsed_doc;
    out_of_range["portals"][0]["to_poly"] =
        static_cast<std::uint32_t>(baked.polygons.size() + 42U);
    REQUIRE(!vcr::parse_navmesh_json(out_of_range.dump(), bad, error));

    nlohmann::json nan_doc = parsed_doc;
    nan_doc["polygons"][0]["center"][0] = "NaN";
    REQUIRE(!vcr::parse_navmesh_json(nan_doc.dump(), bad, error));

    // Procedural terrain bake check (with custom compact bounds to keep test crisp).
    vcr::SceneDocument terrain_scene;
    const vcr::SceneNodeId terrain_id =
        terrain_scene.create_node("terrain", vcr::SceneNodeKind::terrain);
    REQUIRE(terrain_id);
    vcr::NavMeshBakeSettings terrain_settings{};
    terrain_settings.cell_size = 1.0F;
    terrain_settings.include_terrain = true;
    terrain_settings.use_custom_bounds = true;
    terrain_settings.custom_bounds_min = {-8.0F, -6.0F, -8.0F};
    terrain_settings.custom_bounds_max = {8.0F, 12.0F, 8.0F};
    vcr::NavMeshData terrain_nav{};
    REQUIRE_MSG(
        vcr::bake_navmesh_from_scene(
            terrain_scene, terrain_settings, terrain_nav, &error),
        error);
    REQUIRE(terrain_nav.valid());
    REQUIRE(!terrain_nav.empty());
}

// 2. A* + String-Pulling Funnel pathfinding.
void test_astar_and_funnel_pathfinding() {
    // Open multi-polygon floor should string-pull a straight path to 2 waypoints.
    vcr::SceneDocument open_scene;
    REQUIRE(add_box_node(open_scene, "open_floor", {0.0F, -0.5F, 0.0F}, {12.0F, 0.5F, 12.0F}));
    // Add minor bumps at the far corner to split floor into multiple grid rectangles.
    REQUIRE(add_box_node(open_scene, "corner_post", {10.0F, 1.0F, 10.0F}, {0.5F, 1.0F, 0.5F}));

    vcr::NavMeshBakeSettings settings{};
    settings.cell_size = 0.5F;
    settings.include_terrain = false;
    vcr::NavMeshData open_nav{};
    std::string error;
    REQUIRE_MSG(vcr::bake_navmesh_from_scene(open_scene, settings, open_nav, &error), error);
    REQUIRE(open_nav.polygons.size() >= 2U);

    vcr::NavigationRuntime nav;
    nav.set_navmesh(open_nav);

    vcr::NavPath straight_path{};
    REQUIRE(nav.find_path({-6.0F, 0.0F, -4.0F}, {4.0F, 0.0F, 2.0F}, straight_path));
    REQUIRE(straight_path.complete);
    REQUIRE(straight_path.waypoint_count == 2U);

    // Wall obstacle forces multi-waypoint detour avoiding [-0.8, 0.8] x [-5, 5].
    vcr::SceneDocument wall_scene;
    REQUIRE(add_box_node(wall_scene, "floor", {0.0F, -0.5F, 0.0F}, {10.0F, 0.5F, 10.0F}));
    REQUIRE(add_box_node(wall_scene, "center_wall", {0.0F, 1.5F, 0.0F}, {0.8F, 1.5F, 5.0F}));
    vcr::NavMeshData wall_nav{};
    REQUIRE_MSG(vcr::bake_navmesh_from_scene(wall_scene, settings, wall_nav, &error), error);
    nav.set_navmesh(wall_nav);

    vcr::NavPath detour_path{};
    REQUIRE(nav.find_path({-5.0F, 0.0F, 0.0F}, {5.0F, 0.0F, 0.0F}, detour_path));
    REQUIRE(detour_path.complete);
    REQUIRE(detour_path.waypoint_count >= 3U);
    bool cleared_wall_z = false;
    for (const vcr::NavPathWaypoint& wp : detour_path.points()) {
        if (std::abs(wp.position.z) >= 4.5F) {
            cleared_wall_z = true;
        }
        // Waypoint must never sit inside solid center of the wall.
        const bool inside_wall =
            std::abs(wp.position.x) < 0.7F && std::abs(wp.position.z) < 4.5F;
        REQUIRE(!inside_wall);
    }
    REQUIRE(cleared_wall_z);
}

// 3. Dynamic obstacles (NavObstacleComponent + dynamic RigidBodyComponent).
void test_dynamic_obstacles_and_cost_penalties() {
    // Two parallel corridors separating X < 0 from X > 0 via a wide wall with two doors (Z=-5 and Z=+5).
    vcr::SceneDocument scene;
    REQUIRE(add_box_node(scene, "floor", {0.0F, -0.5F, 0.0F}, {10.0F, 0.5F, 10.0F}));
    REQUIRE(add_box_node(scene, "mid_wall", {0.0F, 1.5F, 0.0F}, {0.6F, 1.5F, 2.5F}));
    REQUIRE(add_box_node(scene, "south_wing", {0.0F, 1.5F, -8.5F}, {0.6F, 1.5F, 1.5F}));
    REQUIRE(add_box_node(scene, "north_wing", {0.0F, 1.5F, 8.5F}, {0.6F, 1.5F, 1.5F}));

    vcr::NavMeshBakeSettings settings{};
    settings.cell_size = 0.5F;
    settings.include_terrain = false;
    vcr::NavMeshData nav_data{};
    std::string error;
    REQUIRE_MSG(vcr::bake_navmesh_from_scene(scene, settings, nav_data, &error), error);

    // Door blocker on the south doorway (x=0, z=-5).
    const vcr::SceneNodeId blocker_id =
        add_box_node(scene, "south_gate", {0.0F, 1.0F, -5.0F}, {1.0F, 1.0F, 2.2F});
    vcr::NavObstacleComponent obs{};
    obs.shape = vcr::NavObstacleShape::box;
    obs.half_extents = {1.0F, 1.0F, 1.0F};
    obs.margin = 0.35F;
    obs.block_navigation = true;
    obs.enabled = true;
    REQUIRE(scene.set_nav_obstacle(blocker_id, obs));

    vcr::NavigationRuntime nav;
    nav.set_navmesh(nav_data);
    nav.sync_dynamic_obstacles(scene, nullptr);
    REQUIRE(nav.dynamic_blockers().size() == 1U);

    // Path near south doorway (-4,0,-5) -> (4,0,-5) must detour north around Z > 0.
    vcr::NavPath detoured{};
    REQUIRE(nav.find_path({-4.0F, 0.0F, -5.0F}, {4.0F, 0.0F, -5.0F}, detoured));
    REQUIRE(detoured.complete);
    bool went_north = false;
    for (const vcr::NavPathWaypoint& wp : detoured.points()) {
        if (wp.position.z > 2.5F) {
            went_north = true;
            break;
        }
    }
    REQUIRE(went_north);

    // Disabling obstacle restores direct south doorway route (2 waypoints).
    obs.enabled = false;
    REQUIRE(scene.set_nav_obstacle(blocker_id, obs));
    nav.sync_dynamic_obstacles(scene, nullptr);
    REQUIRE(nav.dynamic_blockers().empty());

    vcr::NavPath direct{};
    REQUIRE(nav.find_path({-4.0F, 0.0F, -5.0F}, {4.0F, 0.0F, -5.0F}, direct));
    REQUIRE(direct.complete);
    REQUIRE(direct.waypoint_count == 2U);

    // Non-blocking high cost_penalty (12.0F) at symmetric midpoint biases towards clean corridor.
    obs.enabled = true;
    obs.block_navigation = false;
    obs.cost_penalty = 20.0F;
    obs.half_extents = {1.0F, 1.0F, 1.4F};
    REQUIRE(scene.set_nav_obstacle(blocker_id, obs));
    nav.sync_dynamic_obstacles(scene, nullptr);
    vcr::NavPath penalty_detour{};
    REQUIRE(nav.find_path({-4.0F, 0.0F, 0.0F}, {4.0F, 0.0F, 0.0F}, penalty_detour));
    REQUIRE(penalty_detour.complete);
    bool chose_north_clean = false;
    for (const vcr::NavPathWaypoint& wp : penalty_detour.points()) {
        if (wp.position.z > 2.5F) {
            chose_north_clean = true;
            break;
        }
    }
    REQUIRE(chose_north_clean);

    // Dynamic RigidBodyComponent without NavObstacleComponent auto-registers in dynamic_blockers().
    const vcr::SceneNodeId dyn_crate =
        add_box_node(scene, "physics_crate", {4.0F, 0.6F, 5.0F}, {0.6F, 0.6F, 0.6F});
    vcr::RigidBodyComponent rb{};
    rb.motion_type = vcr::RigidBodyMotionType::dynamic;
    rb.enabled = true;
    REQUIRE(scene.set_rigid_body(dyn_crate, rb));
    nav.sync_dynamic_obstacles(scene, nullptr);
    bool found_rb_blocker = false;
    for (const vcr::DynamicNavBlocker& b : nav.dynamic_blockers()) {
        if (b.node_id == dyn_crate) {
            found_rb_blocker = true;
        }
    }
    REQUIRE(found_rb_blocker);
}

// 4. Off-Mesh Links (disconnected islands + traversal + unidirectional + flag gating).
void test_off_mesh_links_and_runtime_traversal() {
    vcr::SceneDocument scene;
    // Two separate islands with a 4m chasm: [-10, -2] and [2, 10].
    REQUIRE(add_box_node(scene, "island_a", {-6.0F, -0.5F, 0.0F}, {4.0F, 0.5F, 4.0F}));
    REQUIRE(add_box_node(scene, "island_b", {6.0F, -0.5F, 0.0F}, {4.0F, 0.5F, 4.0F}));

    const vcr::SceneNodeId jump_node =
        scene.create_node("chasm_jump", vcr::SceneNodeKind::folder);
    const vcr::SceneNodeId land_node =
        scene.create_node("chasm_land", vcr::SceneNodeKind::folder);
    REQUIRE(jump_node && land_node);
    if (auto nr = scene.node(jump_node)) {
        nr->get().transform.translation = {-2.8F, 0.0F, 0.0F};
    }
    if (auto nr = scene.node(land_node)) {
        nr->get().transform.translation = {2.8F, 0.0F, 0.0F};
    }
    const auto land_uuid = scene.uuid(land_node);
    REQUIRE(land_uuid.has_value());

    vcr::OffMeshLinkComponent oml{};
    oml.enabled = true;
    oml.bidirectional = true;
    oml.traversal_kind = vcr::OffMeshTraversalKind::jump;
    oml.target_node_uuid = *land_uuid;
    oml.start_offset = {0.0F, 0.0F, 0.0F};
    oml.end_offset = {0.0F, 0.0F, 0.0F};
    oml.radius = 1.0F;
    oml.jump_height = 1.6F;
    oml.traversal_speed = 6.0F;
    REQUIRE(scene.set_off_mesh_link(jump_node, oml));

    vcr::NavMeshBakeSettings settings{};
    settings.cell_size = 0.5F;
    settings.include_terrain = false;
    vcr::NavMeshData nav_data{};
    std::string error;
    REQUIRE_MSG(vcr::bake_navmesh_from_scene(scene, settings, nav_data, &error), error);
    REQUIRE(nav_data.off_mesh_links.size() == 1U);

    vcr::NavigationRuntime nav;
    nav.set_navmesh(nav_data);

    // Forward across bidirectional link succeeds.
    vcr::NavPath forward_path{};
    REQUIRE(nav.find_path({-6.0F, 0.0F, 0.0F}, {6.0F, 0.0F, 0.0F}, forward_path));
    REQUIRE(forward_path.complete);
    REQUIRE(forward_path.used_off_mesh_link);
    bool has_om_wp = false;
    for (const vcr::NavPathWaypoint& wp : forward_path.points()) {
        if (wp.is_off_mesh_traversal) {
            has_om_wp = true;
        }
    }
    REQUIRE(has_om_wp);

    // Disallowing off-mesh links cannot reach island_b.
    vcr::NavPathQueryOptions no_om{};
    no_om.allow_off_mesh_links = false;
    no_om.allow_partial_path = false;
    vcr::NavPath rejected_path{};
    REQUIRE(!nav.find_path({-6.0F, 0.0F, 0.0F}, {6.0F, 0.0F, 0.0F}, rejected_path, no_om));

    // Bidirectional reverse path works, while one-way rejects reverse.
    vcr::NavPath reverse_ok{};
    REQUIRE(nav.find_path({6.0F, 0.0F, 0.0F}, {-6.0F, 0.0F, 0.0F}, reverse_ok));
    REQUIRE(reverse_ok.complete);

    oml.bidirectional = false;
    REQUIRE(scene.set_off_mesh_link(jump_node, oml));
    vcr::NavMeshData oneway_nav{};
    REQUIRE_MSG(vcr::bake_navmesh_from_scene(scene, settings, oneway_nav, &error), error);
    nav.set_navmesh(oneway_nav);
    vcr::NavPathQueryOptions strict_rev{};
    strict_rev.allow_partial_path = false;
    vcr::NavPath reverse_blocked{};
    REQUIRE(!nav.find_path({6.0F, 0.0F, 0.0F}, {-6.0F, 0.0F, 0.0F}, reverse_blocked, strict_rev));

    // NPC runtime agent follows the off-mesh link via parabolic arc to island_b.
    const vcr::SceneNodeId target_node =
        scene.create_node("goal_marker", vcr::SceneNodeKind::folder);
    const vcr::SceneNodeId npc_node =
        scene.create_node("jumper_npc", vcr::SceneNodeKind::cube);
    REQUIRE(target_node && npc_node);
    if (auto nr = scene.node(target_node)) {
        nr->get().transform.translation = {6.0F, 0.0F, 0.0F};
    }
    if (auto nr = scene.node(npc_node)) {
        nr->get().transform.translation = {-5.5F, 0.0F, 0.0F};
        nr->get().transform.rotation_degrees.y = 90.0F;
    }
    vcr::NpcAiComponent npc{};
    npc.target_node_uuid = *scene.uuid(target_node);
    npc.patrol_enabled = false;
    npc.sight_range = 40.0F;
    npc.sight_fov_degrees = 360.0F;
    npc.can_traverse_off_mesh_links = true;
    npc.walk_speed = 4.0F;
    npc.run_speed = 6.0F;
    REQUIRE(scene.set_npc_ai(npc_node, npc));

    vcr::GameplayWorld world;
    vcr::CollisionWorld col_world;
    col_world.rebuild_from_scene(scene, npc_node);
    vcr::NpcAiRuntimeSystem ai_sys;
    ai_sys.sync_agents_from_scene(scene, world);

    bool observed_mid_air_arc = false;
    float peak_y = 0.0F;
    for (int step = 0; step < 300; ++step) {
        ai_sys.update(
            1.0F / 60.0F,
            scene,
            nav,
            &col_world,
            nullptr,
            world,
            nullptr,
            nullptr,
            {6.0F, 0.0F, 0.0F},
            true,
            {});
        const vcr::NpcRuntimeAgent* ag = ai_sys.find_agent(npc_node);
        REQUIRE(ag != nullptr);
        peak_y = std::max(peak_y, ag->position.y);
        if (ag->state == vcr::NpcFsmState::traverse_off_mesh_link &&
            ag->position.y > 0.2F) {
            observed_mid_air_arc = true;
        }
        if (ag->position.x >= 3.5F &&
            ag->state != vcr::NpcFsmState::traverse_off_mesh_link) {
            break;
        }
    }
    const vcr::NpcRuntimeAgent* final_ag = ai_sys.find_agent(npc_node);
    REQUIRE(final_ag != nullptr);
    REQUIRE(observed_mid_air_arc);
    REQUIRE(peak_y >= 0.8F);
    REQUIRE(final_ag->position.x >= 2.5F);
}

// 5. Zero steady-state hot-path allocations verification (no global operator new override for ASan compatibility).
void test_zero_steady_state_hot_path_allocations() {
    vcr::SceneDocument scene;
    REQUIRE(add_box_node(scene, "floor", {0.0F, -0.5F, 0.0F}, {14.0F, 0.5F, 14.0F}));
    const vcr::SceneNodeId dyn_obs =
        add_box_node(scene, "moving_crate", {0.0F, 0.5F, 0.0F}, {0.6F, 0.6F, 0.6F});
    vcr::NavObstacleComponent obs{};
    REQUIRE(scene.set_nav_obstacle(dyn_obs, obs));

    std::array<vcr::SceneNodeId, 4U> npc_ids{};
    for (std::size_t i = 0U; i < npc_ids.size(); ++i) {
        npc_ids[i] = scene.create_node(
            "guard_" + std::to_string(i), vcr::SceneNodeKind::cube);
        if (auto nr = scene.node(npc_ids[i])) {
            nr->get().transform.translation = {
                -6.0F + static_cast<float>(i) * 3.0F, 0.0F, -6.0F};
        }
        vcr::NpcAiComponent npc{};
        npc.patrol_point_a = {-6.0F + static_cast<float>(i) * 3.0F, 0.0F, -6.0F};
        npc.patrol_point_b = {-6.0F + static_cast<float>(i) * 3.0F, 0.0F, 6.0F};
        REQUIRE(scene.set_npc_ai(npc_ids[i], npc));
    }

    vcr::NavMeshBakeSettings settings{};
    settings.cell_size = 0.5F;
    settings.include_terrain = false;
    vcr::NavMeshData nav_data{};
    std::string error;
    REQUIRE_MSG(vcr::bake_navmesh_from_scene(scene, settings, nav_data, &error), error);

    vcr::NavigationRuntime nav;
    nav.set_navmesh(nav_data);
    vcr::CollisionWorld col_world;
    col_world.rebuild_from_scene(scene);
    vcr::GameplayWorld world;
    vcr::NpcAiRuntimeSystem ai_sys;
    ai_sys.sync_agents_from_scene(scene, world);

    // Warmup tick populates A* / world node / steering / agent scratch buffers.
    nav.sync_dynamic_obstacles(scene, nullptr);
    vcr::NavPath warm_path{};
    REQUIRE(nav.find_path({-6.0F, 0.0F, -6.0F}, {6.0F, 0.0F, 6.0F}, warm_path));
    ai_sys.update(
        1.0F / 60.0F, scene, nav, &col_world, nullptr, world, nullptr, nullptr,
        {0.0F, 0.0F, 8.0F}, true, {});

    const std::size_t baseline_nav_cap = nav.scratch_total_capacity();
    const std::size_t baseline_ai_cap = ai_sys.scratch_total_capacity();
    const vcr::DynamicNavBlocker* const baseline_blockers_ptr =
        nav.dynamic_blockers().data();
    const vcr::NpcRuntimeAgent* const baseline_agents_ptr = ai_sys.agents().data();

    for (int frame = 0; frame < 120; ++frame) {
        if ((frame % 20) == 0) {
            ai_sys.perception_mut().emit_noise(
                {2.0F * std::sin(static_cast<float>(frame)), 0.0F, 2.0F},
                1.1F,
                22.0F);
        }
        nav.sync_dynamic_obstacles(scene, nullptr);
        vcr::NavPath frame_path{};
        REQUIRE(nav.find_path({-6.0F, 0.0F, -6.0F}, {6.0F, 0.0F, 6.0F}, frame_path));
        const glm::vec3 steered = nav.compute_local_steering(
            {-4.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 3.0F}, 0.4F, 4.0F);
        REQUIRE(std::isfinite(steered.x) && std::isfinite(steered.z));

        ai_sys.update(
            1.0F / 60.0F, scene, nav, &col_world, nullptr, world, nullptr, nullptr,
            {0.0F, 0.0F, 8.0F}, true, {});
    }

    REQUIRE(nav.scratch_total_capacity() == baseline_nav_cap);
    REQUIRE(ai_sys.scratch_total_capacity() == baseline_ai_cap);
    REQUIRE(nav.dynamic_blockers().data() == baseline_blockers_ptr);
    REQUIRE(ai_sys.agents().data() == baseline_agents_ptr);
}

// 6 & 7. AI Perception: FOV vision cone, LOS raycast occlusion, hearing & wall attenuation.
void test_ai_perception_vision_occlusion_and_hearing() {
    vcr::SceneDocument scene;
    REQUIRE(add_box_node(scene, "floor", {0.0F, -0.5F, 0.0F}, {20.0F, 0.5F, 20.0F}));
    // Solid occluder wall between NPC (0,0,0) and target (0,0,8) at z = 4.
    const vcr::SceneNodeId wall_id =
        add_box_node(scene, "occluder", {0.0F, 1.5F, 4.0F}, {4.0F, 1.5F, 0.4F});
    REQUIRE(wall_id);

    vcr::CollisionWorld wall_world;
    wall_world.rebuild_from_scene(scene);

    vcr::SceneDocument clear_scene;
    REQUIRE(add_box_node(clear_scene, "floor", {0.0F, -0.5F, 0.0F}, {20.0F, 0.5F, 20.0F}));
    vcr::CollisionWorld clear_world;
    clear_world.rebuild_from_scene(clear_scene);

    vcr::AiPerceptionSystem perception;
    vcr::NpcAiComponent npc{};
    npc.sight_range = 15.0F;
    npc.sight_fov_degrees = 90.0F; // +/-45 degrees around +Z at facing yaw 0.
    npc.eye_height = 1.6F;
    npc.hearing_range = 20.0F;
    npc.hearing_threshold = 0.42F;

    vcr::AiPerceptionState state{};
    const vcr::SceneNodeId dummy_npc_id{.index = 99U, .generation = 1U};

    // Inside cone & range + clear LOS => can see target.
    perception.evaluate_perception(
        dummy_npc_id, npc, {0.0F, 0.0F, 0.0F}, 0.0F,
        {0.0F, 0.0F, 8.0F}, {}, true, &clear_world, nullptr, 1.0F / 60.0F, state);
    REQUIRE(state.can_see_target);
    REQUIRE(state.has_last_known_target);
    REQUIRE(close_to(state.time_since_seen, 0.0F));

    // Outside FOV (behind NPC at z = -6) => cannot see.
    perception.evaluate_perception(
        dummy_npc_id, npc, {0.0F, 0.0F, 0.0F}, 0.0F,
        {0.0F, 0.0F, -6.0F}, {}, true, &clear_world, nullptr, 0.25F, state);
    REQUIRE(!state.can_see_target);
    REQUIRE(state.time_since_seen >= 0.24F);

    // Outside sight range => cannot see.
    perception.evaluate_perception(
        dummy_npc_id, npc, {0.0F, 0.0F, 0.0F}, 0.0F,
        {0.0F, 0.0F, 22.0F}, {}, true, &clear_world, nullptr, 0.25F, state);
    REQUIRE(!state.can_see_target);

    // Occluded by wall in wall_world => cannot see, retains last known & accumulates time_since_seen.
    const float prev_since_seen = state.time_since_seen;
    perception.evaluate_perception(
        dummy_npc_id, npc, {0.0F, 0.0F, 0.0F}, 0.0F,
        {0.0F, 0.0F, 8.0F}, {}, true, &wall_world, nullptr, 0.5F, state);
    REQUIRE(!state.can_see_target);
    REQUIRE(state.has_last_known_target);
    REQUIRE(state.time_since_seen >= prev_since_seen + 0.49F);

    // Noise hearing: at d=10, loudness=1.0, max_radius=20 => clear strength=0.5 >= 0.42,
    // occluded strength=0.5*0.7=0.35 < 0.42!
    perception.emit_noise({0.0F, 0.0F, 10.0F}, 1.0F, 20.0F);
    vcr::AiPerceptionState hear_clear{};
    perception.evaluate_perception(
        dummy_npc_id, npc, {0.0F, 0.0F, 0.0F}, 180.0F,
        {0.0F, 0.0F, -20.0F}, {}, true, &clear_world, nullptr, 1.0F / 60.0F, hear_clear);
    REQUIRE(hear_clear.heard_recent_noise);
    REQUIRE(close_to(hear_clear.perceived_noise_strength, 0.5F, 0.02F));

    vcr::AiPerceptionState hear_occluded{};
    perception.evaluate_perception(
        dummy_npc_id, npc, {0.0F, 0.0F, 0.0F}, 180.0F,
        {0.0F, 0.0F, -20.0F}, {}, true, &wall_world, nullptr, 1.0F / 60.0F, hear_occluded);
    REQUIRE(!hear_occluded.heard_recent_noise);

    // Noise re-trigger prevention in NpcAiRuntimeSystem: once investigated, same noise does not re-latch.
    const vcr::SceneNodeId listener_id =
        clear_scene.create_node("listener", vcr::SceneNodeKind::cube);
    if (auto nr = clear_scene.node(listener_id)) {
        nr->get().transform.translation = {0.0F, 0.0F, 0.0F};
    }
    vcr::NpcAiComponent l_npc{};
    l_npc.patrol_enabled = false;
    l_npc.walk_speed = 6.0F;
    l_npc.run_speed = 6.0F;
    l_npc.hearing_range = 20.0F;
    l_npc.hearing_threshold = 0.2F;
    REQUIRE(clear_scene.set_npc_ai(listener_id, l_npc));

    vcr::NavMeshBakeSettings bset{};
    bset.cell_size = 0.5F;
    bset.include_terrain = false;
    vcr::NavMeshData cnav{};
    std::string error;
    REQUIRE_MSG(vcr::bake_navmesh_from_scene(clear_scene, bset, cnav, &error), error);
    vcr::NavigationRuntime nav;
    nav.set_navmesh(cnav);
    vcr::GameplayWorld world;
    vcr::NpcAiRuntimeSystem ai_sys;
    ai_sys.sync_agents_from_scene(clear_scene, world);

    ai_sys.perception_mut().emit_noise({0.0F, 0.0F, 2.0F}, 1.0F, 18.0F, {}, {}, 4.0F);
    bool entered_investigate = false;
    for (int step = 0; step < 90; ++step) {
        ai_sys.update(
            1.0F / 60.0F, clear_scene, nav, &clear_world, nullptr, world, nullptr, nullptr,
            {0.0F, 0.0F, -100.0F}, false, {});
        const vcr::NpcRuntimeAgent* ag = ai_sys.find_agent(listener_id);
        REQUIRE(ag != nullptr);
        if (ag->state == vcr::NpcFsmState::investigate_noise) {
            entered_investigate = true;
        }
        // Once search completes near the noise position, it must not immediately re-enter investigate_noise.
        if (ag->state == vcr::NpcFsmState::search_last_known && step > 30) {
            REQUIRE(!ag->investigating_noise);
        }
    }
    REQUIRE(entered_investigate);
}

// 8. Behavior Tree (.vcrbt.json) cycle validation + GameplaySession::update_headless chase/attack/flee.
void test_behavior_tree_and_gameplay_session_headless() {
    const ScopedTempDir temp;
    const std::filesystem::path asset_root = temp.path() / "assets";
    std::filesystem::create_directories(asset_root);

    vcr::BehaviorTreeAsset bt = vcr::make_default_tactical_behavior_tree(
        "tactical_test", "Tactical Test BT");
    std::string error;
    REQUIRE_MSG(bt.valid(&error), error);
    REQUIRE_MSG(
        vcr::save_behavior_tree_file(bt, asset_root, "ai/tactical_test.vcrbt.json", error),
        error);

    vcr::BehaviorTreeAsset loaded_bt{};
    REQUIRE_MSG(
        vcr::load_behavior_tree_file(asset_root, "ai/tactical_test.vcrbt.json", loaded_bt, error),
        error);
    REQUIRE(loaded_bt == bt);

    // Cyclic Behavior Tree DAG validation must reject cycles.
    vcr::BehaviorTreeAsset cyclic = bt;
    REQUIRE(cyclic.nodes.size() >= 2U);
    REQUIRE(cyclic.nodes[1U].add_child(0U));
    std::string cycle_err;
    REQUIRE(!cyclic.valid(&cycle_err));

    // GameplaySession::update_headless with BT-driven hostile NPC + player.
    vcr::SceneDocument scene;
    REQUIRE(add_box_node(scene, "ground_box", {0.0F, -0.5F, 0.0F}, {16.0F, 0.5F, 16.0F}));
    const vcr::SceneNodeId npc_id =
        add_box_node(scene, "hostile_npc", {0.0F, 0.0F, 0.0F}, {0.4F, 0.9F, 0.4F});
    REQUIRE(npc_id);

    vcr::NpcAiComponent npc{};
    npc.mode = vcr::NpcAiControlMode::behavior_tree;
    REQUIRE(npc.set_behavior_tree_path("ai/tactical_test.vcrbt.json"));
    npc.patrol_enabled = false;
    npc.walk_speed = 4.0F;
    npc.run_speed = 6.0F;
    npc.attack_range = 2.0F;
    npc.attack_damage = 15.0F;
    npc.attack_cooldown = 0.2F;
    npc.sight_range = 24.0F;
    npc.sight_fov_degrees = 140.0F;
    npc.flee_health_fraction = 0.25F;
    REQUIRE(scene.set_npc_ai(npc_id, npc));

    vcr::GameplayComponent gp{};
    REQUIRE(gp.set_behavior("destructible"));
    gp.health = 100.0F;
    gp.max_health = 100.0F;
    REQUIRE(scene.set_gameplay(npc_id, gp));

    vcr::NavMeshBakeSettings bset{};
    bset.cell_size = 0.5F;
    bset.include_terrain = false;
    vcr::NavMeshData baked_nav{};
    REQUIRE_MSG(vcr::bake_navmesh_from_scene(scene, bset, baked_nav, &error), error);

    vcr::GameplaySession session;
    session.set_content_asset_root(asset_root);
    session.set_startup_navmesh(baked_nav);
    session.set_player_setup(vcr::GameplayPlayerSetup{
        .spawn_position = glm::vec3{0.0F, 0.0F, 6.0F},
        .initial_input_context = vcr::InputContext::gameplay,
    });

    vcr::FlyCamera start_cam;
    vcr::FlyCamera out_cam;
    std::vector<vcr::SceneGpuInstance> cube_storage;
    std::vector<vcr::SceneModelInstance> model_storage;
    std::vector<vcr::ScenePunctualLight> light_storage;
    const vcr::SceneRenderView init_view = scene.build_render_view(
        cube_storage, model_storage, light_storage);
    REQUIRE_MSG(
        session.start(scene, init_view, start_cam, out_cam, error), error);

    vcr::GameplayFrameInput frame_in{};
    bool saw_chase = false;
    bool saw_attack = false;
    for (int i = 0; i < 180; ++i) {
        REQUIRE(session.update_headless(frame_in, 1.0F / 60.0F, scene, out_cam));
        if (const vcr::NpcRuntimeAgent* ag = session.npc_ai().find_agent(npc_id)) {
            if (ag->state == vcr::NpcFsmState::chase) {
                saw_chase = true;
            } else if (ag->state == vcr::NpcFsmState::attack) {
                saw_attack = true;
            }
        }
        if (saw_chase && saw_attack &&
            session.gameplay_world().player_health() < 100.0F) {
            break;
        }
    }
    REQUIRE(saw_chase);
    REQUIRE(saw_attack);
    REQUIRE(session.gameplay_world().player_health() < 100.0F);

    // Damage NPC below flee_health_fraction => BT flee branch activates and distance grows.
    const float dist_before_flee = glm::distance(
        session.npc_ai().find_agent(npc_id)->position,
        session.gameplay_world().player_position());
    REQUIRE(session.gameplay_world().apply_damage(npc_id, 90.0F));
    bool saw_flee = false;
    for (int i = 0; i < 60; ++i) {
        REQUIRE(session.update_headless(frame_in, 1.0F / 60.0F, scene, out_cam));
        if (const vcr::NpcRuntimeAgent* ag = session.npc_ai().find_agent(npc_id)) {
            if (ag->state == vcr::NpcFsmState::flee) {
                saw_flee = true;
            }
        }
    }
    const float dist_after_flee = glm::distance(
        session.npc_ai().find_agent(npc_id)->position,
        session.gameplay_world().player_position());
    REQUIRE(saw_flee);
    REQUIRE(dist_after_flee > dist_before_flee + 1.0F);
    session.stop();
}

// 9. Scene / Prefab / Duplicate / Save-Load state persistence.
void test_scene_prefab_duplicate_and_save_load_persistence() {
    const ScopedTempDir temp;
    const std::filesystem::path asset_root = temp.path() / "assets";
    std::filesystem::create_directories(asset_root);

    vcr::SceneDocument scene;
    REQUIRE(add_box_node(scene, "floor", {0.0F, -0.5F, 0.0F}, {16.0F, 0.5F, 16.0F}));
    const vcr::SceneNodeId rig_root =
        scene.create_node("nav_ai_rig", vcr::SceneNodeKind::folder);
    const vcr::SceneNodeId target_node =
        scene.create_node("rig_target", vcr::SceneNodeKind::folder, rig_root);
    const vcr::SceneNodeId rail_node =
        scene.create_node("rig_spline", vcr::SceneNodeKind::folder, rig_root);
    const vcr::SceneNodeId obs_node =
        add_box_node(scene, "rig_obstacle", {2.0F, 0.5F, 2.0F}, {0.5F, 0.5F, 0.5F});
    REQUIRE(scene.reparent(obs_node, rig_root));
    const vcr::SceneNodeId oml_node =
        scene.create_node("rig_jump", vcr::SceneNodeKind::folder, rig_root);
    const vcr::SceneNodeId npc_node =
        scene.create_node("rig_npc", vcr::SceneNodeKind::cube, rig_root);

    vcr::SplineComponent sp{};
    sp.point_count = 2U;
    sp.points[0].position = {0.0F, 0.0F, 0.0F};
    sp.points[1].position = {5.0F, 0.0F, 0.0F};
    REQUIRE(scene.set_spline(rail_node, sp));

    vcr::NavObstacleComponent obs{};
    obs.shape = vcr::NavObstacleShape::cylinder;
    obs.radius = 1.25F;
    obs.height = 2.4F;
    obs.block_navigation = false;
    obs.cost_penalty = 9.5F;
    REQUIRE(scene.set_nav_obstacle(obs_node, obs));

    vcr::OffMeshLinkComponent oml{};
    oml.target_node_uuid = *scene.uuid(target_node);
    oml.traversal_kind = vcr::OffMeshTraversalKind::climb;
    oml.jump_height = 2.0F;
    REQUIRE(scene.set_off_mesh_link(oml_node, oml));

    vcr::NpcAiComponent npc{};
    npc.target_node_uuid = *scene.uuid(target_node);
    npc.patrol_spline_uuid = *scene.uuid(rail_node);
    npc.walk_speed = 3.2F;
    npc.run_speed = 6.4F;
    REQUIRE(scene.set_npc_ai(npc_node, npc));

    // Scene save / load round-trip.
    std::string error;
    const std::filesystem::path scene_path = asset_root / "scenes/nav_persist.vcrscene.json";
    REQUIRE_MSG(vcr::save_scene_file(scene, scene_path, error), error);
    vcr::SceneDocument loaded_scene;
    REQUIRE_MSG(vcr::load_scene_file(scene_path, loaded_scene, error), error);
    const vcr::SceneNodeId l_obs = loaded_scene.find_by_uuid(*scene.uuid(obs_node));
    const vcr::SceneNodeId l_oml = loaded_scene.find_by_uuid(*scene.uuid(oml_node));
    const vcr::SceneNodeId l_npc = loaded_scene.find_by_uuid(*scene.uuid(npc_node));
    REQUIRE(l_obs && loaded_scene.nav_obstacle(l_obs)->get() == obs);
    REQUIRE(l_oml && loaded_scene.off_mesh_link(l_oml)->get() == oml);
    REQUIRE(l_npc && loaded_scene.npc_ai(l_npc)->get() == npc);

    // Prefab save + PrefabNodeOverride + UUID remapping.
    REQUIRE_MSG(
        vcr::save_prefab_file(scene, rig_root, asset_root, "prefabs/ai_rig.vcrprefab.json", error),
        error);
    vcr::NpcAiComponent override_npc = npc;
    override_npc.walk_speed = 5.0F;
    override_npc.run_speed = 9.0F;
    const std::array<vcr::PrefabNodeOverride, 1U> overrides{{
        vcr::PrefabNodeOverride{
            .source_node_uuid = *scene.uuid(npc_node),
            .npc_ai = override_npc,
        },
    }};
    vcr::PrefabInstantiateResult p_inst{};
    REQUIRE_MSG(
        vcr::instantiate_prefab_file(
            loaded_scene,
            asset_root,
            "prefabs/ai_rig.vcrprefab.json",
            vcr::PrefabInstantiateOptions{.overrides = overrides},
            p_inst,
            error),
        error);
    REQUIRE(p_inst.root_id);

    // Collect children under p_inst.root_id to verify internal UUID remapping & override.
    std::vector<vcr::SceneNodeRecord> recs;
    loaded_scene.build_node_records(recs);
    vcr::SceneUuid inst_target_uuid{};
    vcr::SceneUuid inst_spline_uuid{};
    vcr::SceneNodeId inst_oml_id{};
    vcr::SceneNodeId inst_npc_id{};
    for (const auto& r : recs) {
        if (loaded_scene.parent(r.id) == p_inst.root_id) {
            if (r.value.name == "rig_target") inst_target_uuid = r.uuid;
            if (r.value.name == "rig_spline") inst_spline_uuid = r.uuid;
            if (r.value.name == "rig_jump") inst_oml_id = r.id;
            if (r.value.name == "rig_npc") inst_npc_id = r.id;
        }
    }
    REQUIRE(inst_target_uuid && inst_spline_uuid && inst_oml_id && inst_npc_id);
    REQUIRE(loaded_scene.off_mesh_link(inst_oml_id)->get().target_node_uuid == inst_target_uuid);
    REQUIRE(loaded_scene.npc_ai(inst_npc_id)->get().target_node_uuid == inst_target_uuid);
    REQUIRE(loaded_scene.npc_ai(inst_npc_id)->get().patrol_spline_uuid == inst_spline_uuid);
    REQUIRE(close_to(loaded_scene.npc_ai(inst_npc_id)->get().walk_speed, 5.0F));

    // Subtree duplication remaps internal UUIDs away from source.
    std::uint32_t dup_nodes = 0U;
    const vcr::SceneNodeId dup_root = vcr::duplicate_scene_subtree(
        loaded_scene, p_inst.root_id, dup_nodes, error);
    REQUIRE_MSG(dup_root, error);
    loaded_scene.build_node_records(recs);
    vcr::SceneUuid dup_target_uuid{};
    vcr::SceneUuid dup_spline_uuid{};
    vcr::SceneNodeId dup_oml_id{};
    vcr::SceneNodeId dup_npc_id{};
    for (const auto& r : recs) {
        if (loaded_scene.parent(r.id) == dup_root) {
            if (r.value.name == "rig_target") dup_target_uuid = r.uuid;
            if (r.value.name == "rig_spline") dup_spline_uuid = r.uuid;
            if (r.value.name == "rig_jump") dup_oml_id = r.id;
            if (r.value.name == "rig_npc") dup_npc_id = r.id;
        }
    }
    REQUIRE(dup_target_uuid && dup_target_uuid != inst_target_uuid);
    REQUIRE(dup_spline_uuid && dup_spline_uuid != inst_spline_uuid);
    REQUIRE(loaded_scene.off_mesh_link(dup_oml_id)->get().target_node_uuid == dup_target_uuid);
    REQUIRE(loaded_scene.npc_ai(dup_npc_id)->get().target_node_uuid == dup_target_uuid);
    REQUIRE(loaded_scene.npc_ai(dup_npc_id)->get().patrol_spline_uuid == dup_spline_uuid);

    // Save / Load runtime NPC agent state via GameplaySession.
    vcr::GameplaySession session;
    vcr::FlyCamera sc{};
    vcr::FlyCamera oc{};
    std::vector<vcr::SceneGpuInstance> cs;
    std::vector<vcr::SceneModelInstance> ms;
    std::vector<vcr::ScenePunctualLight> ls;
    REQUIRE_MSG(
        session.start(
            scene, scene.build_render_view(cs, ms, ls), sc, oc, error),
        error);
    vcr::NpcRuntimeAgent* mut_ag = session.npc_ai_mut().find_agent_mut(npc_node);
    REQUIRE(mut_ag != nullptr);
    mut_ag->state = vcr::NpcFsmState::search_last_known;
    mut_ag->position = {4.25F, 0.0F, -2.5F};
    mut_ag->facing_yaw_degrees = 135.0F;
    mut_ag->perception.has_last_known_target = true;
    mut_ag->perception.last_known_target_position = {7.0F, 0.0F, -3.0F};

    const std::filesystem::path save_path = temp.path() / "nav_ai.vcrsave.json";
    REQUIRE_MSG(session.save_game_state(save_path, error), error);

    // Mutate runtime before loading snapshot back.
    mut_ag->state = vcr::NpcFsmState::idle;
    mut_ag->position = {0.0F, 0.0F, 0.0F};
    mut_ag->facing_yaw_degrees = 0.0F;
    REQUIRE_MSG(session.load_game_state_headless(save_path, scene, oc, error), error);

    const vcr::NpcRuntimeAgent* restored = session.npc_ai().find_agent(npc_node);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->state == vcr::NpcFsmState::search_last_known);
    REQUIRE(glm::distance(restored->position, {4.25F, 0.0F, -2.5F}) <= 1.0e-3F);
    REQUIRE(close_to(restored->facing_yaw_degrees, 135.0F, 1.0e-2F));
    REQUIRE(restored->perception.has_last_known_target);
    REQUIRE(glm::distance(
                restored->perception.last_known_target_position,
                {7.0F, 0.0F, -3.0F}) <= 1.0e-3F);
    session.stop();
}

// 10. ship_project validation for .vcrnav.json and .vcrbt.json assets & references.
void test_ship_project_nav_and_bt_validation() {
    const ScopedTempDir temp;
    vcr::CreateProjectResult created{};
    std::string error;
    REQUIRE_MSG(
        vcr::create_project_on_disk(
            vcr::CreateProjectOptions{
                .name = "Nav Ship Check",
                .parent_directory = temp.path() / "ship_proj",
                .template_kind = vcr::ProjectTemplateKind::standard,
                .bundle_runtime = false,
            },
            created,
            error),
        error);

    vcr::SceneDocument startup_scene;
    const auto startup_file =
        created.project_directory / "assets" / created.manifest.startup_scene;
    REQUIRE_MSG(vcr::load_scene_file(startup_file, startup_scene, error), error);

    // Author valid .vcrnav.json and .vcrbt.json referenced by an NPC in startup scene.
    vcr::NavMeshData baked_nav{};
    REQUIRE_MSG(
        vcr::bake_navmesh_from_scene(
            startup_scene, vcr::NavMeshBakeSettings{}, baked_nav, &error),
        error);
    REQUIRE_MSG(
        vcr::save_navmesh_file(
            baked_nav, created.project_directory / "assets", "navmesh/main.vcrnav.json", error),
        error);

    const vcr::BehaviorTreeAsset bt =
        vcr::make_default_tactical_behavior_tree("ship_npc", "Ship NPC BT");
    REQUIRE_MSG(
        vcr::save_behavior_tree_file(
            bt, created.project_directory / "assets", "ai/ship_npc.vcrbt.json", error),
        error);

    const vcr::SceneNodeId guard_id =
        startup_scene.create_node("ship_guard", vcr::SceneNodeKind::cube);
    vcr::NpcAiComponent npc{};
    npc.mode = vcr::NpcAiControlMode::behavior_tree;
    REQUIRE(npc.set_behavior_tree_path("ai/ship_npc.vcrbt.json"));
    REQUIRE(startup_scene.set_npc_ai(guard_id, npc));
    REQUIRE_MSG(vcr::save_scene_file(startup_scene, startup_file, error), error);

    const std::filesystem::path ship_ok = temp.path() / "ship_out" / "ok";
    vcr::ShipProjectResult shipped{};
    REQUIRE_MSG(
        vcr::ship_project(created.project_directory, ship_ok, shipped, error),
        error);
    REQUIRE(std::filesystem::is_regular_file(
        ship_ok / "assets/navmesh/main.vcrnav.json"));
    REQUIRE(std::filesystem::is_regular_file(
        ship_ok / "assets/ai/ship_npc.vcrbt.json"));

    // Missing referenced .vcrbt.json must fail ship_project and clean up output dir.
    std::filesystem::remove(created.project_directory / "assets/ai/ship_npc.vcrbt.json");
    const std::filesystem::path ship_missing_bt = temp.path() / "ship_out" / "missing_bt";
    REQUIRE(!vcr::ship_project(
        created.project_directory, ship_missing_bt, shipped, error));
    REQUIRE(!std::filesystem::exists(ship_missing_bt));

    // Restore BT and introduce corrupt .vcrnav.json -> must also abort cleanly.
    REQUIRE_MSG(
        vcr::save_behavior_tree_file(
            bt, created.project_directory / "assets", "ai/ship_npc.vcrbt.json", error),
        error);
    write_text_file(
        created.project_directory / "assets/navmesh/broken.vcrnav.json",
        R"({"format":"bad_nav","version":99})");
    const std::filesystem::path ship_bad_nav = temp.path() / "ship_out" / "bad_nav";
    REQUIRE(!vcr::ship_project(
        created.project_directory, ship_bad_nav, shipped, error));
    REQUIRE(!std::filesystem::exists(ship_bad_nav));
}

} // namespace

int main() {
    test_navmesh_bake_and_serialization();
    test_astar_and_funnel_pathfinding();
    test_dynamic_obstacles_and_cost_penalties();
    test_off_mesh_links_and_runtime_traversal();
    test_zero_steady_state_hot_path_allocations();
    test_ai_perception_vision_occlusion_and_hearing();
    test_behavior_tree_and_gameplay_session_headless();
    test_scene_prefab_duplicate_and_save_load_persistence();
    test_ship_project_nav_and_bt_validation();

    if (g_failures != 0) {
        std::cerr << g_failures << " test check(s) failed.\n";
        return 1;
    }
    std::cout << "All navigation and NPC AI tests passed.\n";
    return 0;
}
