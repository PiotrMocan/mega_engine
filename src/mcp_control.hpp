#pragma once

#include "cli_options.hpp"
#include "scene_document.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace vcr {

enum class McpCommandType : std::uint8_t {
    debug_get_capabilities,
    debug_get_state,
    debug_get_metrics,
    debug_reset_metrics,
    test_run_self_check,
    capture_screenshot,
    render_set_mode,
    render_set_instances,
    render_set_gpu_workload,
    lighting_set,
    render_set_shadow_resolution,
    test_set_roof,
    render_set_present_mode,
    window_set_fullscreen,
    window_set_size,
    window_set_mouse_capture,
    input_key,
    input_mouse_move,
    input_mouse_button,
    input_release_all,
    editor_get_state,
    editor_list_nodes,
    editor_get_node,
    editor_create_node,
    editor_update_node,
    editor_delete_node,
    editor_reparent_node,
    editor_save_scene,
    editor_undo,
    editor_redo,
    editor_list_assets,
    editor_import_asset,
    editor_create_flat_ground,
    editor_scatter_model,
    engine_request_quit,
    engine_commit_quit,
};

enum class McpInputKey : std::uint8_t {
    w,
    a,
    s,
    d,
    shift,
    space,
    e,
    control,
    q,
    escape,
    v,
    f,
    r,
    p,
    digit_1,
    digit_2,
    digit_3,
    f6,
    f7,
    f8,
    arrow_up,
    arrow_down,
};

enum class McpInputAction : std::uint8_t {
    down,
    up,
    press,
};

enum class McpMouseButton : std::uint8_t {
    left,
};

struct McpCommand {
    static constexpr std::size_t text_capacity =
        max_scene_asset_path_bytes + 1U;
    static constexpr std::size_t secondary_text_capacity = 1'024U;
    static constexpr std::uint32_t edit_name = 1U << 0U;
    static constexpr std::uint32_t edit_visible = 1U << 1U;
    static constexpr std::uint32_t edit_translation = 1U << 2U;
    static constexpr std::uint32_t edit_rotation = 1U << 3U;
    static constexpr std::uint32_t edit_scale = 1U << 4U;
    static constexpr std::uint32_t edit_asset_path = 1U << 5U;
    static constexpr std::uint32_t edit_base_color = 1U << 6U;
    static constexpr std::uint32_t edit_metallic = 1U << 7U;
    static constexpr std::uint32_t edit_roughness = 1U << 8U;
    static constexpr std::uint32_t edit_ambient_occlusion = 1U << 9U;

    std::uint64_t token = 0;
    McpCommandType type = McpCommandType::debug_get_state;
    RunMode mode = RunMode::scene;
    PresentPreference present = PresentPreference::automatic;
    McpInputKey key = McpInputKey::w;
    McpInputAction input_action = McpInputAction::press;
    McpMouseButton mouse_button = McpMouseButton::left;
    std::uint32_t value = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float mouse_delta_x = 0.0F;
    float mouse_delta_y = 0.0F;
    bool enabled = false;
    LightingSettings lighting{};
    SceneNodeId node_id{};
    SceneNodeId parent_id{};
    SceneNodeKind node_kind = SceneNodeKind::folder;
    SceneTransform transform{};
    PbrMaterial material{};
    std::array<char, max_scene_node_name_bytes + 1U> name{};
    std::array<char, text_capacity> text{};
    std::array<char, secondary_text_capacity> secondary_text{};
    std::uint32_t edit_mask = 0U;
    std::uint32_t count = 0U;
    std::uint32_t seed = 0U;
    float center_x = 0.0F;
    float center_y = 0.0F;
    float center_z = 0.0F;
    float extent_x = 0.0F;
    float extent_z = 0.0F;
    float minimum_scale = 1.0F;
    float maximum_scale = 1.0F;
    bool parent_supplied = false;
};

static_assert(std::is_trivially_copyable_v<McpCommand>);

struct McpImage {
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct McpCommandResult {
    std::uint64_t token = 0;
    bool success = false;
    std::string message;
    std::string structured_json{"{}"};
    McpImage image{};
};

static_assert(std::is_nothrow_move_assignable_v<McpCommandResult>);

#if defined(_MSC_VER)
#pragma warning(push)
// Cache-line separation is intentional: producer and consumer update distinct
// indices, so MSVC's alignment-padding warning does not indicate wasted layout.
#pragma warning(disable : 4324)
#endif

// Single-producer/single-consumer bounded queue. MCP JSON parsing and response
// formatting stay on the I/O thread; the render thread only moves typed
// commands and results across this queue.
template <typename T, std::size_t Capacity>
class SpscQueue final {
    static_assert(Capacity >= 2);

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    [[nodiscard]] bool try_push(T value) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = increment(head);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[head] = std::move(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& value) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        value = std::move(slots_[tail]);
        tail_.store(increment(tail), std::memory_order_release);
        return true;
    }

private:
    [[nodiscard]] static constexpr std::size_t increment(
        const std::size_t value) noexcept {
        return (value + 1U) % Capacity;
    }

    std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

class McpControlPlane final {
public:
    static constexpr std::size_t queue_capacity = 64;

    McpControlPlane() = default;
    McpControlPlane(const McpControlPlane&) = delete;
    McpControlPlane& operator=(const McpControlPlane&) = delete;

    // MCP I/O thread only.
    [[nodiscard]] bool submit(McpCommand command) noexcept {
        return commands_.try_push(std::move(command));
    }

    // Main/render thread only.
    [[nodiscard]] bool try_next_command(McpCommand& command) noexcept {
        return commands_.try_pop(command);
    }

    // Main/render thread only.
    [[nodiscard]] bool complete(McpCommandResult result) noexcept {
        return results_.try_push(std::move(result));
    }

    // MCP I/O thread only.
    [[nodiscard]] bool try_next_result(McpCommandResult& result) noexcept {
        return results_.try_pop(result);
    }

private:
    SpscQueue<McpCommand, queue_capacity> commands_{};
    SpscQueue<McpCommandResult, queue_capacity> results_{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace vcr
