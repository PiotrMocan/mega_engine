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
#include <initializer_list>
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
constexpr std::array<std::string_view, 21> runtime_tool_names{
    "debug.get_capabilities",
    "debug.get_state",
    "debug.get_metrics",
    "debug.reset_metrics",
    "test.run_self_check",
    "capture.screenshot",
    "render.set_mode",
    "render.set_instances",
    "render.set_gpu_workload",
    "lighting.set",
    "render.set_shadow_resolution",
    "test.set_roof",
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

constexpr std::array<std::string_view, 19> editor_tool_names{
    "editor.get_state",
    "editor.selection.set",
    "editor.scene.list_nodes",
    "editor.scene.get_node",
    "editor.scene.create_node",
    "editor.scene.update_node",
    "editor.scene.delete_node",
    "editor.scene.reparent_node",
    "editor.scene.duplicate_node",
    "editor.camera.focus_selection",
    "editor.scene.save",
    "editor.history.undo",
    "editor.history.redo",
    "editor.assets.list",
    "editor.assets.import",
    "editor.landscape.create_flat_ground",
    "editor.landscape.scatter_model",
    "capture.screenshot",
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

[[nodiscard]] Json make_runtime_tools() {
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
        "lighting.set", "Set daylight and GI",
        "Set hour, cycle and GI; GI resources must have been provisioned at startup.",
        object_schema(
            Json{{"hour", Json{{"type", "number"}, {"minimum", 0}, {"exclusiveMaximum", 24}}},
                 {"cycle", Json{{"type", "boolean"}}},
                 {"gi", Json{{"type", "boolean"}}}},
            Json::array({"hour", "cycle", "gi"})),
        false, false, true));
    tools.push_back(tool_definition(
        "render.set_shadow_resolution", "Set shadow resolution",
        "Set full shadow atlas edge; dynamic lighting uses four half-size cascades. "
        "Applied at the next rendered frame; unsupported GPU sizes are rejected.",
        object_schema(Json{{"resolution", Json{{"type", "integer"},
            {"enum", shadow_resolutions}}}}, Json::array({"resolution"})),
        false, false, true));
    tools.push_back(tool_definition(
        "test.set_roof", "Set test-room roof",
        "Create or destroy the roof in --lighting-room; unavailable in ordinary scenes.",
        object_schema(Json{{"enabled", Json{{"type", "boolean"}}}}, Json::array({"enabled"})),
        false, true, true));
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

[[nodiscard]] Json make_editor_tools() {
    const Json handle_schema = object_schema(
        Json{
            {"index", Json{{"type", "integer"}, {"minimum", 0}}},
            {"generation", Json{{"type", "integer"}, {"minimum", 1}}},
        },
        Json::array({"index", "generation"}));
    const Json optional_parent_schema{
        {"anyOf", Json::array({handle_schema, Json{{"type", "null"}}})},
    };
    const Json vec3_schema{
        {"type", "array"},
        {"items", Json{{"type", "number"}}},
        {"minItems", 3},
        {"maxItems", 3},
    };
    const Json color_schema{
        {"type", "array"},
        {"items", Json{
            {"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
        {"minItems", 4},
        {"maxItems", 4},
    };
    const Json kind_schema{
        {"type", "string"},
        {"enum", Json::array({
            "folder", "cube", "ground", "terrain", "camera",
            "directional_light", "environment", "model",
        })},
    };
    Json tools = Json::array();
    tools.push_back(tool_definition(
        "editor.get_state", "Get editor state",
        "Return the live authoring state, revisions, history, asset root, and import status.",
        object_schema(), true, false, true));
    tools.push_back(tool_definition(
        "editor.selection.set", "Set editor selection",
        "Select one valid scene node in the live editor, reveal it in the hierarchy, or clear selection with null.",
        object_schema(Json{{"id", optional_parent_schema}}, Json::array({"id"})),
        false, false, true));
    tools.push_back(tool_definition(
        "editor.scene.list_nodes", "List scene nodes",
        "Return a stable preorder page of authored nodes and generational handles.",
        object_schema(Json{
            {"offset", Json{{"type", "integer"}, {"minimum", 0}}},
            {"limit", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
        }), true, false, true));
    tools.push_back(tool_definition(
        "editor.scene.get_node", "Get scene node",
        "Return all authored properties for one generational node handle.",
        object_schema(Json{{"id", handle_schema}}, Json::array({"id"})),
        true, false, true));
    tools.push_back(tool_definition(
        "editor.scene.create_node", "Create scene node",
        "Create a node with an optional parent, transform, visibility, and model asset path.",
        object_schema(
            Json{
                {"name", Json{{"type", "string"}, {"minLength", 1},
                               {"maxLength", max_scene_node_name_bytes}}},
                {"kind", kind_schema},
                {"parent", optional_parent_schema},
                {"asset_path", Json{{"type", "string"},
                                      {"maxLength", max_scene_asset_path_bytes}}},
                {"visible", Json{{"type", "boolean"}}},
                {"translation", vec3_schema},
                {"rotation_degrees", vec3_schema},
                {"scale", vec3_schema},
                {"base_color", color_schema},
                {"metallic", Json{{"type", "number"}, {"minimum", 0.0},
                                     {"maximum", 1.0}}},
                {"roughness", Json{{"type", "number"},
                                      {"minimum", minimum_pbr_roughness},
                                      {"maximum", 1.0}}},
                {"ambient_occlusion", Json{{"type", "number"},
                                              {"minimum", 0.0},
                                              {"maximum", 1.0}}},
            },
            Json::array({"name", "kind"})),
        false, false, false));
    tools.push_back(tool_definition(
        "editor.scene.update_node", "Update scene node",
        "Patch authored node identity, visibility, transform, or model asset reference.",
        object_schema(Json{
            {"id", handle_schema},
            {"name", Json{{"type", "string"}, {"minLength", 1},
                           {"maxLength", max_scene_node_name_bytes}}},
            {"asset_path", Json{{"type", "string"},
                                  {"maxLength", max_scene_asset_path_bytes}}},
            {"visible", Json{{"type", "boolean"}}},
            {"translation", vec3_schema},
            {"rotation_degrees", vec3_schema},
            {"scale", vec3_schema},
            {"base_color", color_schema},
            {"metallic", Json{{"type", "number"}, {"minimum", 0.0},
                                 {"maximum", 1.0}}},
            {"roughness", Json{{"type", "number"},
                                  {"minimum", minimum_pbr_roughness},
                                  {"maximum", 1.0}}},
            {"ambient_occlusion", Json{{"type", "number"},
                                          {"minimum", 0.0},
                                          {"maximum", 1.0}}},
        }, Json::array({"id"})), false, false, true));
    tools.push_back(tool_definition(
        "editor.scene.delete_node", "Delete scene subtree",
        "Delete one node and all descendants. The operation can be undone in editor history.",
        object_schema(Json{{"id", handle_schema}}, Json::array({"id"})),
        false, true, true));
    tools.push_back(tool_definition(
        "editor.scene.reparent_node", "Reparent scene node",
        "Move a node below another node or to the scene root; hierarchy cycles are rejected.",
        object_schema(
            Json{{"id", handle_schema}, {"parent", optional_parent_schema}},
            Json::array({"id", "parent"})),
        false, false, true));
    tools.push_back(tool_definition(
        "editor.scene.duplicate_node", "Duplicate scene subtree",
        "Duplicate one node and every descendant under the same parent as one undoable editor operation.",
        object_schema(Json{{"id", handle_schema}}, Json::array({"id"})),
        false, false, false));
    tools.push_back(tool_definition(
        "editor.camera.focus_selection", "Focus selected node",
        "Move the editor camera to frame the current selection without modifying the authored scene.",
        object_schema(), false, false, true));
    tools.push_back(tool_definition(
        "editor.scene.save", "Save authored scene",
        "Atomically save the canonical editor/game scene JSON.",
        object_schema(), false, false, true));
    tools.push_back(tool_definition(
        "editor.history.undo", "Undo scene edit",
        "Undo the latest bounded editor scene transaction.",
        object_schema(), false, false, false));
    tools.push_back(tool_definition(
        "editor.history.redo", "Redo scene edit",
        "Redo the latest undone editor scene transaction.",
        object_schema(), false, false, false));
    tools.push_back(tool_definition(
        "editor.assets.list", "List assets",
        "List one sandboxed directory below the project asset root.",
        object_schema(Json{
            {"directory", Json{{"type", "string"},
                               {"maxLength", max_scene_asset_path_bytes}}},
            {"offset", Json{{"type", "integer"}, {"minimum", 0}}},
            {"limit", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
        }),
        true, false, true));
    tools.push_back(tool_definition(
        "editor.assets.import", "Import source asset",
        "Queue a transactional background import into a directory below the asset root.",
        object_schema(
            Json{
                {"source_path", Json{{"type", "string"}, {"minLength", 1},
                                      {"maxLength", max_scene_asset_path_bytes}}},
                {"destination_directory", Json{{"type", "string"},
                                                {"maxLength", McpCommand::secondary_text_capacity - 1U}}},
            },
            Json::array({"source_path"})),
        false, false, false));
    tools.push_back(tool_definition(
        "editor.landscape.create_flat_ground", "Create flat terrain",
        "Create an explicitly sized flat authored ground plane.",
        object_schema(
            Json{
                {"name", Json{{"type", "string"}, {"minLength", 1},
                               {"maxLength", max_scene_node_name_bytes}}},
                {"parent", optional_parent_schema},
                {"center", vec3_schema},
                {"width", Json{{"type", "number"}, {"exclusiveMinimum", 0}}},
                {"depth", Json{{"type", "number"}, {"exclusiveMinimum", 0}}},
            },
            Json::array({"width", "depth"})),
        false, false, false));
    tools.push_back(tool_definition(
        "editor.landscape.scatter_model", "Scatter model instances",
        "Deterministically scatter an imported model over a flat XZ area using a seed.",
        object_schema(
            Json{
                {"asset_path", Json{{"type", "string"}, {"minLength", 1},
                                     {"maxLength", max_scene_asset_path_bytes}}},
                {"name_prefix", Json{{"type", "string"}, {"minLength", 1},
                                     {"maxLength", max_scene_node_name_bytes - 11U}}},
                {"parent", optional_parent_schema},
                {"center", vec3_schema},
                {"width", Json{{"type", "number"}, {"exclusiveMinimum", 0}}},
                {"depth", Json{{"type", "number"}, {"exclusiveMinimum", 0}}},
                {"minimum_scale", Json{{"type", "number"}, {"exclusiveMinimum", 0}}},
                {"maximum_scale", Json{{"type", "number"}, {"exclusiveMinimum", 0}}},
                {"count", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
                {"seed", Json{{"type", "integer"}, {"minimum", 0},
                               {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
            },
            Json::array({"asset_path", "count", "seed", "width", "depth"})),
        false, false, false));
    tools.push_back(tool_definition(
        "capture.screenshot", "Capture editor window",
        "Capture the next completed editor swapchain image as PNG image content.",
        object_schema(), true, false, false));
    tools.push_back(tool_definition(
        "engine.quit", "Quit the editor",
        "Gracefully stop the editor after this tool response is delivered.",
        object_schema(), false, true, true));
    return tools;
}

[[nodiscard]] Json make_tools(const McpServerProfile profile) {
    return profile == McpServerProfile::editor
        ? make_editor_tools()
        : make_runtime_tools();
}

[[nodiscard]] bool is_known_tool(
    const McpServerProfile profile,
    const std::string_view name) noexcept {
    if (profile == McpServerProfile::editor) {
        return std::find(
                   editor_tool_names.begin(), editor_tool_names.end(), name) !=
            editor_tool_names.end();
    }
    return std::find(
               runtime_tool_names.begin(), runtime_tool_names.end(), name) !=
        runtime_tool_names.end();
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

[[nodiscard]] bool copy_command_text(
    const std::string_view source,
    const std::span<char> destination,
    std::string& error,
    const std::string_view field_name) {
    if (source.size() >= destination.size() ||
        source.find('\0') != std::string_view::npos) {
        error = std::string(field_name) + " exceeds the bounded UTF-8 payload";
        return false;
    }
    std::memcpy(destination.data(), source.data(), source.size());
    destination[source.size()] = '\0';
    return true;
}

[[nodiscard]] bool only_keys(
    const Json& arguments,
    const std::initializer_list<std::string_view> allowed) {
    for (const auto& [key, value] : arguments.items()) {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parse_node_handle(
    const Json& value,
    SceneNodeId& output) {
    if (!value.is_object() || value.size() != 2U) {
        return false;
    }
    const auto index = get_u32(
        value, "index", 0U, std::numeric_limits<std::uint32_t>::max());
    const auto generation = get_u32(
        value, "generation", 1U, std::numeric_limits<std::uint32_t>::max());
    if (!index || !generation) {
        return false;
    }
    output = SceneNodeId{.index = *index, .generation = *generation};
    return true;
}

[[nodiscard]] bool parse_optional_parent(
    const Json& arguments,
    McpCommand& command,
    const bool required,
    std::string& error) {
    const auto iterator = arguments.find("parent");
    if (iterator == arguments.end()) {
        if (required) {
            error = "parent must be a node handle or null";
            return false;
        }
        return true;
    }
    command.parent_supplied = true;
    if (iterator->is_null()) {
        command.parent_id = {};
        return true;
    }
    if (!parse_node_handle(*iterator, command.parent_id)) {
        error = "parent must contain uint32 index and nonzero generation";
        return false;
    }
    return true;
}

[[nodiscard]] bool parse_vec3(
    const Json& arguments,
    const std::string_view name,
    glm::vec3& output) {
    const auto iterator = arguments.find(name);
    if (iterator == arguments.end() || !iterator->is_array() ||
        iterator->size() != 3U) {
        return false;
    }
    glm::vec3 parsed{};
    for (std::size_t index = 0U; index < 3U; ++index) {
        if (!(*iterator)[index].is_number()) {
            return false;
        }
        const double value = (*iterator)[index].get<double>();
        if (!std::isfinite(value) ||
            std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
            return false;
        }
        parsed[static_cast<glm::length_t>(index)] = static_cast<float>(value);
    }
    output = parsed;
    return true;
}

[[nodiscard]] bool parse_color(
    const Json& arguments,
    const std::string_view name,
    glm::vec4& output) {
    const auto iterator = arguments.find(name);
    if (iterator == arguments.end() || !iterator->is_array() ||
        iterator->size() != 4U) {
        return false;
    }
    glm::vec4 parsed{};
    for (std::size_t index = 0U; index < 4U; ++index) {
        if (!(*iterator)[index].is_number()) {
            return false;
        }
        const double value = (*iterator)[index].get<double>();
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return false;
        }
        parsed[static_cast<glm::length_t>(index)] = static_cast<float>(value);
    }
    output = parsed;
    return true;
}

[[nodiscard]] std::optional<float> get_finite_float(
    const Json& arguments,
    const std::string_view name,
    const float minimum,
    const float maximum) {
    const auto iterator = arguments.find(name);
    if (iterator == arguments.end() || !iterator->is_number()) {
        return std::nullopt;
    }
    const double value = iterator->get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        return std::nullopt;
    }
    return static_cast<float>(value);
}

[[nodiscard]] std::optional<SceneNodeKind> parse_node_kind(
    const std::string_view value) noexcept {
    if (value == "folder") return SceneNodeKind::folder;
    if (value == "cube") return SceneNodeKind::cube;
    if (value == "ground") return SceneNodeKind::ground;
    if (value == "terrain") return SceneNodeKind::terrain;
    if (value == "camera") return SceneNodeKind::camera;
    if (value == "directional_light") return SceneNodeKind::directional_light;
    if (value == "environment") return SceneNodeKind::environment;
    if (value == "model") return SceneNodeKind::model;
    return std::nullopt;
}

[[nodiscard]] bool parse_optional_material_fields(
    const Json& arguments,
    McpCommand& command,
    std::string& error) {
    if (arguments.contains("base_color")) {
        if (!parse_color(
                arguments, "base_color", command.material.base_color)) {
            error = "base_color must contain four finite values from 0 through 1";
            return false;
        }
        command.edit_mask |= McpCommand::edit_base_color;
    }
    const auto parse_factor = [&](const std::string_view key,
                                  float& output,
                                  const float minimum,
                                  const std::uint32_t mask) {
        if (!arguments.contains(key)) {
            return true;
        }
        const auto value = get_finite_float(
            arguments, key, minimum, 1.0F);
        if (!value) {
            error = std::string(key) + " must be finite and within its PBR range";
            return false;
        }
        output = *value;
        command.edit_mask |= mask;
        return true;
    };
    return parse_factor(
               "metallic", command.material.metallic, 0.0F,
               McpCommand::edit_metallic) &&
        parse_factor(
               "roughness", command.material.roughness,
               minimum_pbr_roughness, McpCommand::edit_roughness) &&
        parse_factor(
               "ambient_occlusion", command.material.ambient_occlusion, 0.0F,
               McpCommand::edit_ambient_occlusion);
}

[[nodiscard]] bool command_from_editor_tool(
    const std::string_view name,
    const Json& arguments,
    McpCommand& command,
    std::string& error) {
    const auto require_empty = [&]() {
        if (!arguments.empty()) {
            error = "this tool accepts no arguments";
            return false;
        }
        return true;
    };
    if (name == "editor.get_state") {
        command.type = McpCommandType::editor_get_state;
        return require_empty();
    }
    if (name == "editor.camera.focus_selection") {
        command.type = McpCommandType::editor_focus_selection;
        return require_empty();
    }
    if (name == "capture.screenshot") {
        command.type = McpCommandType::capture_screenshot;
        return require_empty();
    }
    if (name == "engine.quit") {
        command.type = McpCommandType::engine_request_quit;
        return require_empty();
    }
    if (name == "editor.scene.save") {
        command.type = McpCommandType::editor_save_scene;
        return require_empty();
    }
    if (name == "editor.history.undo" || name == "editor.history.redo") {
        command.type = name == "editor.history.undo"
            ? McpCommandType::editor_undo
            : McpCommandType::editor_redo;
        return require_empty();
    }
    if (name == "editor.scene.list_nodes") {
        if (!only_keys(arguments, {"offset", "limit"})) {
            error = "only offset and limit are accepted";
            return false;
        }
        command.type = McpCommandType::editor_list_nodes;
        command.value = 0U;
        command.count = 200U;
        if (arguments.contains("offset")) {
            const auto offset = get_u32(
                arguments, "offset", 0U,
                std::numeric_limits<std::uint32_t>::max());
            if (!offset) {
                error = "offset must be a uint32";
                return false;
            }
            command.value = *offset;
        }
        if (arguments.contains("limit")) {
            const auto limit = get_u32(arguments, "limit", 1U, 1000U);
            if (!limit) {
                error = "limit must be from 1 through 1000";
                return false;
            }
            command.count = *limit;
        }
        return true;
    }
    if (name == "editor.scene.get_node" ||
        name == "editor.scene.delete_node" ||
        name == "editor.scene.duplicate_node") {
        if (arguments.size() != 1U || !arguments.contains("id") ||
            !parse_node_handle(arguments["id"], command.node_id)) {
            error = "id must be the only argument and contain index/generation";
            return false;
        }
        command.type = name == "editor.scene.get_node"
            ? McpCommandType::editor_get_node
            : name == "editor.scene.delete_node"
            ? McpCommandType::editor_delete_node
            : McpCommandType::editor_duplicate_node;
        return true;
    }
    if (name == "editor.selection.set") {
        if (arguments.size() != 1U || !arguments.contains("id")) {
            error = "id must be the only argument and contain index/generation or null";
            return false;
        }
        if (!arguments["id"].is_null() &&
            !parse_node_handle(arguments["id"], command.node_id)) {
            error = "id must contain index/generation or be null";
            return false;
        }
        command.type = McpCommandType::editor_set_selection;
        return true;
    }
    if (name == "editor.scene.reparent_node") {
        if (!only_keys(arguments, {"id", "parent"}) ||
            arguments.size() != 2U || !arguments.contains("id") ||
            !parse_node_handle(arguments["id"], command.node_id) ||
            !parse_optional_parent(arguments, command, true, error)) {
            if (error.empty()) {
                error = "id and parent are required node handles; parent may be null";
            }
            return false;
        }
        command.type = McpCommandType::editor_reparent_node;
        return true;
    }
    if (name == "editor.scene.create_node") {
        if (!only_keys(arguments, {
                "name", "kind", "parent", "asset_path", "visible",
                "translation", "rotation_degrees", "scale", "base_color",
                "metallic", "roughness", "ambient_occlusion"})) {
            error = "create_node contains an unknown argument";
            return false;
        }
        const auto node_name = get_string(arguments, "name");
        const auto kind_text = get_string(arguments, "kind");
        const auto kind = kind_text ? parse_node_kind(*kind_text) : std::nullopt;
        if (!node_name || node_name->empty() || !kind ||
            !copy_command_text(*node_name, command.name, error, "name") ||
            !parse_optional_parent(arguments, command, false, error)) {
            if (error.empty()) {
                error = "name and a supported kind are required";
            }
            return false;
        }
        command.type = McpCommandType::editor_create_node;
        command.node_kind = *kind;
        if (arguments.contains("asset_path")) {
            const auto path = get_string(arguments, "asset_path");
            if (!path || !copy_command_text(*path, command.text, error, "asset_path")) {
                return false;
            }
            command.edit_mask |= McpCommand::edit_asset_path;
        }
        if (arguments.contains("visible")) {
            const auto visible = get_bool(arguments, "visible");
            if (!visible) {
                error = "visible must be a boolean";
                return false;
            }
            command.enabled = *visible;
            command.edit_mask |= McpCommand::edit_visible;
        }
        const auto parse_optional_vec = [&](const std::string_view key,
                                             glm::vec3& output,
                                             const std::uint32_t mask) {
            if (!arguments.contains(key)) return true;
            if (!parse_vec3(arguments, key, output)) {
                error = std::string(key) + " must contain three finite numbers";
                return false;
            }
            command.edit_mask |= mask;
            return true;
        };
        return parse_optional_vec("translation", command.transform.translation,
                                  McpCommand::edit_translation) &&
            parse_optional_vec("rotation_degrees", command.transform.rotation_degrees,
                               McpCommand::edit_rotation) &&
            parse_optional_vec("scale", command.transform.scale,
                               McpCommand::edit_scale) &&
            parse_optional_material_fields(arguments, command, error);
    }
    if (name == "editor.scene.update_node") {
        if (!only_keys(arguments, {
                "id", "name", "asset_path", "visible", "translation",
                "rotation_degrees", "scale", "base_color", "metallic",
                "roughness", "ambient_occlusion"}) ||
            !arguments.contains("id") ||
            !parse_node_handle(arguments["id"], command.node_id)) {
            error = "update_node requires a valid id and known patch fields";
            return false;
        }
        command.type = McpCommandType::editor_update_node;
        if (arguments.contains("name")) {
            const auto value = get_string(arguments, "name");
            if (!value || value->empty() ||
                !copy_command_text(*value, command.name, error, "name")) {
                return false;
            }
            command.edit_mask |= McpCommand::edit_name;
        }
        if (arguments.contains("asset_path")) {
            const auto value = get_string(arguments, "asset_path");
            if (!value || !copy_command_text(*value, command.text, error, "asset_path")) {
                return false;
            }
            command.edit_mask |= McpCommand::edit_asset_path;
        }
        if (arguments.contains("visible")) {
            const auto value = get_bool(arguments, "visible");
            if (!value) {
                error = "visible must be a boolean";
                return false;
            }
            command.enabled = *value;
            command.edit_mask |= McpCommand::edit_visible;
        }
        const auto parse_optional_vec = [&](const std::string_view key,
                                             glm::vec3& output,
                                             const std::uint32_t mask) {
            if (!arguments.contains(key)) return true;
            if (!parse_vec3(arguments, key, output)) {
                error = std::string(key) + " must contain three finite numbers";
                return false;
            }
            command.edit_mask |= mask;
            return true;
        };
        if (!parse_optional_vec("translation", command.transform.translation,
                                McpCommand::edit_translation) ||
            !parse_optional_vec("rotation_degrees", command.transform.rotation_degrees,
                                McpCommand::edit_rotation) ||
            !parse_optional_vec("scale", command.transform.scale,
                                McpCommand::edit_scale) ||
            !parse_optional_material_fields(arguments, command, error)) {
            return false;
        }
        if (command.edit_mask == 0U) {
            error = "update_node requires at least one patch field";
            return false;
        }
        return true;
    }
    if (name == "editor.assets.list") {
        if (!only_keys(arguments, {"directory", "offset", "limit"})) {
            error = "only directory, offset, and limit are accepted";
            return false;
        }
        command.type = McpCommandType::editor_list_assets;
        command.value = 0U;
        command.count = 200U;
        if (arguments.contains("directory")) {
            const auto directory = get_string(arguments, "directory");
            if (!directory ||
                !copy_command_text(*directory, command.text, error, "directory")) {
                return false;
            }
        }
        if (arguments.contains("offset")) {
            const auto offset = get_u32(
                arguments, "offset", 0U,
                std::numeric_limits<std::uint32_t>::max());
            if (!offset) {
                error = "offset must be a uint32";
                return false;
            }
            command.value = *offset;
        }
        if (arguments.contains("limit")) {
            const auto limit = get_u32(arguments, "limit", 1U, 1000U);
            if (!limit) {
                error = "limit must be from 1 through 1000";
                return false;
            }
            command.count = *limit;
        }
        return true;
    }
    if (name == "editor.assets.import") {
        if (!only_keys(arguments, {"source_path", "destination_directory"})) {
            error = "only source_path and destination_directory are accepted";
            return false;
        }
        const auto source = get_string(arguments, "source_path");
        if (!source || source->empty() ||
            !copy_command_text(*source, command.text, error, "source_path")) {
            return false;
        }
        if (arguments.contains("destination_directory")) {
            const auto destination = get_string(arguments, "destination_directory");
            if (!destination || !copy_command_text(
                    *destination, command.secondary_text, error,
                    "destination_directory")) {
                return false;
            }
        }
        command.type = McpCommandType::editor_import_asset;
        return true;
    }
    if (name == "editor.landscape.create_flat_ground") {
        if (!only_keys(arguments, {"name", "parent", "center", "width", "depth"}) ||
            !parse_optional_parent(arguments, command, false, error)) {
            if (error.empty()) error = "flat-ground arguments are invalid";
            return false;
        }
        const auto width = get_finite_float(arguments, "width", 0.001F, 8192.0F);
        const auto depth = get_finite_float(arguments, "depth", 0.001F, 8192.0F);
        if (!width || !depth) {
            error = "width and depth must be finite positive values up to 8192";
            return false;
        }
        if (arguments.contains("center") &&
            !parse_vec3(arguments, "center", command.transform.translation)) {
            error = "center must contain three finite numbers";
            return false;
        }
        const auto optional_name = get_string(arguments, "name");
        if (arguments.contains("name") && !optional_name) {
            error = "name must be a string";
            return false;
        }
        const std::string_view ground_name = optional_name
            ? *optional_name
            : std::string_view{"Flat Terrain"};
        if (ground_name.empty() ||
            !copy_command_text(ground_name, command.name, error, "name")) {
            return false;
        }
        command.type = McpCommandType::editor_create_flat_ground;
        command.extent_x = *width;
        command.extent_z = *depth;
        return true;
    }
    if (name == "editor.landscape.scatter_model") {
        if (!only_keys(arguments, {
                "asset_path", "name_prefix", "parent", "center", "width",
                "depth", "minimum_scale", "maximum_scale", "count", "seed"}) ||
            !parse_optional_parent(arguments, command, false, error)) {
            if (error.empty()) error = "scatter arguments are invalid";
            return false;
        }
        const auto asset_path = get_string(arguments, "asset_path");
        const auto count = get_u32(arguments, "count", 1U, 10'000U);
        const auto seed = get_u32(
            arguments, "seed", 0U, std::numeric_limits<std::uint32_t>::max());
        const auto width = get_finite_float(arguments, "width", 0.001F, 8192.0F);
        const auto depth = get_finite_float(arguments, "depth", 0.001F, 8192.0F);
        if (!asset_path || !count || !seed || !width || !depth ||
            !copy_command_text(*asset_path, command.text, error, "asset_path")) {
            if (error.empty()) error = "asset_path, count, seed, width, and depth are required";
            return false;
        }
        const auto optional_prefix = get_string(arguments, "name_prefix");
        if (arguments.contains("name_prefix") && !optional_prefix) {
            error = "name_prefix must be a string";
            return false;
        }
        const std::string_view prefix = optional_prefix
            ? *optional_prefix
            : std::string_view{"Plant"};
        if (prefix.empty() ||
            !copy_command_text(prefix, command.name, error, "name_prefix")) {
            return false;
        }
        if (arguments.contains("center") &&
            !parse_vec3(arguments, "center", command.transform.translation)) {
            error = "center must contain three finite numbers";
            return false;
        }
        command.minimum_scale = 0.8F;
        command.maximum_scale = 1.2F;
        if (arguments.contains("minimum_scale")) {
            const auto value = get_finite_float(
                arguments, "minimum_scale", 0.001F, 100.0F);
            if (!value) {
                error = "minimum_scale must be positive and at most 100";
                return false;
            }
            command.minimum_scale = *value;
        }
        if (arguments.contains("maximum_scale")) {
            const auto value = get_finite_float(
                arguments, "maximum_scale", 0.001F, 100.0F);
            if (!value) {
                error = "maximum_scale must be positive and at most 100";
                return false;
            }
            command.maximum_scale = *value;
        }
        if (command.minimum_scale > command.maximum_scale) {
            error = "minimum_scale must not exceed maximum_scale";
            return false;
        }
        command.type = McpCommandType::editor_scatter_model;
        command.count = *count;
        command.seed = *seed;
        command.extent_x = *width;
        command.extent_z = *depth;
        return true;
    }

    error = "unknown editor tool";
    return false;
}

[[nodiscard]] bool command_from_tool(
    const McpServerProfile profile,
    const std::string_view name,
    const Json& arguments,
    const std::uint64_t token,
    McpCommand& command,
    std::string& error) {
    command = {};
    command.token = token;

    if (profile == McpServerProfile::editor) {
        return command_from_editor_tool(name, arguments, command, error);
    }

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
    if (name == "test.set_roof") {
        if (arguments.size() != 1 || !arguments.contains("enabled") ||
            !arguments["enabled"].is_boolean()) {
            error = "enabled must be the only boolean argument";
            return false;
        }
        command.type = McpCommandType::test_set_roof;
        command.enabled = arguments["enabled"].get<bool>();
        return true;
    }
    if (name == "render.set_shadow_resolution") {
        const auto resolution = get_u32(arguments, "resolution", shadow_resolutions.front(),
                                         shadow_resolutions.back());
        if (arguments.size() != 1 || !resolution || !valid_shadow_resolution(*resolution)) {
            error = "resolution must be 1024, 2048, 4096 or 8192";
            return false;
        }
        command.type = McpCommandType::render_set_shadow_resolution;
        command.value = *resolution;
        return true;
    }
    if (name == "lighting.set") {
        if (arguments.size() != 3 || !arguments.contains("hour") ||
            !arguments["hour"].is_number() || !arguments.contains("cycle") ||
            !arguments["cycle"].is_boolean() || !arguments.contains("gi") ||
            !arguments["gi"].is_boolean()) {
            error = "hour, cycle and gi are required";
            return false;
        }
        const double hour = arguments["hour"].get<double>();
        if (!std::isfinite(hour) || hour < 0 || hour >= 24) {
            error = "hour must be in [0,24)";
            return false;
        }
        command.type = McpCommandType::lighting_set;
        command.lighting.hour = hour;
        command.lighting.cycle = arguments["cycle"].get<bool>();
        command.lighting.voxel_gi = arguments["gi"].get<bool>();
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
                const bool editor_profile =
                    profile_ == McpServerProfile::editor;
                const Json result{
                    {"protocolVersion", protocol_version},
                    {"capabilities", Json{
                        {"tools", Json{{"listChanged", false}}},
                    }},
                    {"serverInfo", Json{
                        {"name", editor_profile
                            ? "vulkan-cube-editor"
                            : "vulkan-cube-renderer"},
                        {"title", editor_profile
                            ? "Vulkan Cube AI Level Editor"
                            : "Vulkan Cube AI Control Plane"},
                        {"version", "0.3.0"},
                        {"description",
                         editor_profile
                            ? "Live MCP authoring plane for static game levels"
                            : "Low-overhead MCP control plane for the Vulkan renderer"},
                    }},
                    {"instructions",
                     editor_profile
                        ? "Inspect nodes and assets before editing. Use generational "
                          "handles returned by list/get, batch vegetation through "
                          "landscape.scatter_model, save explicitly, and visually "
                          "verify with capture.screenshot."
                        : "Use debug and test tools to inspect state, render/window "
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
                        id, Json{{"tools", make_tools(profile_)}}))) {
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
            if (!is_known_tool(profile_, tool_name)) {
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
                    profile_,
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
