#include "collision.hpp"
#include "gameplay.hpp"
#include "physics_world.hpp"
#include "scene_document.hpp"
#include "terrain_height.hpp"

#include <cmath>
#include <iostream>
#include <limits>

#include <glm/geometric.hpp>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] bool close_to(
    const float value,
    const float expected,
    const float epsilon = 0.0001F) {
    return std::abs(value - expected) <= epsilon;
}

[[nodiscard]] float planar_distance(const glm::vec3 value) {
    return std::sqrt(value.x * value.x + value.z * value.z);
}

} // namespace

int main() {
    const glm::vec3 camera_forward{0.0F, 0.0F, -1.0F};

    {
        vcr::CharacterController walking;
        walking.reset({}, 0.0F);
        vcr::CharacterInput input{.forward = true};
        for (int step = 0; step < 20; ++step) {
            walking.update(input, camera_forward, 0.05F);
        }
        expect(
            close_to(walking.position().z, -3.5F),
            "W moves the character at walk speed relative to the camera");
        expect(
            std::abs(walking.facing_yaw()) > 0.1F,
            "movement turns the character toward its travel direction");

        vcr::CharacterController running;
        running.reset({}, 3.14159265359F);
        input.run = true;
        for (int step = 0; step < 20; ++step) {
            running.update(input, camera_forward, 0.05F);
        }
        expect(
            close_to(running.position().z, -7.0F),
            "Shift run uses the configured run speed");
    }

    {
        vcr::CharacterController straight;
        vcr::CharacterController diagonal;
        straight.reset({}, 3.14159265359F);
        diagonal.reset({}, 3.14159265359F);
        vcr::CharacterInput input{.forward = true};
        straight.update(input, camera_forward, 0.05F);
        input.right = true;
        diagonal.update(input, camera_forward, 0.05F);
        expect(
            close_to(
                planar_distance(straight.position()),
                planar_distance(diagonal.position())),
            "diagonal character movement is normalized");

        input = {
            .forward = true,
            .backward = true,
            .left = true,
            .right = true,
        };
        const glm::vec3 before = diagonal.position();
        diagonal.update(input, camera_forward, 0.05F);
        expect(
            glm::length(diagonal.position() - before) < 0.0001F,
            "opposing character inputs cancel");
    }

    {
        vcr::CharacterController character(vcr::CharacterConfig{
            .ground_height = -2.0F,
        });
        character.reset({0.0F, -2.0F, 0.0F}, 3.14159265359F);
        vcr::CharacterInput input{.jump = true};
        character.update(input, camera_forward, 0.05F);
        expect(
            !character.grounded() && character.position().y > -2.0F &&
                character.vertical_velocity() > 0.0F,
            "Space starts an upward jump from the authored ground height");
        for (int step = 0; step < 100; ++step) {
            character.update(input, camera_forward, 0.05F);
        }
        expect(
            character.grounded() && close_to(character.position().y, -2.0F),
            "holding Space lands without automatic repeated jumps");
        input.jump = false;
        character.update(input, camera_forward, 0.05F);
        input.jump = true;
        character.update(input, camera_forward, 0.05F);
        expect(
            !character.grounded() && character.position().y > -2.0F,
            "releasing Space arms the next jump");
    }

    {
        vcr::CharacterController character;
        character.reset({}, 3.14159265359F);
        const vcr::CharacterInput input{.forward = true};
        character.update(input, camera_forward, 1.0F);
        expect(
            close_to(character.position().z, -0.175F),
            "character simulation clamps long frame deltas");
        const glm::vec3 before = character.position();
        character.update(input, camera_forward, -1.0F);
        character.update(
            input,
            camera_forward,
            std::numeric_limits<float>::quiet_NaN());
        expect(
            glm::length(character.position() - before) < 0.0001F,
            "invalid character deltas do not move the character");
    }

    {
        vcr::ThirdPersonCamera camera;
        camera.reset(-1.57079632679F);
        const glm::vec3 target{0.0F, 0.0F, 0.0F};
        const glm::vec3 initial_position = camera.position(target);
        expect(
            initial_position.y > 1.2F && initial_position.z > 0.0F &&
                camera.planar_forward().z < -0.99F,
            "third-person camera starts above and behind its target");
        const glm::vec3 initial_forward = camera.forward();
        camera.rotate(100.0F, 0.0F);
        expect(
            glm::length(camera.forward() - initial_forward) > 0.01F,
            "mouse X orbits the third-person camera");
        camera.rotate(0.0F, 100'000.0F);
        expect(
            camera.pitch() < 0.0F && camera.pitch() >= -1.3091F,
            "third-person camera pitch stays above the character and avoids a pole");
        const float yaw = camera.yaw();
        camera.rotate(std::numeric_limits<float>::quiet_NaN(), 0.0F);
        expect(
            camera.yaw() == yaw,
            "invalid mouse input does not change the third-person camera");

        vcr::ThirdPersonCamera invalid_config(vcr::ThirdPersonCameraConfig{
            .default_pitch_radians =
                std::numeric_limits<float>::quiet_NaN(),
        });
        invalid_config.reset(0.0F);
        expect(
            std::isfinite(invalid_config.pitch()),
            "invalid camera configuration falls back to a finite pitch");
    }

    // 3D CollisionWorld raycasts: OBB, sphere, capsule, terrain, and trigger filtering.
    {
        vcr::SceneDocument scene;
        const auto wall = scene.create_node("Wall", vcr::SceneNodeKind::cube);
        scene.node(wall)->get().transform.translation = {0.0F, 1.0F, -5.0F};
        scene.node(wall)->get().transform.scale = {2.0F, 2.0F, 0.5F};

        const auto orb = scene.create_node("SphereCol", vcr::SceneNodeKind::folder);
        scene.node(orb)->get().transform.translation = {6.0F, 1.0F, -4.0F};
        static_cast<void>(scene.set_collider(
            orb,
            vcr::ColliderComponent{
                .radius = 1.25F,
                .shape = vcr::ColliderShape::sphere,
            }));

        const auto pillar = scene.create_node("Pillar", vcr::SceneNodeKind::folder);
        scene.node(pillar)->get().transform.translation = {-6.0F, 1.5F, -4.0F};
        static_cast<void>(scene.set_collider(
            pillar,
            vcr::ColliderComponent{
                .radius = 0.5F,
                .half_height = 1.0F,
                .shape = vcr::ColliderShape::capsule,
            }));

        const auto trigger = scene.create_node("Zone", vcr::SceneNodeKind::folder);
        scene.node(trigger)->get().transform.translation = {0.0F, 1.0F, -2.0F};
        static_cast<void>(scene.set_collider(
            trigger,
            vcr::ColliderComponent{
                .half_extents = {1.0F, 1.0F, 0.5F},
                .shape = vcr::ColliderShape::box,
                .is_trigger = true,
            }));

        const auto terrain = scene.create_node("Hills", vcr::SceneNodeKind::terrain);
        static_cast<void>(terrain);

        vcr::CollisionWorld world;
        world.rebuild_from_scene(scene);
        expect(
            world.solid_count() == 3U &&
                world.trigger_count() == 1U &&
                world.terrain_enabled(),
            "CollisionWorld categorizes solid colliders, triggers, and terrain");

        const auto solid_hit = world.raycast(
            {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, 20.0F, 0xFFFFFFFFU, false);
        expect(
            solid_hit && solid_hit->node_id == wall &&
                close_to(solid_hit->distance, 4.5F, 0.001F) &&
                close_to(solid_hit->normal.z, 1.0F, 0.001F),
            "raycast ignores triggers by default and hits the solid OBB face");

        const auto trigger_hit = world.raycast(
            {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, 20.0F, 0xFFFFFFFFU, true);
        expect(
            trigger_hit && trigger_hit->node_id == trigger &&
                trigger_hit->is_trigger &&
                close_to(trigger_hit->distance, 1.5F, 0.001F),
            "raycast optionally reports trigger intersections");

        const auto sphere_hit = world.raycast(
            {6.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, 20.0F);
        expect(
            sphere_hit && sphere_hit->node_id == orb &&
                close_to(sphere_hit->distance, 2.75F, 0.001F),
            "raycast hits sphere collider accurately");

        const auto capsule_hit = world.raycast(
            {-6.0F, 1.5F, 0.0F}, {0.0F, 0.0F, -1.0F}, 20.0F);
        expect(
            capsule_hit && capsule_hit->node_id == pillar &&
                close_to(capsule_hit->distance, 3.5F, 0.001F),
            "raycast hits capsule collider cylinder accurately");

        const float expected_terrain_y =
            vcr::procedural_landscape_height(-10.0F, -10.0F);
        const auto terrain_hit = world.raycast(
            {-10.0F, 12.0F, -10.0F}, {0.0F, -1.0F, 0.0F}, 30.0F);
        expect(
            terrain_hit && terrain_hit->hit_terrain &&
                close_to(terrain_hit->point.y, expected_terrain_y, 0.005F),
            "raycast intersects procedural terrain heightfield accurately");
    }

    // CharacterController: wall blocking, angled wall sliding, step-up, platform landing, camera anti-clip, and triggers.
    {
        vcr::SceneDocument scene;
        const auto ground = scene.create_node("Floor", vcr::SceneNodeKind::ground);
        scene.node(ground)->get().transform.scale = {40.0F, 1.0F, 40.0F};

        // Tall solid wall at Z = -3.0 (half-extent Z = 0.5 => front face at Z = -2.5)
        const auto tall_wall = scene.create_node("TallWall", vcr::SceneNodeKind::cube);
        scene.node(tall_wall)->get().transform.translation = {0.0F, 1.5F, -3.0F};
        scene.node(tall_wall)->get().transform.scale = {3.0F, 1.5F, 0.5F};

        // Angled wall at X = 8.0, rotated 45 deg around Y
        const auto angled_wall = scene.create_node("AngledWall", vcr::SceneNodeKind::cube);
        scene.node(angled_wall)->get().transform.translation = {8.0F, 1.5F, -3.0F};
        scene.node(angled_wall)->get().transform.rotation_degrees = {0.0F, 45.0F, 0.0F};
        scene.node(angled_wall)->get().transform.scale = {2.5F, 1.5F, 0.4F};

        // Low step (top at Y = 0.30m <= step_height 0.45m) at X = -8.0, Z = -3.0
        const auto low_step = scene.create_node("LowStep", vcr::SceneNodeKind::cube);
        scene.node(low_step)->get().transform.translation = {-8.0F, 0.15F, -3.0F};
        scene.node(low_step)->get().transform.scale = {1.5F, 0.15F, 1.5F};

        // Trigger zone at X = 0, Z = 6
        const auto trigger_box = scene.create_node("TriggerBox", vcr::SceneNodeKind::folder);
        scene.node(trigger_box)->get().transform.translation = {0.0F, 1.0F, 6.0F};
        static_cast<void>(scene.set_collider(
            trigger_box,
            vcr::ColliderComponent{
                .half_extents = {1.5F, 1.5F, 1.5F},
                .shape = vcr::ColliderShape::box,
                .is_trigger = true,
            }));

        vcr::CollisionWorld world;
        world.rebuild_from_scene(scene);

        // 1. Tall wall stops forward walk without penetration.
        vcr::CharacterController player;
        player.reset({0.0F, 0.0F, 0.0F}, 3.14159265359F);
        const vcr::CharacterInput walk_forward{.forward = true};
        for (int i = 0; i < 60; ++i) {
            player.update(walk_forward, camera_forward, 0.03F, world);
        }
        expect(
            player.position().z > -2.5F + player.config().capsule_radius - 0.02F &&
                player.position().z < -1.9F &&
                player.grounded(),
            "tall wall blocks horizontal character penetration");

        // 2. Angled wall slides character laterally along X while advancing into -Z.
        player.reset({8.0F, 0.0F, 0.0F}, 3.14159265359F);
        for (int i = 0; i < 60; ++i) {
            player.update(walk_forward, camera_forward, 0.03F, world);
        }
        expect(
            std::abs(player.position().x - 8.0F) > 0.4F &&
                player.position().z < -1.5F,
            "angled wall slides the character smoothly instead of snagging");

        // 3. Low step is climbed automatically via step-up, then exited onto the floor.
        player.reset({-8.0F, 0.0F, -0.5F}, 3.14159265359F);
        for (int i = 0; i < 25; ++i) {
            player.update(walk_forward, camera_forward, 0.03F, world);
        }
        expect(
            player.grounded() &&
                close_to(player.position().y, 0.30F, 0.02F) &&
                player.position().z < -2.2F &&
                player.position().z > -4.2F,
            "character steps up onto a curb below step_height");
        for (int i = 0; i < 30; ++i) {
            player.update(walk_forward, camera_forward, 0.03F, world);
        }
        expect(
            player.grounded() &&
                close_to(player.position().y, 0.0F, 0.02F) &&
                player.position().z < -5.0F,
            "character steps back down cleanly after crossing the curb");

        // 4. ThirdPersonCamera pulls in front of solid walls, ignores triggers.
        vcr::ThirdPersonCamera cam;
        cam.reset(1.57079632679F); // Looking toward +Z, so camera arm extends toward -Z into TallWall!
        const glm::vec3 unobstructed = cam.position({0.0F, 0.0F, -0.5F});
        const glm::vec3 obstructed = cam.position({0.0F, 0.0F, -0.5F}, world);
        expect(
            unobstructed.z < -3.5F &&
                obstructed.z > -2.5F &&
                obstructed.z > unobstructed.z,
            "third-person camera pulls in front of an occluding wall");

        // 5. TriggerOverlapTracker emits enter once, holds active, then emits exit.
        vcr::TriggerOverlapTracker tracker;
        tracker.update(world, {0.0F, 0.0F, 0.0F}, 0.38F, 1.8F);
        expect(
            tracker.active_overlaps().empty() && tracker.events().empty(),
            "trigger tracker starts empty outside trigger volumes");

        tracker.update(world, {0.0F, 0.0F, 6.0F}, 0.38F, 1.8F);
        expect(
            tracker.active_overlaps().size() == 1U &&
                tracker.events().size() == 1U &&
                tracker.events()[0].node_id == trigger_box &&
                tracker.events()[0].kind == vcr::TriggerEventKind::entered,
            "entering a trigger volume emits one entered event");

        tracker.update(world, {0.2F, 0.0F, 6.0F}, 0.38F, 1.8F);
        expect(
            tracker.active_overlaps().size() == 1U && tracker.events().empty(),
            "remaining inside a trigger volume emits no duplicate events");

        tracker.update(world, {0.0F, 0.0F, 0.0F}, 0.38F, 1.8F);
        expect(
            tracker.active_overlaps().empty() &&
                tracker.events().size() == 1U &&
                tracker.events()[0].node_id == trigger_box &&
                tracker.events()[0].kind == vcr::TriggerEventKind::exited,
            "leaving a trigger volume emits one exited event");
    }

    // Cooked convex_hull, triangle_mesh, and compound submesh_boxes in CollisionWorld:
    {
        vcr::CookedModelCollision pyramid_collision{};
        pyramid_collision.local_bounds = vcr::CookedCollisionBox{
            .center = {0.0F, 1.0F, 0.0F},
            .half_extents = {2.0F, 1.0F, 2.0F},
        };
        pyramid_collision.submesh_boxes = {
            vcr::CookedCollisionBox{
                .center = {-1.0F, 0.5F, 0.0F},
                .half_extents = {0.5F, 0.5F, 0.5F},
            },
            vcr::CookedCollisionBox{
                .center = {1.0F, 0.5F, 0.0F},
                .half_extents = {0.5F, 0.5F, 0.5F},
            },
        };
        pyramid_collision.convex_hull_vertices = {
            {-2.0F, 0.0F, -2.0F},
            {2.0F, 0.0F, -2.0F},
            {2.0F, 0.0F, 2.0F},
            {-2.0F, 0.0F, 2.0F},
            {0.0F, 2.0F, 0.0F},
        };
        pyramid_collision.triangle_positions = pyramid_collision.convex_hull_vertices;
        pyramid_collision.triangle_indices = {
            0U, 1U, 4U,
            1U, 2U, 4U,
            2U, 3U, 4U,
            3U, 0U, 4U,
            0U, 3U, 2U,
            0U, 2U, 1U,
        };

        const vcr::ModelCollisionProvider provider{
            .context = &pyramid_collision,
            .query = [](const void* const ctx,
                        const std::string_view path) noexcept
                -> const vcr::CookedModelCollision* {
                return path == "models/pyramid.vcrasset.json"
                    ? static_cast<const vcr::CookedModelCollision*>(ctx)
                    : nullptr;
            },
        };

        vcr::SceneDocument scene;
        const auto compound_model = scene.create_node(
            "CompoundPillars", vcr::SceneNodeKind::model);
        scene.node(compound_model)->get().asset_path =
            "models/pyramid.vcrasset.json";

        const auto hull_model = scene.create_node(
            "HullPyramid", vcr::SceneNodeKind::model);
        scene.node(hull_model)->get().asset_path =
            "models/pyramid.vcrasset.json";
        scene.node(hull_model)->get().transform.translation = {8.0F, 0.0F, 0.0F};
        vcr::ColliderComponent hull_col{};
        hull_col.shape = vcr::ColliderShape::convex_hull;
        expect(scene.set_collider(hull_model, hull_col), "attach convex_hull collider");

        const auto tri_model = scene.create_node(
            "TriPyramid", vcr::SceneNodeKind::model);
        scene.node(tri_model)->get().asset_path =
            "models/pyramid.vcrasset.json";
        scene.node(tri_model)->get().transform.translation = {16.0F, 0.0F, 0.0F};
        vcr::ColliderComponent tri_col{};
        tri_col.shape = vcr::ColliderShape::triangle_mesh;
        expect(scene.set_collider(tri_model, tri_col), "attach triangle_mesh collider");

        vcr::CollisionWorld world;
        world.rebuild_from_scene(scene, {}, {}, provider);

        // Gap between the two submesh boxes at x = 0 should NOT hit!
        const auto gap_miss = world.raycast({0.0F, 0.5F, 5.0F}, {0.0F, 0.0F, -1.0F}, 10.0F);
        const auto left_hit = world.raycast({-1.0F, 0.5F, 5.0F}, {0.0F, 0.0F, -1.0F}, 10.0F);
        expect(
            !gap_miss.has_value() &&
                left_hit.has_value() && left_hit->node_id == compound_model,
            "auto_bounds model builds per-submesh compound OBBs preserving interior gaps");

        const auto hull_hit = world.raycast({8.0F, 5.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 10.0F);
        expect(
            hull_hit.has_value() &&
                hull_hit->node_id == hull_model &&
                close_to(hull_hit->point.y, 2.0F, 0.05F),
            "convex_hull collider raycasts against pyramid apex");

        const auto tri_hit = world.raycast({16.0F, 5.0F, 1.0F}, {0.0F, -1.0F, 0.0F}, 10.0F);
        expect(
            tri_hit.has_value() &&
                tri_hit->node_id == tri_model &&
                close_to(tri_hit->point.y, 1.0F, 0.05F),
            "triangle_mesh collider raycasts accurately against sloped pyramid face");

        const auto tri_capsule =
            world.resolve_capsule({16.0F, 0.8F, 1.0F}, 0.35F, 1.6F, 0.7F);
        expect(
            tri_capsule.resolved_feet.y > 0.95F,
            "triangle_mesh collider resolves capsule penetration upward on sloped face");
    }

    // PhysicsWorld: free-fall, sleep, elastic bounce, stacking, raycast, and TransformRows:
    {
        vcr::SceneDocument scene;
        const auto ground = scene.create_node("Ground", vcr::SceneNodeKind::ground);
        scene.node(ground)->get().transform.translation = {0.0F, 0.0F, 0.0F};
        scene.node(ground)->get().transform.scale = {20.0F, 1.0F, 20.0F};

        const auto crate_a = scene.create_node("CrateA", vcr::SceneNodeKind::cube);
        scene.node(crate_a)->get().transform.translation = {0.0F, 1.8F, 0.0F};
        scene.node(crate_a)->get().transform.scale = {0.5F, 0.5F, 0.5F};
        expect(
            scene.set_rigid_body(
                crate_a,
                vcr::RigidBodyComponent{
                    .mass = 15.0F,
                    .linear_damping = 0.25F,
                    .angular_damping = 0.5F,
                    .restitution = 0.05F,
                    .friction = 0.7F,
                    .motion_type = vcr::RigidBodyMotionType::dynamic,
                }),
            "attach dynamic rigid_body to CrateA");

        const auto crate_b = scene.create_node("CrateB", vcr::SceneNodeKind::cube);
        scene.node(crate_b)->get().transform.translation = {0.0F, 3.1F, 0.0F};
        scene.node(crate_b)->get().transform.scale = {0.5F, 0.5F, 0.5F};
        expect(
            scene.set_rigid_body(
                crate_b,
                vcr::RigidBodyComponent{
                    .mass = 10.0F,
                    .linear_damping = 0.25F,
                    .angular_damping = 0.5F,
                    .restitution = 0.05F,
                    .friction = 0.7F,
                    .motion_type = vcr::RigidBodyMotionType::dynamic,
                }),
            "attach dynamic rigid_body to stacked CrateB");

        const auto bouncy_ball = scene.create_node("BouncyBall", vcr::SceneNodeKind::cube);
        scene.node(bouncy_ball)->get().transform.translation = {4.0F, 5.0F, 0.0F};
        vcr::ColliderComponent sphere_col{};
        sphere_col.shape = vcr::ColliderShape::sphere;
        sphere_col.radius = 0.5F;
        expect(scene.set_collider(bouncy_ball, sphere_col), "attach sphere collider");
        expect(
            scene.set_rigid_body(
                bouncy_ball,
                vcr::RigidBodyComponent{
                    .mass = 2.0F,
                    .linear_damping = 0.0F,
                    .angular_damping = 0.0F,
                    .restitution = 0.9F,
                    .friction = 0.2F,
                    .motion_type = vcr::RigidBodyMotionType::dynamic,
                }),
            "attach high-restitution rigid_body to BouncyBall");

        vcr::CollisionWorld static_world;
        static_world.rebuild_from_scene(scene, {}, {}, {}, true);
        expect(
            static_world.solid_count() == 1U,
            "CollisionWorld skips enabled rigid bodies when skip_enabled_rigid_bodies is true");

        vcr::PhysicsWorld physics;
        physics.rebuild_from_scene(scene);
        expect(physics.body_count() == 3U, "PhysicsWorld builds three rigid bodies");

        // Verify scale and translation TransformRows preservation at t=0:
        const vcr::TransformRows rows_t0 = physics.body_transform_rows(0U);
        expect(
            close_to(rows_t0.x.x, 0.5F, 0.001F) &&
                close_to(rows_t0.y.y, 0.5F, 0.001F) &&
                close_to(rows_t0.z.z, 0.5F, 0.001F) &&
                close_to(rows_t0.y.w, 1.8F, 0.001F),
            "PhysicsWorld reconstructs exact scale and translation in TransformRows");

        float max_rebound_y = 0.5F;
        bool touched_ground = false;
        for (int frame = 0; frame < 240; ++frame) {
            physics.step(1.0F / 60.0F, static_world);
            const auto ball_state = physics.body_state(bouncy_ball);
            if (ball_state) {
                if (ball_state->position.y < 0.85F) {
                    touched_ground = true;
                } else if (touched_ground) {
                    max_rebound_y = std::max(max_rebound_y, ball_state->position.y);
                }
            }
        }
        expect(
            touched_ground && max_rebound_y > 2.8F,
            "high-restitution dynamic sphere rebounds strongly off static ground");

        // Step another 3 seconds so stacked crates settle and sleep:
        for (int frame = 0; frame < 180; ++frame) {
            physics.step(1.0F / 60.0F, static_world);
        }
        const auto state_a = physics.body_state(crate_a);
        const auto state_b = physics.body_state(crate_b);
        expect(
            state_a && state_b &&
                close_to(state_a->position.y, 0.5F, 0.04F) &&
                close_to(state_b->position.y, 1.5F, 0.06F) &&
                std::abs(state_b->position.x) < 0.1F &&
                state_a->sleeping && state_b->sleeping,
            "two stacked dynamic crates settle stably at y=0.5 and y=1.5 and enter sleep");

        const auto dyn_ray = physics.raycast({-5.0F, 1.5F, 0.0F}, {1.0F, 0.0F, 0.0F}, 10.0F);
        expect(
            dyn_ray.has_value() && dyn_ray->node_id == crate_b,
            "PhysicsWorld::raycast hits dynamic stacked crate");
    }

    // PhysicsWorld + two-way CharacterController push & platform support:
    {
        vcr::SceneDocument scene;
        const auto ground = scene.create_node("Ground", vcr::SceneNodeKind::ground);
        scene.node(ground)->get().transform.translation = {0.0F, 0.0F, 0.0F};
        scene.node(ground)->get().transform.scale = {20.0F, 1.0F, 20.0F};

        const auto push_crate = scene.create_node("PushCrate", vcr::SceneNodeKind::cube);
        scene.node(push_crate)->get().transform.translation = {0.0F, 0.5F, -1.1F};
        scene.node(push_crate)->get().transform.scale = {0.5F, 0.5F, 0.5F};
        expect(
            scene.set_rigid_body(
                push_crate,
                vcr::RigidBodyComponent{
                    .mass = 8.0F,
                    .linear_damping = 0.1F,
                    .angular_damping = 0.4F,
                    .restitution = 0.05F,
                    .friction = 0.25F,
                    .motion_type = vcr::RigidBodyMotionType::dynamic,
                    .lock_rotation = true,
                }),
            "attach light pushable rigid_body");

        const auto platform = scene.create_node("Platform", vcr::SceneNodeKind::cube);
        scene.node(platform)->get().transform.translation = {6.0F, 0.5F, 0.0F};
        scene.node(platform)->get().transform.scale = {1.5F, 0.5F, 1.5F};
        expect(
            scene.set_rigid_body(
                platform,
                vcr::RigidBodyComponent{
                    .mass = 100.0F,
                    .motion_type = vcr::RigidBodyMotionType::kinematic,
                }),
            "attach kinematic platform rigid_body");

        vcr::CollisionWorld static_world;
        static_world.rebuild_from_scene(scene, {}, {}, {}, true);
        vcr::PhysicsWorld physics;
        physics.rebuild_from_scene(scene);

        vcr::CharacterController character;
        character.reset({0.0F, 0.0F, 0.0F}, 0.0F);
        const vcr::CharacterInput walk_forward{.forward = true};
        const float dt = 1.0F / 60.0F;
        for (int frame = 0; frame < 60; ++frame) {
            character.update(walk_forward, camera_forward, dt, static_world);
            physics.apply_character_push(
                character, {0.0F, 0.0F, -3.5F}, dt);
            physics.step(dt, static_world);
            physics.resolve_character_post_step(character, static_world, dt);
        }
        const auto pushed_state = physics.body_state(push_crate);
        expect(
            pushed_state && pushed_state->position.z < -1.5F,
            "CharacterController walking into a dynamic crate pushes it forward");

        // Drop character onto kinematic platform at (6, 1.0, 0):
        character.reset({6.0F, 2.0F, 0.0F}, 0.0F);
        for (int frame = 0; frame < 45; ++frame) {
            character.update({}, camera_forward, dt, static_world);
            physics.step(dt, static_world);
            physics.resolve_character_post_step(character, static_world, dt);
        }
        expect(
            character.grounded() && close_to(character.position().y, 1.0F, 0.05F),
            "CharacterController lands and stands grounded on top of a kinematic rigid body");
    }

    // Analog move_axis scaling, just_jumped(), and landing_impact_speed()
    {
        vcr::CharacterController analog_half;
        vcr::CharacterController analog_full;
        analog_half.reset({0.0F, 0.0F, 0.0F}, 0.0F);
        analog_full.reset({0.0F, 0.0F, 0.0F}, 0.0F);
        const float dt = 1.0F / 60.0F;
        analog_half.update(
            vcr::CharacterInput{.move_axis = {0.0F, 0.5F}}, camera_forward, dt);
        analog_full.update(
            vcr::CharacterInput{.move_axis = {0.0F, 1.0F}}, camera_forward, dt);
        expect(
            close_to(
                analog_half.position().z,
                analog_full.position().z * 0.5F,
                1.0e-3F),
            "partial analog move_axis scales horizontal character velocity proportionally");

        vcr::CharacterController jumper;
        jumper.reset({0.0F, 0.0F, 0.0F}, 0.0F);
        jumper.update(vcr::CharacterInput{.jump = true}, camera_forward, dt);
        expect(
            jumper.just_jumped() && !jumper.grounded(),
            "just_jumped() pulses true on takeoff frame");
        jumper.update(vcr::CharacterInput{.jump = true}, camera_forward, dt);
        expect(
            !jumper.just_jumped(),
            "just_jumped() clears on subsequent airborne frame");
        float max_landing_speed = 0.0F;
        for (int frame = 0; frame < 90; ++frame) {
            jumper.update({}, camera_forward, dt);
            max_landing_speed =
                std::max(max_landing_speed, jumper.landing_impact_speed());
            if (jumper.grounded()) {
                break;
            }
        }
        expect(
            jumper.grounded() && max_landing_speed > 4.0F,
            "landing_impact_speed() reports downward impact speed on touchdown");
    }

    if (failures != 0) {
        std::cerr << failures << " gameplay test(s) failed\n";
        return 1;
    }
    std::cout << "All gameplay tests passed\n";
    return 0;
}
