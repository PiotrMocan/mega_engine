#include "gameplay_session.hpp"

#include "fly_camera.hpp"
#include "renderer.hpp"

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vcr {
namespace {

constexpr glm::vec3 player_half_extents{0.45F, 0.9F, 0.3F};
constexpr float radians_to_degrees = 57.2957795131F;

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
    std::string& error) {
    error.clear();
    if (active_) {
        error = "Gameplay session is already active.";
        return false;
    }
    player_node_ = {};

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

    const float ground_height = initial_view.has_ground
        ? initial_view.ground.model_row_1.w
        : 0.0F;
    glm::vec3 spawn = startup_camera.position() + movement_forward * 5.0F;
    spawn.y = ground_height;
    const float facing_yaw = std::atan2(movement_forward.x, movement_forward.z);
    character_ = CharacterController(CharacterConfig{
        .ground_height = ground_height,
    });
    character_.reset(spawn, facing_yaw);

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
    SceneNode& player = player_node->get();
    player.transform.translation =
        spawn + glm::vec3(0.0F, player_half_extents.y, 0.0F);
    player.transform.rotation_degrees.y = facing_yaw * radians_to_degrees;
    player.transform.scale = player_half_extents;
    player.material = PbrMaterial{
        .base_color = glm::vec4(0.12F, 0.42F, 0.95F, 1.0F),
        .metallic = 0.0F,
        .roughness = 0.55F,
        .ambient_occlusion = 1.0F,
    };
    scene.mark_content_changed();

    const glm::vec3 startup_forward = startup_camera.forward();
    camera_.reset(std::atan2(startup_forward.z, startup_forward.x));
    output_camera = FlyCamera(
        camera_.position(character_.position()),
        camera_.yaw(),
        camera_.pitch());
    active_ = true;
    return true;
}

bool GameplaySession::update(
    const GameplayFrameInput& input,
    const float delta_seconds,
    Renderer& renderer,
    FlyCamera& output_camera) noexcept {
    if (!active_) {
        return false;
    }
    camera_.rotate(input.mouse_delta_x, input.mouse_delta_y);
    character_.update(
        input.character,
        camera_.planar_forward(),
        delta_seconds);
    if (!renderer.set_runtime_transform(
            player_node_,
            player_transform(character_.position(), character_.facing_yaw()))) {
        return false;
    }
    output_camera = FlyCamera(
        camera_.position(character_.position()),
        camera_.yaw(),
        camera_.pitch());
    return true;
}

void GameplaySession::stop() noexcept {
    player_node_ = {};
    active_ = false;
}

} // namespace vcr
