#include "gameplay_session.hpp"

#include "fly_camera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

namespace vcr {
namespace {

constexpr glm::vec3 player_half_extents{0.45F, 0.9F, 0.3F};
constexpr float radians_to_degrees = 57.2957795131F;
constexpr float maximum_session_delta_seconds = 0.1F;

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

[[nodiscard]] glm::mat4 player_cube_matrix(
    const glm::vec3 feet_position,
    const float facing_yaw) noexcept {
    glm::mat4 model = glm::translate(
        glm::mat4(1.0F),
        feet_position + glm::vec3(0.0F, player_half_extents.y, 0.0F));
    model = glm::rotate(model, facing_yaw, glm::vec3(0.0F, 1.0F, 0.0F));
    model = glm::scale(model, player_half_extents);
    return model;
}

[[nodiscard]] glm::mat4 trs_matrix(
    const glm::vec3 position,
    const float yaw_radians,
    const glm::vec3 scale) noexcept {
    glm::mat4 model = glm::translate(glm::mat4(1.0F), position);
    model = glm::rotate(model, yaw_radians, glm::vec3(0.0F, 1.0F, 0.0F));
    model = glm::scale(model, scale);
    return model;
}

[[nodiscard]] TransformRows mat4_to_rows(const glm::mat4& m) noexcept {
    return TransformRows{
        glm::row(m, 0),
        glm::row(m, 1),
        glm::row(m, 2),
    };
}

[[nodiscard]] glm::mat4 rows_to_mat4(const TransformRows& r) noexcept {
    glm::mat4 m(1.0F);
    m[0] = {r.x.x, r.y.x, r.z.x, 0.0F};
    m[1] = {r.x.y, r.y.y, r.z.y, 0.0F};
    m[2] = {r.x.z, r.y.z, r.z.z, 0.0F};
    m[3] = {r.x.w, r.y.w, r.z.w, 1.0F};
    return m;
}

void forward_to_yaw_pitch(
    glm::vec3 forward,
    float& out_yaw_radians,
    float& out_pitch_radians) noexcept {
    const float len_sq = glm::dot(forward, forward);
    if (!std::isfinite(len_sq) || len_sq <= 1.0e-8F) {
        out_yaw_radians = -1.57079632679F;
        out_pitch_radians = 0.0F;
        return;
    }
    forward *= 1.0F / std::sqrt(len_sq);
    out_yaw_radians = std::atan2(forward.z, forward.x);
    out_pitch_radians = std::asin(std::clamp(forward.y, -0.999F, 0.999F));
}

void extract_matrix_basis(
    const glm::mat4& m,
    glm::vec3& out_pos,
    glm::vec3& out_fwd,
    glm::vec3& out_up) noexcept {
    out_pos = glm::vec3(m[3]);
    glm::vec3 raw_fwd = -glm::vec3(m[2]);
    glm::vec3 raw_up = glm::vec3(m[1]);
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
}

} // namespace

void GameplayUpdateContext::set_camera_pose(
    const glm::vec3& position,
    const glm::vec3& forward) noexcept {
    camera_override_position = position;
    forward_to_yaw_pitch(
        forward,
        camera_override_yaw_radians,
        camera_override_pitch_radians);
    has_camera_override = true;
}

void GameplayUpdateContext::set_camera_angles(
    const glm::vec3& position,
    const float yaw_radians,
    const float pitch_radians) noexcept {
    camera_override_position = position;
    camera_override_yaw_radians = yaw_radians;
    camera_override_pitch_radians = pitch_radians;
    has_camera_override = true;
}

void GameplaySession::set_player_setup(const GameplayPlayerSetup& setup) {
    player_setup_ = setup;
}

void GameplaySession::set_extension_callbacks(
    const GameplayExtensionCallbacks& callbacks) noexcept {
    extensions_ = callbacks;
}

bool GameplaySession::start(
    SceneDocument& scene,
    const SceneRenderView& initial_view,
    const FlyCamera& startup_camera,
    FlyCamera& output_camera,
    std::string& error,
    const GameplayRendererBridge* const renderer_bridge,
    AudioEngine* const audio_engine,
    InputActionSystem* const input_actions) {
    error.clear();
    if (active_) {
        error = "Gameplay session is already active.";
        return false;
    }
    player_node_ = {};
    owns_player_cube_ = false;
    player_initial_scale_ = glm::vec3(1.0F);
    collision_world_.clear();
    physics_world_.clear();
    trigger_tracker_.reset();
    gameplay_world_.clear();
    animator_runtime_.clear(nullptr);
    transform_registry_.clear();
    transform_registry_.reserve(scene.size() + 16U);
    overlay_canvas_.clear();
    runtime_audio_nodes_.clear();
    has_prev_listener_position_ = false;
    default_camera_look_enabled_ = true;

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

    if (player_setup_.custom_player_node &&
        scene.valid(player_setup_.custom_player_node)) {
        player_node_ = player_setup_.custom_player_node;
        owns_player_cube_ = false;
        if (const auto custom_node = scene.node(player_node_)) {
            player_initial_scale_ = custom_node->get().transform.scale;
        }
    } else if (!player_setup_.disable_default_player_cube) {
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
        owns_player_cube_ = true;
        player_initial_scale_ = player_half_extents;
    }

    const ModelBoundsProvider bounds_provider =
        renderer_bridge != nullptr
            ? renderer_bridge->bounds_provider
            : ModelBoundsProvider{};
    const ModelCollisionProvider collision_provider =
        renderer_bridge != nullptr
            ? renderer_bridge->collision_provider
            : ModelCollisionProvider{};
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

    glm::vec3 spawn{0.0F};
    if (player_setup_.spawn_position.has_value()) {
        spawn = *player_setup_.spawn_position;
    } else {
        spawn = startup_camera.position() + movement_forward * 5.0F;
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
    }

    CharacterConfig base_cfg = player_setup_.use_animated_character
        ? player_setup_.animated_character_config.movement
        : CharacterConfig{};
    if (!player_setup_.spawn_position.has_value() ||
        base_cfg.ground_height == 0.0F) {
        base_cfg.ground_height = fallback_ground;
    }
    character_ = CharacterController(base_cfg);
    if (!collision_world_.empty()) {
        const float walkable_cos = std::cos(
            glm::radians(character_.config().max_slope_degrees));
        spawn = collision_world_
                    .resolve_capsule(
                        spawn,
                        character_.config().capsule_radius,
                        character_.config().capsule_height,
                        walkable_cos,
                        0xFFFFFFFFU,
                        player_node_)
                    .resolved_feet;
    }

    const float default_yaw = player_setup_.use_animated_character
        ? std::atan2(movement_forward.x, movement_forward.z)
        : std::atan2(movement_forward.x, movement_forward.z);
    const float facing_yaw =
        player_setup_.spawn_yaw_radians.value_or(default_yaw);

    character_.reset(spawn, facing_yaw);
    physics_world_.resolve_character_post_step(
        character_,
        collision_world_,
        0.0F);

    AnimatedCharacterConfig anim_cfg = player_setup_.animated_character_config;
    anim_cfg.movement = character_.config();
    anim_cfg.spawn_position = character_.position();
    anim_cfg.spawn_yaw_radians = facing_yaw;
    if (!anim_cfg.character_node) {
        anim_cfg.character_node = player_node_;
    }
    animated_player_.initialize(anim_cfg);

    if (owns_player_cube_ && player_node_) {
        SceneNode& player = scene.node(player_node_)->get();
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
    }

    if (player_node_) {
        const glm::mat4 init_player_mat = owns_player_cube_
            ? player_cube_matrix(character_.position(), facing_yaw)
            : (player_setup_.use_animated_character
                   ? animated_player_.model_matrix()
                   : trs_matrix(
                         character_.position(),
                         facing_yaw,
                         player_initial_scale_));
        static_cast<void>(transform_registry_.register_node(
            player_node_, init_player_mat));
        if (!anim_cfg.animation.root_motion_bone.empty()) {
            animator_runtime_.set_root_motion_bone(
                player_node_, anim_cfg.animation.root_motion_bone);
        }
    }

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
    gameplay_world_.start(
        scene,
        &collision_world_,
        &physics_world_,
        &animator_runtime_,
        audio_engine);

    const glm::vec3 startup_forward = startup_camera.forward();
    camera_.reset(std::atan2(startup_forward.z, startup_forward.x));
    const glm::vec3 initial_cam_pos = camera_.position(
        character_.position(),
        collision_world_,
        physics_world_);
    output_camera = FlyCamera(initial_cam_pos, camera_.yaw(), camera_.pitch());

    if (input_actions != nullptr) {
        input_actions->set_context(player_setup_.initial_input_context);
    }

    active_ = true;
    if (extensions_.on_start != nullptr) {
        if (!extensions_.on_start(extensions_.user_data, *this, scene, error)) {
            if (error.empty()) {
                error = "Project extension on_start callback failed.";
            }
            stop(audio_engine, renderer_bridge);
            return false;
        }
        transform_registry_.propagate_hierarchy();
        scene.build_world_nodes(cached_world_nodes_, world_ancestor_scratch_);
        cached_content_revision_ = scene.content_revision();
        cached_structure_revision_ = scene.structure_revision();
    }

    if (!content_asset_root_.empty()) {
        const std::string saved_locale(localization_.locale());
        localization_.reset_to_builtins();
        localization_.load_directory(content_asset_root_ / "localization", nullptr);
        if (!saved_locale.empty()) {
            static_cast<void>(localization_.set_locale(saved_locale));
        }
        game_ui_.clear_dialogues();
        game_ui_.load_dialogue_directory(content_asset_root_ / "dialogues", nullptr);
    }

    if (!game_ui_configured_ &&
        (extensions_.on_input_ai != nullptr ||
         extensions_.on_camera_audio_overlay != nullptr)) {
        GameUiConfig custom_cfg = game_ui_.config();
        custom_cfg.auto_pause_enabled = false;
        custom_cfg.show_default_hud = false;
        game_ui_.configure(custom_cfg);
    }
    game_ui_.reset_session_state();
    ui_audio_paused_ = false;

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
    const GameplayRendererBridge& renderer_bridge,
    FlyCamera& output_camera,
    AudioEngine* const audio_engine,
    InputActionSystem* const input_actions) noexcept {
    const glm::uvec2 raw_extent =
        renderer_bridge.query_output_extent != nullptr
            ? renderer_bridge.query_output_extent(renderer_bridge.context)
            : glm::uvec2{1280U, 720U};
    const glm::uvec2 viewport{
        std::max(1U, raw_extent.x),
        std::max(1U, raw_extent.y),
    };
    const bool ok = update_internal(
        input,
        delta_seconds,
        scene,
        &renderer_bridge,
        output_camera,
        renderer_bridge.transform_sink,
        renderer_bridge.skeletal_provider,
        viewport,
        audio_engine,
        input_actions);
    if (renderer_bridge.submit_overlay_vertices != nullptr) {
        renderer_bridge.submit_overlay_vertices(
            renderer_bridge.context, overlay_canvas_.vertices());
    }
    return ok;
}

bool GameplaySession::update_headless(
    const GameplayFrameInput& input,
    const float delta_seconds,
    const SceneDocument& scene,
    FlyCamera& output_camera,
    const RuntimeTransformSink& transform_sink,
    const SkeletalAssetProvider& skeletal_provider,
    const glm::uvec2 viewport_extent,
    AudioEngine* const audio_engine,
    InputActionSystem* const input_actions) noexcept {
    return update_internal(
        input,
        delta_seconds,
        scene,
        nullptr,
        output_camera,
        transform_sink,
        skeletal_provider,
        viewport_extent,
        audio_engine,
        input_actions);
}

bool GameplaySession::update_internal(
    const GameplayFrameInput& input,
    const float delta_seconds,
    const SceneDocument& scene,
    const GameplayRendererBridge* const renderer_bridge,
    FlyCamera& output_camera,
    const RuntimeTransformSink& transform_sink,
    const SkeletalAssetProvider& skeletal_provider,
    const glm::uvec2 viewport_extent,
    AudioEngine* const audio_engine,
    InputActionSystem* const input_actions) noexcept {
    if (!active_) {
        return false;
    }

    float dt = std::clamp(
        std::isfinite(delta_seconds) ? delta_seconds : 0.0F,
        0.0F,
        maximum_session_delta_seconds);

    if (scene.content_revision() != cached_content_revision_ ||
        scene.structure_revision() != cached_structure_revision_) {
        const ModelBoundsProvider bounds_provider =
            renderer_bridge != nullptr
                ? renderer_bridge->bounds_provider
                : ModelBoundsProvider{};
        const ModelCollisionProvider collision_provider =
            renderer_bridge != nullptr
                ? renderer_bridge->collision_provider
                : ModelCollisionProvider{};
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
        gameplay_world_.sync_scene(
            scene,
            &collision_world_,
            &physics_world_,
            &animator_runtime_,
            audio_engine);
        cached_content_revision_ = scene.content_revision();
        cached_structure_revision_ = scene.structure_revision();
    }

    const std::uint32_t vp_w = std::max(1U, viewport_extent.x);
    const std::uint32_t vp_h = std::max(1U, viewport_extent.y);
    const glm::vec3 ui_player_pos = player_setup_.use_animated_character
        ? animated_player_.world_position()
        : character_.position();
    const GameUiUpdateInput ui_in{
        .delta_seconds = dt,
        .viewport_extent = glm::uvec2(vp_w, vp_h),
        .mouse_pos_px = input.mouse_pos_px,
        .mouse_left_pressed = input.mouse_left_pressed,
        .display_fps = input.display_fps,
        .camera_pos = output_camera.position(),
        .camera_forward = output_camera.forward(),
        .camera_up = glm::vec3{0.0F, 1.0F, 0.0F},
        .player_pos = ui_player_pos,
        .player_health = gameplay_world_.player_health(),
        .player_max_health = gameplay_world_.player_max_health(),
        .scene = &scene,
        .world_nodes = cached_world_nodes_,
        .gameplay_world = &gameplay_world_,
        .input_actions = input_actions,
        .audio_engine = audio_engine,
        .localization = &localization_,
    };
    game_ui_.update(ui_in);
    if (game_ui_.blocks_gameplay_simulation()) {
        if (audio_engine != nullptr && audio_engine->initialized()) {
            if (!ui_audio_paused_) {
                audio_engine->set_gameplay_paused(true);
                ui_audio_paused_ = true;
            }
            audio_engine->update(dt);
        }
        overlay_canvas_.set_viewport(
            static_cast<float>(vp_w),
            static_cast<float>(vp_h));
        overlay_canvas_.clear();
        game_ui_.render(overlay_canvas_, ui_in);
        return true;
    }
    if (ui_audio_paused_) {
        if (audio_engine != nullptr && audio_engine->initialized()) {
            audio_engine->set_gameplay_paused(false);
        }
        ui_audio_paused_ = false;
    }

    GameplayUpdateContext ctx{
        .input = input,
        .scene = scene,
        .skeletal_provider = skeletal_provider,
        .renderer_bridge = renderer_bridge,
        .audio_engine = audio_engine,
        .input_actions = input_actions,
        .delta_seconds = dt,
    };
    if (game_ui_.blocks_player_control()) {
        ctx.input.character = CharacterInput{};
        ctx.input.mouse_delta_x = 0.0F;
        ctx.input.mouse_delta_y = 0.0F;
        ctx.input.mouse_left_down = false;
        ctx.input.mouse_left_pressed = false;
        ctx.suppress_default_camera_look = true;
    }

    // Stage 1: Input & AI.
    if (extensions_.on_input_ai != nullptr) {
        extensions_.on_input_ai(extensions_.user_data, *this, ctx);
    }
    dt = std::clamp(ctx.delta_seconds, 0.0F, maximum_session_delta_seconds);
    if (default_camera_look_enabled_ && !ctx.suppress_default_camera_look) {
        camera_.rotate(ctx.input.mouse_delta_x, ctx.input.mouse_delta_y);
    }

    // Stage 2: Movement & Physics.
    const glm::vec3 cam_forward = camera_.planar_forward();
    if (player_setup_.use_animated_character) {
        const CookedSkeletonAsset* skel = nullptr;
        const std::vector<AnimationClip>* clips_ptr = nullptr;
        if (const auto pnode = scene.node(player_node_);
            pnode && !pnode->get().asset_path.empty()) {
            skel = skeletal_provider.skeleton(pnode->get().asset_path);
            clips_ptr = skeletal_provider.animations(pnode->get().asset_path);
        }
        const std::span<const AnimationClip> clips = clips_ptr != nullptr
            ? std::span<const AnimationClip>(*clips_ptr)
            : std::span<const AnimationClip>{};
        if (const auto anim_comp = scene.animator(player_node_)) {
            player_anim_scratch_ = anim_comp->get();
        }
        const float anim_cam_yaw = std::atan2(cam_forward.x, cam_forward.z);
        animated_player_.update(
            ctx.input.character,
            anim_cam_yaw,
            dt,
            &collision_world_,
            &player_anim_scratch_,
            skel,
            clips);
        if (animated_player_.movement_enabled()) {
            character_.reset(
                animated_player_.world_position(),
                animated_player_.world_yaw_radians());
            if (scene.animator(player_node_)) {
                animator_runtime_.set_preview(
                    player_node_,
                    player_anim_scratch_.clip_name_view(),
                    false,
                    animated_player_.current_animation_time(),
                    player_anim_scratch_.looping);
            }
        }
    } else if (player_node_) {
        character_.update(
            ctx.input.character,
            cam_forward,
            dt,
            collision_world_);
    }

    if (extensions_.on_movement_physics != nullptr) {
        extensions_.on_movement_physics(extensions_.user_data, *this, ctx);
    }

    if (!physics_world_.empty()) {
        if (player_node_ &&
            (!player_setup_.use_animated_character ||
             animated_player_.movement_enabled())) {
            const glm::vec3 desired_vel = compute_desired_planar_velocity(
                ctx.input.character,
                cam_forward,
                character_.config());
            physics_world_.apply_character_push(character_, desired_vel, dt);
        }
        physics_world_.step(dt, collision_world_);
        if (player_node_ &&
            (!player_setup_.use_animated_character ||
             animated_player_.movement_enabled())) {
            physics_world_.resolve_character_post_step(
                character_, collision_world_, dt);
            if (player_setup_.use_animated_character) {
                animated_player_.apply_external_resolution(character_.position());
            }
        }

        const std::size_t body_count = physics_world_.body_count();
        for (std::size_t i = 0U; i < body_count; ++i) {
            const SceneNodeId id = physics_world_.body_node_id(i);
            if (!id) {
                continue;
            }
            const TransformRows rows = physics_world_.body_transform_rows(i);
            if (transform_registry_.has_node(id)) {
                if (!transform_registry_.is_attached(id)) {
                    static_cast<void>(transform_registry_.set_world_matrix(
                        id, rows_to_mat4(rows)));
                }
            } else if (transform_sink.apply != nullptr) {
                static_cast<void>(transform_sink.apply(
                    transform_sink.context, id, rows));
            }
        }
    }

    const glm::vec3 active_player_pos =
        player_setup_.use_animated_character
            ? animated_player_.world_position()
            : character_.position();

    trigger_tracker_.update(
        collision_world_,
        active_player_pos,
        character_.config().capsule_radius,
        character_.config().capsule_height);

    gameplay_world_.update(
        dt,
        scene,
        active_player_pos,
        ctx.input.character.interact,
        trigger_tracker_.events(),
        &collision_world_,
        &physics_world_,
        &animator_runtime_,
        audio_engine,
        transform_sink);

    // Stage 3: Authoritative Runtime Transforms & Child Attachments.
    if (player_node_ && transform_registry_.has_node(player_node_) &&
        !transform_registry_.is_attached(player_node_)) {
        if (owns_player_cube_) {
            static_cast<void>(transform_registry_.set_world_matrix(
                player_node_,
                player_cube_matrix(
                    character_.position(), character_.facing_yaw())));
        } else if (player_setup_.use_animated_character) {
            if (animated_player_.movement_enabled()) {
                static_cast<void>(transform_registry_.set_world_matrix(
                    player_node_, animated_player_.model_matrix()));
            }
        } else {
            static_cast<void>(transform_registry_.set_world_matrix(
                player_node_,
                trs_matrix(
                    character_.position(),
                    character_.facing_yaw(),
                    player_initial_scale_)));
        }
    }

    RuntimeTransformSink mutable_sink = transform_sink;
    transform_registry_.sync_outputs(
        &collision_world_,
        mutable_sink.apply != nullptr ? &mutable_sink : nullptr);

    if (player_node_ && !transform_registry_.has_node(player_node_) &&
        mutable_sink.apply != nullptr) {
        const TransformRows rows = owns_player_cube_
            ? mat4_to_rows(player_cube_matrix(
                  character_.position(), character_.facing_yaw()))
            : mat4_to_rows(trs_matrix(
                  character_.position(),
                  character_.facing_yaw(),
                  player_initial_scale_));
        if (!mutable_sink.apply(mutable_sink.context, player_node_, rows) &&
            owns_player_cube_) {
            return false;
        }
    } else if (owns_player_cube_ && mutable_sink.apply != nullptr && player_node_) {
        if (const auto wm = transform_registry_.world_matrix(player_node_)) {
            if (!mutable_sink.apply(
                    mutable_sink.context, player_node_, mat4_to_rows(*wm))) {
                return false;
            }
        }
    }

    if (extensions_.on_post_transforms != nullptr) {
        extensions_.on_post_transforms(extensions_.user_data, *this, ctx);
        // Publish project-driven transform updates in this submission.
        transform_registry_.sync_outputs(
            &collision_world_, mutable_sink.apply != nullptr ? &mutable_sink : nullptr);
    }

    // Stage 4: Animation & IK (uses updated live world matrices).
    const auto& live_pool = transform_registry_.live_world_nodes();
    for (SceneWorldNode& wn : cached_world_nodes_) {
        if (const auto live_mat = live_pool.get(wn.id)) {
            wn.world_matrix = live_mat->get();
        }
    }

    animator_runtime_.update(
        dt,
        scene,
        &collision_world_,
        &physics_world_,
        skeletal_provider,
        audio_engine,
        cached_world_nodes_,
        false,
        mutable_sink.apply != nullptr ? &mutable_sink : nullptr);

    for (const SceneNodeId anim_id : scene.animators().owners()) {
        if (const auto anim_comp = scene.animator(anim_id);
            anim_comp && anim_comp->get().root_motion_enabled) {
            const AnimatorNodeRuntime* rt =
                animator_runtime_.node_runtime(anim_id);
            GameplayEntityState* entity =
                gameplay_world_.find_entity(anim_id);
            if (rt != nullptr && entity != nullptr) {
                entity->runtime_position = rt->root_motion_position;
                entity->runtime_rotation_degrees =
                    rt->root_motion_rotation_degrees;
            }
        }
    }

    if (extensions_.on_post_animation != nullptr) {
        extensions_.on_post_animation(extensions_.user_data, *this, ctx);
    }

    // Stage 5: Camera, Spatial Audio & UI Overlay.
    const glm::vec3 cam_focus =
        player_setup_.use_animated_character
            ? animated_player_.world_position()
            : character_.position();
    const glm::vec3 on_foot_cam_pos = camera_.position(
        cam_focus, collision_world_, physics_world_);
    output_camera = FlyCamera(on_foot_cam_pos, camera_.yaw(), camera_.pitch());

    overlay_canvas_.set_viewport(
        static_cast<float>(vp_w),
        static_cast<float>(vp_h));
    overlay_canvas_.clear();

    if (extensions_.on_camera_audio_overlay != nullptr) {
        extensions_.on_camera_audio_overlay(extensions_.user_data, *this, ctx);
        if (ctx.has_camera_override) {
            output_camera = FlyCamera(
                ctx.camera_override_position,
                ctx.camera_override_yaw_radians,
                ctx.camera_override_pitch_radians);
        }
    }

    const GameUiUpdateInput ui_final_input{
        .delta_seconds = 0.0F,
        .viewport_extent = glm::uvec2(vp_w, vp_h),
        .mouse_pos_px = input.mouse_pos_px,
        .mouse_left_pressed = input.mouse_left_pressed,
        .display_fps = input.display_fps,
        .camera_pos = output_camera.position(),
        .camera_forward = output_camera.forward(),
        .camera_up = glm::vec3{0.0F, 1.0F, 0.0F},
        .player_pos = active_player_pos,
        .player_health = gameplay_world_.player_health(),
        .player_max_health = gameplay_world_.player_max_health(),
        .scene = &scene,
        .world_nodes = cached_world_nodes_,
        .gameplay_world = &gameplay_world_,
        .input_actions = input_actions,
        .audio_engine = audio_engine,
        .localization = &localization_,
    };
    game_ui_.refresh_markers(ui_final_input);
    game_ui_.render(overlay_canvas_, ui_final_input);

    if (input_actions != nullptr) {
        if (player_setup_.use_animated_character) {
            if (animated_player_.just_jumped()) {
                input_actions->trigger_rumble(0.18F, 0.35F, 0.08F);
            } else if (animated_player_.just_landed()) {
                input_actions->trigger_rumble(0.40F, 0.25F, 0.12F);
            }
        } else {
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
    }

    if (audio_engine != nullptr && audio_engine->initialized()) {
        sync_scene_audio(scene, output_camera, dt, false, *audio_engine);
        audio_engine->update(dt);
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
        if (const auto live_wm = transform_registry_.world_matrix(id)) {
            out_visible = wn != nullptr ? wn->world_visible : true;
            extract_matrix_basis(*live_wm, out_pos, out_fwd, out_up);
            return true;
        }
        if (wn == nullptr) {
            out_visible = false;
            return false;
        }
        out_visible = wn->world_visible;
        extract_matrix_basis(wn->world_matrix, out_pos, out_fwd, out_up);

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
        if (resolve_world_pose(
                listener_owners[i], pos, fwd, up, visible) &&
            visible) {
            listener.position = pos;
            listener.forward = fwd;
            listener.up = up;
            listener.volume_scale = listener_values[i].volume_scale;
            listener.doppler_scale = listener_values[i].doppler_scale;
            break;
        }
    }
    if (!initial_frame && has_prev_listener_position_ &&
        delta_seconds > 1.0e-4F) {
        listener.velocity =
            (listener.position - prev_listener_position_) *
            (1.0F / delta_seconds);
    }
    prev_listener_position_ = listener.position;
    has_prev_listener_position_ = true;
    audio_engine.set_listener(listener);

    // 2. Active reverb zone blend.
    reverb_scratch_.clear();
    const auto zone_owners = scene.reverb_zones().owners();
    const auto zone_values = scene.reverb_zones().values();
    for (std::size_t i = 0U; i < zone_owners.size(); ++i) {
        if (!zone_values[i].enabled) {
            continue;
        }
        glm::vec3 pos{};
        glm::vec3 fwd{};
        glm::vec3 up{};
        bool visible = false;
        if (resolve_world_pose(zone_owners[i], pos, fwd, up, visible) &&
            visible) {
            reverb_scratch_.emplace_back(pos, zone_values[i]);
        }
    }
    audio_engine.set_reverb_state(
        evaluate_reverb_zones_at_listener(listener.position, reverb_scratch_));

    // 3. Scene spatial/ambient AudioSourceComponents.
    const auto source_owners = scene.audio_sources().owners();
    const auto source_values = scene.audio_sources().values();

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
        if (!initial_frame && tracked->has_prev_position &&
            delta_seconds > 1.0e-4F) {
            src_velocity =
                (pos - tracked->prev_position) * (1.0F / delta_seconds);
        }
        tracked->prev_position = pos;
        tracked->has_prev_position = true;

        float target_occlusion = 0.0F;
        if (src.spatial && src.occlusion) {
            const glm::vec3 to_src = pos - listener.position;
            const float dist_sq = glm::dot(to_src, to_src);
            if (dist_sq > 0.09F &&
                dist_sq <= src.max_distance * src.max_distance) {
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

bool GameplaySession::save_game_state(
    const std::filesystem::path& path,
    std::string& error) const {
    if (!active_) {
        error = "Gameplay session is not active.";
        return false;
    }
    const glm::vec3 player_pos =
        player_setup_.use_animated_character
            ? animated_player_.world_position()
            : character_.position();
    const float player_yaw =
        player_setup_.use_animated_character
            ? animated_player_.world_yaw_radians()
            : character_.facing_yaw();
    GameStateSnapshot snap{};
    gameplay_world_.capture_snapshot(
        player_pos,
        player_yaw,
        camera_.yaw(),
        camera_.pitch(),
        &physics_world_,
        &animator_runtime_,
        snap);
    if (extensions_.on_save_project_state != nullptr) {
        extensions_.on_save_project_state(
            extensions_.user_data, *this, snap.custom_project_json);
    }
    return save_game_state_file(path, snap, error);
}

bool GameplaySession::load_game_state(
    const std::filesystem::path& path,
    const SceneDocument& scene,
    const GameplayRendererBridge& renderer_bridge,
    FlyCamera& output_camera,
    std::string& error) {
    return load_game_state_headless(
        path, scene, output_camera, error, renderer_bridge.transform_sink);
}

bool GameplaySession::load_game_state_headless(
    const std::filesystem::path& path,
    const SceneDocument& scene,
    FlyCamera& output_camera,
    std::string& error,
    const RuntimeTransformSink& transform_sink) {
    if (!active_) {
        error = "Gameplay session is not active.";
        return false;
    }
    GameStateSnapshot snap{};
    if (!load_game_state_file(path, snap, error)) {
        return false;
    }
    character_.reset(snap.player_position, snap.player_facing_yaw);
    animated_player_.teleport(snap.player_position, snap.player_facing_yaw);
    camera_.set_angles(snap.camera_yaw, snap.camera_pitch);
    gameplay_world_.restore_snapshot(
        snap, scene, &physics_world_, &animator_runtime_, transform_sink);

    if (extensions_.on_load_project_state != nullptr) {
        extensions_.on_load_project_state(
            extensions_.user_data, *this, snap.custom_project_json);
    }

    if (player_node_) {
        const glm::mat4 player_mat = owns_player_cube_
            ? player_cube_matrix(
                  character_.position(), character_.facing_yaw())
            : (player_setup_.use_animated_character
                   ? animated_player_.model_matrix()
                   : trs_matrix(
                         character_.position(),
                         character_.facing_yaw(),
                         player_initial_scale_));
        if (transform_registry_.has_node(player_node_)) {
            if (!transform_registry_.is_attached(player_node_)) {
                static_cast<void>(transform_registry_.set_world_matrix(
                    player_node_, player_mat));
            }
            RuntimeTransformSink mutable_sink = transform_sink;
            transform_registry_.sync_outputs(
                &collision_world_,
                mutable_sink.apply != nullptr ? &mutable_sink : nullptr);
        } else if (transform_sink.apply != nullptr) {
            static_cast<void>(transform_sink.apply(
                transform_sink.context,
                player_node_,
                mat4_to_rows(player_mat)));
        }
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

void GameplaySession::stop(
    AudioEngine* const audio_engine,
    const GameplayRendererBridge* const renderer_bridge) noexcept {
    if (active_ && extensions_.on_stop != nullptr) {
        extensions_.on_stop(extensions_.user_data, *this);
    }
    if (audio_engine != nullptr && audio_engine->initialized()) {
        if (ui_audio_paused_) {
            audio_engine->set_gameplay_paused(false);
        }
        audio_engine->stop_gameplay_audio(0.03F);
    }
    ui_audio_paused_ = false;
    game_ui_.reset_session_state();
    const SkeletalAssetProvider skeletal_provider =
        renderer_bridge != nullptr
            ? renderer_bridge->skeletal_provider
            : SkeletalAssetProvider{};
    animator_runtime_.clear(
        skeletal_provider.remove_pose != nullptr ? &skeletal_provider : nullptr);
    if (renderer_bridge != nullptr) {
        if (renderer_bridge->clear_skinned_poses != nullptr) {
            renderer_bridge->clear_skinned_poses(renderer_bridge->context);
        }
        if (renderer_bridge->submit_overlay_vertices != nullptr) {
            renderer_bridge->submit_overlay_vertices(renderer_bridge->context, {});
        }
    }
    gameplay_world_.clear();
    transform_registry_.clear();
    overlay_canvas_.clear();
    runtime_audio_nodes_.clear();
    has_prev_listener_position_ = false;
    collision_world_.clear();
    physics_world_.clear();
    trigger_tracker_.reset();
    player_node_ = {};
    owns_player_cube_ = false;
    player_initial_scale_ = glm::vec3(1.0F);
    cached_content_revision_ = 0U;
    cached_structure_revision_ = 0U;
    default_camera_look_enabled_ = true;
    active_ = false;
}

void GameplaySession::query_project_debug_state(
    std::string& out_json_object) const {
    out_json_object.clear();
    if (active_ && extensions_.on_query_debug_state != nullptr) {
        extensions_.on_query_debug_state(
            extensions_.user_data, *this, out_json_object);
    }
}

} // namespace vcr
