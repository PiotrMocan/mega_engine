#pragma once

#include "gameplay.hpp"
#include "scene_document.hpp"

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
        std::string& error);
    [[nodiscard]] bool update(
        const GameplayFrameInput& input,
        float delta_seconds,
        Renderer& renderer,
        FlyCamera& output_camera) noexcept;
    void stop() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] SceneNodeId player_node() const noexcept {
        return player_node_;
    }

private:
    CharacterController character_{};
    ThirdPersonCamera camera_{};
    SceneNodeId player_node_{};
    bool active_ = false;
};

} // namespace vcr
