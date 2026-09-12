#pragma once

#include "anti_aliasing.hpp"
#include "lighting.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vcr {

enum class RunMode : std::uint8_t {
    scene,
    instancing,
    gpu_saturation,
};

// "automatic" is deliberately kept Vulkan-independent. The swapchain can map
// the ordered preferences returned by present_preference_order() to the modes
// actually exposed by the surface.
enum class PresentPreference : std::uint8_t {
    automatic,
    immediate,
    mailbox,
    fifo,
};

#if defined(VCR_VALIDATION_DEFAULT)
inline constexpr bool default_validation_enabled =
    VCR_VALIDATION_DEFAULT != 0;
#elif defined(NDEBUG)
inline constexpr bool default_validation_enabled = false;
#else
inline constexpr bool default_validation_enabled = true;
#endif

struct LightingOverrides {
    bool dynamic = false;
    bool voxel_gi = false;
    bool cycle = false;
    bool hour = false;
    bool day_seconds = false;
    bool gi_extent = false;
    bool gi_strength = false;
    bool triangle_budget = false;
    bool gi_update_hz = false;
    bool shadow_resolution = false;
};

struct CliOptions {
    static constexpr std::uint32_t default_width = 1920;
    static constexpr std::uint32_t default_height = 1080;
    static constexpr double default_benchmark_duration_seconds = 15.0;
    static constexpr double default_benchmark_warmup_seconds = 5.0;

    std::uint32_t width = default_width;
    std::uint32_t height = default_height;
    std::uint32_t instances = 1;
    std::uint32_t gpu_workload_iterations = 0;
    double benchmark_duration_seconds = default_benchmark_duration_seconds;
    double benchmark_warmup_seconds = default_benchmark_warmup_seconds;
    RunMode initial_mode = RunMode::scene;
    PresentPreference present = PresentPreference::automatic;
    AntiAliasingSettings anti_aliasing{};
    bool benchmark = false;
    bool mcp_stdio = false;
    bool validation = default_validation_enabled;
    bool show_help = false;
    bool frustum_culling = true;
    bool occlusion_culling = true;
    bool lod = true;
    bool dynamic_shadows = true;
    bool animate_scene = false;
    std::uint32_t scene_count = 0;
    std::string scene_asset{};
    std::string stream_asset{};
    bool animate_camera = false;
    std::string visibility_backend = "auto";
    double camera_z = 12.0;
    bool camera_z_explicit = false;
    double scene_scale = 1.0;
    bool scene_walls = true;
    bool lighting_room = false;
    LightingSettings lighting{};
    LightingOverrides lighting_overrides{};
};

[[nodiscard]] LightingSettings resolve_scene_lighting(
    const CliOptions &options, const std::optional<LightingSettings> &authored) noexcept;

struct CliParseResult {
    CliOptions options{};
    std::string error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

struct PresentPreferenceOrder {
    std::array<PresentPreference, 3> values{};
    std::size_t size = 0;

    [[nodiscard]] constexpr auto begin() const noexcept {
        return values.begin();
    }

    [[nodiscard]] constexpr auto end() const noexcept {
        return values.begin() + static_cast<std::ptrdiff_t>(size);
    }
};

// Parsing happens once before renderer initialization. Both "--flag value" and
// "--flag=value" spellings are supported.
[[nodiscard]] CliParseResult parse_cli_options(
    int argc, const char* const* argv);

[[nodiscard]] inline CliParseResult parse_cli(
    int argc, const char* const* argv) {
    return parse_cli_options(argc, argv);
}

[[nodiscard]] std::string cli_usage(std::string_view executable_name);
[[nodiscard]] std::string_view to_string(RunMode mode) noexcept;
[[nodiscard]] std::string_view to_string(PresentPreference preference) noexcept;

// Auto-selection is uncapped whenever the surface supports it:
// IMMEDIATE -> MAILBOX -> FIFO. An explicit mode keeps its own preference
// order and falls back to a mode supported by the surface.
[[nodiscard]] PresentPreferenceOrder present_preference_order(
    PresentPreference requested, bool benchmark) noexcept;

} // namespace vcr
