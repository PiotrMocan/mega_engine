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
    const float analog_rgt =
        std::isfinite(input.move_axis.x) ? input.move_axis.x : 0.0F;
    const float analog_fwd =
        std::isfinite(input.move_axis.y) ? input.move_axis.y : 0.0F;
    const float fwd =
        (static_cast<float>(input.forward) - static_cast<float>(input.backward)) +
        analog_fwd;
    const float rgt =
        (static_cast<float>(input.right) - static_cast<float>(input.left)) +
        analog_rgt;
    const glm::vec3 raw = planar_forward * fwd + planar_right * rgt;
    const float raw_sq = glm::dot(raw, raw);
    if (raw_sq <= 1.0e-8F) {
        return glm::vec3(0.0F);
    }
    const float raw_len = std::sqrt(raw_sq);
    const float clamped_mag = std::min(raw_len, 1.0F);
    const float speed = input.run ? cfg.run_speed : cfg.walk_speed;
    return raw * ((speed * clamped_mag) / raw_len);
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
    const Renderer* const renderer,
    AudioEngine* const audio_engine) {
    error.clear();
    if (active_) {
        error = "Gameplay session is already active.";
        return false;
    }
    player_node_ = {};
    collision_world_.clear();
    physics_world_.clear();
    trigger_tracker_.reset();
    runtime_audio_nodes_.clear();
    has_prev_listener_position_ = false;

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
    scene.build_world_nodes(cached_world_nodes_, world_ancestor_scratch_);
    cached_content_revision_ = scene.content_revision();
    cached_structure_revision_ = scene.structure_revision();
    runtime_audio_nodes_.reserve(scene.audio_sources().size() + 8U);
    reverb_scratch_.reserve(scene.reverb_zones().size() + 4U);

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
    if (audio_engine != nullptr && audio_engine->initialized()) {
        audio_engine->set_gameplay_paused(false);
        sync_scene_audio(scene, output_camera, 0.0F, true, *audio_engine);
    }
    return true;
}

bool GameplaySession::update(
    const GameplayFrameInput& input,
    const float delta_seconds,
    const SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& output_camera,
    AudioEngine* const audio_engine,
    InputActionSystem* const input_actions) noexcept {
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
        scene.build_world_nodes(cached_world_nodes_, world_ancestor_scratch_);
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

    if (input_actions != nullptr) {
        if (character_.just_jumped()) {
            input_actions->trigger_rumble(0.18F, 0.35F, 0.08F);
        } else if (character_.landing_impact_speed() >= 3.5F) {
            const float intensity = std::clamp(
                (character_.landing_impact_speed() - 3.5F) / 10.0F,
                0.15F,
                0.85F);
            input_actions->trigger_rumble(
                intensity, intensity * 0.6F, 0.12F);
        }
    }

    if (audio_engine != nullptr && audio_engine->initialized()) {
        sync_scene_audio(scene, output_camera, delta_seconds, false, *audio_engine);
        audio_engine->update(delta_seconds);
    }
    return true;
}

