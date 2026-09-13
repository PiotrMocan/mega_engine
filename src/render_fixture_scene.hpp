#pragma once
#include "render_fixture.hpp"
#include "scene_document.hpp"

namespace vcr {
// Startup-only procedural scene. Uses the ordinary document and upload path.
[[nodiscard]] SceneDocument make_cascade_cache_fixture(float camera_z);
[[nodiscard]] SceneDocument make_overdraw_fixture(float camera_z, unsigned layers);
[[nodiscard]] SceneDocument make_light_swarm_fixture(
    float camera_z,
    std::uint32_t light_count,
    LightSwarmLayout layout);
[[nodiscard]] SceneDocument make_aa_wires_fixture(float camera_z);
[[nodiscard]] SceneDocument make_motion_bars_fixture(float camera_z);
}
