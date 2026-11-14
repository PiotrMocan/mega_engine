#pragma once

#include "cli_options.hpp"
#include "cooked_model_asset.hpp"
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
    render_set_shadow_cache,
    render_set_tone_mapper,
    render_set_exposure,
    render_set_color_grading,
    render_set_subsurface,
    render_set_ray_tracing,
    render_set_motion_blur,
    render_set_anti_aliasing,
    render_set_depth_prepass,
    render_set_lod_fade,
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
    editor_set_selection,
    editor_list_nodes,
    editor_get_node,
    editor_find_by_uuid,
    editor_set_collider,
    editor_remove_collider,
    editor_set_rigid_body,
    editor_remove_rigid_body,
    editor_set_audio_source,
    editor_remove_audio_source,
    editor_set_audio_listener,
    editor_remove_audio_listener,
    editor_set_reverb_zone,
    editor_remove_reverb_zone,
    editor_audio_set_bus,
    editor_audio_preview,
    editor_prefab_save,
    editor_prefab_instantiate,
    editor_prefab_unpack,
    editor_create_node,
    editor_update_node,
    editor_delete_node,
    editor_reparent_node,
    editor_duplicate_node,
    editor_focus_selection,
    editor_save_scene,
    editor_undo,
    editor_redo,
    editor_list_assets,
    editor_import_asset,
    editor_get_material,
    editor_set_material_tessellation,
    editor_create_flat_ground,
    editor_scatter_model,
    editor_set_animator,
    editor_remove_animator,
    editor_animation_preview,
    editor_set_gameplay,
    editor_remove_gameplay,
    editor_gameplay_save_or_load,
    editor_set_ui_marker,
    editor_remove_ui_marker,
    editor_configure_ui,
    editor_set_spline,
    editor_remove_spline,
    editor_sequencer_control,
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

struct McpAudioSourcePayload {
    AudioBusId bus = AudioBusId::sfx;
    AudioAttenuationModel attenuation = AudioAttenuationModel::inverse_clamped;
    bool enabled = true;
    bool spatial = true;
    bool loop = false;
    bool play_on_start = true;
    bool streaming = false;
    bool occlusion = true;
    float volume = 1.0F;
    float pitch = 1.0F;
    float pitch_variance = 0.0F;
    float volume_variance = 0.0F;
    float min_distance = 1.5F;
    float max_distance = 35.0F;
    float rolloff = 1.0F;
    float doppler_factor = 1.0F;
    float inner_cone_degrees = 360.0F;
    float outer_cone_degrees = 360.0F;
    float outer_cone_gain = 0.25F;
};

static_assert(std::is_trivially_copyable_v<McpAudioSourcePayload>);

struct McpUiConfigPayload {
    static constexpr std::uint32_t flag_start_in_main_menu = 1U << 0U;
    static constexpr std::uint32_t flag_auto_pause_enabled = 1U << 1U;
    static constexpr std::uint32_t flag_show_default_hud = 1U << 2U;
    static constexpr std::uint32_t flag_show_crosshair = 1U << 3U;
    static constexpr std::uint32_t flag_show_fps = 1U << 4U;
    static constexpr std::uint32_t flag_show_subtitles = 1U << 5U;
    static constexpr std::uint32_t flag_show_world_markers = 1U << 6U;
    static constexpr std::uint32_t flag_auto_trigger_marker_dialogue = 1U << 7U;
    static constexpr std::uint32_t flag_scale_mode = 1U << 8U;
    static constexpr std::uint32_t flag_user_scale = 1U << 9U;
    static constexpr std::uint32_t flag_safe_area_fraction = 1U << 10U;
    static constexpr std::uint32_t flag_locale = 1U << 11U;
    static constexpr std::uint32_t flag_screen = 1U << 12U;
    static constexpr std::uint32_t flag_subtitle = 1U << 13U;
    static constexpr std::uint32_t flag_toast = 1U << 14U;
    static constexpr std::uint32_t flag_dialogue = 1U << 15U;
    static constexpr std::uint32_t flag_reload_assets = 1U << 16U;

