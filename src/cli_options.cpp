#include "cli_options.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace vcr {
namespace {

struct Argument {
    std::string_view name;
    std::string_view inline_value;
    bool has_inline_value = false;
};

[[nodiscard]] Argument split_argument(std::string_view text) noexcept {
    const auto equals = text.find('=');
    if (equals == std::string_view::npos) {
        return {text, {}, false};
    }
    return {text.substr(0, equals), text.substr(equals + 1), true};
}

[[nodiscard]] bool parse_u32(
    std::string_view text, std::uint32_t& value) noexcept {
    if (text.empty()) {
        return false;
    }

    std::uint64_t parsed = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [end, error] = std::from_chars(first, last, parsed, 10);
    if (error != std::errc{} || end != last ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    value = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] bool parse_nonnegative_double(
    std::string_view text, double& value) noexcept {
    if (text.empty()) {
        return false;
    }

    double parsed = 0.0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [end, error] =
        std::from_chars(first, last, parsed, std::chars_format::general);
    if (error != std::errc{} || end != last || !std::isfinite(parsed) ||
        parsed < 0.0) {
        return false;
    }

    value = parsed;
    return true;
}

[[nodiscard]] std::string missing_value(std::string_view name) {
    std::string result{"missing value for "};
    result.append(name);
    return result;
}

[[nodiscard]] std::string invalid_value(
    std::string_view name, std::string_view value) {
    std::string result{"invalid value for "};
    result.append(name);
    result.append(": '");
    result.append(value);
    result.push_back('\'');
    return result;
}

[[nodiscard]] bool take_value(
    const Argument& argument,
    int& index,
    int argc,
    const char* const* argv,
    std::string_view& value,
    std::string& error) {
    if (argument.has_inline_value) {
        if (argument.inline_value.empty()) {
            error = missing_value(argument.name);
            return false;
        }
        value = argument.inline_value;
        return true;
    }

    if (index + 1 >= argc || argv[index + 1] == nullptr) {
        error = missing_value(argument.name);
        return false;
    }

    value = argv[++index];
    return true;
}

[[nodiscard]] bool parse_mode(
    std::string_view value, RunMode& mode) noexcept {
    if (value == "scene" || value == "minimal") {
        mode = RunMode::scene;
        return true;
    }
    if (value == "instancing" || value == "instances") {
        mode = RunMode::instancing;
        return true;
    }
    if (value == "gpu-saturation" || value == "gpu_saturation" ||
        value == "gpu") {
        mode = RunMode::gpu_saturation;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_present(
    std::string_view value, PresentPreference& preference) noexcept {
    if (value == "auto" || value == "automatic") {
        preference = PresentPreference::automatic;
        return true;
    }
    if (value == "immediate") {
        preference = PresentPreference::immediate;
        return true;
    }
    if (value == "mailbox") {
        preference = PresentPreference::mailbox;
        return true;
    }
    if (value == "fifo" || value == "vsync") {
        preference = PresentPreference::fifo;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_anti_aliasing(
    std::string_view value, AntiAliasingMethod& method) noexcept {
    if (value == "off" || value == "none") {
        method = AntiAliasingMethod::off;
        return true;
    }
    if (value == "smaa") {
        method = AntiAliasingMethod::smaa;
        return true;
    }
    if (value == "fsr3" || value == "fsr") {
        method = AntiAliasingMethod::fsr3;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_smaa_quality(
    std::string_view value, SmaaQuality& quality) noexcept {
    if (value == "low") {
        quality = SmaaQuality::low;
        return true;
    }
    if (value == "medium") {
        quality = SmaaQuality::medium;
        return true;
    }
    if (value == "high") {
        quality = SmaaQuality::high;
        return true;
    }
    if (value == "ultra") {
        quality = SmaaQuality::ultra;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_fsr3_quality(
    std::string_view value, Fsr3Quality& quality) noexcept {
    if (value == "native" || value == "native-aa" ||
        value == "native_aa") {
        quality = Fsr3Quality::native_aa;
        return true;
    }
    if (value == "quality") {
        quality = Fsr3Quality::quality;
        return true;
    }
    if (value == "balanced") {
        quality = Fsr3Quality::balanced;
        return true;
    }
    if (value == "performance") {
        quality = Fsr3Quality::performance;
        return true;
    }
    if (value == "ultra-performance" ||
        value == "ultra_performance") {
        quality = Fsr3Quality::ultra_performance;
        return true;
    }
    return false;
}

} // namespace

CliParseResult parse_cli_options(int argc, const char* const* argv) {
    CliParseResult result{};

    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        result.error = "invalid argc/argv";
        return result;
    }

    bool instances_was_explicit = false;
    bool workload_was_explicit = false;
    bool mode_was_explicit = false;

    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            result.error = "null command-line argument";
            return result;
        }

        const std::string_view raw = argv[index];
        const Argument argument = split_argument(raw);

        if (argument.name == "--help" || argument.name == "-h") {
            if (argument.has_inline_value) {
                result.error = invalid_value(argument.name, argument.inline_value);
                return result;
            }
            result.options.show_help = true;
            continue;
        }
        if (argument.name == "--benchmark") {
            if (argument.has_inline_value) {
                result.error = invalid_value(argument.name, argument.inline_value);
                return result;
            }
            result.options.benchmark = true;
            continue;
        }
        if (argument.name == "--mcp-stdio") {
            if (argument.has_inline_value) {
                result.error = invalid_value(argument.name, argument.inline_value);
                return result;
            }
            result.options.mcp_stdio = true;
            continue;
        }
        if (argument.name == "--validation") {
            if (argument.has_inline_value) {
                result.error = invalid_value(argument.name, argument.inline_value);
                return result;
            }
            result.options.validation = true;
            continue;
        }
        if (argument.name == "--no-validation") {
            if (argument.has_inline_value) {
                result.error = invalid_value(argument.name, argument.inline_value);
                return result;
            }
            result.options.validation = false;
            continue;
        }

        if (argument.name == "--lighting-room") {
            if (argument.has_inline_value) {
                result.error = invalid_value(argument.name, argument.inline_value);
                return result;
            }
            result.options.lighting_room = true;
            continue;
        }
        if (argument.name == "--animate-scene" || argument.name == "--animate-camera") {
            if (argument.has_inline_value) {
                result.error = invalid_value(argument.name, argument.inline_value);
                return result;
            }
            if (argument.name == "--animate-scene") {
                result.options.animate_scene = true;
            } else {
                result.options.animate_camera = true;
            }
            continue;
        }
        std::string_view value;
        if (argument.name == "--lighting" || argument.name == "--gi" ||
            argument.name == "--day-cycle" || argument.name == "--time-of-day" ||
            argument.name == "--day-length" || argument.name == "--gi-extent" ||
            argument.name == "--gi-strength" || argument.name == "--gi-update-hz" ||
            argument.name == "--gi-triangle-budget") {
            if (!take_value(argument, index, argc, argv, value, result.error))
                return result;
            auto &s = result.options.lighting;
            bool valid = true;
            double number = 0;
            if (argument.name == "--lighting") {
                valid = value == "dynamic" || value == "legacy";
                s.dynamic = value == "dynamic";
                if (!s.dynamic) {
                    s.voxel_gi = false;
                    s.cycle = false;
                }
            } else if (argument.name == "--gi") {
                valid = value == "voxel" || value == "off";
                s.voxel_gi = value == "voxel";
            } else if (argument.name == "--day-cycle") {
                valid = value == "on" || value == "off";
                s.cycle = value == "on";
            } else {
                valid = parse_nonnegative_double(value, number);
                if (argument.name == "--time-of-day") {
                    valid = valid && number < 24;
                    s.hour = number;
                } else if (argument.name == "--day-length") {
                    valid = valid && number >= 1 && number <= 864000;
                    s.day_seconds = number;
                } else if (argument.name == "--gi-extent") {
                    valid = valid && number >= 16 && number <= 256;
                    s.gi_extent = static_cast<float>(number);
                } else if (argument.name == "--gi-strength") {
                    valid = valid && number <= 4;
                    s.gi_strength = static_cast<float>(number);
                } else if (argument.name == "--gi-update-hz") {
                    valid = valid && number >= 1 && number <= 120;
                    s.gi_update_hz = static_cast<float>(number);
                } else {
                    valid =
                        valid && number >= 1024 && number <= 262144 && std::floor(number) == number;
                    s.triangle_budget = valid ? static_cast<std::uint32_t>(number) : 65536;
                }
            }
            if (!valid) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--scene-scale") {
            if (!take_value(argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_nonnegative_double(value, result.options.scene_scale) ||
                result.options.scene_scale < 0.001 || result.options.scene_scale > 100) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--camera-z") {
            if (!take_value(argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_nonnegative_double(value, result.options.camera_z) ||
                result.options.camera_z > 400) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--culling" || argument.name == "--lod" ||
            argument.name == "--dynamic-shadows" || argument.name == "--scene-count" ||
            argument.name == "--scene-asset" || argument.name == "--stream-asset" ||
            argument.name == "--visibility-backend" || argument.name == "--scene-walls") {
            if (!take_value(argument, index, argc, argv, value, result.error)) {
                return result;
            }
            bool valid = true;
            if (argument.name == "--visibility-backend") {
                valid = value == "auto" || value == "cpu" || value == "gpu";
                result.options.visibility_backend = value;
            } else if (argument.name == "--culling") {
                valid = value == "none" || value == "frustum" || value == "occlusion";
                result.options.frustum_culling = value != "none";
                result.options.occlusion_culling = value == "occlusion";
            } else if (argument.name == "--scene-count") {
                valid = parse_u32(value, result.options.scene_count) &&
                        result.options.scene_count > 0 && result.options.scene_count <= 100000;
            } else if (argument.name == "--scene-asset" || argument.name == "--stream-asset") {
                valid = !value.empty() && value.size() <= 4096;
                if (argument.name == "--scene-asset") {
                    result.options.scene_asset = value;
                } else {
                    result.options.stream_asset = value;
                }
            } else {
                valid = value == "on" || value == "off";
                if (argument.name == "--lod") {
                    result.options.lod = value == "on";
                } else if (argument.name == "--scene-walls") {
                    result.options.scene_walls = value == "on";
                } else {
                    result.options.dynamic_shadows = value == "on";
                }
            }
            if (!valid) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--width") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_u32(value, result.options.width) ||
                result.options.width == 0) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--height") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_u32(value, result.options.height) ||
                result.options.height == 0) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--instances") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_u32(value, result.options.instances) ||
                result.options.instances == 0) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            instances_was_explicit = true;
            continue;
        }
        if (argument.name == "--gpu-workload") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_u32(value, result.options.gpu_workload_iterations)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            workload_was_explicit = true;
            continue;
        }
        if (argument.name == "--duration") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_nonnegative_double(
                    value, result.options.benchmark_duration_seconds) ||
                result.options.benchmark_duration_seconds == 0.0) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--warmup") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_nonnegative_double(
                    value, result.options.benchmark_warmup_seconds)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--present") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_present(value, result.options.present)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--mode") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_mode(value, result.options.initial_mode)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            mode_was_explicit = true;
            continue;
        }
        if (argument.name == "--aa") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_anti_aliasing(
                    value, result.options.anti_aliasing.method)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--smaa-quality") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_smaa_quality(
                    value, result.options.anti_aliasing.smaa_quality)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--fsr-quality") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_fsr3_quality(
                    value, result.options.anti_aliasing.fsr3_quality)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }

        result.error = "unknown command-line option: ";
        result.error.append(raw);
        return result;
    }

    // Explicit workload knobs are convenient mode selectors, while --mode
    // always wins when the user supplied it.
    if (!result.options.scene_asset.empty() && result.options.scene_count == 0) {
        result.error = "--scene-asset requires --scene-count";
        return result;
    }
    if (!result.options.stream_asset.empty() && result.options.scene_count >= 100000) {
        result.error = "--stream-asset requires one free scene node";
        return result;
    }
    if (!mode_was_explicit) {
        if (workload_was_explicit &&
            result.options.gpu_workload_iterations > 0) {
            result.options.initial_mode = RunMode::gpu_saturation;
        } else if (instances_was_explicit && result.options.instances > 1) {
            result.options.initial_mode = RunMode::instancing;
        }
    }

    return result;
}

