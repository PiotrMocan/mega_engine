#include "mcp_server.hpp"

#include "png_encoder.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::string_view protocol_version{"2025-11-25"};
constexpr std::size_t maximum_input_message_bytes = 1024U * 1024U;
constexpr std::array<std::string_view, 18> tool_names{
    "debug.get_capabilities",
    "debug.get_state",
    "debug.get_metrics",
    "debug.reset_metrics",
    "test.run_self_check",
    "capture.screenshot",
    "render.set_mode",
    "render.set_instances",
    "render.set_gpu_workload",
    "render.set_present_mode",
    "window.set_fullscreen",
    "window.set_size",
    "window.set_mouse_capture",
    "input.key",
    "input.mouse_move",
    "input.mouse_button",
    "input.release_all",
    "engine.quit",
};

enum class LineStatus : std::uint8_t {
    line,
    end_of_stream,
    stopped,
    too_large,
    error,
};

enum class ChunkStatus : std::uint8_t {
    data,
    idle,
    end_of_stream,
    error,
};

class StdinLineReader final {
public:
    [[nodiscard]] LineStatus read_line(
        const std::stop_token stop_token,
        std::string& line) {
        line.clear();
        while (!stop_token.stop_requested()) {
            if (discard_until_newline_) {
                const auto newline = pending_.find('\n');
                if (newline != std::string::npos) {
                    pending_.erase(0, newline + 1U);
                    discard_until_newline_ = false;
                } else {
                    pending_.clear();
                }
            } else {
                const auto newline = pending_.find('\n');
                if (newline != std::string::npos) {
                    if (newline > maximum_input_message_bytes) {
                        pending_.erase(0, newline + 1U);
                        return LineStatus::too_large;
                    }
                    line.assign(pending_.data(), newline);
                    pending_.erase(0, newline + 1U);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    return LineStatus::line;
                }
                if (pending_.size() > maximum_input_message_bytes) {
                    pending_.clear();
                    discard_until_newline_ = true;
                    return LineStatus::too_large;
                }
            }

            std::array<char, 4096> chunk{};
            std::size_t bytes_read = 0;
            const ChunkStatus status = read_chunk(
                stop_token, chunk, bytes_read);
            if (status == ChunkStatus::idle) {
                continue;
            }
            if (status == ChunkStatus::data) {
                pending_.append(chunk.data(), bytes_read);
                continue;
            }
            if (status == ChunkStatus::end_of_stream) {
                if (!pending_.empty() && !discard_until_newline_) {
                    if (pending_.size() > maximum_input_message_bytes) {
                        pending_.clear();
                        return LineStatus::too_large;
                    }
                    line = std::move(pending_);
                    pending_.clear();
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    return LineStatus::line;
                }
                return LineStatus::end_of_stream;
            }
            return LineStatus::error;
        }
        return LineStatus::stopped;
    }

private:
    [[nodiscard]] static ChunkStatus read_chunk(
        [[maybe_unused]] const std::stop_token stop_token,
        const std::span<char> output,
        std::size_t& bytes_read) noexcept {
        bytes_read = 0;
#if defined(_WIN32)
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE) {
            return ChunkStatus::error;
        }
        const DWORD type = GetFileType(input);
        if (type == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (!PeekNamedPipe(
                    input, nullptr, 0, nullptr, &available, nullptr)) {
                return GetLastError() == ERROR_BROKEN_PIPE
                    ? ChunkStatus::end_of_stream
                    : ChunkStatus::error;
            }
            if (available == 0U) {
                std::this_thread::sleep_for(10ms);
                return ChunkStatus::idle;
            }
            DWORD read = 0;
            const DWORD requested = static_cast<DWORD>(
                std::min<std::size_t>(output.size(), available));
            if (!ReadFile(input, output.data(), requested, &read, nullptr)) {
                return GetLastError() == ERROR_BROKEN_PIPE
                    ? ChunkStatus::end_of_stream
                    : ChunkStatus::error;
            }
            bytes_read = read;
            return read == 0U
                ? ChunkStatus::end_of_stream
                : ChunkStatus::data;
        }

