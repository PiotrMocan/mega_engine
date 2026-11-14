#pragma once

#include "scene_document.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vcr {

enum class SignalAspect : std::uint8_t {
    red,
    green,
    yellow
};

struct SignalPhaseConfig {
    std::uint32_t active_vehicle_group = 0U;
    std::uint32_t active_pedestrian_group = 0U;
    float green_seconds = 10.0F;
    float yellow_seconds = 2.5F;
    float all_red_seconds = 1.0F;
};

class SignalIntersectionSystem final {
public:
    SignalIntersectionSystem() = default;
    ~SignalIntersectionSystem() = default;

    SignalIntersectionSystem(const SignalIntersectionSystem&) = delete;
    SignalIntersectionSystem& operator=(const SignalIntersectionSystem&) = delete;
    SignalIntersectionSystem(SignalIntersectionSystem&&) noexcept = default;
    SignalIntersectionSystem& operator=(SignalIntersectionSystem&&) noexcept = default;

    void configure(std::span<const SignalPhaseConfig> phases);
    void reset() noexcept;
    void update(float delta_seconds) noexcept;

    [[nodiscard]] SignalAspect vehicle_signal(std::uint32_t group_id) const noexcept;
    [[nodiscard]] bool pedestrian_can_cross(std::uint32_t ped_group_id) const noexcept;

    [[nodiscard]] static bool query_pedestrian_signal(
        std::uint32_t group_id,
        void* user_data) noexcept {
        if (user_data == nullptr) {
            return true;
        }
        const auto* self = static_cast<const SignalIntersectionSystem*>(user_data);
        return self->pedestrian_can_cross(group_id);
    }

    [[nodiscard]] std::uint32_t current_phase_index() const noexcept;
    [[nodiscard]] float phase_time_remaining() const noexcept;

private:
    enum class PhaseStage : std::uint8_t {
        green,
        yellow,
        all_red
    };

    std::vector<SignalPhaseConfig> phases_{};
    std::uint32_t current_phase_index_ = 0U;
    PhaseStage stage_ = PhaseStage::green;
    float elapsed_in_stage_ = 0.0F;
};

struct LaneNode {
    std::uint32_t id = 0U;
    glm::vec3 position{0.0F};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
    float width = 3.5F;
};

struct LaneSegment {
    std::uint32_t id = 0U;
    std::uint32_t start_node = 0U;
    std::uint32_t end_node = 0U;
    float speed_limit_mps = 13.8F;
    bool has_stop_line = false;
    float stop_line_offset_from_end = 2.0F;
    std::uint32_t signal_group_id = 0U;
    std::array<std::uint32_t, 4U> next_segments{};
    std::uint32_t next_count = 0U;
};

class LaneGraph final {
public:
    LaneGraph() = default;
    ~LaneGraph() = default;

    LaneGraph(const LaneGraph&) = delete;
    LaneGraph& operator=(const LaneGraph&) = delete;
    LaneGraph(LaneGraph&&) noexcept = default;
    LaneGraph& operator=(LaneGraph&&) noexcept = default;

    void clear() noexcept;
    std::uint32_t add_node(const LaneNode& node);
    std::uint32_t add_segment(const LaneSegment& segment);
    bool build();

    [[nodiscard]] const LaneSegment* segment(std::uint32_t id) const noexcept;
    [[nodiscard]] const LaneNode* node(std::uint32_t id) const noexcept;
    [[nodiscard]] glm::vec3 sample_point(
        std::uint32_t segment_id,
        float distance_along) const noexcept;
    [[nodiscard]] glm::vec3 segment_direction(std::uint32_t segment_id) const noexcept;
    [[nodiscard]] float segment_length(std::uint32_t segment_id) const noexcept;
    [[nodiscard]] std::size_t segment_count() const noexcept;

private:
    std::vector<LaneNode> nodes_{};
    std::vector<LaneSegment> segments_{};
    std::vector<float> segment_lengths_{};
    std::vector<glm::vec3> segment_directions_{};
    bool built_ = false;
};