void GameplaySession::sync_scene_audio(
    const SceneDocument& scene,
    const FlyCamera& output_camera,
    const float delta_seconds,
    const bool initial_frame,
    AudioEngine& audio_engine) noexcept {
    const auto find_world_node =
        [this](const SceneNodeId id) noexcept -> const SceneWorldNode* {
        for (const auto& wn : cached_world_nodes_) {
            if (wn.id == id) {
                return &wn;
            }
        }
        return nullptr;
    };

    const auto resolve_world_pose =
        [this, &find_world_node](
            const SceneNodeId id,
            glm::vec3& out_pos,
            glm::vec3& out_fwd,
            glm::vec3& out_up,
            bool& out_visible) noexcept -> bool {
        const SceneWorldNode* const wn = find_world_node(id);
        if (wn == nullptr) {
            out_visible = false;
            return false;
        }
        out_visible = wn->world_visible;
        out_pos = glm::vec3(wn->world_matrix[3]);
        glm::vec3 raw_fwd = -glm::vec3(wn->world_matrix[2]);
        glm::vec3 raw_up = glm::vec3(wn->world_matrix[1]);
        if (const float f_sq = glm::dot(raw_fwd, raw_fwd); f_sq > 1.0e-8F) {
            raw_fwd *= 1.0F / std::sqrt(f_sq);
        } else {
            raw_fwd = {0.0F, 0.0F, -1.0F};
        }
        if (const float u_sq = glm::dot(raw_up, raw_up); u_sq > 1.0e-8F) {
            raw_up *= 1.0F / std::sqrt(u_sq);
        } else {
            raw_up = {0.0F, 1.0F, 0.0F};
        }
        out_fwd = raw_fwd;
        out_up = raw_up;

        // If this node is driven by PhysicsWorld, prefer its live dynamic pose.
        const std::size_t body_count = physics_world_.body_count();
        for (std::size_t b = 0U; b < body_count; ++b) {
            if (physics_world_.body_node_id(b) == id) {
                const TransformRows rows = physics_world_.body_transform_rows(b);
                out_pos = {rows.x.w, rows.y.w, rows.z.w};
                break;
            }
        }
        return true;
    };

    // 1. Listener pose + velocity (explicit AudioListenerComponent or camera).
    AudioListenerState listener{};
    listener.position = output_camera.position();
    listener.forward = output_camera.forward();
    listener.up = glm::vec3{0.0F, 1.0F, 0.0F};
    listener.volume_scale = 1.0F;
    listener.doppler_scale = 1.0F;

    const auto listener_owners = scene.audio_listeners().owners();
    const auto listener_values = scene.audio_listeners().values();
    for (std::size_t i = 0U; i < listener_owners.size(); ++i) {
        if (!listener_values[i].enabled) {
            continue;
        }
        glm::vec3 pos{};
        glm::vec3 fwd{};
        glm::vec3 up{};
        bool visible = false;
        if (resolve_world_pose(listener_owners[i], pos, fwd, up, visible) &&
            visible) {
            listener.position = pos;
            listener.forward = fwd;
            listener.up = up;
            listener.volume_scale = listener_values[i].volume_scale;
            listener.doppler_scale = listener_values[i].doppler_scale;
            break;
        }
    }

    if (!initial_frame && has_prev_listener_position_ && delta_seconds > 1.0e-4F) {
        listener.velocity =
            (listener.position - prev_listener_position_) * (1.0F / delta_seconds);
    }
    prev_listener_position_ = listener.position;
    has_prev_listener_position_ = true;
    audio_engine.set_listener(listener);

    // 2. Reverb zones around listener.
    reverb_scratch_.clear();
    const auto reverb_owners = scene.reverb_zones().owners();
    const auto reverb_values = scene.reverb_zones().values();
    for (std::size_t i = 0U; i < reverb_owners.size(); ++i) {
        if (!reverb_values[i].enabled) {
            continue;
        }
        glm::vec3 pos{};
        glm::vec3 fwd{};
        glm::vec3 up{};
        bool visible = false;
        if (resolve_world_pose(reverb_owners[i], pos, fwd, up, visible) &&
            visible) {
            reverb_scratch_.emplace_back(pos, reverb_values[i]);
        }
    }
    audio_engine.set_reverb_state(
        evaluate_reverb_zones_at_listener(listener.position, reverb_scratch_));

    // 3. Scene AudioSourceComponents (spatialization, Doppler, raycast occlusion).
    const auto source_owners = scene.audio_sources().owners();
    const auto source_values = scene.audio_sources().values();

    // Stop voices whose node or component was removed/disabled.
    for (std::size_t idx = 0U; idx < runtime_audio_nodes_.size();) {
        RuntimeAudioNode& entry = runtime_audio_nodes_[idx];
        const auto comp = scene.audio_source(entry.id);
        if (!comp || !comp->get().enabled || comp->get().path_view().empty()) {
            if (entry.voice) {
                audio_engine.stop_voice(entry.voice, 0.03F);
            }
            entry = runtime_audio_nodes_.back();
            runtime_audio_nodes_.pop_back();
            continue;
        }
        ++idx;
    }

    const float occ_blend = delta_seconds > 0.0F
        ? std::clamp(1.0F - std::exp(-delta_seconds * 10.0F), 0.05F, 1.0F)
        : 1.0F;

    for (std::size_t i = 0U; i < source_owners.size(); ++i) {
        const SceneNodeId id = source_owners[i];
        const AudioSourceComponent& src = source_values[i];
        if (!src.enabled || src.path_view().empty()) {
            continue;
        }

        glm::vec3 pos{};
        glm::vec3 fwd{};
        glm::vec3 up{};
        bool visible = false;
        if (!resolve_world_pose(id, pos, fwd, up, visible) || !visible) {
            continue;
        }

        RuntimeAudioNode* tracked = nullptr;
        for (auto& existing : runtime_audio_nodes_) {
            if (existing.id == id) {
                tracked = &existing;
                break;
            }
        }
        if (tracked == nullptr) {
            runtime_audio_nodes_.push_back(RuntimeAudioNode{.id = id});
            tracked = &runtime_audio_nodes_.back();
        }

        glm::vec3 src_velocity{0.0F};
        if (!initial_frame && tracked->has_prev_position && delta_seconds > 1.0e-4F) {
            src_velocity =
                (pos - tracked->prev_position) * (1.0F / delta_seconds);
        }
        tracked->prev_position = pos;
        tracked->has_prev_position = true;

        float target_occlusion = 0.0F;
        if (src.spatial && src.occlusion) {
            const glm::vec3 to_src = pos - listener.position;
            const float dist_sq = glm::dot(to_src, to_src);
            if (dist_sq > 0.09F && dist_sq <= src.max_distance * src.max_distance) {
                const float dist = std::sqrt(dist_sq);
                const glm::vec3 ray_dir = to_src * (1.0F / dist);
                const float probe_dist = std::max(dist - 0.15F, 0.05F);
                bool hit_blocker = false;
                if (!collision_world_.empty() &&
                    collision_world_.raycast(
                        listener.position,
                        ray_dir,
                        probe_dist,
                        0xFFFFFFFFU,
                        false).has_value()) {
                    hit_blocker = true;
                }
                if (!hit_blocker && !physics_world_.empty() &&
                    physics_world_.raycast(
                        listener.position,
                        ray_dir,
                        probe_dist,
                        0xFFFFFFFFU).has_value()) {
                    hit_blocker = true;
                }
                target_occlusion = hit_blocker ? 1.0F : 0.0F;
            }
        }
        tracked->smoothed_occlusion +=
            occ_blend * (target_occlusion - tracked->smoothed_occlusion);

        AudioSpatialParams spatial{};
        spatial.position = pos;
        spatial.forward = fwd;
        spatial.velocity = src_velocity;
        spatial.min_distance = src.min_distance;
        spatial.max_distance = src.max_distance;
        spatial.rolloff = src.rolloff;
        spatial.doppler_factor = src.doppler_factor;
        spatial.inner_cone_degrees = src.inner_cone_degrees;
        spatial.outer_cone_degrees = src.outer_cone_degrees;
        spatial.outer_cone_gain = src.outer_cone_gain;
        spatial.occlusion = tracked->smoothed_occlusion;
        spatial.attenuation = src.attenuation;

        if (!tracked->started_once && src.play_on_start) {
            tracked->started_once = true;
            AudioPlayParams params{};
            params.bus = src.bus;
            params.volume = src.volume;
            params.pitch = src.pitch;
            params.loop = src.loop;
            params.spatial = src.spatial;
            params.streaming = src.streaming;
            params.spatial_params = spatial;
            std::string play_error;
            tracked->voice =
                audio_engine.play_asset(src.path_view(), params, play_error);
        } else if (tracked->voice && audio_engine.is_voice_active(tracked->voice)) {
            audio_engine.update_voice(
                tracked->voice, src.volume, src.pitch, spatial);
        }
    }
}

void GameplaySession::stop(AudioEngine* const audio_engine) noexcept {
    if (audio_engine != nullptr && audio_engine->initialized()) {
        audio_engine->stop_gameplay_audio(0.03F);
    }
    runtime_audio_nodes_.clear();
    has_prev_listener_position_ = false;
    collision_world_.clear();
    physics_world_.clear();
    trigger_tracker_.reset();
    player_node_ = {};
    cached_content_revision_ = 0U;
    cached_structure_revision_ = 0U;
    active_ = false;
}

} // namespace vcr