        if (type != FILE_TYPE_DISK) {
            return ChunkStatus::error;
        }
        DWORD read = 0;
        if (!ReadFile(
                input,
                output.data(),
                static_cast<DWORD>(output.size()),
                &read,
                nullptr)) {
            return ChunkStatus::error;
        }
        bytes_read = read;
        return read == 0U
            ? ChunkStatus::end_of_stream
            : ChunkStatus::data;
#else
        pollfd descriptor{
            .fd = STDIN_FILENO,
            .events = static_cast<short>(POLLIN | POLLHUP),
            .revents = 0,
        };
        const int poll_result = ::poll(&descriptor, 1, 25);
        if (poll_result == 0) {
            return ChunkStatus::idle;
        }
        if (poll_result < 0) {
            return errno == EINTR
                ? ChunkStatus::idle
                : ChunkStatus::error;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            return ChunkStatus::error;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
            return ChunkStatus::idle;
        }
        const ssize_t read =
            ::read(STDIN_FILENO, output.data(), output.size());
        if (read == 0) {
            return ChunkStatus::end_of_stream;
        }
        if (read < 0) {
            return errno == EINTR || errno == EAGAIN
                ? ChunkStatus::idle
                : ChunkStatus::error;
        }
        bytes_read = static_cast<std::size_t>(read);
        return ChunkStatus::data;
#endif
    }

    std::string pending_;
    bool discard_until_newline_ = false;
};

[[nodiscard]] Json rpc_response(const Json& id, Json result) {
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    };
}

[[nodiscard]] Json rpc_error(
    const Json& id,
    const int code,
    const std::string_view message) {
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", Json{
            {"code", code},
            {"message", message},
        }},
    };
}

[[nodiscard]] bool write_message(const Json& message) noexcept {
    try {
        std::string serialized = message.dump();
        serialized.push_back('\n');
        const std::size_t written =
            std::fwrite(serialized.data(), 1, serialized.size(), stdout);
        return written == serialized.size() && std::fflush(stdout) == 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP response serialization failed: "
                  << error.what() << '\n';
        return false;
    }
}

[[nodiscard]] Json object_schema(
    Json properties = Json::object(),
    Json required = Json::array()) {
    return Json{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
        {"additionalProperties", false},
    };
}

[[nodiscard]] Json tool_definition(
    const std::string_view name,
    const std::string_view title,
    const std::string_view description,
    Json input_schema,
    const bool read_only,
    const bool destructive,
    const bool idempotent) {
    return Json{
        {"name", name},
        {"title", title},
        {"description", description},
        {"inputSchema", std::move(input_schema)},
        {"annotations", Json{
            {"readOnlyHint", read_only},
            {"destructiveHint", destructive},
            {"idempotentHint", idempotent},
            {"openWorldHint", false},
        }},
    };
}

