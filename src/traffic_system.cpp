#include "traffic_system.hpp"

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

void SignalIntersectionSystem::configure(std::span<const SignalPhaseConfig> phases) {
    phases_.assign(phases.begin(), phases.end());
    current_phase_index_ = 0U;
    stage_ = PhaseStage::green;
    elapsed_in_stage_ = 0.0F;
}

void SignalIntersectionSystem::reset() noexcept {
    current_phase_index_ = 0U;
    stage_ = PhaseStage::green;
    elapsed_in_stage_ = 0.0F;
}

void SignalIntersectionSystem::update(float delta_seconds) noexcept {
    if (phases_.empty() || !std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }

    float remaining = delta_seconds;
    int guard = 0;
    while (remaining > 1.0e-6F && guard < 64) {
        ++guard;
        const auto& phase = phases_[current_phase_index_];
        float duration = 0.0F;
        if (stage_ == PhaseStage::green) {
            duration = std::max(phase.green_seconds, 0.001F);
        } else if (stage_ == PhaseStage::yellow) {
            duration = std::max(phase.yellow_seconds, 0.0F);
        } else {
            duration = std::max(phase.all_red_seconds, 0.0F);
        }

        const float needed = std::max(0.0F, duration - elapsed_in_stage_);
        if (remaining + 1.0e-6F < needed) {
            elapsed_in_stage_ += remaining;
            break;
        }

        remaining = std::max(0.0F, remaining - needed);
        elapsed_in_stage_ = 0.0F;
        if (stage_ == PhaseStage::green) {
            stage_ = PhaseStage::yellow;
        } else if (stage_ == PhaseStage::yellow) {
            stage_ = PhaseStage::all_red;
        } else {
            stage_ = PhaseStage::green;
            current_phase_index_ = (current_phase_index_ + 1U) %
                static_cast<std::uint32_t>(phases_.size());
        }
    }
}

SignalAspect SignalIntersectionSystem::vehicle_signal(std::uint32_t group_id) const noexcept {
    if (group_id == 0U || phases_.empty()) {
        return SignalAspect::green;
    }
    const auto& phase = phases_[current_phase_index_];
    if (group_id == phase.active_vehicle_group) {
        if (stage_ == PhaseStage::green) {
            return SignalAspect::green;
        }
        if (stage_ == PhaseStage::yellow) {
            return SignalAspect::yellow;
        }
    }
    return SignalAspect::red;
}

bool SignalIntersectionSystem::pedestrian_can_cross(std::uint32_t ped_group_id) const noexcept {
    if (ped_group_id == 0U || phases_.empty()) {
        return true;
    }
    const auto& phase = phases_[current_phase_index_];
    if (ped_group_id == phase.active_pedestrian_group && stage_ == PhaseStage::green) {
        return true;
    }
    return false;
}

std::uint32_t SignalIntersectionSystem::current_phase_index() const noexcept {
    return current_phase_index_;
}

float SignalIntersectionSystem::phase_time_remaining() const noexcept {
    if (phases_.empty()) {
        return 0.0F;
    }
    const auto& phase = phases_[current_phase_index_];
    float remaining = 0.0F;
    if (stage_ == PhaseStage::green) {
        remaining = (phase.green_seconds - elapsed_in_stage_) +
            phase.yellow_seconds + phase.all_red_seconds;
    } else if (stage_ == PhaseStage::yellow) {
        remaining = (phase.yellow_seconds - elapsed_in_stage_) + phase.all_red_seconds;
    } else {
        remaining = phase.all_red_seconds - elapsed_in_stage_;
    }
    return std::max(0.0F, remaining);
}

void LaneGraph::clear() noexcept {
    nodes_.clear();
    segments_.clear();
    segment_lengths_.clear();
    segment_directions_.clear();
    built_ = false;
}

std::uint32_t LaneGraph::add_node(const LaneNode& node) {
    LaneNode copy = node;
    copy.id = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(copy);
    built_ = false;
    return copy.id;
}

std::uint32_t LaneGraph::add_segment(const LaneSegment& segment) {
    LaneSegment copy = segment;
    copy.id = static_cast<std::uint32_t>(segments_.size());
    segments_.push_back(copy);
    built_ = false;
    return copy.id;
}

