#pragma once

#include "scene_document.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <type_traits>

namespace vcr {

struct FlyCameraInput {
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool boost = false;
    float mouse_delta_x = 0.0F;
    float mouse_delta_y = 0.0F;
};

static_assert(std::is_trivially_copyable_v<FlyCameraInput>);

struct FlyCameraConfig {
    float movement_speed = 6.0F;
    float boost_multiplier = 4.0F;
    float mouse_sensitivity = 0.0025F;
    float maximum_delta_seconds = 0.05F;
};

class FlyCamera final {
public:
    explicit FlyCamera(
        glm::vec3 position = glm::vec3(0.0F, 4.0F, 12.0F),
        float yaw_radians = -1.57079632679F,
        float pitch_radians = -0.20943951023F,
        FlyCameraConfig config = {}) noexcept;

    void update(const FlyCameraInput& input, float delta_seconds) noexcept;

    [[nodiscard]] glm::mat4 view_matrix() const noexcept;
    [[nodiscard]] glm::vec3 position() const noexcept { return position_; }
    [[nodiscard]] glm::vec3 forward() const noexcept { return forward_; }

private:
    void rebuild_basis() noexcept;

    glm::vec3 position_{};
    glm::vec3 forward_{0.0F, 0.0F, -1.0F};
    glm::vec3 right_{1.0F, 0.0F, 0.0F};
    float yaw_radians_ = -1.57079632679F;
    float pitch_radians_ = 0.0F;
    FlyCameraConfig config_{};
};

// An explicit CLI Z requests the legacy benchmark pose; otherwise use the authored camera.
[[nodiscard]] inline FlyCamera scene_startup_camera(
    const SceneDocument &scene, std::optional<double> camera_z = std::nullopt) noexcept {
    if (!camera_z) {
        if (const auto pose = scene.startup_camera_pose())
            return FlyCamera(pose->position, pose->yaw_radians, pose->pitch_radians);
    }
    return FlyCamera({0, 4, static_cast<float>(camera_z.value_or(12.0))});
}

} // namespace vcr