[[nodiscard]] Json make_tools() {
    const Json action_schema{
        {"type", "string"},
        {"enum", Json::array({"down", "up", "press"})},
    };
    const Json enabled_schema{{"type", "boolean"}};
    Json tools = Json::array();

    tools.push_back(tool_definition(
        "debug.get_capabilities",
        "Get renderer capabilities",
        "Return GPU, Vulkan, present-mode, capture, and runtime limits.",
        object_schema(),
        true,
        false,
        true));
    tools.push_back(tool_definition(
        "debug.get_state",
        "Get live engine state",
        "Return renderer, window, camera, and control-plane state.",
        object_schema(),
        true,
        false,
        true));
    tools.push_back(tool_definition(
        "debug.get_metrics",
        "Get performance metrics",
        "Return the current FPS, 1% low, CPU-stage, and GPU timestamp metrics.",
        object_schema(),
        true,
        false,
        true));
    tools.push_back(tool_definition(
        "debug.reset_metrics",
        "Reset performance metrics",
        "Reset the bounded live performance accumulator.",
        object_schema(),
        false,
        false,
        true));
    tools.push_back(tool_definition(
        "test.run_self_check",
        "Run runtime self-check",
        "Check initialized Vulkan, swapchain, dimensions, and renderer invariants.",
        object_schema(),
        true,
        false,
        true));
    tools.push_back(tool_definition(
        "capture.screenshot",
        "Capture the rendered frame",
        "Capture the next completed swapchain image and return it as PNG image content.",
        object_schema(),
        true,
        false,
        false));
    tools.push_back(tool_definition(
        "render.set_mode",
        "Set render mode",
        "Select scene, instancing, or gpu-saturation mode.",
        object_schema(
            Json{{"mode", Json{
                {"type", "string"},
                {"enum", Json::array({
                    "scene", "instancing", "gpu-saturation"})},
            }}},
            Json::array({"mode"})),
        false,
        false,
        true));
    tools.push_back(tool_definition(
        "render.set_instances",
        "Set instance count",
        "Set the instancing workload count within the detected GPU limit.",
        object_schema(
            Json{{"count", Json{
                {"type", "integer"},
                {"minimum", 1},
                {"maximum", std::numeric_limits<std::uint32_t>::max()},
            }}},
            Json::array({"count"})),
        false,
        false,
        true));
    tools.push_back(tool_definition(
        "render.set_gpu_workload",
        "Set GPU workload",
        "Set compute workload iterations from 0 through 4096.",
        object_schema(
            Json{{"iterations", Json{
                {"type", "integer"},
                {"minimum", 0},
                {"maximum", 4096},
            }}},
            Json::array({"iterations"})),
        false,
        false,
        true));
    tools.push_back(tool_definition(
        "render.set_present_mode",
        "Set present mode",
        "Request auto, immediate, mailbox, or fifo with the renderer fallback policy.",
        object_schema(
            Json{{"mode", Json{
                {"type", "string"},
                {"enum", Json::array({
                    "auto", "immediate", "mailbox", "fifo"})},
            }}},
            Json::array({"mode"})),
        false,
        false,
        true));
    tools.push_back(tool_definition(
        "window.set_fullscreen",
        "Set fullscreen",
        "Enter or leave native fullscreen mode.",
        object_schema(
            Json{{"enabled", enabled_schema}},
            Json::array({"enabled"})),
        false,
        false,
        true));
    tools.push_back(tool_definition(
        "window.set_size",
        "Set window size",
        "Resize the window in logical pixels.",
        object_schema(
            Json{
                {"width", Json{
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 16'384},
                }},
                {"height", Json{
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 16'384},
                }},
            },
            Json::array({"width", "height"})),
        false,
        false,
        true));
    tools.push_back(tool_definition(
        "window.set_mouse_capture",
        "Set mouse capture",
        "Enable or disable relative mouse capture for physical input.",
        object_schema(
            Json{{"enabled", enabled_schema}},
            Json::array({"enabled"})),
        false,
        false,
        true));
    tools.push_back(tool_definition(
        "input.key",
        "Emulate an engine key",
        "Inject a deterministic engine-local key down, up, or press action.",
        object_schema(
            Json{
                {"key", Json{
                    {"type", "string"},
                    {"enum", Json::array({
                        "w", "a", "s", "d", "shift", "space", "e",
                        "ctrl", "q", "escape", "v", "f", "r", "p",
                        "1", "2", "3", "f6", "f7", "f8", "up", "down",
                    })},
                }},
                {"action", action_schema},
            },
            Json::array({"key", "action"})),
        false,
        false,
        false));
    tools.push_back(tool_definition(
        "input.mouse_move",
        "Emulate relative mouse movement",
        "Inject relative mouse deltas directly into the fly-camera input path.",
        object_schema(
            Json{
                {"delta_x", Json{{"type", "number"}}},
                {"delta_y", Json{{"type", "number"}}},
            },
            Json::array({"delta_x", "delta_y"})),
        false,
        false,
        false));
    tools.push_back(tool_definition(
        "input.mouse_button",
        "Emulate a mouse button",
        "Inject the left mouse button; pressing it captures the window mouse.",
        object_schema(
            Json{
                {"button", Json{
                    {"type", "string"},
                    {"enum", Json::array({"left"})},
                }},
                {"action", action_schema},
            },
            Json::array({"button", "action"})),
        false,
        false,
        false));
    tools.push_back(tool_definition(
        "input.release_all",
        "Release virtual input",
        "Release every held MCP key and discard queued virtual mouse deltas.",
        object_schema(),
        false,
        false,
        true));
    tools.push_back(tool_definition(
        "engine.quit",
        "Quit the engine",
        "Gracefully stop rendering after this tool response is delivered.",
        object_schema(),
        false,
        true,
        true));
    return tools;
}

