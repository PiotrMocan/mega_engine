#include "navigation_crowd.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vcr {

namespace {

constexpr float pi_f = 3.14159265358979323846F;
constexpr float two_pi_f = 6.28318530717958647692F;

[[nodiscard]] float wrap_angle_radians(float angle) noexcept {
    while (angle < -pi_f) {
        angle += two_pi_f;
    }
    while (angle > pi_f) {
        angle -= two_pi_f;
    }
    return angle;
}

} // namespace

void NavigationGraph::clear() noexcept {
    nodes_.clear();
    edges_.clear();
    edge_lengths_.clear();
    outgoing_edge_indices_.clear();
    node_edge_offsets_.clear();
    node_edge_counts_.clear();
    built_ = false;
}

std::uint32_t NavigationGraph::add_node(const NavWaypoint& node) {
    NavWaypoint copy = node;
    copy.id = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(copy);
    built_ = false;
    return copy.id;
}

void NavigationGraph::add_edge(const NavEdge& edge) {
    edges_.push_back(edge);
    built_ = false;
}

bool NavigationGraph::build() {
    built_ = false;
    const std::size_t count = nodes_.size();
    for (const auto& e : edges_) {
        if (e.from_node >= count || e.to_node >= count) {
            return false;
        }
    }

    edge_lengths_.resize(edges_.size());
    for (std::size_t i = 0; i < edges_.size(); ++i) {
        const auto& e = edges_[i];
        edge_lengths_[i] = glm::distance(nodes_[e.from_node].position, nodes_[e.to_node].position);
    }

    node_edge_counts_.assign(count, 0U);
    for (const auto& e : edges_) {
        node_edge_counts_[e.from_node]++;
        if (e.bidirectional && e.to_node != e.from_node) {
            node_edge_counts_[e.to_node]++;
        }
    }

    node_edge_offsets_.assign(count, 0U);
    std::uint32_t total = 0U;
    for (std::size_t i = 0; i < count; ++i) {
        node_edge_offsets_[i] = total;
        total += node_edge_counts_[i];
    }

    outgoing_edge_indices_.assign(total, 0U);
    std::vector<std::uint32_t> cursors = node_edge_offsets_;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(edges_.size()); ++i) {
        const auto& e = edges_[i];
        outgoing_edge_indices_[cursors[e.from_node]++] = i;
        if (e.bidirectional && e.to_node != e.from_node) {
            outgoing_edge_indices_[cursors[e.to_node]++] = i;
        }
    }

    built_ = true;
    return true;
}

std::optional<std::uint32_t> NavigationGraph::find_nearest_node(
    const glm::vec3& position) const noexcept {
    if (nodes_.empty()) {
        return std::nullopt;
    }
    float best_dist_sq = std::numeric_limits<float>::max();
    std::uint32_t best_id = 0U;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const glm::vec3 diff = nodes_[i].position - position;
        const float dist_sq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_id = static_cast<std::uint32_t>(i);
        }
    }
    return best_id;
}

const NavWaypoint* NavigationGraph::node(std::uint32_t id) const noexcept {
    if (id < nodes_.size()) {
        return &nodes_[id];
    }
    return nullptr;
}

std::span<const std::uint32_t> NavigationGraph::outgoing_edges(
    std::uint32_t node_id) const noexcept {
    if (!built_ || node_id >= nodes_.size()) {
        return {};
    }
    return std::span<const std::uint32_t>(
        outgoing_edge_indices_.data() + node_edge_offsets_[node_id],
        node_edge_counts_[node_id]);
}

const NavEdge* NavigationGraph::edge(std::uint32_t edge_index) const noexcept {
    if (edge_index < edges_.size()) {
        return &edges_[edge_index];
    }
    return nullptr;
}

std::size_t NavigationGraph::node_count() const noexcept {
    return nodes_.size();
}

void CrowdSystem::initialize(const CrowdConfig& config, const NavigationGraph& graph) {
    config_ = config;
    rng_ = RngState(config.seed);
    graph_ = &graph;
    agents_.assign(config.max_agents, InternalAgent{});
    snapshots_.assign(config.max_agents, CrowdAgentSnapshot{});
    active_count_ = 0U;
    next_agent_id_ = 1U;
    round_robin_cursor_ = 0U;
}

void CrowdSystem::reset() noexcept {
    for (auto& a : agents_) {
        a = InternalAgent{};
    }
    for (auto& s : snapshots_) {
        s = CrowdAgentSnapshot{};
    }
    active_count_ = 0U;
    next_agent_id_ = 1U;
    round_robin_cursor_ = 0U;
}