std::string cli_usage(std::string_view executable_name) {
    std::string result{"Usage: "};
    result.append(
        executable_name.empty() ? std::string_view{"vulkan_cube"}
                                : executable_name);
    result.append(" [options]\n"
                  "  --benchmark             Run timed benchmark and write benchmark.json\n"
                  "  --mcp-stdio             Expose AI toolsets over MCP stdio\n"
                  "  --width N               Initial drawable width (default: 1920)\n"
                  "  --height N              Initial drawable height (default: 1080)\n"
                  "  --instances N           Cube instance count (default: 1)\n"
                  "  --gpu-workload N        GPU workload iterations (default: 0)\n"
                  "  --culling MODE          none, frustum, occlusion (default: occlusion)\n"
                  "  --visibility-backend MODE  auto, cpu, gpu (default: auto)\n"
                  "  --lod on|off            Screen-error mesh/terrain LOD (default: on)\n"
                  "  --dynamic-shadows on|off  Authored casters and invalidation (default: on)\n"
                  "  --animate-scene         Animate one runtime object (game executable)\n"
                  "  --animate-camera        Repeatable moving-camera benchmark path\n"
                  "  --camera-z N            Initial camera Z, 0..400 (default: 12)\n"
                  "  --scene-scale N         Stress object scale, .001..100 (default: 1)\n"
                  "  --scene-count N         Generated stress scene, 1..100000 objects\n"
                  "  --scene-walls on|off    Include large occluders in the stress scene\n"
                  "  --scene-asset PATH      Repeat this cooked model in the stress scene\n"
                  "  --stream-asset PATH     Load/place a model while rendering\n"
                  "  --present MODE          auto (uncapped), immediate, mailbox, or fifo\n"
                  "  --mode MODE             scene, instancing,\n"
                  "                          or gpu-saturation\n"
                  "  --aa MODE               off, smaa, or fsr3 (default: off)\n"
                  "  --smaa-quality MODE     low, medium, high, or ultra\n"
                  "  --fsr-quality MODE      native-aa, quality, balanced, performance,\n"
                  "                          or ultra-performance\n"
                  "  --duration SECONDS      Measurement duration (default: 15)\n"
                  "  --warmup SECONDS        Warmup duration (default: 5)\n"
                  "  --validation            Enable Vulkan validation\n"
                  "  --no-validation         Disable Vulkan validation\n"
                  "  -h, --help              Show this help\n"
                  "\n"
                  "Controls:\n"
                  "  Mouse                   Look around (left click captures)\n"
                  "  W/A/S/D                 Fly forward/left/back/right\n"
                  "  Shift                   Move four times faster\n"
                  "  Space or E              Fly up\n"
                  "  Ctrl or Q               Fly down\n"
                  "  Esc                     Release mouse, then exit\n"
                  "  1/2/3                   Scene/instancing/GPU workload mode\n"
                  "  Up/Down                 Adjust active benchmark load\n"
                  "  --lighting dynamic|legacy  Lighting path (dynamic by default)\n"
                  "  --gi voxel|off             Dynamic diffuse GI (voxel by default)\n"
                  "  --time-of-day 0..24         Initial hour (14)\n"
                  "  --day-cycle on|off          Advance time (on)\n"
                  "  --day-length SECONDS        Full day duration (600)\n"
                  "  --gi-extent 16..256         Volume width in world units (64)\n"
                  "  --gi-update-hz 1..120       Motion GI/relight frequency (30)\n"
                  "  --gi-triangle-budget N      Geometry work per frame (65536)\n"
                  "  F6                      Cycle off/SMAA/FSR 3 anti-aliasing\n"
                  "  F7/F8                   Decrease/increase AA quality\n"
                  "  V/F/R/P                 VSync/fullscreen/reset/print stats\n");
    return result;
}