[[nodiscard]] bool is_known_tool(const std::string_view name) noexcept {
    return std::find(tool_names.begin(), tool_names.end(), name) !=
           tool_names.end();
}

[[nodiscard]] std::optional<std::uint32_t> get_u32(
    const Json& arguments,
    const std::string_view name,
    const std::uint32_t minimum,
    const std::uint32_t maximum) {
    const auto iterator = arguments.find(name);
    if (iterator == arguments.end() ||
        (!iterator->is_number_unsigned() &&
         !iterator->is_number_integer())) {
        return std::nullopt;
    }
    const std::int64_t signed_value = iterator->is_number_unsigned()
        ? (iterator->get<std::uint64_t>() >
                   static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())
               ? -1
               : static_cast<std::int64_t>(
                     iterator->get<std::uint64_t>()))
        : iterator->get<std::int64_t>();
    if (signed_value < static_cast<std::int64_t>(minimum) ||
        signed_value > static_cast<std::int64_t>(maximum)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(signed_value);
}

[[nodiscard]] std::optional<bool> get_bool(
    const Json& arguments,
    const std::string_view name) {
    const auto iterator = arguments.find(name);
    if (iterator == arguments.end() || !iterator->is_boolean()) {
        return std::nullopt;
    }
    return iterator->get<bool>();
}

[[nodiscard]] std::optional<std::string_view> get_string(
    const Json& arguments,
    const std::string_view name) {
    const auto iterator = arguments.find(name);
    if (iterator == arguments.end() || !iterator->is_string()) {
        return std::nullopt;
    }
    return iterator->get_ref<const std::string&>();
}