    std::uint32_t mask = 0U;
    bool start_in_main_menu = false;
    bool auto_pause_enabled = true;
    bool show_default_hud = true;
    bool show_crosshair = true;
    bool show_fps = false;
    bool show_subtitles = true;
    bool show_world_markers = true;
    bool auto_trigger_marker_dialogue = true;
    bool reload_assets = false;
    std::uint8_t scale_mode = 0U;
    std::uint8_t screen = 0U;
    float user_scale = 1.0F;
    float safe_area_fraction = 0.025F;
    float duration_seconds = 3.5F;
    std::array<char, 16U> locale{};
    std::array<char, 64U> speaker{};
    std::array<char, 64U> dialogue_id{};
};

static_assert(std::is_trivially_copyable_v<McpUiConfigPayload>);

enum class McpSequencerAction : std::uint8_t {
    new_sequence = 0U,
    open_sequence = 1U,
    save_sequence = 2U,
    play_preview = 3U,
    pause_preview = 4U,
    stop_preview = 5U,
    scrub_preview = 6U,
};

struct McpSequencerControlPayload {
    McpSequencerAction action = McpSequencerAction::new_sequence;
    bool looping = false;
    bool has_scrub_time = false;
    bool lock_camera = true;
    bool has_lock_camera = false;
    float duration_seconds = 5.0F;
    float frame_rate = 30.0F;
    float scrub_time_seconds = 0.0F;
    std::array<char, 96U> id{};
    std::array<char, 128U> title{};
};

static_assert(std::is_trivially_copyable_v<McpSequencerControlPayload>);

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
    static constexpr std::uint32_t edit_ior = 1U << 10U;
    static constexpr std::uint32_t edit_material_model = 1U << 11U;
    static constexpr std::uint32_t edit_light_color = 1U << 12U;
    static constexpr std::uint32_t edit_light_intensity = 1U << 13U;
    static constexpr std::uint32_t edit_light_range = 1U << 14U;
    static constexpr std::uint32_t edit_spot_inner = 1U << 15U;
    static constexpr std::uint32_t edit_spot_outer = 1U << 16U;
    static constexpr std::uint32_t edit_subsurface_profile = 1U << 17U;
    static constexpr std::uint32_t edit_subsurface_strength = 1U << 18U;

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
    float lod_fade_seconds = 0.25F;
    bool lod_fade_seconds_supplied = false;
    bool enabled = false;
    LightingSettings lighting{};
    ExposureSettings exposure{};
    ColorGrading color_grading = ColorGrading::off;
    ColorGradingLutSize color_grading_lut_size = ColorGradingLutSize::size_32;
    SubsurfaceMode subsurface = SubsurfaceMode::off;
    RayTracingMode ray_tracing = RayTracingMode::off;
    MotionBlurSettings motion_blur{};
    SmaaMode smaa_mode = SmaaMode::x1;
    bool smaa_mode_supplied = false;
    SceneNodeId node_id{};
    SceneNodeId parent_id{};
    SceneUuid node_uuid{};
    ColliderComponent collider{};
    RigidBodyComponent rigid_body{};
    McpAudioSourcePayload audio_source{};
    AudioListenerComponent audio_listener{};
    AudioReverbZoneComponent reverb_zone{};
    bool audio_bus_volume_supplied = false;
    bool audio_bus_muted_supplied = false;
    bool prefab_attach_instance = true;
    AnimatorComponent animator{};
    GameplayComponent gameplay{};
    UiMarkerComponent ui_marker{};
    McpUiConfigPayload ui_config{};
    SplineComponent spline{};
    McpSequencerControlPayload sequencer{};
    float preview_time_seconds = -1.0F;
    bool preview_stop = false;
    bool preview_params_supplied = false;
    std::uint8_t gameplay_io_action = 0U;
    SceneNodeKind node_kind = SceneNodeKind::folder;
    SceneTransform transform{};
    PbrMaterial material{};
    MaterialTessellation tessellation{};
    PunctualLight light{};
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
