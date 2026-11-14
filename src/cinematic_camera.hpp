#pragma once

#include "camera_projection.hpp"
#include "scene_components.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {

class FlyCamera;

struct SplineSample {
    glm::vec3 position{0.0F};
    glm::vec3 tangent{0.0F, 0.0F, 1.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    float roll_degrees = 0.0F;
    float speed_scale = 1.0F;
};

struct SplineArcLengthTable {
    static constexpr std::uint32_t sample_count = 65U;

    std::array<float, sample_count> cumulative_lengths{};
    float total_length = 0.0F;
};

[[nodiscard]] SplineSample evaluate_spline(
    const SplineComponent& spline,
    const glm::mat4& node_world,
    float normalized_u) noexcept;

[[nodiscard]] SplineArcLengthTable build_spline_arc_table(
    const SplineComponent& spline,
    const glm::mat4& node_world) noexcept;

[[nodiscard]] float map_distance_to_spline_u(
    const SplineArcLengthTable& table,
    float distance_meters,
    bool closed_loop) noexcept;

[[nodiscard]] float closest_spline_u_to_world_point(
    const SplineComponent& spline,
    const glm::mat4& node_world,
    const glm::vec3& world_pos,
    std::uint32_t coarse_samples = 64U) noexcept;

struct CameraShakeParams {
    glm::vec3 amplitude_position{0.03F, 0.03F, 0.015F};
    glm::vec3 amplitude_rotation_degrees{1.2F, 1.2F, 0.8F};
    float amplitude_fov_degrees = 0.0F;
    float frequency_hz = 12.0F;
    float fade_in_seconds = 0.05F;
    float fade_out_seconds = 0.15F;
    std::uint32_t seed = 1337U;

    friend bool operator==(
        const CameraShakeParams&,
        const CameraShakeParams&) = default;
};

struct CameraShakeSample {
    glm::vec3 local_offset{0.0F};
    float pitch_radians = 0.0F;
    float yaw_radians = 0.0F;
    float roll_radians = 0.0F;
    float fov_delta_degrees = 0.0F;
};

[[nodiscard]] CameraShakeSample evaluate_camera_shake_clip(
    const CameraShakeParams& params,
    float local_time,
    float duration) noexcept;

enum class CameraBlendEasing : std::uint8_t {
    cut = 0U,
    linear = 1U,
    smoothstep = 2U,
    ease_in_out_cubic = 3U,
};

[[nodiscard]] constexpr std::string_view to_token(
    const CameraBlendEasing easing) noexcept {
    switch (easing) {
    case CameraBlendEasing::cut: return "cut";
    case CameraBlendEasing::linear: return "linear";
    case CameraBlendEasing::smoothstep: return "smoothstep";
    case CameraBlendEasing::ease_in_out_cubic: return "ease_in_out_cubic";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<CameraBlendEasing>
parse_camera_blend_easing(const std::string_view token) noexcept {
    if (token == "cut") return CameraBlendEasing::cut;
    if (token == "linear") return CameraBlendEasing::linear;
    if (token == "smoothstep") return CameraBlendEasing::smoothstep;
    if (token == "ease_in_out_cubic") {
        return CameraBlendEasing::ease_in_out_cubic;
    }
    return std::nullopt;
}

[[nodiscard]] float evaluate_blend_easing(
    CameraBlendEasing easing,
    float t) noexcept;

struct CinematicCameraPose {
    glm::vec3 position{0.0F, 1.6F, 4.0F};
    glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
    float vertical_fov_degrees = camera_vertical_fov;
    float exposure_offset_ev = 0.0F;
    CameraFocusState focus{};
};

[[nodiscard]] CinematicCameraPose pose_from_fly_camera(
    const FlyCamera& camera,
    float vertical_fov_degrees = camera_vertical_fov,
    float exposure_offset_ev = 0.0F,
    const CameraFocusState& focus = {}) noexcept;

void pose_to_fly_camera(
    const CinematicCameraPose& pose,
    FlyCamera& camera) noexcept;

void aim_pose_at_target(
    CinematicCameraPose& pose,
    const glm::vec3& target_world,
    float extra_roll_radians = 0.0F) noexcept;

[[nodiscard]] CinematicCameraPose blend_camera_poses(
    const CinematicCameraPose& a,
    const CinematicCameraPose& b,
    float t,
    CameraBlendEasing easing) noexcept;

class CameraShakeSystem final {
public:
    static constexpr std::size_t max_active_shakes = 8U;

    void reset() noexcept;

    void add_trauma(float amount) noexcept;
    void set_trauma_decay(float decay_per_second) noexcept;
    void set_trauma_params(const CameraShakeParams& params) noexcept;

    void trigger_shake(
        const CameraShakeParams& params,
        float duration_seconds) noexcept;

    void update(float delta_seconds) noexcept;

    [[nodiscard]] float trauma() const noexcept {
        return trauma_;
    }
    [[nodiscard]] bool has_active_shake() const noexcept;
    [[nodiscard]] const CameraShakeSample& current_sample() const noexcept {
        return current_;
    }

    void apply_to_pose(CinematicCameraPose& pose) const noexcept;

private:
    struct ActiveShakeSlot {
        CameraShakeParams params{};
        float elapsed_seconds = 0.0F;
        float duration_seconds = 0.0F;
        bool active = false;
    };

    std::array<ActiveShakeSlot, max_active_shakes> slots_{};
    CameraShakeParams trauma_params_{};
    float trauma_ = 0.0F;
    float trauma_decay_per_second_ = 1.6F;
    float trauma_time_ = 0.0F;
    CameraShakeSample current_{};
};

} // namespace vcr