std::string_view to_string(RunMode mode) noexcept {
    switch (mode) {
    case RunMode::scene:
        return "scene";
    case RunMode::instancing:
        return "instancing";
    case RunMode::gpu_saturation:
        return "gpu-saturation";
    }
    return "unknown";
}

std::string_view to_string(PresentPreference preference) noexcept {
    switch (preference) {
    case PresentPreference::automatic:
        return "auto";
    case PresentPreference::immediate:
        return "immediate";
    case PresentPreference::mailbox:
        return "mailbox";
    case PresentPreference::fifo:
        return "fifo";
    }
    return "unknown";
}

PresentPreferenceOrder present_preference_order(
    PresentPreference requested, [[maybe_unused]] bool benchmark) noexcept {
    using P = PresentPreference;
    switch (requested) {
    case P::automatic:
        return {{{P::immediate, P::mailbox, P::fifo}}, 3};
    case P::immediate:
        return {{{P::immediate, P::mailbox, P::fifo}}, 3};
    case P::mailbox:
        return {{{P::mailbox, P::fifo, P::immediate}}, 3};
    case P::fifo:
        return {{{P::fifo, P::mailbox, P::immediate}}, 3};
    }
    return {{{P::fifo, P::mailbox, P::immediate}}, 3};
}

} // namespace vcr
