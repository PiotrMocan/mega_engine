#pragma once

#include "animated_character.hpp"
#include "audio_engine.hpp"
#include "collision.hpp"
#include "game_overlay.hpp"
#include "gameplay.hpp"
#include "gameplay_framework.hpp"
#include "input_actions.hpp"
#include "physics_world.hpp"
#include "runtime_transform_registry.hpp"
#include "scene_document.hpp"
#include "vehicle_camera.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vcr {

class FlyCamera;
class GameplaySession;

struct GameplayRendererBridge {
    void* context = nullptr;
    ModelBoundsProvider bounds_provider{};
    ModelCollisionProvider collision_provider{};
    RuntimeTransformSink transform_sink{};
    SkeletalAssetProvider skeletal_provider{};
    glm::uvec2 (*query_output_extent)(const void* context) noexcept = nullptr;
    void (*submit_overlay_vertices)(
        void* context,
        std::span<const GameUiVertex> vertices) = nullptr;
    void (*clear_skinned_poses)(void* context) noexcept = nullptr;
};

struct GameplayFrameInput {
    CharacterInput character{};
    float mouse_delta_x = 0.0F;
    float mouse_delta_y = 0.0F;
};

struct GameplayPlayerSetup {
    SceneNodeId custom_player_node{};
    bool disable_default_player_cube = false;
    bool use_animated_character = false;
    AnimatedCharacterConfig animated_character_config{};
    std::optional<glm::vec3> spawn_position{};
    std::optional<float> spawn_yaw_radians{};
    InputContext initial_input_context = InputContext::gameplay;
};

struct GameplayUpdateContext {
    GameplayFrameInput input{};
    const SceneDocument& scene;
    SkeletalAssetProvider skeletal_provider{};
    const GameplayRendererBridge* renderer_bridge = nullptr;
    AudioEngine* audio_engine = nullptr;
    InputActionSystem* input_actions = nullptr;
    float delta_seconds = 0.0F;
    glm::vec3 camera_override_position{0.0F};
    float camera_override_yaw_radians = -1.57079632679F;
    float camera_override_pitch_radians = -0.20943951023F;
    bool has_camera_override = false;

    void set_camera_pose(
        const glm::vec3& position,
        const glm::vec3& forward) noexcept;
    void set_camera_angles(
        const glm::vec3& position,
        float yaw_radians,
        float pitch_radians) noexcept;
};

struct GameplayExtensionCallbacks {
    void* user_data = nullptr;
    bool (*on_start)(
        void* user_data,
        GameplaySession& session,
        SceneDocument& scene,
        std::string& out_error) = nullptr;
    void (*on_stop)(
        void* user_data,
        GameplaySession& session) noexcept = nullptr;
    void (*on_input_ai)(
        void* user_data,
        GameplaySession& session,
        GameplayUpdateContext& ctx) noexcept = nullptr;
    void (*on_movement_physics)(
        void* user_data,
        GameplaySession& session,
        GameplayUpdateContext& ctx) noexcept = nullptr;
    void (*on_post_transforms)(
        void* user_data,
        GameplaySession& session,
        GameplayUpdateContext& ctx) noexcept = nullptr;
    void (*on_post_animation)(
        void* user_data,
        GameplaySession& session,
        GameplayUpdateContext& ctx) noexcept = nullptr;
    void (*on_camera_audio_overlay)(
        void* user_data,
        GameplaySession& session,
        GameplayUpdateContext& ctx) noexcept = nullptr;
    void (*on_save_project_state)(
        void* user_data,
        const GameplaySession& session,
        std::string& out_json) = nullptr;
    void (*on_load_project_state)(
        void* user_data,
        GameplaySession& session,
        std::string_view json) = nullptr;
    void (*on_query_debug_state)(
        void* user_data,
        const GameplaySession& session,
        std::string& out_json_object) = nullptr;
};

class GameplaySession final {
public:
    void set_player_setup(const GameplayPlayerSetup& setup);
    [[nodiscard]] const GameplayPlayerSetup& player_setup() const noexcept {
        return player_setup_;
    }

    void set_extension_callbacks(
        const GameplayExtensionCallbacks& callbacks) noexcept;
    [[nodiscard]] const GameplayExtensionCallbacks& extension_callbacks()
        const noexcept {
        return extensions_;
    }

    [[nodiscard]] bool start(
        SceneDocument& scene,
        const SceneRenderView& initial_view,
        const FlyCamera& startup_camera,
        FlyCamera& output_camera,
        std::string& error,
        const GameplayRendererBridge* renderer_bridge = nullptr,
        AudioEngine* audio_engine = nullptr,
        InputActionSystem* input_actions = nullptr);

    [[nodiscard]] bool update(
        const GameplayFrameInput& input,
        float delta_seconds,
        const SceneDocument& scene,
        const GameplayRendererBridge& renderer_bridge,
        FlyCamera& output_camera,
        AudioEngine* audio_engine = nullptr,
        InputActionSystem* input_actions = nullptr) noexcept;

    [[nodiscard]] bool update_headless(
        const GameplayFrameInput& input,
        float delta_seconds,
        const SceneDocument& scene,
        FlyCamera& output_camera,
        const RuntimeTransformSink& transform_sink = {},
        const SkeletalAssetProvider& skeletal_provider = {},
        glm::uvec2 viewport_extent = {1280U, 720U},
        AudioEngine* audio_engine = nullptr,
        InputActionSystem* input_actions = nullptr) noexcept;

    void stop(
        AudioEngine* audio_engine = nullptr,
        const GameplayRendererBridge* renderer_bridge = nullptr) noexcept;