struct TrafficConfig {
    std::uint32_t max_vehicles = 24U;
    float idm_time_headway_s = 1.6F;
    float idm_min_gap_m = 4.5F;
    float idm_max_accel_mps2 = 2.8F;
    float idm_comfort_decel_mps2 = 4.2F;
    float emergency_decel_mps2 = 9.0F;
    float min_spawn_radius_m = 25.0F;
    float despawn_radius_m = 160.0F;
    float stuck_timeout_s = 12.0F;
    std::uint64_t seed = 4242ULL;
};

struct TrafficObstacle {
    glm::vec3 position{0.0F};
    float radius = 0.6F;
    SceneNodeId ignore_node{};
};

struct TrafficVehicleSnapshot {
    std::uint32_t vehicle_id = 0U;
    SceneNodeId scene_node{};
    std::uint32_t segment_id = 0U;
    float distance_along = 0.0F;
    glm::vec3 position{0.0F};
    float yaw_radians = 0.0F;
    float speed_mps = 0.0F;
    float throttle = 0.0F;
    float brake = 0.0F;
    float steer = 0.0F;
    bool brake_lights = false;
    bool active = false;
    bool released_to_player = false;
};

class TrafficSystem final {
public:
    TrafficSystem() = default;
    ~TrafficSystem() = default;

    TrafficSystem(const TrafficSystem&) = delete;
    TrafficSystem& operator=(const TrafficSystem&) = delete;
    TrafficSystem(TrafficSystem&&) noexcept = default;
    TrafficSystem& operator=(TrafficSystem&&) noexcept = default;

    void initialize(const TrafficConfig& config, const LaneGraph& graph);
    void reset() noexcept;

    std::optional<std::uint32_t> spawn_vehicle(
        std::uint32_t segment_id,
        float distance_along,
        SceneNodeId bound_node = {});

    bool release_vehicle_to_player(std::uint32_t vehicle_id) noexcept;

    void recycle_far_vehicles(
        const glm::vec3& camera_position,
        const LaneGraph& graph) noexcept;

    void update(
        float delta_seconds,
        const LaneGraph& graph,
        const SignalIntersectionSystem* signals,
        std::span<const TrafficObstacle> obstacles = {},
        const glm::vec3& camera_position = glm::vec3(0.0F)) noexcept;

    [[nodiscard]] std::span<const TrafficVehicleSnapshot> vehicles() const noexcept;
    [[nodiscard]] std::size_t active_vehicle_count() const noexcept;

private:
    struct InternalVehicle {
        std::uint32_t vehicle_id = 0U;
        SceneNodeId scene_node{};
        std::uint32_t segment_id = 0U;
        float distance_along = 0.0F;
        glm::vec3 position{0.0F};
        float yaw_radians = 0.0F;
        float speed_mps = 0.0F;
        float throttle = 0.0F;
        float brake = 0.0F;
        float steer = 0.0F;
        bool brake_lights = false;
        bool active = false;
        bool released_to_player = false;
        float stuck_timer_s = 0.0F;
    };

    struct RngState {
        std::uint64_t state = 4242ULL;

        constexpr explicit RngState(std::uint64_t seed = 4242ULL) noexcept
            : state(seed == 0ULL ? 4242ULL : seed) {}

        constexpr std::uint64_t next_u64() noexcept {
            std::uint64_t x = state;
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            state = x;
            return x * 0x2545F4914F6CDD1DULL;
        }

        constexpr float next_unorm() noexcept {
            return static_cast<float>(next_u64() >> 40) * (1.0F / 16777216.0F);
        }
    };

    TrafficConfig config_{};
    RngState prng_{};
    const LaneGraph* graph_ = nullptr;
    std::vector<InternalVehicle> vehicles_{};
    std::vector<TrafficVehicleSnapshot> snapshots_{};
    std::size_t active_count_ = 0U;
    std::uint32_t next_vehicle_id_ = 1U;
};

} // namespace vcr
