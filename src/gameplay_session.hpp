#pragma once

#include "collision.hpp"
#include "gameplay.hpp"
#include "scene_document.hpp"

#include <cstdint>
#include <span>
#include <string>

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
        const Renderer* renderer = nullptr);
    [[nodiscard]] bool update(
        const GameplayFrameInput& input,
        float delta_seconds,
        const SceneDocument& scene,
        Renderer& renderer,
        FlyCamera& output_camera) noexcept;
    void stop() noexcept;

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
    [[nodiscard]] std::span<const TriggerOverlap> active_triggers()
        const noexcept {
        return trigger_tracker_.active_overlaps();
    }
    [[nodiscard]] std::span<const TriggerEvent> trigger_events()
        const noexcept {
        return trigger_tracker_.events();
    }

private:
    CharacterController character_{};
    ThirdPersonCamera camera_{};
    CollisionWorld collision_world_{};
    TriggerOverlapTracker trigger_tracker_{};
    SceneNodeId player_node_{};
    std::uint64_t cached_content_revision_ = 0U;
    std::uint64_t cached_structure_revision_ = 0U;
    bool active_ = false;
};

} // namespace vcr