std::optional<std::uint32_t> CrowdSystem::spawn_agent(
    std::uint32_t start_node,
    SceneNodeId bound_node) {
    if (agents_.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        if (!agents_[i].active) {
            InternalAgent& a = agents_[i];
            a.active = true;
            a.agent_id = next_agent_id_++;
            a.scene_node = bound_node;
            a.current_node = start_node;
            a.target_node = start_node;
            a.current_edge_index = UINT32_MAX;
            a.state = CrowdAgentState::walk;
            a.state_timer_s = 0.0F;
            a.accumulated_dt_s = 0.0F;
            a.position = glm::vec3(0.0F);
            a.velocity = glm::vec3(0.0F);
            a.speed_mps = 0.0F;
            a.yaw_radians = 0.0F;
            a.evade_direction = glm::vec3(0.0F, 0.0F, 1.0F);

            if (graph_ != nullptr) {
                const NavWaypoint* wp = graph_->node(start_node);
                if (wp != nullptr) {
                    a.position = wp->position;
                }
                const auto edges = graph_->outgoing_edges(start_node);
                if (!edges.empty()) {
                    const std::uint32_t chosen_idx = edges[rng_.next_u64() % edges.size()];
                    const NavEdge* e = graph_->edge(chosen_idx);
                    if (e != nullptr) {
                        a.current_edge_index = chosen_idx;
                        a.target_node = (e->from_node == start_node) ? e->to_node : e->from_node;
                        const NavWaypoint* target_wp = graph_->node(a.target_node);
                        if (target_wp != nullptr) {
                            const glm::vec3 diff = target_wp->position - a.position;
                            if (std::hypot(diff.x, diff.z) > 0.001F) {
                                a.yaw_radians = std::atan2(diff.x, diff.z);
                            }
                        }
                    }
                }
            }

            CrowdAgentSnapshot& snap = snapshots_[i];
            snap.agent_id = a.agent_id;
            snap.scene_node = a.scene_node;
            snap.state = a.state;
            snap.position = a.position;
            snap.yaw_radians = a.yaw_radians;
            snap.velocity = a.velocity;
            snap.speed_mps = a.speed_mps;
            snap.current_node = a.current_node;
            snap.target_node = a.target_node;
            snap.active = true;

            ++active_count_;
            return a.agent_id;
        }
    }
    return std::nullopt;
}

bool CrowdSystem::despawn_agent(std::uint32_t agent_id) noexcept {
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        if (agents_[i].active && agents_[i].agent_id == agent_id) {
            agents_[i].active = false;
            snapshots_[i].active = false;
            if (active_count_ > 0U) {
                --active_count_;
            }
            return true;
        }
    }
    return false;
}

void CrowdSystem::set_agent_scene_node(
    std::uint32_t agent_id,
    SceneNodeId bound_node) noexcept {
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        if (agents_[i].active && agents_[i].agent_id == agent_id) {
            agents_[i].scene_node = bound_node;
            snapshots_[i].scene_node = bound_node;
            break;
        }
    }
}

