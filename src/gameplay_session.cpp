#include "gameplay_session.hpp"

#include "fly_camera.hpp"
#include "renderer.hpp"

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

namespace vcr {
namespace {

constexpr glm::vec3 player_half_extents{0.45F, 0.9F, 0.3F};
constexpr float radians_to_degrees = 57.2957795131F;

[[nodiscard]] ModelBoundsProvider make_renderer_bounds_provider(
    const Renderer* const renderer) noexcept {
    if (renderer == nullptr) {
        return {};
    }
#if VCR_EDITOR_ENABLED
    return ModelBoundsProvider{
        .context = renderer,
        .query = +[](const void* const ctx,
                     const SceneNodeId id) noexcept -> std::optional<Bounds> {
            return static_cast<const Renderer*>(ctx)->editor_node_bounds(id);
        },
    };
#else
    return {};
#endif
}

[[nodiscard]] ModelCollisionProvider make_renderer_collision_provider(
    const Renderer* const renderer) noexcept {
    if (renderer == nullptr) {
        return {};
    }
#if VCR_EDITOR_ENABLED
    return ModelCollisionProvider{
        .context = renderer,
        .query = +[](const void* const ctx,
                     const std::string_view path) noexcept
                     -> const CookedModelCollision* {
            return static_cast<const Renderer*>(ctx)
                ->editor_model_collision(path);
        },
    };
#else
    return {};
#endif
}

[[nodiscard]] glm::vec3 compute_desired_planar_velocity(
    const CharacterInput& input,
    glm::vec3 planar_forward,
    const CharacterConfig& cfg) noexcept {
    planar_forward.y = 0.0F;
    const float f_sq = glm::dot(planar_forward, planar_forward);
    if (!std::isfinite(f_sq) || f_sq <= 1.0e-8F) {
        planar_forward = {0.0F, 0.0F, -1.0F};
    } else {
        planar_forward *= 1.0F / std::sqrt(f_sq);
    }
    const glm::vec3 planar_right{-planar_forward.z, 0.0F, planar_forward.x};
    const float fwd =
        static_cast<float>(input.forward) - static_cast<float>(input.backward);
    const float rgt =
        static_cast<float>(input.right) - static_cast<float>(input.left);
    const glm::vec3 raw = planar_forward * fwd + planar_right * rgt;
    const float raw_sq = glm::dot(raw, raw);
    if (raw_sq <= 1.0e-8F) {
        return glm::vec3(0.0F);
    }
    const float speed = input.run ? cfg.run_speed : cfg.walk_speed;
    return raw * (speed / std::sqrt(raw_sq));
}

[[nodiscard]] TransformRows player_transform(
    const glm::vec3 feet_position,
    const float facing_yaw) noexcept {
    glm::mat4 model = glm::translate(
        glm::mat4(1.0F),
        feet_position + glm::vec3(0.0F, player_half_extents.y, 0.0F));
    model = glm::rotate(model, facing_yaw, glm::vec3(0.0F, 1.0F, 0.0F));
    model = glm::scale(model, player_half_extents);
    return {
        glm::row(model, 0),
        glm::row(model, 1),
        glm::row(model, 2),
    };
}

} // namespace

bool GameplaySession::start(
    SceneDocument& scene,
    const SceneRenderView& initial_view,
    const FlyCamera& startup_camera,
    FlyCamera& output_camera,
    std::string& error,
    const Renderer* const renderer) {
    error.clear();
    if (active_) {
        error = "Gameplay session is already active.";
        return false;
    }
    player_node_ = {};
    collision_world_.clear();
    physics_world_.clear();
    trigger_tracker_.reset();

    glm::vec3 movement_forward = startup_camera.forward();
    movement_forward.y = 0.0F;
    const float direction_length_squared =
        glm::dot(movement_forward, movement_forward);
    if (!std::isfinite(direction_length_squared) ||
        direction_length_squared <= 1.0e-8F) {
        movement_forward = {0.0F, 0.0F, -1.0F};
    } else {
        movement_forward *= 1.0F / std::sqrt(direction_length_squared);
    }

    player_node_ = scene.create_node("Runtime Player", SceneNodeKind::cube);
    if (!player_node_) {
        error = "Scene has no capacity for the runtime player.";
        return false;
    }
    const auto player_node = scene.node(player_node_);
    if (!player_node) {
        static_cast<void>(scene.destroy_subtree(player_node_));
        player_node_ = {};
        error = "Runtime player handle was not retained by the scene.";
        return false;
    }

    const ModelBoundsProvider bounds_provider =
        make_renderer_bounds_provider(renderer);
    const ModelCollisionProvider collision_provider =
        make_renderer_collision_provider(renderer);
    collision_world_.rebuild_from_scene(
        scene,
        player_node_,
        bounds_provider,
        collision_provider,
        true);
    physics_world_.rebuild_from_scene(
        scene,
        player_node_,
        bounds_provider,
        collision_provider);

    const float fallback_ground = initial_view.has_ground
        ? initial_view.ground.model_row_1.w
        : 0.0F;
    glm::vec3 spawn = startup_camera.position() + movement_forward * 5.0F;
    auto sampled_ground = collision_world_.sample_supporting_height(
        spawn.x,
        spawn.z,
        startup_camera.position().y,
        200.0F);
    if (!sampled_ground) {
        sampled_ground = collision_world_.sample_supporting_height(
            spawn.x,
            spawn.z,
            startup_camera.position().y + 24.0F,
            224.0F);
    }
    spawn.y = sampled_ground.value_or(fallback_ground);

    character_ = CharacterController(CharacterConfig{
        .ground_height = fallback_ground,
    });
    const float walkable_cos = std::cos(
        glm::radians(character_.config().max_slope_degrees));
    spawn = collision_world_
                .resolve_capsule(
                    spawn,
                    character_.config().capsule_radius,
                    character_.config().capsule_height,
                    walkable_cos)
                .resolved_feet;

    const float facing_yaw = std::atan2(movement_forward.x, movement_forward.z);
    character_.reset(spawn, facing_yaw);
    physics_world_.resolve_character_post_step(
        character_,
        collision_world_,
        0.0F);

    SceneNode& player = player_node->get();
    player.transform.translation =
        character_.position() + glm::vec3(0.0F, player_half_extents.y, 0.0F);
    player.transform.rotation_degrees.y = facing_yaw * radians_to_degrees;
    player.transform.scale = player_half_extents;
    player.material = PbrMaterial{
        .base_color = glm::vec4(0.12F, 0.42F, 0.95F, 1.0F),
        .metallic = 0.0F,
        .roughness = 0.55F,
        .ambient_occlusion = 1.0F,
    };
    scene.mark_content_changed();
    cached_content_revision_ = scene.content_revision();
    cached_structure_revision_ = scene.structure_revision();

    trigger_tracker_.update(
        collision_world_,
        character_.position(),
        character_.config().capsule_radius,
        character_.config().capsule_height);

    const glm::vec3 startup_forward = startup_camera.forward();
    camera_.reset(std::atan2(startup_forward.z, startup_forward.x));
    output_camera = FlyCamera(
        camera_.position(
            character_.position(),
            collision_world_,
            physics_world_),
        camera_.yaw(),
        camera_.pitch());
    active_ = true;
    return true;
}

bool GameplaySession::update(
    const GameplayFrameInput& input,
    const float delta_seconds,
    const SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& output_camera) noexcept {
    if (!active_) {
        return false;
    }
    if (scene.content_revision() != cached_content_revision_ ||
        scene.structure_revision() != cached_structure_revision_) {
        const ModelBoundsProvider bounds_provider =
            make_renderer_bounds_provider(&renderer);
        const ModelCollisionProvider collision_provider =
            make_renderer_collision_provider(&renderer);
        collision_world_.rebuild_from_scene(
            scene,
            player_node_,
            bounds_provider,
            collision_provider,
            true);
        physics_world_.rebuild_from_scene(
            scene,
            player_node_,
            bounds_provider,
            collision_provider);
        cached_content_revision_ = scene.content_revision();
        cached_structure_revision_ = scene.structure_revision();
    }

    camera_.rotate(input.mouse_delta_x, input.mouse_delta_y);
    const glm::vec3 cam_forward = camera_.planar_forward();
    character_.update(
        input.character,
        cam_forward,
        delta_seconds,
        collision_world_);

    if (!physics_world_.empty()) {
        const glm::vec3 desired_vel = compute_desired_planar_velocity(
            input.character,
            cam_forward,
            character_.config());
        physics_world_.apply_character_push(
            character_,
            desired_vel,
            delta_seconds);
        physics_world_.step(delta_seconds, collision_world_);
        physics_world_.resolve_character_post_step(
            character_,
            collision_world_,
            delta_seconds);

        const std::size_t body_count = physics_world_.body_count();
        for (std::size_t i = 0U; i < body_count; ++i) {
            const SceneNodeId id = physics_world_.body_node_id(i);
            if (id) {
                static_cast<void>(renderer.set_runtime_transform(
                    id,
                    physics_world_.body_transform_rows(i)));
            }
        }
    }

    trigger_tracker_.update(
        collision_world_,
        character_.position(),
        character_.config().capsule_radius,
        character_.config().capsule_height);

    if (!renderer.set_runtime_transform(
            player_node_,
            player_transform(character_.position(), character_.facing_yaw()))) {
        return false;
    }
    output_camera = FlyCamera(
        camera_.position(
            character_.position(),
            collision_world_,
            physics_world_),
        camera_.yaw(),
        camera_.pitch());
    return true;
}

void GameplaySession::stop() noexcept {
    collision_world_.clear();
    physics_world_.clear();
    trigger_tracker_.reset();
    player_node_ = {};
    cached_content_revision_ = 0U;
    cached_structure_revision_ = 0U;
    active_ = false;
}

} // namespace vcr