[[nodiscard]] std::optional<McpInputAction> parse_input_action(
    const std::string_view text) noexcept {
    if (text == "down") {
        return McpInputAction::down;
    }
    if (text == "up") {
        return McpInputAction::up;
    }
    if (text == "press") {
        return McpInputAction::press;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<McpInputKey> parse_input_key(
    const std::string_view text) noexcept {
    using Key = McpInputKey;
    if (text == "w") return Key::w;
    if (text == "a") return Key::a;
    if (text == "s") return Key::s;
    if (text == "d") return Key::d;
    if (text == "shift") return Key::shift;
    if (text == "space") return Key::space;
    if (text == "e") return Key::e;
    if (text == "ctrl") return Key::control;
    if (text == "q") return Key::q;
    if (text == "escape") return Key::escape;
    if (text == "v") return Key::v;
    if (text == "f") return Key::f;
    if (text == "r") return Key::r;
    if (text == "p") return Key::p;
    if (text == "1") return Key::digit_1;
    if (text == "2") return Key::digit_2;
    if (text == "3") return Key::digit_3;
    if (text == "f6") return Key::f6;
    if (text == "f7") return Key::f7;
    if (text == "f8") return Key::f8;
    if (text == "up") return Key::arrow_up;
    if (text == "down") return Key::arrow_down;
    return std::nullopt;
}

[[nodiscard]] bool command_from_tool(
    const std::string_view name,
    const Json& arguments,
    const std::uint64_t token,
    McpCommand& command,
    std::string& error) {
    command = {};
    command.token = token;

    const auto require_empty = [&]() {
        if (!arguments.empty()) {
            error = "this tool accepts no arguments";
            return false;
        }
        return true;
    };

    if (name == "debug.get_capabilities") {
        command.type = McpCommandType::debug_get_capabilities;
        return require_empty();
    }
    if (name == "debug.get_state") {
        command.type = McpCommandType::debug_get_state;
        return require_empty();
    }
    if (name == "debug.get_metrics") {
        command.type = McpCommandType::debug_get_metrics;
        return require_empty();
    }
    if (name == "debug.reset_metrics") {
        command.type = McpCommandType::debug_reset_metrics;
        return require_empty();
    }
    if (name == "test.run_self_check") {
        command.type = McpCommandType::test_run_self_check;
        return require_empty();
    }
    if (name == "capture.screenshot") {
        command.type = McpCommandType::capture_screenshot;
        return require_empty();
    }
    if (name == "input.release_all") {
        command.type = McpCommandType::input_release_all;
        return require_empty();
    }
    if (name == "engine.quit") {
        command.type = McpCommandType::engine_request_quit;
        return require_empty();
    }

    if (name == "render.set_mode") {
        const auto mode = get_string(arguments, "mode");
        if (!mode || arguments.size() != 1U) {
            error = "mode must be the only argument";
            return false;
        }
        command.type = McpCommandType::render_set_mode;
        if (*mode == "scene") {
            command.mode = RunMode::scene;
        } else if (*mode == "instancing") {
            command.mode = RunMode::instancing;
        } else if (*mode == "gpu-saturation") {
            command.mode = RunMode::gpu_saturation;
        } else {
            error = "unsupported render mode";
            return false;
        }
        return true;
    }
    if (name == "render.set_instances") {
        const auto count = get_u32(
            arguments, "count", 1U,
            std::numeric_limits<std::uint32_t>::max());
        if (!count || arguments.size() != 1U) {
            error = "count must be a positive uint32";
            return false;
        }
        command.type = McpCommandType::render_set_instances;
        command.value = *count;
        return true;
    }
    if (name == "render.set_gpu_workload") {
        const auto iterations =
            get_u32(arguments, "iterations", 0U, 4096U);
        if (!iterations || arguments.size() != 1U) {
            error = "iterations must be between 0 and 4096";
            return false;
        }
        command.type = McpCommandType::render_set_gpu_workload;
        command.value = *iterations;
        return true;
    }
    if (name == "render.set_present_mode") {
        const auto mode = get_string(arguments, "mode");
        if (!mode || arguments.size() != 1U) {
            error = "mode must be the only argument";
            return false;
        }
        command.type = McpCommandType::render_set_present_mode;
        if (*mode == "auto") {
            command.present = PresentPreference::automatic;
        } else if (*mode == "immediate") {
            command.present = PresentPreference::immediate;
        } else if (*mode == "mailbox") {
            command.present = PresentPreference::mailbox;
        } else if (*mode == "fifo") {
            command.present = PresentPreference::fifo;
        } else {
            error = "unsupported present mode";
            return false;
        }
        return true;
    }
    if (name == "window.set_fullscreen" ||
        name == "window.set_mouse_capture") {
        const auto enabled = get_bool(arguments, "enabled");
        if (!enabled || arguments.size() != 1U) {
            error = "enabled must be the only argument and a boolean";
            return false;
        }
        command.type = name == "window.set_fullscreen"
            ? McpCommandType::window_set_fullscreen
            : McpCommandType::window_set_mouse_capture;
        command.enabled = *enabled;
        return true;
    }
    if (name == "window.set_size") {
        const auto width = get_u32(arguments, "width", 1U, 16'384U);
        const auto height = get_u32(arguments, "height", 1U, 16'384U);
        if (!width || !height || arguments.size() != 2U) {
            error = "width and height must be integers from 1 through 16384";
            return false;
        }
        command.type = McpCommandType::window_set_size;
        command.width = *width;
        command.height = *height;
        return true;
    }
    if (name == "input.key") {
        const auto key_text = get_string(arguments, "key");
        const auto action_text = get_string(arguments, "action");
        if (!key_text || !action_text || arguments.size() != 2U) {
            error = "key and action must be the only string arguments";
            return false;
        }
        const auto key = parse_input_key(*key_text);
        const auto action = parse_input_action(*action_text);
        if (!key || !action) {
            error = "unsupported key or input action";
            return false;
        }
        command.type = McpCommandType::input_key;
        command.key = *key;
        command.input_action = *action;
        return true;
    }
    if (name == "input.mouse_move") {
        const auto x = arguments.find("delta_x");
        const auto y = arguments.find("delta_y");
        if (x == arguments.end() || y == arguments.end() ||
            !x->is_number() || !y->is_number() ||
            arguments.size() != 2U) {
            error = "delta_x and delta_y must be the only numeric arguments";
            return false;
        }
        const double delta_x = x->get<double>();
        const double delta_y = y->get<double>();
        constexpr double maximum_delta = 1'000'000.0;
        if (!std::isfinite(delta_x) || !std::isfinite(delta_y) ||
            std::abs(delta_x) > maximum_delta ||
            std::abs(delta_y) > maximum_delta) {
            error = "mouse deltas must be finite and within +/-1000000";
            return false;
        }
        command.type = McpCommandType::input_mouse_move;
        command.mouse_delta_x = static_cast<float>(delta_x);
        command.mouse_delta_y = static_cast<float>(delta_y);
        return true;
    }
    if (name == "input.mouse_button") {
        const auto button = get_string(arguments, "button");
        const auto action_text = get_string(arguments, "action");
        if (!button || !action_text || arguments.size() != 2U ||
            *button != "left") {
            error = "only the left mouse button is supported";
            return false;
        }
        const auto action = parse_input_action(*action_text);
        if (!action) {
            error = "unsupported mouse-button action";
            return false;
        }
        command.type = McpCommandType::input_mouse_button;
        command.mouse_button = McpMouseButton::left;
        command.input_action = *action;
        return true;
    }

    error = "unknown tool";
    return false;
}

[[nodiscard]] std::string base64_encode(
    const std::span<const std::uint8_t> bytes) {
    constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    if (bytes.size() >
        (std::numeric_limits<std::size_t>::max() - 2U) / 3U) {
        return {};
    }
    std::string output;
    output.reserve(((bytes.size() + 2U) / 3U) * 4U);
    std::size_t index = 0;
    while (index + 3U <= bytes.size()) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (static_cast<std::uint32_t>(bytes[index + 1U]) << 8U) |
            static_cast<std::uint32_t>(bytes[index + 2U]);
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.push_back(alphabet[(value >> 6U) & 0x3fU]);
        output.push_back(alphabet[value & 0x3fU]);
        index += 3U;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1U) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(bytes[index]) << 16U;
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.append("==");
    } else if (remaining == 2U) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (static_cast<std::uint32_t>(bytes[index + 1U]) << 8U);
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.push_back(alphabet[(value >> 6U) & 0x3fU]);
        output.push_back('=');
    }
    return output;
}

[[nodiscard]] Json command_result_to_tool_result(
    McpCommandResult result) {
    if (!result.success) {
        return Json{
            {"content", Json::array({Json{
                {"type", "text"},
                {"text", result.message},
            }})},
            {"isError", true},
        };
    }

    Json structured = Json::parse(
        result.structured_json, nullptr, false);
    if (structured.is_discarded() || !structured.is_object()) {
        return Json{
            {"content", Json::array({Json{
                {"type", "text"},
                {"text", "engine returned invalid structured JSON"},
            }})},
            {"isError", true},
        };
    }

    Json content = Json::array();
    if (!result.image.rgba.empty()) {
        PngEncodeResult png = encode_png_rgba(
            result.image.rgba,
            result.image.width,
            result.image.height);
        if (!png) {
            return Json{
                {"content", Json::array({Json{
                    {"type", "text"},
                    {"text", png.error},
                }})},
                {"isError", true},
            };
        }
        structured["width"] = result.image.width;
        structured["height"] = result.image.height;
        structured["mime_type"] = "image/png";
        content.push_back(Json{
            {"type", "text"},
            {"text", structured.dump()},
        });
        content.push_back(Json{
            {"type", "image"},
            {"data", base64_encode(png.bytes)},
            {"mimeType", "image/png"},
        });
    } else {
        if (!result.message.empty() &&
            structured.find("message") == structured.end()) {
            structured["message"] = result.message;
        }
        content.push_back(Json{
            {"type", "text"},
            {"text", structured.dump()},
        });
    }
    return Json{
        {"content", std::move(content)},
        {"structuredContent", std::move(structured)},
        {"isError", false},
    };
}

[[nodiscard]] bool enqueue_shutdown(
    McpControlPlane& control,
    const std::stop_token stop_token) noexcept {
    McpCommand command{};
    command.type = McpCommandType::engine_commit_quit;
    while (!stop_token.stop_requested()) {
        if (control.submit(command)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

} // namespace

McpServer::~McpServer() {
    stop();
}

void McpServer::start() {
    if (thread_.joinable()) {
        return;
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::jthread(
        [this](const std::stop_token stop_token) {
            run(stop_token);
        });
}

void McpServer::stop() noexcept {
    if (!thread_.joinable()) {
        running_.store(false, std::memory_order_release);
        return;
    }
    thread_.request_stop();
    thread_.join();
    running_.store(false, std::memory_order_release);
}

void McpServer::run(const std::stop_token stop_token) noexcept {
    try {
        StdinLineReader reader;
        std::uint64_t next_token = 1U;
        bool initialize_answered = false;
        bool initialized = false;
        while (!stop_token.stop_requested()) {
            std::string line;
            const LineStatus line_status =
                reader.read_line(stop_token, line);
            if (line_status == LineStatus::stopped) {
                break;
            }
            if (line_status == LineStatus::end_of_stream) {
                (void)enqueue_shutdown(control_.get(), stop_token);
                break;
            }
            if (line_status == LineStatus::too_large) {
                if (!write_message(rpc_error(
                        nullptr,
                        -32700,
                        "MCP message exceeds 1 MiB input limit"))) {
                    break;
                }
                continue;
            }
            if (line_status == LineStatus::error) {
                std::cerr << "MCP stdin transport failed\n";
                (void)enqueue_shutdown(control_.get(), stop_token);
                break;
            }
            if (line.empty()) {
                continue;
            }

            Json request = Json::parse(line, nullptr, false);
            if (request.is_discarded()) {
                if (!write_message(rpc_error(
                        nullptr, -32700, "invalid JSON"))) {
                    break;
                }
                continue;
            }
            if (!request.is_object() ||
                !request.contains("jsonrpc") ||
                !request["jsonrpc"].is_string() ||
                request["jsonrpc"] != "2.0" ||
                !request.contains("method") ||
                !request["method"].is_string()) {
                const Json id = request.is_object() &&
                        request.contains("id")
                    ? request["id"]
                    : Json(nullptr);
                if (!write_message(rpc_error(
                        id, -32600, "invalid JSON-RPC request"))) {
                    break;
                }
                continue;
            }

            const std::string method = request["method"].get<std::string>();
            const bool has_id = request.contains("id");
            const Json id = has_id ? request["id"] : Json(nullptr);
            if (method == "notifications/initialized") {
                initialized = initialize_answered;
                continue;
            }
            if (method == "notifications/cancelled" ||
                method == "notifications/cancelled_task") {
                continue;
            }
            if (!has_id) {
                continue;
            }
            if (method == "initialize") {
                if (initialize_answered) {
                    if (!write_message(rpc_error(
                            id,
                            -32600,
                            "server is already initialized"))) {
                        break;
                    }
                    continue;
                }
                const Json result{
                    {"protocolVersion", protocol_version},
                    {"capabilities", Json{
                        {"tools", Json{{"listChanged", false}}},
                    }},
                    {"serverInfo", Json{
                        {"name", "vulkan-cube-renderer"},
                        {"title", "Vulkan Cube AI Control Plane"},
                        {"version", "0.2.0"},
                        {"description",
                         "Low-overhead MCP control plane for the Vulkan renderer"},
                    }},
                    {"instructions",
                     "Use debug and test tools to inspect state, render/window "
                     "tools for idempotent controls, capture.screenshot for "
                     "visual verification, and input tools for deterministic "
                     "engine-local input injection."},
                };
                if (!write_message(rpc_response(id, result))) {
                    break;
                }
                initialize_answered = true;
                continue;
            }
            if (method == "ping") {
                if (!write_message(rpc_response(id, Json::object()))) {
                    break;
                }
                continue;
            }
            if (!initialized) {
                if (!write_message(rpc_error(
                        id, -32002, "server is not initialized"))) {
                    break;
                }
                continue;
            }
            if (method == "tools/list") {
                if (!write_message(rpc_response(
                        id, Json{{"tools", make_tools()}}))) {
                    break;
                }
                continue;
            }
            if (method != "tools/call") {
                if (!write_message(rpc_error(
                        id, -32601, "method not found"))) {
                    break;
                }
                continue;
            }

            const auto params_iterator = request.find("params");
            if (params_iterator == request.end() ||
                !params_iterator->is_object()) {
                if (!write_message(rpc_error(
                        id, -32602, "tools/call params must be an object"))) {
                    break;
                }
                continue;
            }
            const Json& params = *params_iterator;
            const auto name_iterator = params.find("name");
            if (name_iterator == params.end() ||
                !name_iterator->is_string()) {
                if (!write_message(rpc_error(
                        id, -32602, "tool name must be a string"))) {
                    break;
                }
                continue;
            }
            Json empty_arguments = Json::object();
            const Json* arguments = &empty_arguments;
            const auto arguments_iterator = params.find("arguments");
            if (arguments_iterator != params.end()) {
                if (!arguments_iterator->is_object()) {
                    if (!write_message(rpc_error(
                            id,
                            -32602,
                            "tool arguments must be an object"))) {
                        break;
                    }
                    continue;
                }
                arguments = &*arguments_iterator;
            }
            const std::string& tool_name =
                name_iterator->get_ref<const std::string&>();
            if (!is_known_tool(tool_name)) {
                if (!write_message(rpc_error(
                        id, -32602, "unknown tool"))) {
                    break;
                }
                continue;
            }

            McpCommand command{};
            std::string command_error;
            const std::uint64_t token = next_token++;
            if (!command_from_tool(
                    tool_name,
                    *arguments,
                    token,
                    command,
                    command_error)) {
                const Json tool_error{
                    {"content", Json::array({Json{
                        {"type", "text"},
                        {"text", command_error},
                    }})},
                    {"isError", true},
                };
                if (!write_message(rpc_response(id, tool_error))) {
                    break;
                }
                continue;
            }
            const bool quit_after_response =
                command.type == McpCommandType::engine_request_quit;
            if (!control_.get().submit(command)) {
                const Json busy_error{
                    {"content", Json::array({Json{
                        {"type", "text"},
                        {"text", "MCP command queue is full"},
                    }})},
                    {"isError", true},
                };
                if (!write_message(rpc_response(id, busy_error))) {
                    break;
                }
                continue;
            }

            McpCommandResult command_result{};
            bool received = false;
            while (!stop_token.stop_requested()) {
                if (control_.get().try_next_result(command_result)) {
                    if (command_result.token != token) {
                        std::cerr
                            << "MCP response token ordering invariant failed\n";
                        continue;
                    }
                    received = true;
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
            if (!received) {
                break;
            }
            if (!write_message(rpc_response(
                    id,
                    command_result_to_tool_result(
                        std::move(command_result))))) {
                break;
            }
            if (quit_after_response) {
                (void)enqueue_shutdown(control_.get(), stop_token);
                break;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "MCP server exception: " << error.what() << '\n';
        if (!stop_token.stop_requested()) {
            (void)enqueue_shutdown(control_.get(), stop_token);
        }
    }
    running_.store(false, std::memory_order_release);
}

} // namespace vcr