bool LaneGraph::build() {
    built_ = false;
    const std::size_t node_sz = nodes_.size();
    const std::size_t seg_sz = segments_.size();

    for (const auto& s : segments_) {
        if (s.start_node >= node_sz || s.end_node >= node_sz) {
            return false;
        }
        for (std::uint32_t k = 0; k < s.next_count && k < s.next_segments.size(); ++k) {
            if (s.next_segments[k] >= seg_sz) {
                return false;
            }
        }
    }

    segment_lengths_.resize(seg_sz);
    segment_directions_.resize(seg_sz);

    for (std::size_t i = 0; i < seg_sz; ++i) {
        const auto& s = segments_[i];
        const glm::vec3 p0 = nodes_[s.start_node].position;
        const glm::vec3 p1 = nodes_[s.end_node].position;
        const glm::vec3 diff = p1 - p0;
        const float len = glm::length(diff);
        segment_lengths_[i] = len;
        segment_directions_[i] = (len > 0.0001F)
            ? (diff / len)
            : nodes_[s.start_node].forward;
    }

    built_ = true;
    return true;
}

const LaneSegment* LaneGraph::segment(std::uint32_t id) const noexcept {
    if (id < segments_.size()) {
        return &segments_[id];
    }
    return nullptr;
}

const LaneNode* LaneGraph::node(std::uint32_t id) const noexcept {
    if (id < nodes_.size()) {
        return &nodes_[id];
    }
    return nullptr;
}

glm::vec3 LaneGraph::sample_point(
    std::uint32_t segment_id,
    float distance_along) const noexcept {
    if (segment_id >= segments_.size()) {
        return glm::vec3(0.0F);
    }
    const auto& s = segments_[segment_id];
    const float len = segment_lengths_[segment_id];
    const float clamped_dist = std::clamp(distance_along, 0.0F, len);
    return nodes_[s.start_node].position + segment_directions_[segment_id] * clamped_dist;
}

glm::vec3 LaneGraph::segment_direction(std::uint32_t segment_id) const noexcept {
    if (segment_id < segment_directions_.size()) {
        return segment_directions_[segment_id];
    }
    return glm::vec3(0.0F, 0.0F, 1.0F);
}

float LaneGraph::segment_length(std::uint32_t segment_id) const noexcept {
    if (segment_id < segment_lengths_.size()) {
        return segment_lengths_[segment_id];
    }
    return 0.0F;
}

std::size_t LaneGraph::segment_count() const noexcept {
    return segments_.size();
}

void TrafficSystem::initialize(const TrafficConfig& config, const LaneGraph& graph) {
    config_ = config;
    prng_ = RngState(config.seed);
    graph_ = &graph;
    vehicles_.assign(config.max_vehicles, InternalVehicle{});
    snapshots_.assign(config.max_vehicles, TrafficVehicleSnapshot{});
    active_count_ = 0U;
    next_vehicle_id_ = 1U;
}

void TrafficSystem::reset() noexcept {
    for (auto& v : vehicles_) {
        v = InternalVehicle{};
    }
    for (auto& s : snapshots_) {
        s = TrafficVehicleSnapshot{};
    }
    active_count_ = 0U;
    next_vehicle_id_ = 1U;
}

