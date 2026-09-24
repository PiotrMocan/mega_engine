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

[[nodiscard]] bool parse_finite_double(
    const std::string_view text,
    double& value) noexcept {
    if (text.empty()) return false;
    double parsed = 0.0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [end, error] =
        std::from_chars(first, last, parsed, std::chars_format::general);
    if (error != std::errc{} || end != last || !std::isfinite(parsed)) return false;
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
    if (value == "msaa2x") {
        method = AntiAliasingMethod::msaa2x;
        return true;
    }
    if (value == "msaa4x") {
        method = AntiAliasingMethod::msaa4x;
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

[[nodiscard]] bool parse_smaa_mode(
    std::string_view value, SmaaMode& mode) noexcept {
    if (value == "1x") {
        mode = SmaaMode::x1;
        return true;
    }
    if (value == "t2x") {
        mode = SmaaMode::t2x;
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
    bool local_light_count_was_explicit = false;
    bool light_swarm_layout_was_explicit = false;
    bool motion_fixture_fps_was_explicit = false;

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
        if (argument.name == "--cpu-culling") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            if (value != "linear" && value != "hierarchy") {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.cpu_hierarchy = value == "hierarchy";
            continue;
        }
        if (argument.name == "--front-to-back") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            if (value != "off" && value != "coarse") {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.coarse_front_to_back = value == "coarse";
            continue;
        }
        if (argument.name == "--depth-prepass") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto prepass = parse_depth_prepass(value);
            if (!prepass) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.depth_prepass = *prepass;
            continue;
        }
        if (argument.name == "--lod-fade") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto pattern = parse_lod_fade_pattern(value);
            if (!pattern) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.lod_fade = *pattern;
            continue;
        }
        if (argument.name == "--lod-fade-seconds") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            double seconds = 0.0;
            if (!parse_nonnegative_double(value, seconds) || seconds < 0.05 || seconds > 2.0) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.lod_fade_seconds = static_cast<float>(seconds);
            continue;
        }
        if (argument.name == "--local-lighting") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto backend = parse_local_light_backend(value);
            if (!backend) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.local_light_backend = *backend;
            continue;
        }
        if (argument.name == "--local-light-count") {
            local_light_count_was_explicit = true;
            if (!take_value(argument, index, argc, argv, value, result.error) ||
                !parse_u32(value, result.options.local_light_count) ||
                result.options.local_light_count > maximum_local_lights) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--light-swarm-layout") {
            light_swarm_layout_was_explicit = true;
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto layout = parse_light_swarm_layout(value);
            if (!layout) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.light_swarm_layout = *layout;
            continue;
        }
        if (argument.name == "--render-fixture") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto fixture = parse_render_fixture(value);
            if (!fixture) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.render_fixture = *fixture;
            continue;
        }
        if (argument.name == "--motion-fixture-fps") {
            motion_fixture_fps_was_explicit = true;
            if (!take_value(argument, index, argc, argv, value, result.error) ||
                !parse_u32(value, result.options.motion_fixture_fps) ||
                (result.options.motion_fixture_fps != 30U &&
                 result.options.motion_fixture_fps != 60U &&
                 result.options.motion_fixture_fps != 144U)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            continue;
        }
        if (argument.name == "--shadow-depth") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto depth = parse_shadow_depth_format(value);
            if (!depth) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.lighting.shadow_depth = *depth;
            result.options.lighting_overrides.shadow_depth = true;
            continue;
        }
        if (argument.name == "--shadow-cache") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            if (value != "full" && value != "cascade") {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.lighting.cascade_cache = value == "cascade";
            result.options.lighting_overrides.cascade_cache = true;
            continue;
        }
        if (argument.name == "--tone-mapper") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto mapper = parse_tone_mapper(value);
            if (!mapper) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.tone_mapper = *mapper;
            continue;
        }
        if (argument.name == "--fog") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            if (value != "off" && value != "height") {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.fog.enabled = value == "height";
            continue;
        }
        if (argument.name == "--fog-density" || argument.name == "--fog-height-falloff" ||
            argument.name == "--fog-base-height" || argument.name == "--fog-start" ||
            argument.name == "--fog-distance" || argument.name == "--fog-max-opacity" ||
            argument.name == "--fog-color-r" || argument.name == "--fog-color-g" ||
            argument.name == "--fog-color-b") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            double number = 0.0;
            if (!parse_finite_double(value, number) || std::abs(number) > 100'000.0) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            auto& fog = result.options.fog;
            const float scalar = static_cast<float>(number);
            if (argument.name == "--fog-density") fog.density = scalar;
            else if (argument.name == "--fog-height-falloff") fog.height_falloff = scalar;
            else if (argument.name == "--fog-base-height") fog.base_height = scalar;
            else if (argument.name == "--fog-start") fog.start_distance = scalar;
            else if (argument.name == "--fog-distance") fog.maximum_distance = scalar;
            else if (argument.name == "--fog-max-opacity") fog.maximum_opacity = scalar;
            else if (argument.name == "--fog-color-r") fog.color[0] = scalar;
            else if (argument.name == "--fog-color-g") fog.color[1] = scalar;
            else fog.color[2] = scalar;
            continue;
        }
        if (argument.name == "--subsurface") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto mode = parse_subsurface_mode(value);
            if (!mode) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.subsurface = *mode;
            continue;
        }
        if (argument.name == "--exposure") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto mode = parse_exposure_mode(value);
            if (!mode) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.exposure.mode = *mode;
            continue;
        }
        if (argument.name == "--exposure-metering") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto metering = parse_exposure_metering(value);
            if (!metering) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.exposure.metering = *metering;
            continue;
        }
        if (argument.name == "--grading") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto grading = parse_color_grading(value);
            if (!grading) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.color_grading = *grading;
            continue;
        }
        if (argument.name == "--grading-lut-size") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            const auto size = parse_color_grading_lut_size(value);
            if (!size) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.color_grading_lut_size = *size;
            continue;
        }
        if (argument.name == "--exposure-ev" ||
            argument.name == "--exposure-min-ev" ||
            argument.name == "--exposure-max-ev" ||
            argument.name == "--exposure-low-percent" ||
            argument.name == "--exposure-high-percent" ||
            argument.name == "--exposure-brighten-seconds" ||
            argument.name == "--exposure-darken-seconds") {
            if (!take_value(argument, index, argc, argv, value, result.error)) return result;
            double number = 0.0;
            if (!parse_finite_double(value, number)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            auto& exposure = result.options.exposure;
            if (argument.name == "--exposure-ev") exposure.compensation_ev = static_cast<float>(number);
            else if (argument.name == "--exposure-min-ev") exposure.minimum_ev = static_cast<float>(number);
            else if (argument.name == "--exposure-max-ev") exposure.maximum_ev = static_cast<float>(number);
            else if (argument.name == "--exposure-low-percent") exposure.low_percentile = static_cast<float>(number);
            else if (argument.name == "--exposure-high-percent") exposure.high_percentile = static_cast<float>(number);
            else if (argument.name == "--exposure-brighten-seconds") exposure.brighten_seconds = static_cast<float>(number);
            else exposure.darken_seconds = static_cast<float>(number);
            continue;
        }
        if (argument.name == "--lighting" || argument.name == "--gi" ||
            argument.name == "--day-cycle" || argument.name == "--time-of-day" ||
            argument.name == "--day-length" || argument.name == "--gi-extent" ||
            argument.name == "--gi-strength" || argument.name == "--gi-update-hz" ||
            argument.name == "--gi-triangle-budget" || argument.name == "--shadow-resolution") {
            if (!take_value(argument, index, argc, argv, value, result.error))
                return result;
            auto &s = result.options.lighting;
            auto &overrides = result.options.lighting_overrides;
            bool valid = true;
            double number = 0;
            if (argument.name == "--lighting") {
                valid = value == "dynamic" || value == "legacy";
                overrides.dynamic = true;
                s.dynamic = value == "dynamic";
                if (!s.dynamic) {
                    overrides.voxel_gi = overrides.cycle = true;
                    s.voxel_gi = false;
                    s.cycle = false;
                }
            } else if (argument.name == "--gi") {
                valid = value == "voxel" || value == "off";
                overrides.voxel_gi = true;
                s.voxel_gi = value == "voxel";
            } else if (argument.name == "--shadow-resolution") {
                overrides.shadow_resolution = true;
                valid = parse_u32(value, s.shadow_resolution) &&
                        valid_shadow_resolution(s.shadow_resolution);
            } else if (argument.name == "--day-cycle") {
                valid = value == "on" || value == "off";
                overrides.cycle = true;
                s.cycle = value == "on";
            } else {
                valid = parse_nonnegative_double(value, number);
                if (argument.name == "--time-of-day") {
                    valid = valid && number < 24;
                    overrides.hour = true;
                    s.hour = number;
                } else if (argument.name == "--day-length") {
                    valid = valid && number >= 1 && number <= 864000;
                    overrides.day_seconds = true;
                    s.day_seconds = number;
                } else if (argument.name == "--gi-extent") {
                    valid = valid && number >= 16 && number <= 256;
                    overrides.gi_extent = true;
                    s.gi_extent = static_cast<float>(number);
                } else if (argument.name == "--gi-strength") {
                    valid = valid && number <= 4;
                    overrides.gi_strength = true;
                    s.gi_strength = static_cast<float>(number);
                } else if (argument.name == "--gi-update-hz") {
                    valid = valid && number >= 1 && number <= 120;
                    overrides.gi_update_hz = true;
                    s.gi_update_hz = static_cast<float>(number);
                } else {
                    valid =
                        valid && number >= 1024 && number <= 262144 && std::floor(number) == number;
                    overrides.triangle_budget = true;
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
            result.options.camera_z_explicit = true;
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
        if (argument.name == "--motion-blur") {
            if (!take_value(argument, index, argc, argv, value, result.error)) {
                return result;
            }
            const auto mode = parse_motion_blur_mode(value);
            if (!mode) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.motion_blur.mode = *mode;
            continue;
        }
        if (argument.name == "--motion-blur-quality") {
            if (!take_value(argument, index, argc, argv, value, result.error)) {
                return result;
            }
            const auto quality = parse_motion_blur_quality(value);
            if (!quality) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.motion_blur.quality = *quality;
            continue;
        }
        if (argument.name == "--motion-blur-shutter") {
            if (!take_value(argument, index, argc, argv, value, result.error)) {
                return result;
            }
            const auto shutter = parse_motion_blur_shutter(value);
            if (!shutter) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            result.options.motion_blur.shutter = *shutter;
            continue;
        }
        if (argument.name == "--motion-blur-angle" ||
            argument.name == "--motion-blur-radius" ||
            argument.name == "--motion-blur-camera-fade-start" ||
            argument.name == "--motion-blur-camera-fade-end") {
            if (!take_value(argument, index, argc, argv, value, result.error)) {
                return result;
            }
            double number = 0.0;
            if (!parse_nonnegative_double(value, number)) {
                result.error = invalid_value(argument.name, value);
                return result;
            }
            if (argument.name == "--motion-blur-angle") {
                result.options.motion_blur.shutter_angle_degrees =
                    static_cast<float>(number);
            } else if (argument.name == "--motion-blur-radius") {
                result.options.motion_blur.maximum_radius_pixels =
                    static_cast<float>(number);
            } else if (argument.name == "--motion-blur-camera-fade-start") {
                result.options.motion_blur.camera_fade_start =
                    static_cast<float>(number);
            } else {
                result.options.motion_blur.camera_fade_end =
                    static_cast<float>(number);
            }
            continue;
        }
        if (argument.name == "--smaa-mode") {
            if (!take_value(
                    argument, index, argc, argv, value, result.error)) {
                return result;
            }
            if (!parse_smaa_mode(
                    value, result.options.anti_aliasing.smaa_mode)) {
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
    if (!valid_fog_settings(result.options.fog)) {
        result.error = "invalid fog settings (require 0 <= start < distance <= 100000)";
        return result;
    }
    if (!valid_exposure_settings(result.options.exposure)) {
        result.error = "invalid exposure settings";
        return result;
    }
    if (!valid_motion_blur_settings(result.options.motion_blur)) {
        result.error = "invalid motion blur settings";
        return result;
    }
    if (result.options.motion_blur.mode != MotionBlurMode::off &&
        (is_msaa(result.options.anti_aliasing.method) ||
         is_temporal_smaa(result.options.anti_aliasing) ||
         (result.options.anti_aliasing.method == AntiAliasingMethod::fsr3 &&
          result.options.anti_aliasing.fsr3_quality != Fsr3Quality::native_aa))) {
        result.error =
            "motion blur supports AA off, SMAA 1x, or FSR native-aa";
        return result;
    }
    if (is_msaa(result.options.anti_aliasing.method) &&
        result.options.subsurface == SubsurfaceMode::full_resolution) {
        result.error = "MSAA cannot be combined with --subsurface full";
        return result;
    }
    if (result.options.render_fixture != RenderFixture::none &&
        (result.options.lighting_room || result.options.scene_count > 0)) {
        result.error = "--render-fixture cannot be combined with --lighting-room or --scene-count";
        return result;
    }
    if (motion_fixture_fps_was_explicit &&
        (result.options.render_fixture != RenderFixture::motion_bars ||
         !result.options.animate_scene)) {
        result.error =
            "--motion-fixture-fps requires --render-fixture motion-bars and --animate-scene";
        return result;
    }
    if ((local_light_count_was_explicit || light_swarm_layout_was_explicit) &&
        result.options.render_fixture != RenderFixture::light_swarm) {
        result.error = "--local-light-count and --light-swarm-layout require --render-fixture light-swarm";
        return result;
    }
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

LightingSettings resolve_scene_lighting(
    const CliOptions &options, const std::optional<LightingSettings> &authored) noexcept {
    auto result = authored.value_or(options.lighting);
    if (options.lighting_overrides.dynamic) result.dynamic = options.lighting.dynamic;
    if (options.lighting_overrides.voxel_gi) result.voxel_gi = options.lighting.voxel_gi;
    if (options.lighting_overrides.cycle) result.cycle = options.lighting.cycle;
    if (options.lighting_overrides.hour) result.hour = options.lighting.hour;
    if (options.lighting_overrides.day_seconds) result.day_seconds = options.lighting.day_seconds;
    if (options.lighting_overrides.gi_extent) result.gi_extent = options.lighting.gi_extent;
    if (options.lighting_overrides.gi_strength) result.gi_strength = options.lighting.gi_strength;
    if (options.lighting_overrides.triangle_budget) result.triangle_budget = options.lighting.triangle_budget;
    if (options.lighting_overrides.gi_update_hz) result.gi_update_hz = options.lighting.gi_update_hz;
    if (options.lighting_overrides.shadow_resolution) result.shadow_resolution = options.lighting.shadow_resolution;
    if (options.lighting_overrides.shadow_depth) result.shadow_depth = options.lighting.shadow_depth;
    if (options.lighting_overrides.cascade_cache) result.cascade_cache = options.lighting.cascade_cache;
    // Selecting legacy lighting disables features that require the dynamic path.
    if (!result.dynamic) {
        result.voxel_gi = false;
        result.cycle = false;
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
                  "  --front-to-back MODE    off, coarse (CPU visibility only; default: off)\n"
                  "  --cpu-culling MODE      linear, hierarchy (CPU frustum; default: linear)\n"
                  "  --lod on|off            Screen-error mesh/terrain LOD (default: on)\n"
                  "  --dynamic-shadows on|off  Authored casters and invalidation (default: on)\n"
                  "  --animate-scene         Animate one runtime object (game executable)\n"
                  "  --animate-camera        Repeatable moving-camera benchmark path\n"
                  "  --camera-z N            Legacy camera pose with Z, 0..400 (default: scene or 12)\n"
                  "  --scene-scale N         Stress object scale, .001..100 (default: 1)\n"
                  "  --scene-count N         Generated stress scene, 1..100000 objects\n"
                  "  --scene-walls on|off    Include large occluders in the stress scene\n"
                  "  --render-fixture MODE   none, cascade-cache, overdraw-1/4/16 (default: none)\n"
                  "                          material-spheres, subsurface-profiles, light-swarm,\n"
                  "                          aa-wires, or motion-bars\n"
                  "  --motion-fixture-fps N fixed motion-bars simulation step: 30, 60, or 144\n"
                  "  --local-light-count N   light-swarm lights, 0..4096 (default: 256)\n"
                  "  --light-swarm-layout L  separated or overlap (default: separated)\n"
                  "  --scene-asset PATH      Repeat this cooked model in the stress scene\n"
                  "  --stream-asset PATH     Load/place a model while rendering\n"
                  "  --present MODE          auto (uncapped), immediate, mailbox, or fifo\n"
                  "  --mode MODE             scene, instancing,\n"
                  "                          or gpu-saturation\n"
                  "  --aa MODE               off, smaa, fsr3, msaa2x, or msaa4x (default: off)\n"
                  "  --motion-blur MODE      off, camera, objects, all (default: off)\n"
                  "  --motion-blur-quality Q low or high (default: high)\n"
                  "  --motion-blur-shutter S centered or trailing (default: centered)\n"
                  "  --motion-blur-angle DEG shutter angle, 0..360 (default: 180)\n"
                  "  --motion-blur-radius PX maximum radius, 1..64 (default: 32)\n"
                  "  --motion-blur-camera-fade-start M  optional camera fade start\n"
                  "  --motion-blur-camera-fade-end M    optional camera fade end\n"
                  "  --depth-prepass MODE    off, partial, full (default: off)\n"
                  "                          partial: terrain/primitives; full: all opaque\n"
                  "  --local-lighting MODE   off, forward, clustered, auto (default: auto)\n"
                  "  --subsurface off|full   Full-resolution masked separable SSS (default: off)\n"
                  "  --exposure fixed|auto   Scene exposure mode (default: fixed)\n"
                  "  --exposure-ev EV        Fixed EV or auto compensation, -24..24 (default: 0)\n"
                  "  --exposure-metering M   uniform or center (default: uniform)\n"
                  "  --exposure-min-ev EV    Auto lower clamp, -24..24 (default: -10)\n"
                  "  --exposure-max-ev EV    Auto upper clamp, -24..24 (default: 10)\n"
                  "  --exposure-low-percent N  Histogram lower fraction, 0..1 (default: .02)\n"
                  "  --exposure-high-percent N Histogram upper fraction, 0..1 (default: .98)\n"
                  "  --exposure-brighten-seconds N  Dark-scene time constant (default: 1)\n"
                  "  --exposure-darken-seconds N    Bright-scene time constant (default: .25)\n"
                  "  --grading MODE          off, trilinear, tetrahedral (default: off)\n"
                  "  --grading-lut-size N    Identity/reference LUT: 32 or 64 (default: 32)\n"
                  "  --lod-fade PATTERN      Model LOD: off, bayer4, bayer8 (default: off)\n"
                  "  --lod-fade-seconds SEC  Model LOD transition: 0.05..2 (default: 0.25)\n"
                  "  --smaa-quality MODE     low, medium, high, or ultra\n"
                  "  --smaa-mode MODE        1x or t2x (default: 1x)\n"
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
                  "  --tone-mapper reinhard|gt7  SDR display mapping (reinhard)\n"
                  "  --fog off|height           Atmospheric height fog (off)\n"
                  "  --fog-density 0..1         Extinction per scene unit (0.002)\n"
                  "  --fog-height-falloff 0..10 Height density falloff (0.05; 0 = uniform)\n"
                  "  --fog-base-height N        World Y of the fog base (0)\n"
                  "  --fog-start N              Clear distance from camera (10)\n"
                  "  --fog-distance N           Maximum ray distance, > start (1000)\n"
                  "  --fog-max-opacity 0..1     Maximum fog opacity (0.95)\n"
                  "  --fog-color-r/g/b 0..16    Linear HDR color channels (0.55/0.65/0.8)\n"
                  "  --gi voxel|off             Dynamic diffuse GI (voxel by default)\n"
                  "  --shadow-resolution N      Atlas edge: 1024|2048|4096|8192 (2048)\n"
                  "  --shadow-depth auto|d32|d16 Startup shadow depth profile (auto)\n"
                  "  --shadow-cache full|cascade Cascade refresh policy (full)\n"
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
