#pragma once

#include "audio_engine.hpp"
#include "collision.hpp"
#include "gameplay.hpp"
#include "input_actions.hpp"
#include "physics_world.hpp"
#include "scene_document.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace vcr {

class FlyCamera;
class Renderer;

struct GameplayFrameInput {
    CharacterInput character{};
    float mouse_delta_x = 0.0F;
    float mouse_delta_y = 0.0F;
};

class GameplaySession final {
public:
    [[nodiscard]] bool start(
        SceneDocument& scene,
        const SceneRenderView& initial_view,
        const FlyCamera& startup_camera,
        FlyCamera& output_camera,
        std::string& error,
        const Renderer* renderer = nullptr,
        AudioEngine* audio_engine = nullptr);
    [[nodiscard]] bool update(
        const GameplayFrameInput& input,
        float delta_seconds,
        const SceneDocument& scene,
        Renderer& renderer,
        FlyCamera& output_camera,
        AudioEngine* audio_engine = nullptr,
        InputActionSystem* input_actions = nullptr) noexcept;
    void stop(AudioEngine* audio_engine = nullptr) noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] SceneNodeId player_node() const noexcept {
        return player_node_;
    }
    [[nodiscard]] const CharacterController& character() const noexcept {
        return character_;
    }
    [[nodiscard]] const CollisionWorld& collision_world() const noexcept {
        return collision_world_;
    }
    [[nodiscard]] const PhysicsWorld& physics_world() const noexcept {
        return physics_world_;
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

    void sync_scene_audio(
        const SceneDocument& scene,
        const FlyCamera& output_camera,
        float delta_seconds,
        bool initial_frame,
        AudioEngine& audio_engine) noexcept;

    CharacterController character_{};
    ThirdPersonCamera camera_{};
    CollisionWorld collision_world_{};
    PhysicsWorld physics_world_{};
    TriggerOverlapTracker trigger_tracker_{};
    std::vector<SceneWorldNode> cached_world_nodes_{};
    std::vector<std::uint32_t> world_ancestor_scratch_{};
    std::vector<RuntimeAudioNode> runtime_audio_nodes_{};
    std::vector<std::pair<glm::vec3, AudioReverbZoneComponent>> reverb_scratch_{};
    glm::vec3 prev_listener_position_{0.0F};
    bool has_prev_listener_position_ = false;
    SceneNodeId player_node_{};
    std::uint64_t cached_content_revision_ = 0U;
    std::uint64_t cached_structure_revision_ = 0U;
    bool active_ = false;
};

} // namespace vcr