std::optional<std::uint32_t> TrafficSystem::spawn_vehicle(
    std::uint32_t segment_id,
    float distance_along,
    SceneNodeId bound_node) {
    if (vehicles_.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < vehicles_.size(); ++i) {
        if (!vehicles_[i].active && !vehicles_[i].released_to_player) {
            InternalVehicle& v = vehicles_[i];
            v.vehicle_id = next_vehicle_id_++;
            v.scene_node = bound_node;
            v.segment_id = segment_id;
            v.distance_along = distance_along;
            v.position = glm::vec3(0.0F);
            v.yaw_radians = 0.0F;
            v.speed_mps = 0.0F;
            v.throttle = 0.0F;
            v.brake = 0.0F;
            v.steer = 0.0F;
            v.brake_lights = false;
            v.active = true;
            v.released_to_player = false;
            v.stuck_timer_s = 0.0F;

            if (graph_ != nullptr) {
                v.position = graph_->sample_point(segment_id, distance_along);
                const glm::vec3 dir = graph_->segment_direction(segment_id);
                v.yaw_radians = std::atan2(dir.x, dir.z);
            }

            TrafficVehicleSnapshot& snap = snapshots_[i];
            snap.vehicle_id = v.vehicle_id;
            snap.scene_node = v.scene_node;
            snap.segment_id = v.segment_id;
            snap.distance_along = v.distance_along;
            snap.position = v.position;
            snap.yaw_radians = v.yaw_radians;
            snap.speed_mps = v.speed_mps;
            snap.throttle = v.throttle;
            snap.brake = v.brake;
            snap.steer = v.steer;
            snap.brake_lights = v.brake_lights;
            snap.active = true;
            snap.released_to_player = false;

            ++active_count_;
            return v.vehicle_id;
        }
    }
    return std::nullopt;
}

bool TrafficSystem::release_vehicle_to_player(std::uint32_t vehicle_id) noexcept {
    for (std::size_t i = 0; i < vehicles_.size(); ++i) {
        if (vehicles_[i].vehicle_id == vehicle_id &&
            vehicles_[i].active &&
            !vehicles_[i].released_to_player) {
            vehicles_[i].released_to_player = true;
            vehicles_[i].active = false;
            snapshots_[i].released_to_player = true;
            snapshots_[i].active = false;
            if (active_count_ > 0U) {
                --active_count_;
            }
            return true;
        }
    }
    return false;
}

void TrafficSystem::recycle_far_vehicles(
    const glm::vec3& camera_position,
    const LaneGraph& /*graph*/) noexcept {
    for (std::size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.active || v.released_to_player) {
            continue;
        }
        const float dist = glm::distance(v.position, camera_position);
        if (dist > config_.despawn_radius_m) {
            v.active = false;
            snapshots_[i].active = false;
            if (active_count_ > 0U) {
                --active_count_;
            }
        }
    }
}

