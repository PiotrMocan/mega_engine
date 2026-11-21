#include "vegetation_studio_mcp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <iostream>
#include <mutex>
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
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::string_view kProtocolVersion{"2025-11-25"};
constexpr std::size_t kMaxInputMessageBytes = 1024U * 1024U;

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
                    if (newline > kMaxInputMessageBytes) {
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
                if (pending_.size() > kMaxInputMessageBytes) {
                    pending_.clear();
                    discard_until_newline_ = true;
                    return LineStatus::too_large;
                }
            }

            std::array<char, 4096U> chunk{};
            std::size_t bytes_read = 0U;
            const ChunkStatus status = read_chunk(stop_token, chunk, bytes_read);
            if (status == ChunkStatus::idle) {
                continue;
            }
            if (status == ChunkStatus::data) {
                pending_.append(chunk.data(), bytes_read);
                continue;
            }
            if (status == ChunkStatus::end_of_stream) {
                if (!pending_.empty() && !discard_until_newline_) {
                    if (pending_.size() > kMaxInputMessageBytes) {
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
        bytes_read = 0U;
#if defined(_WIN32)
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE) {
            return ChunkStatus::error;
        }
        const DWORD type = GetFileType(input);
        if (type == FILE_TYPE_PIPE) {
            DWORD available = 0U;
            if (!PeekNamedPipe(input, nullptr, 0U, nullptr, &available, nullptr)) {
                return GetLastError() == ERROR_BROKEN_PIPE
                    ? ChunkStatus::end_of_stream
                    : ChunkStatus::error;
            }
            if (available == 0U) {
                std::this_thread::sleep_for(10ms);
                return ChunkStatus::idle;
            }
            DWORD read = 0U;
            const DWORD requested = static_cast<DWORD>(
                std::min<std::size_t>(output.size(), available));
            if (!ReadFile(input, output.data(), requested, &read, nullptr)) {
                return GetLastError() == ERROR_BROKEN_PIPE
                    ? ChunkStatus::end_of_stream
                    : ChunkStatus::error;
            }
            bytes_read = read;
            return read == 0U ? ChunkStatus::end_of_stream : ChunkStatus::data;
        }
        if (type == FILE_TYPE_DISK) {
            DWORD read = 0U;
            if (!ReadFile(
                    input,
                    output.data(),
                    static_cast<DWORD>(output.size()),
                    &read,
                    nullptr)) {
                return ChunkStatus::error;
            }
            bytes_read = read;
            return read == 0U ? ChunkStatus::end_of_stream : ChunkStatus::data;
        }
#endif
        std::string fallback_line;
        if (!std::getline(std::cin, fallback_line)) {
            return ChunkStatus::end_of_stream;
        }
        fallback_line.push_back('\n');
        const std::size_t copy_len = std::min(fallback_line.size(), output.size());
        std::copy_n(fallback_line.data(), copy_len, output.data());
        bytes_read = copy_len;
        return ChunkStatus::data;
    }

    std::string pending_{};
    bool discard_until_newline_ = false;
};

std::mutex g_stdout_write_mutex;

[[nodiscard]] bool write_rpc_message(const Json& message) noexcept {
    try {
        std::string serialized = message.dump();
        serialized.push_back('\n');
        std::lock_guard<std::mutex> lock(g_stdout_write_mutex);
        const std::size_t written = std::fwrite(
            serialized.data(), 1U, serialized.size(), stdout);
        return written == serialized.size() && std::fflush(stdout) == 0;
    } catch (const std::exception& ex) {
        std::cerr << "Vegetation Studio MCP write error: " << ex.what() << '\n';
        return false;
    }
}

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

