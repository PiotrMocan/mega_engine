#pragma once

#include "collision.hpp"
#include "scene_document.hpp"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vcr {

struct NavWaypoint {
    std::uint32_t id = 0U;
    glm::vec3 position{0.0F};
    float radius = 0.8F;
    std::uint32_t flags = 0U;
};

struct NavEdge {
    std::uint32_t from_node = 0U;
    std::uint32_t to_node = 0U;
    bool bidirectional = true;
    bool is_crosswalk = false;
    std::uint32_t signal_group_id = 0U;
    float speed_limit_mps = 1.45F;
};

class NavigationGraph final {
public:
    NavigationGraph() = default;
    ~NavigationGraph() = default;

    NavigationGraph(const NavigationGraph&) = delete;
    NavigationGraph& operator=(const NavigationGraph&) = delete;
    NavigationGraph(NavigationGraph&&) noexcept = default;
    NavigationGraph& operator=(NavigationGraph&&) noexcept = default;

    void clear() noexcept;
    std::uint32_t add_node(const NavWaypoint& node);
    void add_edge(const NavEdge& edge);
    bool build();

    [[nodiscard]] std::optional<std::uint32_t> find_nearest_node(
        const glm::vec3& position) const noexcept;
    [[nodiscard]] const NavWaypoint* node(std::uint32_t id) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> outgoing_edges(
        std::uint32_t node_id) const noexcept;
    [[nodiscard]] const NavEdge* edge(std::uint32_t edge_index) const noexcept;
    [[nodiscard]] std::size_t node_count() const noexcept;

private:
    std::vector<NavWaypoint> nodes_{};
    std::vector<NavEdge> edges_{};
    std::vector<float> edge_lengths_{};
    std::vector<std::uint32_t> outgoing_edge_indices_{};
    std::vector<std::uint32_t> node_edge_offsets_{};
    std::vector<std::uint32_t> node_edge_counts_{};
    bool built_ = false;
};

enum class CrowdAgentState : std::uint8_t {
    walk,
    wait_crosswalk,
    look_around,
    evade_threat
};

struct CrowdThreat {
    glm::vec3 position{0.0F};
    glm::vec3 velocity{0.0F};
    float radius = 2.0F;
};

using SignalPassableFn = bool (*)(std::uint32_t signal_group_id, void* user_data) noexcept;

struct CrowdConfig {
    std::uint32_t max_agents = 64U;
    std::uint32_t max_updates_per_tick = 64U;
    float desired_walk_speed_mps = 1.35F;
    float evade_speed_mps = 3.2F;
    float agent_radius = 0.35F;
    float separation_weight = 1.8F;
    float threat_reaction_distance = 7.0F;
    float threat_min_speed_mps = 2.5F;
    std::uint64_t seed = 1337ULL;
};

struct CrowdAgentSnapshot {
    std::uint32_t agent_id = 0U;
    SceneNodeId scene_node{};
    CrowdAgentState state = CrowdAgentState::walk;
    glm::vec3 position{0.0F};
    float yaw_radians = 0.0F;
    glm::vec3 velocity{0.0F};
    float speed_mps = 0.0F;
    std::uint32_t current_node = 0U;
    std::uint32_t target_node = 0U;
    bool active = false;
};

class CrowdSystem final {
public:
    CrowdSystem() = default;
    ~CrowdSystem() = default;

    CrowdSystem(const CrowdSystem&) = delete;
    CrowdSystem& operator=(const CrowdSystem&) = delete;
    CrowdSystem(CrowdSystem&&) noexcept = default;
    CrowdSystem& operator=(CrowdSystem&&) noexcept = default;

    void initialize(const CrowdConfig& config, const NavigationGraph& graph);
    void reset() noexcept;

    std::optional<std::uint32_t> spawn_agent(
        std::uint32_t start_node,
        SceneNodeId bound_node = {});
    bool despawn_agent(std::uint32_t agent_id) noexcept;
    void set_agent_scene_node(std::uint32_t agent_id, SceneNodeId bound_node) noexcept;

    void update(
        float delta_seconds,
        const NavigationGraph& graph,
        std::span<const CrowdThreat> threats,
        SignalPassableFn signal_query = nullptr,
        void* signal_user_data = nullptr,
        const CollisionWorld* ground_raycast_world = nullptr) noexcept;

    [[nodiscard]] std::span<const CrowdAgentSnapshot> agents() const noexcept;
    [[nodiscard]] std::size_t active_agent_count() const noexcept;

private:
    struct InternalAgent {
        std::uint32_t agent_id = 0U;
        SceneNodeId scene_node{};
        CrowdAgentState state = CrowdAgentState::walk;
        glm::vec3 position{0.0F};
        float yaw_radians = 0.0F;
        glm::vec3 velocity{0.0F};
        float speed_mps = 0.0F;
        std::uint32_t current_node = 0U;
        std::uint32_t target_node = 0U;
        std::uint32_t current_edge_index = UINT32_MAX;
        glm::vec3 evade_direction{0.0F, 0.0F, 1.0F};
        float state_timer_s = 0.0F;
        float accumulated_dt_s = 0.0F;
        bool active = false;
    };

    struct RngState {
        std::uint64_t state = 1337ULL;

        constexpr explicit RngState(std::uint64_t seed = 1337ULL) noexcept
            : state(seed == 0ULL ? 1337ULL : seed) {}

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

    CrowdConfig config_{};
    RngState rng_{};
    const NavigationGraph* graph_ = nullptr;
    std::vector<InternalAgent> agents_{};
    std::vector<CrowdAgentSnapshot> snapshots_{};
    std::size_t active_count_ = 0U;
    std::uint32_t next_agent_id_ = 1U;
    std::uint32_t round_robin_cursor_ = 0U;
};

} // namespace vcr
