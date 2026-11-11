#include "collision.hpp"
#include "gameplay.hpp"
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

    if (failures != 0) {
        std::cerr << failures << " gameplay test(s) failed\n";
        return 1;
    }
    std::cout << "All gameplay tests passed\n";
    return 0;
}
