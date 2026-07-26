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

        std::string_view value;
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

        result.error = "unknown command-line option: ";
        result.error.append(raw);
        return result;
    }

    // Explicit workload knobs are convenient mode selectors, while --mode
    // always wins when the user supplied it.
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
    result.append(
        " [options]\n"
        "  --benchmark             Run timed benchmark and write benchmark.json\n"
        "  --mcp-stdio             Expose AI toolsets over MCP stdio\n"
        "  --width N               Initial drawable width (default: 1280)\n"
        "  --height N              Initial drawable height (default: 720)\n"
        "  --instances N           Cube instance count (default: 1)\n"
        "  --gpu-workload N        GPU workload iterations (default: 0)\n"
        "  --present MODE          auto (uncapped), immediate, mailbox, or fifo\n"
        "  --mode MODE             scene, instancing, or gpu-saturation\n"
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