[[nodiscard]] std::string base64_encode(const std::span<const std::uint8_t> bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2U) / 3U) * 4U);
    std::size_t index = 0U;
    while (index + 3U <= bytes.size()) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (static_cast<std::uint32_t>(bytes[index + 1U]) << 8U) |
            static_cast<std::uint32_t>(bytes[index + 2U]);
        output.push_back(kAlphabet[(value >> 18U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 12U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 6U) & 0x3fU]);
        output.push_back(kAlphabet[value & 0x3fU]);
        index += 3U;
    }
    const std::size_t rem = bytes.size() - index;
    if (rem == 1U) {
        const std::uint32_t value = static_cast<std::uint32_t>(bytes[index]) << 16U;
        output.push_back(kAlphabet[(value >> 18U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 12U) & 0x3fU]);
        output.push_back('=');
        output.push_back('=');
    } else if (rem == 2U) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (static_cast<std::uint32_t>(bytes[index + 1U]) << 8U);
        output.push_back(kAlphabet[(value >> 18U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 12U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 6U) & 0x3fU]);
        output.push_back('=');
    }
    return output;
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

[[nodiscard]] Json vec3_array_schema(const std::string_view description) {
    return Json{
        {"type", "array"},
        {"description", description},
        {"items", Json{{"type", "number"}}},
        {"minItems", 3},
        {"maxItems", 3},
    };
}

[[nodiscard]] Json make_tool_entry(
    const std::string_view name,
    const std::string_view description,
    Json input_schema,
    const bool read_only,
    const bool idempotent) {
    return Json{
        {"name", name},
        {"description", description},
        {"inputSchema", std::move(input_schema)},
        {"annotations", Json{
            {"readOnlyHint", read_only},
            {"idempotentHint", idempotent},
            {"destructiveHint", false},
            {"openWorldHint", false},
        }},
    };
}