void TrafficSystem::update(
    float delta_seconds,
    const LaneGraph& graph,
    const SignalIntersectionSystem* signals,
    std::span<const TrafficObstacle> obstacles,
    const glm::vec3& camera_position) noexcept {
    if (active_count_ == 0U || delta_seconds <= 0.0F || vehicles_.empty()) {
        return;
    }

    const float clamped_dt = std::min(delta_seconds, 0.25F);

    for (std::size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.active || v.released_to_player) {
            continue;
        }

        const LaneSegment* cur_seg = graph.segment(v.segment_id);
        if (cur_seg == nullptr) {
            v.active = false;
            snapshots_[i].active = false;
            if (active_count_ > 0U) {
                --active_count_;
            }
            continue;
        }

        const float seg_len = graph.segment_length(v.segment_id);
        const glm::vec3 seg_dir = graph.segment_direction(v.segment_id);
        const float speed_limit = cur_seg->speed_limit_mps;
        const float v_curr = v.speed_mps;

        float lead_gap = std::numeric_limits<float>::max();
        float lead_speed = 0.0F;
        bool has_lead = false;

        // 1. Lead vehicles on current segment ahead of vehicle
        for (std::size_t j = 0; j < vehicles_.size(); ++j) {
            if (j == i) {
                continue;
            }
            const auto& other = vehicles_[j];
            if ((other.active || other.released_to_player) && other.segment_id == v.segment_id) {
                if (other.distance_along > v.distance_along) {
                    const float d = other.distance_along - v.distance_along;
                    if (d < lead_gap) {
                        lead_gap = d;
                        lead_speed = other.speed_mps;
                        has_lead = true;
                    }
                }
            }
        }

        // 2. Lead vehicles on downstream segments
        const float dist_to_seg_end = seg_len - v.distance_along;
        if (lead_gap > dist_to_seg_end) {
            for (std::uint32_t k = 0; k < cur_seg->next_count && k < cur_seg->next_segments.size(); ++k) {
                const std::uint32_t next_id = cur_seg->next_segments[k];
                for (std::size_t j = 0; j < vehicles_.size(); ++j) {
                    if (j == i) {
                        continue;
                    }
                    const auto& other = vehicles_[j];
                    if ((other.active || other.released_to_player) && other.segment_id == next_id) {
                        const float d = dist_to_seg_end + other.distance_along;
                        if (d < lead_gap) {
                            lead_gap = d;
                            lead_speed = other.speed_mps;
                            has_lead = true;
                        }
                    }
                }
            }
        }

        // 3. Traffic signal stop line
        bool stopped_at_red = false;
        bool lead_is_stop_line = false;
        if (signals != nullptr && cur_seg->has_stop_line) {
            const float stop_line_pos = std::max(0.0F, seg_len - cur_seg->stop_line_offset_from_end);
            if (v.distance_along <= stop_line_pos) {
                const float dist_to_stop = stop_line_pos - v.distance_along;
                const SignalAspect aspect = signals->vehicle_signal(cur_seg->signal_group_id);
                if (aspect == SignalAspect::red) {
                    if (dist_to_stop < lead_gap) {
                        lead_gap = dist_to_stop;
                        lead_speed = 0.0F;
                        has_lead = true;
                        lead_is_stop_line = true;
                        stopped_at_red = (dist_to_stop < 8.0F);
                    }
                } else if (aspect == SignalAspect::yellow) {
                    const float stopping_dist = (v_curr * v_curr) /
                        (2.0F * config_.idm_comfort_decel_mps2);
                    if (dist_to_stop >= stopping_dist && dist_to_stop < lead_gap) {
                        lead_gap = dist_to_stop;
                        lead_speed = 0.0F;
                        has_lead = true;
                        lead_is_stop_line = true;
                    }
                }
            }
        }

        // 4. Nearby obstacles (pedestrians/player in forward cone)
        bool emergency_brake = false;
        for (const auto& obs : obstacles) {
            if (obs.ignore_node && obs.ignore_node == v.scene_node) {
                continue;
            }
            const glm::vec3 to_obs = obs.position - v.position;
            const float fwd = glm::dot(to_obs, seg_dir);
            if (fwd > 0.0F && fwd < 25.0F) {
                const glm::vec3 lat = to_obs - fwd * seg_dir;
                const float lat_dist = std::hypot(lat.x, lat.z);
                constexpr float lane_half_width = 1.8F;
                if (lat_dist < (lane_half_width + obs.radius)) {
                    const float gap = std::max(0.1F, fwd - obs.radius);
                    if (gap < lead_gap) {
                        lead_gap = gap;
                        lead_speed = 0.0F;
                        has_lead = true;
                        lead_is_stop_line = false;
                    }
                    if (fwd < 8.0F && v_curr > 0.5F) {
                        emergency_brake = true;
                    }
                }
            }
        }

        // 5. Intelligent Driver Model (IDM) acceleration
        float accel = 0.0F;
        const float a = config_.idm_max_accel_mps2;
        const float b = config_.idm_comfort_decel_mps2;
        const float s0 = config_.idm_min_gap_m;
        const float T = config_.idm_time_headway_s;

        if (emergency_brake) {
            accel = -config_.emergency_decel_mps2;
        } else if (has_lead) {
            const float net_gap = lead_is_stop_line
                ? std::max(0.1F, lead_gap - 2.2F)
                : std::max(0.1F, lead_gap - 4.5F);
            const float s0_eff = lead_is_stop_line ? 1.2F : s0;
            const float t_eff = lead_is_stop_line ? std::min(T, 0.9F) : T;
            if (lead_is_stop_line && v_curr < 0.8F && net_gap <= 2.0F) {
                accel = -b;
            } else {
                const float delta_v = v_curr - lead_speed;
                const float sqrt_ab = 2.0F * std::sqrt(std::max(0.01F, a * b));
                const float s_star = s0_eff + std::max(0.0F, v_curr * t_eff + (v_curr * delta_v) / sqrt_ab);
                const float v_ratio = (speed_limit > 0.1F) ? (v_curr / speed_limit) : 0.0F;
                const float gap_ratio = s_star / net_gap;
                accel = a * (1.0F - (v_ratio * v_ratio * v_ratio * v_ratio) - (gap_ratio * gap_ratio));
            }
        } else {
            const float v_ratio = (speed_limit > 0.1F) ? (v_curr / speed_limit) : 0.0F;
            accel = a * (1.0F - (v_ratio * v_ratio * v_ratio * v_ratio));
        }

        accel = std::clamp(accel, -config_.emergency_decel_mps2, config_.idm_max_accel_mps2);

        v.speed_mps += accel * clamped_dt;
        if (v.speed_mps < 0.0F) {
            v.speed_mps = 0.0F;
        }

        // Controls
        if (accel > 0.05F) {
            v.throttle = std::clamp(accel / config_.idm_max_accel_mps2, 0.0F, 1.0F);
            v.brake = 0.0F;
            v.brake_lights = false;
        } else if (accel < -0.15F) {
            v.throttle = 0.0F;
            v.brake = std::clamp(-accel / config_.emergency_decel_mps2, 0.0F, 1.0F);
            v.brake_lights = true;
        } else {
            v.throttle = 0.0F;
            v.brake = 0.0F;
            v.brake_lights = false;
        }

        // Advance distance along lane
        v.distance_along += v.speed_mps * clamped_dt;
        if (v.distance_along >= seg_len) {
            if (cur_seg->next_count > 0U) {
                const std::uint32_t choice = prng_.next_u64() % cur_seg->next_count;
                const std::uint32_t next_seg_id = cur_seg->next_segments[choice];
                v.distance_along -= seg_len;
                v.segment_id = next_seg_id;
                cur_seg = graph.segment(v.segment_id);
            } else {
                v.active = false;
                snapshots_[i].active = false;
                if (active_count_ > 0U) {
                    --active_count_;
                }
                continue;
            }
        }

        v.position = graph.sample_point(v.segment_id, v.distance_along);
        const glm::vec3 dir = graph.segment_direction(v.segment_id);
        const float target_yaw = std::atan2(dir.x, dir.z);

        const float yaw_diff = wrap_angle_radians(target_yaw - v.yaw_radians);
        constexpr float max_steer_angle = 0.785398F; // 45 deg
        v.steer = std::clamp(yaw_diff / max_steer_angle, -1.0F, 1.0F);

        constexpr float turn_speed = 6.0F; // rad/s
        v.yaw_radians = wrap_angle_radians(
            v.yaw_radians + std::clamp(yaw_diff, -turn_speed * clamped_dt, turn_speed * clamped_dt));

        // Stuck detection
        if (v.speed_mps < 0.25F && !stopped_at_red) {
            v.stuck_timer_s += clamped_dt;
            if (v.stuck_timer_s > config_.stuck_timeout_s) {
                const float dist_cam = glm::distance(v.position, camera_position);
                if (dist_cam > config_.min_spawn_radius_m) {
                    v.active = false;
                    snapshots_[i].active = false;
                    v.stuck_timer_s = 0.0F;
                    if (active_count_ > 0U) {
                        --active_count_;
                    }
                    continue;
                } else {
                    v.distance_along += 1.5F;
                    v.speed_mps = 2.0F;
                    v.stuck_timer_s = 0.0F;
                }
            }
        } else {
            v.stuck_timer_s = 0.0F;
        }

        // Snapshot copy
        auto& snap = snapshots_[i];
        snap.vehicle_id = v.vehicle_id;
        snap.scene_node = v.scene_node;
        snap.segment_id = v.segment_id;
        snap.distance_along = v.distance_along;
        snap.position = v.position;
        snap.yaw_radians = v.yaw_radians;
        snap.speed_mps = v.speed_mps;
        snap.throttle = v.throttle;
        snap.brake = v.brake;
        snap.steer = v.steer;
        snap.brake_lights = v.brake_lights;
        snap.active = v.active;
        snap.released_to_player = v.released_to_player;
    }
}

std::span<const TrafficVehicleSnapshot> TrafficSystem::vehicles() const noexcept {
    return snapshots_;
}

std::size_t TrafficSystem::active_vehicle_count() const noexcept {
    return active_count_;
}

} // namespace vcr