void CrowdSystem::update(
    float delta_seconds,
    const NavigationGraph& graph,
    std::span<const CrowdThreat> threats,
    SignalPassableFn signal_query,
    void* signal_user_data,
    const CollisionWorld* ground_raycast_world) noexcept {
    if (active_count_ == 0U || agents_.empty()) {
        return;
    }

    // Accumulate elapsed delta_seconds for all active agents
    for (auto& a : agents_) {
        if (a.active) {
            a.accumulated_dt_s += delta_seconds;
        }
    }

    const std::uint32_t budget = std::min(
        config_.max_updates_per_tick,
        static_cast<std::uint32_t>(agents_.size()));

    const auto step_single_agent = [&](InternalAgent& agent, float dt) noexcept {
        if (dt <= 0.0F) {
            return;
        }

        // Clamp dt to avoid teleporting if an agent was skipped for several frames
        const float clamped_dt = std::min(dt, 0.25F);

        // 1. Check threats if not already evading
        if (agent.state != CrowdAgentState::evade_threat) {
            for (const auto& threat : threats) {
                const float threat_speed = glm::length(threat.velocity);
                if (threat_speed >= config_.threat_min_speed_mps) {
                    const glm::vec3 threat_to_agent = agent.position - threat.position;
                    const glm::vec3 threat_dir = threat.velocity / threat_speed;
                    const float proj = glm::dot(threat_to_agent, threat_dir);
                    if (proj > -1.0F && proj < config_.threat_reaction_distance) {
                        const glm::vec3 lateral = threat_to_agent - proj * threat_dir;
                        const float lat_dist = std::hypot(lateral.x, lateral.z);
                        if (lat_dist < (threat.radius + config_.agent_radius + 1.5F)) {
                            agent.state = CrowdAgentState::evade_threat;
                            agent.state_timer_s = 1.6F;
                            if (lat_dist > 0.01F) {
                                agent.evade_direction = glm::normalize(
                                    glm::vec3(lateral.x, 0.0F, lateral.z));
                            } else {
                                agent.evade_direction = glm::normalize(
                                    glm::vec3(-threat_dir.z, 0.0F, threat_dir.x));
                            }
                            break;
                        }
                    }
                }
            }
        }

        // 2. State machine handling
        if (agent.state == CrowdAgentState::evade_threat) {
            agent.state_timer_s -= clamped_dt;
            agent.speed_mps = config_.evade_speed_mps;
            agent.velocity = agent.evade_direction * config_.evade_speed_mps;
            agent.position += agent.velocity * clamped_dt;
            agent.yaw_radians = std::atan2(agent.evade_direction.x, agent.evade_direction.z);
            if (agent.state_timer_s <= 0.0F) {
                agent.state = CrowdAgentState::walk;
                const auto nearest = graph.find_nearest_node(agent.position);
                if (nearest.has_value()) {
                    agent.target_node = *nearest;
                }
            }
        } else if (agent.state == CrowdAgentState::look_around) {
            agent.state_timer_s -= clamped_dt;
            agent.speed_mps = 0.0F;
            agent.velocity = glm::vec3(0.0F);
            agent.yaw_radians = wrap_angle_radians(
                agent.yaw_radians + std::sin(agent.state_timer_s * 2.5F) * 0.4F * clamped_dt);
            if (agent.state_timer_s <= 0.0F) {
                agent.state = CrowdAgentState::walk;
                const auto edges = graph.outgoing_edges(agent.current_node);
                if (!edges.empty()) {
                    const std::uint32_t chosen_idx = edges[rng_.next_u64() % edges.size()];
                    const NavEdge* e = graph.edge(chosen_idx);
                    if (e != nullptr) {
                        agent.current_edge_index = chosen_idx;
                        agent.target_node = (e->from_node == agent.current_node)
                            ? e->to_node
                            : e->from_node;
                    }
                }
            }
        } else if (agent.state == CrowdAgentState::wait_crosswalk) {
            agent.speed_mps = 0.0F;
            agent.velocity = glm::vec3(0.0F);
            const NavEdge* edge = graph.edge(agent.current_edge_index);
            if (edge != nullptr) {
                const bool passable = (signal_query == nullptr) ||
                    signal_query(edge->signal_group_id, signal_user_data);
                if (passable) {
                    agent.state = CrowdAgentState::walk;
                }
            } else {
                agent.state = CrowdAgentState::walk;
            }
        } else { // CrowdAgentState::walk
            // If target node is invalid or agent just spawned, sync target node
            const NavWaypoint* target_wp = graph.node(agent.target_node);
            if (target_wp == nullptr) {
                const auto edges = graph.outgoing_edges(agent.current_node);
                if (!edges.empty()) {
                    const std::uint32_t chosen_idx = edges[rng_.next_u64() % edges.size()];
                    const NavEdge* e = graph.edge(chosen_idx);
                    if (e != nullptr) {
                        agent.current_edge_index = chosen_idx;
                        agent.target_node = (e->from_node == agent.current_node)
                            ? e->to_node
                            : e->from_node;
                        target_wp = graph.node(agent.target_node);
                    }
                }
            }

            if (target_wp == nullptr) {
                agent.state = CrowdAgentState::look_around;
                agent.state_timer_s = 1.5F;
                agent.speed_mps = 0.0F;
                agent.velocity = glm::vec3(0.0F);
            } else {
                const glm::vec3 to_target = target_wp->position - agent.position;
                const float dist_xz = std::hypot(to_target.x, to_target.z);
                const float arrive_threshold = std::max(0.35F, target_wp->radius * 0.5F);

                if (dist_xz <= arrive_threshold) {
                    agent.current_node = agent.target_node;

                    // Periodic deterministic look_around pause at waypoints
                    if (rng_.next_unorm() < 0.08F) {
                        agent.state = CrowdAgentState::look_around;
                        agent.state_timer_s = 1.5F + rng_.next_unorm() * 2.0F;
                        agent.speed_mps = 0.0F;
                        agent.velocity = glm::vec3(0.0F);
                    } else {
                        const auto edges = graph.outgoing_edges(agent.current_node);
                        if (!edges.empty()) {
                            const std::uint32_t chosen_idx =
                                edges[rng_.next_u64() % edges.size()];
                            const NavEdge* next_edge = graph.edge(chosen_idx);
                            if (next_edge != nullptr) {
                                const std::uint32_t next_target =
                                    (next_edge->from_node == agent.current_node)
                                    ? next_edge->to_node
                                    : next_edge->from_node;

                                if (next_edge->is_crosswalk &&
                                    next_edge->signal_group_id != 0U) {
                                    const bool passable = (signal_query == nullptr) ||
                                        signal_query(next_edge->signal_group_id, signal_user_data);
                                    if (!passable) {
                                        agent.state = CrowdAgentState::wait_crosswalk;
                                        agent.current_edge_index = chosen_idx;
                                        agent.target_node = next_target;
                                        agent.speed_mps = 0.0F;
                                        agent.velocity = glm::vec3(0.0F);
                                    } else {
                                        agent.current_edge_index = chosen_idx;
                                        agent.target_node = next_target;
                                    }
                                } else {
                                    agent.current_edge_index = chosen_idx;
                                    agent.target_node = next_target;
                                }
                            }
                        }
                    }
                }

                if (agent.state == CrowdAgentState::walk) {
                    const glm::vec3 dir = (dist_xz > 0.001F)
                        ? glm::vec3(to_target.x / dist_xz, 0.0F, to_target.z / dist_xz)
                        : glm::vec3(0.0F, 0.0F, 1.0F);

                    float speed_limit = config_.desired_walk_speed_mps;
                    const NavEdge* cur_edge = graph.edge(agent.current_edge_index);
                    if (cur_edge != nullptr && cur_edge->speed_limit_mps > 0.1F) {
                        speed_limit = std::min(speed_limit, cur_edge->speed_limit_mps);
                    }

                    glm::vec3 desired_vel = dir * speed_limit;

                    // Neighbor separation force (zero heap allocations)
                    glm::vec3 separation{0.0F};
                    for (const auto& other : agents_) {
                        if (!other.active || other.agent_id == agent.agent_id) {
                            continue;
                        }
                        const glm::vec3 diff = agent.position - other.position;
                        const float d2 = diff.x * diff.x + diff.z * diff.z;
                        const float min_d = config_.agent_radius * 2.0F;
                        if (d2 < min_d * min_d && d2 > 0.0001F) {
                            const float d = std::sqrt(d2);
                            separation += (diff / d) * ((min_d - d) / min_d);
                        }
                    }

                    desired_vel += glm::vec3(separation.x, 0.0F, separation.z) *
                        config_.separation_weight;

                    const float plan_speed = std::hypot(desired_vel.x, desired_vel.z);
                    if (plan_speed > speed_limit * 1.35F) {
                        desired_vel = (desired_vel / plan_speed) * (speed_limit * 1.35F);
                        agent.speed_mps = speed_limit * 1.35F;
                    } else {
                        agent.speed_mps = plan_speed;
                    }

                    agent.velocity = desired_vel;
                    agent.position += agent.velocity * clamped_dt;

                    if (agent.speed_mps > 0.05F) {
                        const float target_yaw = std::atan2(
                            agent.velocity.x,
                            agent.velocity.z);
                        const float diff = wrap_angle_radians(target_yaw - agent.yaw_radians);
                        agent.yaw_radians = wrap_angle_radians(
                            agent.yaw_radians + std::clamp(diff, -7.0F * clamped_dt, 7.0F * clamped_dt));
                    }
                }
            }
        }

        // Height snap using collision supporting surface
        if (ground_raycast_world != nullptr) {
            const auto height = ground_raycast_world->sample_supporting_height(
                agent.position.x,
                agent.position.z,
                agent.position.y + 0.5F);
            if (height.has_value()) {
                agent.position.y = *height;
            }
        }
    };

    if (active_count_ <= budget) {
        // Update all active agents
        for (auto& a : agents_) {
            if (a.active) {
                step_single_agent(a, a.accumulated_dt_s);
                a.accumulated_dt_s = 0.0F;
            }
        }
    } else {
        // Budgeted round-robin update
        std::uint32_t updated_count = 0U;
        const std::size_t total_slots = agents_.size();
        std::size_t index = round_robin_cursor_;

        for (std::size_t step = 0; step < total_slots && updated_count < budget; ++step) {
            InternalAgent& a = agents_[index];
            if (a.active) {
                step_single_agent(a, a.accumulated_dt_s);
                a.accumulated_dt_s = 0.0F;
                ++updated_count;
            }
            index = (index + 1U) % total_slots;
        }
        round_robin_cursor_ = static_cast<std::uint32_t>(index);
    }

    // Sync snapshots
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        const auto& a = agents_[i];
        auto& s = snapshots_[i];
        s.agent_id = a.agent_id;
        s.scene_node = a.scene_node;
        s.state = a.state;
        s.position = a.position;
        s.yaw_radians = a.yaw_radians;
        s.velocity = a.velocity;
        s.speed_mps = a.speed_mps;
        s.current_node = a.current_node;
        s.target_node = a.target_node;
        s.active = a.active;
    }
}

std::span<const CrowdAgentSnapshot> CrowdSystem::agents() const noexcept {
    return snapshots_;
}

std::size_t CrowdSystem::active_agent_count() const noexcept {
    return active_count_;
}

} // namespace vcr