    void query_project_debug_state(std::string& out_json_object) const;

    [[nodiscard]] bool save_game_state(
        const std::filesystem::path& path,
        std::string& error) const;
    [[nodiscard]] bool load_game_state(
        const std::filesystem::path& path,
        const SceneDocument& scene,
        const GameplayRendererBridge& renderer_bridge,
        FlyCamera& output_camera,
        std::string& error);
    [[nodiscard]] bool load_game_state_headless(
        const std::filesystem::path& path,
        const SceneDocument& scene,
        FlyCamera& output_camera,
        std::string& error,
        const RuntimeTransformSink& transform_sink = {});

    [[nodiscard]] GameplayWorld& gameplay_world() noexcept {
        return gameplay_world_;
    }
    [[nodiscard]] const GameplayWorld& gameplay_world() const noexcept {
        return gameplay_world_;
    }
    [[nodiscard]] AnimatorRuntimeSystem& animator_runtime() noexcept {
        return animator_runtime_;
    }
    [[nodiscard]] const AnimatorRuntimeSystem& animator_runtime()
        const noexcept {
        return animator_runtime_;
    }
    [[nodiscard]] RuntimeTransformRegistry& transform_registry() noexcept {
        return transform_registry_;
    }
    [[nodiscard]] const RuntimeTransformRegistry& transform_registry()
        const noexcept {
        return transform_registry_;
    }
    [[nodiscard]] GameOverlayCanvas& overlay_canvas() noexcept {
        return overlay_canvas_;
    }
    [[nodiscard]] const GameOverlayCanvas& overlay_canvas() const noexcept {
        return overlay_canvas_;
    }
    [[nodiscard]] AnimatedCharacterController& animated_player() noexcept {
        return animated_player_;
    }
    [[nodiscard]] const AnimatedCharacterController& animated_player()
        const noexcept {
        return animated_player_;
    }
    [[nodiscard]] GameplayCameraRig& camera_rig() noexcept {
        return camera_rig_;
    }
    [[nodiscard]] const GameplayCameraRig& camera_rig() const noexcept {
        return camera_rig_;
    }
    [[nodiscard]] ThirdPersonCamera& third_person_camera() noexcept {
        return camera_;
    }
    [[nodiscard]] const ThirdPersonCamera& third_person_camera()
        const noexcept {
        return camera_;
    }
    [[nodiscard]] CollisionWorld& collision_world_mut() noexcept {
        return collision_world_;
    }
    [[nodiscard]] const CollisionWorld& collision_world() const noexcept {
        return collision_world_;
    }
    [[nodiscard]] PhysicsWorld& physics_world_mut() noexcept {
        return physics_world_;
    }
    [[nodiscard]] const PhysicsWorld& physics_world() const noexcept {
        return physics_world_;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] SceneNodeId player_node() const noexcept {
        return player_node_;
    }
    [[nodiscard]] const CharacterController& character() const noexcept {
        return character_;
    }
    [[nodiscard]] std::span<const TriggerOverlap> active_triggers()
        const noexcept {
        return trigger_tracker_.active_overlaps();
    }
    [[nodiscard]] std::span<const TriggerEvent> trigger_events()
        const noexcept {
        return trigger_tracker_.events();
    }

private:
    struct RuntimeAudioNode {
        SceneNodeId id{};
        AudioVoiceHandle voice{};
        glm::vec3 prev_position{0.0F};
        float smoothed_occlusion = 0.0F;
        bool has_prev_position = false;
        bool started_once = false;
    };

    [[nodiscard]] bool update_internal(
        const GameplayFrameInput& input,
        float delta_seconds,
        const SceneDocument& scene,
        const GameplayRendererBridge* renderer_bridge,
        FlyCamera& output_camera,
        const RuntimeTransformSink& transform_sink,
        const SkeletalAssetProvider& skeletal_provider,
        glm::uvec2 viewport_extent,
        AudioEngine* audio_engine,
        InputActionSystem* input_actions) noexcept;

    void sync_scene_audio(
        const SceneDocument& scene,
        const FlyCamera& output_camera,
        float delta_seconds,
        bool initial_frame,
        AudioEngine& audio_engine) noexcept;

    GameplayPlayerSetup player_setup_{};
    GameplayExtensionCallbacks extensions_{};

    CharacterController character_{};
    AnimatedCharacterController animated_player_{};
    ThirdPersonCamera camera_{};
    GameplayCameraRig camera_rig_{};
    CollisionWorld collision_world_{};
    PhysicsWorld physics_world_{};
    TriggerOverlapTracker trigger_tracker_{};
    GameplayWorld gameplay_world_{};
    AnimatorRuntimeSystem animator_runtime_{};
    RuntimeTransformRegistry transform_registry_{};
    GameOverlayCanvas overlay_canvas_{};
    AnimatorComponent player_anim_scratch_{};

    std::vector<SceneWorldNode> cached_world_nodes_{};
    std::vector<std::uint32_t> world_ancestor_scratch_{};
    std::vector<RuntimeAudioNode> runtime_audio_nodes_{};
    std::vector<std::pair<glm::vec3, AudioReverbZoneComponent>> reverb_scratch_{};
    glm::vec3 prev_listener_position_{0.0F};
    glm::vec3 player_initial_scale_{1.0F};
    bool has_prev_listener_position_ = false;
    SceneNodeId player_node_{};
    bool owns_player_cube_ = false;
    std::uint64_t cached_content_revision_ = 0U;
    std::uint64_t cached_structure_revision_ = 0U;
    bool active_ = false;
};

} // namespace vcr