[[nodiscard]] Json build_tool_list() {
    Json tools = Json::array();

    tools.push_back(make_tool_entry(
        "veg.list_presets",
        "List all built-in procedural vegetation presets grouped by archetype.",
        object_schema(),
        true,
        true));

    tools.push_back(make_tool_entry(
        "veg.get_state",
        "Return current vegetation recipe, LOD optimization budget report, active stress arena, and live telemetry.",
        object_schema(),
        true,
        true));

    tools.push_back(make_tool_entry(
        "veg.apply_preset",
        "Load a curated procedural vegetation preset by preset_id with optional deterministic seed override.",
        object_schema(
            Json{
                {"preset_id", Json{{"type", "string"}, {"description", "Built-in preset id from veg.list_presets."}}},
                {"seed", Json{{"type", "integer"}, {"minimum", 0}, {"description", "Optional deterministic uint32 seed override."}}},
            },
            Json::array({"preset_id"})),
        false,
        true));

    Json set_recipe_schema{
        {"type", "object"},
        {"description", "Partial or full recipe JSON fields (either top-level or nested under 'recipe') merged into the active VegetationRecipe."},
        {"additionalProperties", true},
    };
    tools.push_back(make_tool_entry(
        "veg.set_recipe",
        "Merge partial or full procedural parameters (archetype, height_meters, foliage_mode, colors, branches, fronds, etc.) into the current recipe and regenerate LODs.",
        std::move(set_recipe_schema),
        false,
        true));

    tools.push_back(make_tool_entry(
        "veg.randomize_seed",
        "Assign a new deterministic seed (or explicit seed) and regenerate geometry and textures.",
        object_schema(
            Json{
                {"seed", Json{{"type", "integer"}, {"minimum", 0}}},
            }),
        false,
        false));

    tools.push_back(make_tool_entry(
        "veg.paint_foliage",
        "Apply a 3D spherical foliage paint or pruning stroke in plant-local meters.",
        object_schema(
            Json{
                {"mode", Json{
                    {"type", "string"},
                    {"enum", Json::array({"tint", "autumn", "dry", "prune_bare"})},
                }},
                {"center", vec3_array_schema("3D stroke center in plant-local coordinates (meters).")},
                {"radius", Json{{"type", "number"}, {"minimum", 0.05}, {"maximum", 12.0}}},
                {"strength", Json{{"type", "number"}, {"minimum", 0.05}, {"maximum", 1.0}}},
                {"falloff", Json{{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
                {"color", vec3_array_schema("Optional sRGB [r,g,b] tint color for mode='tint'.")},
            },
            Json::array({"mode", "center"})),
        false,
        false));

    tools.push_back(make_tool_entry(
        "veg.clear_paint",
        "Remove all interactive 3D crown paint and pruning strokes from the active plant.",
        object_schema(),
        false,
        true));

    tools.push_back(make_tool_entry(
        "veg.undo_paint", "Undo the last crown brush gesture or clear.", object_schema(), false, false));
    tools.push_back(make_tool_entry(
        "veg.redo_paint", "Restore the last undone crown brush gesture or clear.", object_schema(), false, false));
    tools.push_back(make_tool_entry(
        "veg.set_arena",
        "Switch the live preview arena (hero_single, lod_lineup, grove_100, forest_2500, meadow_10000) and optional spacing.",
        object_schema(
            Json{
                {"mode", Json{
                    {"type", "string"},
                    {"enum", Json::array({"hero_single", "lod_lineup", "grove_100", "forest_2500", "meadow_10000"})},
                }},
                {"spacing", Json{{"type", "number"}, {"minimum", 0.5}, {"maximum", 60.0}}},
            },
            Json::array({"mode"})),
        false,
        true));

    tools.push_back(make_tool_entry(
        "veg.set_camera",
        "Position or re-frame the preview viewport camera.",
        object_schema(
            Json{
                {"position", vec3_array_schema("Camera world position [x,y,z].")},
                {"target", vec3_array_schema("Optional look-at target [x,y,z].")},
                {"frame_bounds", Json{{"type", "boolean"}, {"description", "Automatically frame the current arena bounds."}}},
                {"fov_degrees", Json{{"type", "number"}, {"minimum", 10.0}, {"maximum", 140.0}}},
            }),
        false,
        true));

    tools.push_back(make_tool_entry(
        "veg.export_to_project",
        "Export cooked engine assets (*.vcrasset.json, *.vcrmesh, *.vcrmaterial.json, *.vcrtexture.json, PNGs) plus *.vcrveg.json sidecar into a project's assets directory.",
        object_schema(
            Json{
                {"project_dir", Json{{"type", "string"}}},
                {"category_subdir", Json{{"type", "string"}}},
                {"asset_slug", Json{{"type", "string"}}},
            }),
        false,
        true));

    tools.push_back(make_tool_entry(
        "veg.open_from_project",
        "Load a *.vcrveg.json or *.vcrasset.json vegetation package from disk for lossless editing.",
        object_schema(
            Json{
                {"path", Json{{"type", "string"}}},
            },
            Json::array({"path"})),
        false,
        true));

    tools.push_back(make_tool_entry(
        "veg.list_project_vegetation",
        "Discover *.vcrveg.json vegetation packages inside the active or specified project directory.",
        object_schema(
            Json{
                {"project_dir", Json{{"type", "string"}}},
            }),
        true,
        true));

    tools.push_back(make_tool_entry(
        "render.get_metrics",
        "Return real-time Vulkan FPS, 1% low, CPU/GPU timings, active LOD instance counts, and resident GPU memory.",
        object_schema(),
        true,
        true));

    tools.push_back(make_tool_entry(
        "capture.screenshot",
        "Capture the rendered Vulkan swapchain frame as PNG (returned inline as MCP ImageContent and optionally saved to output_path).",
        object_schema(
            Json{
                {"output_path", Json{{"type", "string"}}},
            }),
        true,
        false));

    tools.push_back(make_tool_entry(
        "engine.quit",
        "Cleanly exit Vulkan Vegetation Studio.",
        object_schema(),
        false,
        true));

    return tools;
}

[[nodiscard]] bool is_known_tool(const std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 17U> kKnownTools{
        "veg.list_presets",
        "veg.get_state",
        "veg.apply_preset",
        "veg.set_recipe",
        "veg.randomize_seed",
        "veg.paint_foliage",
        "veg.clear_paint",
        "veg.undo_paint",
        "veg.redo_paint",
        "veg.set_arena",
        "veg.set_camera",
        "veg.export_to_project",
        "veg.open_from_project",
        "veg.list_project_vegetation",
        "render.get_metrics",
        "capture.screenshot",
        "engine.quit",
    };
    return std::find(kKnownTools.begin(), kKnownTools.end(), name) != kKnownTools.end();
}

} // namespace

VegetationStudioMcpServer::VegetationStudioMcpServer(
    VegetationStudioMcpControl& control) noexcept
    : control_(control) {}

VegetationStudioMcpServer::~VegetationStudioMcpServer() {
    stop();
}

void VegetationStudioMcpServer::start() {
    if (worker_.joinable()) {
        return;
    }
    worker_ = std::jthread([this](const std::stop_token st) { run(st); });
}

void VegetationStudioMcpServer::stop() noexcept {
    control_.notify_response_available();
    if (!worker_.joinable()) {
        return;
    }
    worker_.request_stop();
    control_.notify_response_available();
    worker_.join();
}

void VegetationStudioMcpServer::run(const std::stop_token stop_token) {
    try {
        StdinLineReader reader;
        while (!stop_token.stop_requested() &&
               !control_.quit_requested.load(std::memory_order_acquire)) {
            std::string line;
            const LineStatus status = reader.read_line(stop_token, line);
            if (status == LineStatus::stopped) {
                break;
            }
            if (status == LineStatus::end_of_stream) {
                control_.quit_requested.store(true, std::memory_order_release);
                break;
            }
            if (status == LineStatus::too_large) {
                if (!write_rpc_message(rpc_error(
                        nullptr,
                        -32700,
                        "MCP message exceeds 1 MiB limit."))) {
                    break;
                }
                continue;
            }
            if (status == LineStatus::error) {
                control_.quit_requested.store(true, std::memory_order_release);
                break;
            }
            if (line.empty()) {
                continue;
            }
            handle_line(line);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Vegetation Studio MCP server exception: " << ex.what() << '\n';
        control_.quit_requested.store(true, std::memory_order_release);
    }
}

void VegetationStudioMcpServer::handle_line(const std::string& line) {
    const Json request = Json::parse(line, nullptr, false);
    if (request.is_discarded()) {
        static_cast<void>(write_rpc_message(
            rpc_error(nullptr, -32700, "Invalid JSON.")));
        return;
    }
    if (!request.is_object() ||
        !request.contains("jsonrpc") ||
        !request["jsonrpc"].is_string() ||
        request["jsonrpc"] != "2.0" ||
        !request.contains("method") ||
        !request["method"].is_string()) {
        const Json id = (request.is_object() && request.contains("id"))
            ? request["id"]
            : Json(nullptr);
        static_cast<void>(write_rpc_message(
            rpc_error(id, -32600, "Invalid JSON-RPC 2.0 request.")));
        return;
    }

    const std::string method = request["method"].get<std::string>();
    const bool has_id = request.contains("id");
    const Json id = has_id ? request["id"] : Json(nullptr);

    if (method == "notifications/initialized" ||
        method == "notifications/cancelled" ||
        method == "notifications/cancelled_task") {
        return;
    }
    if (!has_id) {
        return;
    }

    if (method == "initialize") {
        initialized_ = true;
        const Json result{
            {"protocolVersion", kProtocolVersion},
            {"capabilities", Json{
                {"tools", Json{{"listChanged", false}}},
            }},
            {"serverInfo", Json{
                {"name", "vulkan_vegetation_studio"},
                {"title", "Vulkan Vegetation Studio"},
                {"version", "0.1.0"},
            }},
            {"instructions",
             "Inspect presets with veg.list_presets and current asset telemetry with "
             "veg.get_state. Modify growth/foliage via veg.apply_preset / veg.set_recipe / "
             "veg.paint_foliage, test LOD and instancing scalability with veg.set_arena and "
             "render.get_metrics, capture PNG verification with capture.screenshot, and "
             "publish cooked engine assets via veg.export_to_project."},
        };
        static_cast<void>(write_rpc_message(rpc_response(id, result)));
        return;
    }

    if (method == "ping") {
        static_cast<void>(write_rpc_message(rpc_response(id, Json::object())));
        return;
    }

    if (!initialized_) {
        static_cast<void>(write_rpc_message(
            rpc_error(id, -32002, "Server is not initialized.")));
        return;
    }

    if (method == "tools/list") {
        static_cast<void>(write_rpc_message(
            rpc_response(id, Json{{"tools", build_tool_list()}})));
        return;
    }

    if (method == "tools/call") {
        const Json params = request.contains("params")
            ? request["params"]
            : Json::object();
        handle_tools_call(id, params);
        return;
    }

    static_cast<void>(write_rpc_message(
        rpc_error(id, -32601, "Method not found.")));
}

void VegetationStudioMcpServer::handle_tools_call(
    const nlohmann::json& id,
    const nlohmann::json& params) {
    if (!params.is_object() ||
        !params.contains("name") ||
        !params["name"].is_string()) {
        static_cast<void>(write_rpc_message(
            rpc_error(id, -32602, "tools/call requires a string params.name.")));
        return;
    }

    const std::string tool_name = params["name"].get<std::string>();
    if (!is_known_tool(tool_name)) {
        static_cast<void>(write_rpc_message(
            rpc_error(id, -32602, "Unknown tool name.")));
        return;
    }

    Json arguments = Json::object();
    if (params.contains("arguments")) {
        if (!params["arguments"].is_object()) {
            static_cast<void>(write_rpc_message(
                rpc_error(id, -32602, "tools/call params.arguments must be an object.")));
            return;
        }
        arguments = params["arguments"];
    }

    VegMcpResponse response{};
    if (!dispatch_and_wait(tool_name, arguments, response)) {
        static_cast<void>(write_rpc_message(
            rpc_error(id, -32000, response.error.empty()
                ? "Vegetation Studio shut down before completing tool call."
                : response.error)));
        return;
    }

    Json result{};
    if (!response.ok) {
        const std::string err_text = response.error.empty()
            ? "Vegetation Studio tool call failed."
            : response.error;
        result = Json{
            {"isError", true},
            {"structuredContent", Json{{"error", err_text}}},
            {"content", Json::array({Json{
                {"type", "text"},
                {"text", err_text},
            }})},
        };
    } else {
        Json structured = response.payload.is_object()
            ? response.payload
            : Json::object();
        Json content = Json::array({Json{
            {"type", "text"},
            {"text", structured.dump(2)},
        }});
        if (!response.screenshot_png.empty()) {
            content.push_back(Json{
                {"type", "image"},
                {"mimeType", "image/png"},
                {"data", base64_encode(response.screenshot_png)},
            });
        }
        result = Json{
            {"isError", false},
            {"structuredContent", std::move(structured)},
            {"content", std::move(content)},
        };
    }

    static_cast<void>(write_rpc_message(rpc_response(id, std::move(result))));

    if (tool_name == "engine.quit" && response.ok) {
        control_.quit_requested.store(true, std::memory_order_release);
    }
}

bool VegetationStudioMcpServer::dispatch_and_wait(
    const std::string_view tool_name,
    const nlohmann::json& arguments,
    VegMcpResponse& out_response) {
    const std::uint64_t request_id =
        next_request_id_.fetch_add(1U, std::memory_order_relaxed);
    VegMcpCommand cmd{
        .request_id = request_id,
        .tool_name = std::string(tool_name),
        .arguments = arguments,
    };

    const std::stop_token st = worker_.get_stop_token();
    while (!st.stop_requested() &&
           !control_.quit_requested.load(std::memory_order_acquire)) {
        if (control_.requests.try_push(cmd)) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    if (st.stop_requested() ||
        control_.quit_requested.load(std::memory_order_acquire)) {
        out_response.ok = false;
        out_response.error = "Vegetation Studio MCP server stopping.";
        return false;
    }

    while (!st.stop_requested()) {
        std::unique_lock<std::mutex> lock(control_.response_mutex);
        static_cast<void>(control_.response_cv.wait_for(lock, 50ms, [&]() {
            if (control_.responses.try_pop(out_response)) {
                return true;
            }
            return st.stop_requested() ||
                control_.quit_requested.load(std::memory_order_acquire);
        }));
        if (out_response.request_id == request_id) {
            return true;
        }
        if (out_response.request_id != 0U) {
            if (control_.quit_requested.load(std::memory_order_acquire)) {
                return false;
            }
            out_response = VegMcpResponse{};
            continue;
        }
        if (st.stop_requested() ||
            control_.quit_requested.load(std::memory_order_acquire)) {
            out_response.ok = false;
            out_response.error = "Vegetation Studio quit requested.";
            return false;
        }
    }
    out_response.ok = false;
    out_response.error = "Vegetation Studio MCP worker stopped.";
    return false;
}

} // namespace vcr
