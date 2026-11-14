#include "editor_ui.hpp"

#include "asset_importer.hpp"
#include "cinematic_camera.hpp"
#include "cooked_audio_asset.hpp"
#include "cooked_model_asset.hpp"
#include "cutscene_sequencer.hpp"
#include "editor_scene_tools.hpp"
#include "gameplay_framework.hpp"
#include "project_system.hpp"
#include "renderer.hpp"
#include "runtime_assets.hpp"
#include "scene_io.hpp"
#include "scene_uuid.hpp"
#include "sdl_input_bridge.hpp"
#include "sound_cue_io.hpp"
#include "vulkan_common.hpp"

#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vcr {

inline constexpr std::size_t maximum_import_sources = 128U;
inline constexpr std::size_t maximum_import_path_bytes = 32U * 1024U;

struct AssetImportUiState {
    struct Completion {
        std::filesystem::path source{};
        std::optional<AssetImportResult> result{};
        std::optional<AssetImportError> error{};
    };

    std::vector<std::filesystem::path> sources{};
    std::filesystem::path destination_directory{};
    std::vector<Completion> completions{};
    std::array<char, maximum_import_path_bytes> source_path{};
    std::string status{};
    bool import_popup_requested = false;
    bool import_popup_open = false;
    bool results_window_open = false;
};

struct ExternalAssetDropState {
    ImVec2 minimum{};
    ImVec2 maximum{};
    std::vector<std::filesystem::path> sources{};
    SDL_WindowID window_id = 0U;
    bool region_valid = false;
    bool batch_active = false;
    bool hover = false;
};

enum class MaterialAlphaMode : std::uint8_t {
    opaque = 0U,
    mask = 1U,
    blend = 2U,
};

[[nodiscard]] constexpr std::string_view to_token(
    const MaterialAlphaMode mode) noexcept {
    switch (mode) {
    case MaterialAlphaMode::opaque: return "opaque";
    case MaterialAlphaMode::mask: return "mask";
    case MaterialAlphaMode::blend: return "blend";
    }
    return "opaque";
}

struct MaterialEditorUiState {
    nlohmann::json document{};
    std::filesystem::path relative_path{};
    std::filesystem::path absolute_path{};
    std::array<char, 256U> name{};
    PbrMaterial material{};
    MaterialTessellation tessellation{};
    MaterialAlphaMode alpha_mode = MaterialAlphaMode::opaque;
    float alpha_cutoff = 0.5F;
    bool double_sided = false;
    bool glass = false;
    bool packed_ao = false;
    bool has_displacement_texture = false;
    std::string error{};
    std::string status{};
    bool open = false;
    bool loaded = false;
    bool dirty = false;
    bool close_confirmation_requested = false;
};

[[nodiscard]] std::uint32_t compute_material_coverage(
    const MaterialEditorUiState& state) noexcept {
    std::uint32_t coverage = 0U;
    if (state.alpha_mode == MaterialAlphaMode::mask) {
        coverage = material_coverage_alpha_mask_bit;
    } else if (state.alpha_mode == MaterialAlphaMode::blend) {
        coverage = material_coverage_alpha_blend_bit;
    }
    if (state.glass) {
        coverage |= material_coverage_alpha_blend_bit | material_coverage_glass_bit;
    }
    if (state.double_sided) {
        coverage |= material_coverage_two_sided_bit;
    }
    if (state.packed_ao) {
        coverage |= material_coverage_packed_ao_bit;
    }
    if ((coverage & material_coverage_alpha_mask_bit) != 0U) {
        coverage |= static_cast<std::uint32_t>(std::lround(
            std::clamp(state.alpha_cutoff, 0.0F, 1.0F) * 65535.0F)) << 16U;
    }
    return coverage;
}

[[nodiscard]] SavedMaterialUpdate make_saved_material_update(
    const MaterialEditorUiState& state) {
    return SavedMaterialUpdate{
        .relative_path = state.relative_path.generic_string(),
        .material = state.material,
        .tessellation = state.tessellation,
        .coverage = compute_material_coverage(state),
    };
}

[[nodiscard]] MaterialEditorSnapshot make_material_snapshot(
    const MaterialEditorUiState& state) {
    return MaterialEditorSnapshot{
        .relative_path = state.relative_path.generic_string(),
        .name = std::string(state.name.data()),
        .displacement_texture = state.has_displacement_texture
            ? state.document["textures"]["displacement"].get<std::string>()
            : std::string{},
        .material = state.material,
        .tessellation = state.tessellation,
        .coverage = compute_material_coverage(state),
    };
}

struct AudioInputUiState {
    enum class CaptureMode : std::uint8_t {
        none,
        primary_key,
        secondary_key,
        gamepad_button,
    };

    std::optional<InputAction> capturing_action{};
    CaptureMode capture_mode = CaptureMode::none;
    GamepadState last_gamepad_for_capture{};
    std::string status{};
    std::string error{};
    bool open = true;
};

struct SequencerEditorUiState {
    enum class SelectionKind : std::uint8_t {
        none,
        camera_cut,
        camera_shake,
        property_track,
        animation_clip,
        audio_clip,
        subtitle,
        event_key,
    };

    enum class CameraSubCurve : std::uint8_t {
        fov = 0U,
        roll = 1U,
        exposure = 2U,
        focus_distance = 3U,
        rail_u = 4U,
    };

    CutsceneSequence sequence{};
    std::string relative_path{};
    std::array<char, max_scene_asset_path_bytes + 1U> path_buffer{};
    std::array<char, 96U> id_buffer{};
    std::array<char, 128U> title_buffer{};
    bool has_sequence = false;
    bool dirty = false;
    bool preview_active = false;
    bool preview_playing = false;
    bool lock_camera = true;
    bool show_curve_editor = false;
    bool files_scanned = false;
    bool needs_preview_eval = false;
    bool dragging_playhead = false;
    bool show_settings = false;
    float playhead_seconds = 0.0F;
    float timeline_zoom = 1.0F;
    float timeline_scroll_seconds = 0.0F;
    std::optional<SceneDocument> baseline_scene{};
    std::optional<FlyCamera> baseline_camera{};
    std::vector<SceneNodeId> previewed_anim_nodes{};
    std::vector<SceneWorldNode> wireframe_world_nodes{};
    std::vector<std::uint32_t> wireframe_ancestors{};
    CutsceneEvalScratch eval_scratch{};
    CutsceneFrameResult last_eval_frame{};
    std::vector<std::string> available_files{};
    SelectionKind selection_kind = SelectionKind::none;
    std::size_t selected_track_index = 0U;
    std::size_t selected_key_index = 0U;
    CameraSubCurve camera_sub_curve = CameraSubCurve::fov;
    std::string pending_runtime_cutscene{};
    std::string status{};
    std::string error{};

    SequencerEditorUiState() {
        reset_to_default();
    }

    void reset_to_default(
        const std::string_view new_id = "sequence",
        const std::string_view new_title = "Cutscene",
        const float duration_sec = 5.0F,
        const float fps = 30.0F,
        const bool is_looping = false) {
        sequence = CutsceneSequence{};
        sequence.id = new_id.empty() ? std::string{"sequence"} : std::string{new_id};
        sequence.title = new_title.empty() ? std::string{"Cutscene"} : std::string{new_title};
        sequence.duration_seconds = std::clamp(duration_sec, 0.25F, 3600.0F);
        sequence.frame_rate = std::clamp(fps, 1.0F, 240.0F);
        sequence.looping = is_looping;
        sequence = sanitize_cutscene_sequence(std::move(sequence));
        relative_path = make_default_cutscene_relative_path(sequence.id);
        has_sequence = true;
        dirty = false;
        preview_active = false;
        preview_playing = false;
        needs_preview_eval = false;
        playhead_seconds = 0.0F;
        timeline_scroll_seconds = 0.0F;
        selection_kind = SelectionKind::none;
        selected_track_index = 0U;
        selected_key_index = 0U;
        eval_scratch.reset_frame_state();
        last_eval_frame = {};
        sync_buffers();
    }

    void sync_buffers() {
        path_buffer.fill('\0');
        const std::size_t path_copy = std::min(relative_path.size(), path_buffer.size() - 1U);
        std::memcpy(path_buffer.data(), relative_path.data(), path_copy);
        id_buffer.fill('\0');
        const std::size_t id_copy = std::min(sequence.id.size(), id_buffer.size() - 1U);
        std::memcpy(id_buffer.data(), sequence.id.data(), id_copy);
        title_buffer.fill('\0');
        const std::size_t title_copy = std::min(sequence.title.size(), title_buffer.size() - 1U);
        std::memcpy(title_buffer.data(), sequence.title.data(), title_copy);
    }
};

class AssetImportWorker final {
public:
    struct Batch {
        std::vector<std::filesystem::path> sources{};
        ImportAssetOptions options{};
    };

    AssetImportWorker()
        : worker_([this](const std::stop_token stop_token) {
              run(stop_token);
          }) {}

    ~AssetImportWorker() {
        stop();
    }

    AssetImportWorker(const AssetImportWorker&) = delete;
    AssetImportWorker& operator=(const AssetImportWorker&) = delete;

    [[nodiscard]] bool submit(Batch batch) {
        constexpr std::size_t maximum_pending_batches = 4U;
        std::lock_guard lock(mutex_);
        if (stopping_ ||
            failed_.load(std::memory_order_acquire) ||
            pending_.size() >= maximum_pending_batches) {
            return false;
        }
        pending_.push_back(std::move(batch));
        outstanding_batches_.fetch_add(
            1U, std::memory_order_release);
        condition_.notify_one();
        return true;
    }

    [[nodiscard]] bool drain(
        std::vector<AssetImportUiState::Completion>& output) {
        if (!completed_ready_.load(std::memory_order_acquire)) {
            return false;
        }
        std::lock_guard lock(mutex_);
        if (completed_.empty()) {
            completed_ready_.store(
                false, std::memory_order_release);
            return false;
        }
        output.clear();
        output.reserve(completed_.size());
        while (!completed_.empty()) {
            output.push_back(std::move(completed_.front()));
            completed_.pop_front();
        }
        completed_ready_.store(false, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool busy() const noexcept {
        return outstanding_batches_.load(
                   std::memory_order_acquire) != 0U;
    }

    [[nodiscard]] bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        outstanding_batches_.store(0U, std::memory_order_release);
    }

private:
    void run(const std::stop_token stop_token) noexcept {
        try {
            run_throwing(stop_token);
        } catch (const std::exception& exception) {
            std::fprintf(
                stderr,
                "Asset import worker stopped after an exception: %s\n",
                exception.what());
            mark_failed();
        } catch (...) {
            std::fputs(
                "Asset import worker stopped after an unknown exception.\n",
                stderr);
            mark_failed();
        }
    }

    void mark_failed() noexcept {
        failed_.store(true, std::memory_order_release);
        try {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            pending_.clear();
        } catch (...) {
            // An already-failing worker must never let an exception escape
            // its noexcept thread boundary.
        }
        outstanding_batches_.store(0U, std::memory_order_release);
        condition_.notify_all();
    }

    void run_throwing(const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            Batch batch;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(
                    lock,
                    [this, &stop_token] {
                        return stopping_ ||
                            stop_token.stop_requested() ||
                            !pending_.empty();
                    });
                if (stopping_ || stop_token.stop_requested()) {
                    break;
                }
                batch = std::move(pending_.front());
                pending_.pop_front();
            }

            std::vector<AssetImportUiState::Completion>
                batch_completions;
            batch_completions.reserve(batch.sources.size());
            for (const std::filesystem::path& source :
                 batch.sources) {
                if (stop_token.stop_requested()) {
                    break;
                }
                AssetImportUiState::Completion completion{
                    .source = source,
                };
                try {
                    auto outcome =
                        import_asset(source, batch.options);
                    if (outcome) {
                        completion.result =
                            std::move(outcome).value();
                    } else {
                        completion.error =
                            std::move(outcome).error();
                    }
                } catch (const std::exception& exception) {
                    completion.error = AssetImportError{
                        .code =
                            AssetImportErrorCode::filesystem_error,
                        .message =
                            "Asset importer raised an exception: " +
                            std::string(exception.what()),
                        .path = source,
                    };
                } catch (...) {
                    completion.error = AssetImportError{
                        .code =
                            AssetImportErrorCode::filesystem_error,
                        .message =
                            "Asset importer raised an unknown exception.",
                        .path = source,
                    };
                }
                batch_completions.push_back(
                    std::move(completion));
            }

            {
                std::lock_guard lock(mutex_);
                for (auto& completion : batch_completions) {
                    completed_.push_back(
                        std::move(completion));
                }
                completed_ready_.store(
                    !completed_.empty(),
                    std::memory_order_release);
            }
            outstanding_batches_.fetch_sub(
                1U, std::memory_order_release);
        }
    }

    mutable std::mutex mutex_{};
    std::condition_variable condition_{};
    std::deque<Batch> pending_{};
    std::deque<AssetImportUiState::Completion> completed_{};
    std::atomic<std::size_t> outstanding_batches_{0U};
    std::atomic<bool> completed_ready_{false};
    std::atomic<bool> failed_{false};
    bool stopping_ = false;
    // Must be last: the thread can observe every preceding member as soon
    // as it starts, and it is joined before those members are destroyed.
    std::jthread worker_{};
};

namespace {

void check_imgui_vk_result(const VkResult result) {
    if (result != VK_SUCCESS) {
        check_vk(result, "Dear ImGui Vulkan backend", __FILE__, __LINE__);
    }
}

[[nodiscard]] ImTextureID imgui_texture_id(
    const VkDescriptorSet descriptor_set) noexcept {
    return static_cast<ImTextureID>(
        reinterpret_cast<std::uintptr_t>(descriptor_set));
}

[[nodiscard]] bool same_extent(
    const VkExtent2D lhs,
    const VkExtent2D rhs) noexcept {
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

struct EditorViewportRay {
    glm::vec3 origin{};
    glm::vec3 direction{};
};

[[nodiscard]] glm::mat4 editor_projection(
    const float width,
    const float height,
    const float vertical_fov_degrees = camera_vertical_fov) noexcept {
    const float fov = std::clamp(
        std::isfinite(vertical_fov_degrees)
            ? vertical_fov_degrees
            : camera_vertical_fov,
        minimum_camera_vertical_fov,
        maximum_camera_vertical_fov);
    return glm::perspective(
        glm::radians(fov),
        width / std::max(height, 1.0F),
        camera_near,
        camera_far);
}

[[nodiscard]] std::optional<EditorViewportRay> viewport_ray(
    const ImVec2 mouse,
    const ImVec2 minimum,
    const ImVec2 maximum,
    const FlyCamera& camera,
    const float vertical_fov_degrees = camera_vertical_fov) noexcept {
    const float width = maximum.x - minimum.x;
    const float height = maximum.y - minimum.y;
    if (width <= 1.0F || height <= 1.0F ||
        mouse.x < minimum.x || mouse.x > maximum.x ||
        mouse.y < minimum.y || mouse.y > maximum.y) {
        return std::nullopt;
    }
    const float normalized_x =
        ((mouse.x - minimum.x) / width) * 2.0F - 1.0F;
    const float normalized_y =
        1.0F - ((mouse.y - minimum.y) / height) * 2.0F;
    const glm::mat4 inverse_view_projection = glm::inverse(
        editor_projection(width, height, vertical_fov_degrees) *
        camera.view_matrix());
    glm::vec4 far_point = inverse_view_projection * glm::vec4(
        normalized_x, normalized_y, 1.0F, 1.0F);
    if (!std::isfinite(far_point.w) ||
        std::abs(far_point.w) <= std::numeric_limits<float>::epsilon()) {
        return std::nullopt;
    }
    far_point /= far_point.w;
    const glm::vec3 direction =
        glm::vec3(far_point) - camera.position();
    const float length_squared = glm::dot(direction, direction);
    if (!std::isfinite(length_squared) || length_squared <= 0.0F) {
        return std::nullopt;
    }
    return EditorViewportRay{
        .origin = camera.position(),
        .direction = direction / std::sqrt(length_squared),
    };
}

[[nodiscard]] ImGuizmo::OPERATION imguizmo_operation(
    const EditorGizmoOperation operation) noexcept {
    switch (operation) {
    case EditorGizmoOperation::translate:
        return ImGuizmo::TRANSLATE;
    case EditorGizmoOperation::rotate:
        return ImGuizmo::ROTATE;
    case EditorGizmoOperation::scale:
        return ImGuizmo::SCALE;
    }
    return ImGuizmo::TRANSLATE;
}

[[nodiscard]] ImGuizmo::MODE imguizmo_space(
    const EditorGizmoSpace space) noexcept {
    return space == EditorGizmoSpace::local
        ? ImGuizmo::LOCAL
        : ImGuizmo::WORLD;
}

[[nodiscard]] const char* play_state_name(
    const EditorPlayState state) noexcept {
    switch (state) {
    case EditorPlayState::edit: return "EDIT";
    case EditorPlayState::playing: return "PLAYING";
    case EditorPlayState::paused: return "PAUSED";
    }
    return "UNKNOWN";
}

[[nodiscard]] EditorIcon node_icon(
    const SceneNodeKind kind) noexcept {
    switch (kind) {
    case SceneNodeKind::folder: return EditorIcon::folder;
    case SceneNodeKind::cube: return EditorIcon::cube;
    case SceneNodeKind::model: return EditorIcon::model;
    case SceneNodeKind::ground: return EditorIcon::ground;
    case SceneNodeKind::ocean: return EditorIcon::ground;
    case SceneNodeKind::terrain: return EditorIcon::terrain;
    case SceneNodeKind::camera: return EditorIcon::camera;
    case SceneNodeKind::directional_light:
    case SceneNodeKind::point_light:
    case SceneNodeKind::spot_light:
        return EditorIcon::light;
    case SceneNodeKind::environment:
        return EditorIcon::environment;
    }
    return EditorIcon::cube;
}

[[nodiscard]] EditorIcon asset_icon(
    const AssetEntryKind kind) noexcept {
    switch (kind) {
    case AssetEntryKind::directory: return EditorIcon::folder;
    case AssetEntryKind::model: return EditorIcon::model;
    case AssetEntryKind::material: return EditorIcon::material;
    case AssetEntryKind::image: return EditorIcon::environment;
    case AssetEntryKind::shader: return EditorIcon::renderer;
    case AssetEntryKind::scene: return EditorIcon::cube;
    case AssetEntryKind::prefab: return EditorIcon::model;
    case AssetEntryKind::audio: return EditorIcon::play;
    case AssetEntryKind::sound_cue: return EditorIcon::play;
    case AssetEntryKind::animation: return EditorIcon::play;
    case AssetEntryKind::cutscene: return EditorIcon::camera;
    case AssetEntryKind::save_state: return EditorIcon::cube;
    case AssetEntryKind::binary: return EditorIcon::import_asset;
    case AssetEntryKind::other: return EditorIcon::import_asset;
    }
    return EditorIcon::import_asset;
}

inline constexpr std::uintmax_t maximum_material_file_bytes =
    1024U * 1024U;

[[nodiscard]] const char* imported_asset_kind_name(
    const ImportedAssetKind kind) noexcept {
    switch (kind) {
    case ImportedAssetKind::model: return "Model";
    case ImportedAssetKind::material: return "Material";
    case ImportedAssetKind::texture: return "Texture";
    case ImportedAssetKind::mesh_binary: return "Mesh data";
    case ImportedAssetKind::skeleton: return "Skeleton";
    case ImportedAssetKind::animation: return "Animation";
    case ImportedAssetKind::audio: return "Audio";
    case ImportedAssetKind::source: return "Source";
    }
    return "Asset";
}

[[nodiscard]] bool path_escapes_asset_root(
    const std::filesystem::path& relative) {
    if (relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory()) {
        return true;
    }
    for (const auto& component : relative.lexically_normal()) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::filesystem::path path_from_utf8(
    const std::string_view text) {
    std::u8string encoded(text.size(), u8'\0');
    std::memcpy(encoded.data(), text.data(), text.size());
    return std::filesystem::path(std::move(encoded));
}

[[nodiscard]] std::string path_to_utf8(
    const std::filesystem::path& path) {
    const std::u8string encoded = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size());
}

[[nodiscard]] bool resolve_material_path(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& relative_path,
    std::filesystem::path& absolute_path,
    std::string& error) {
    error.clear();
    if (asset_root.empty() || relative_path.empty() ||
        path_escapes_asset_root(relative_path)) {
        error = "Material path escapes the configured asset root.";
        return false;
    }

    std::error_code filesystem_error;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(
            asset_root, filesystem_error);
    if (filesystem_error) {
        error = "Unable to canonicalize the asset root.";
        return false;
    }

    std::filesystem::path component_path = canonical_root;
    for (const auto& component : relative_path.lexically_normal()) {
        if (component == ".") {
            continue;
        }
        component_path /= component;
        const auto status = std::filesystem::symlink_status(
            component_path, filesystem_error);
        if (filesystem_error) {
            error = "Unable to inspect the material path.";
            return false;
        }
        if (std::filesystem::is_symlink(status)) {
            error = "Symbolic links are not followed for material assets.";
            return false;
        }
    }

    absolute_path = std::filesystem::weakly_canonical(
        canonical_root / relative_path, filesystem_error);
    if (filesystem_error) {
        error = "Unable to canonicalize the material path.";
        return false;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(
            absolute_path, canonical_root, filesystem_error);
    if (filesystem_error ||
        path_escapes_asset_root(relative) ||
        !std::filesystem::is_regular_file(
            absolute_path, filesystem_error) ||
        filesystem_error) {
        error = "Material asset is outside the root or inaccessible.";
        return false;
    }
    return true;
}

[[nodiscard]] bool read_material_factor(
    const nlohmann::json& value,
    const float minimum,
    const float maximum,
    float& output) {
    if (!value.is_number()) {
        return false;
    }
    const double decoded = value.get<double>();
    if (!std::isfinite(decoded) ||
        decoded < static_cast<double>(minimum) ||
        decoded > static_cast<double>(maximum)) {
        return false;
    }
    output = static_cast<float>(decoded);
    return true;
}

[[nodiscard]] bool load_material_document(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& relative_path,
    MaterialEditorUiState& state,
    std::string& error) {
    std::filesystem::path absolute_path;
    if (!resolve_material_path(
            asset_root,
            relative_path,
            absolute_path,
            error)) {
        return false;
    }

    std::error_code filesystem_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(
            absolute_path, filesystem_error);
    if (filesystem_error || file_size == 0U ||
        file_size > maximum_material_file_bytes) {
        error =
            "Material file is empty or exceeds the 1 MiB safety limit.";
        return false;
    }
    std::ifstream input(absolute_path, std::ios::binary);
    if (!input) {
        error = "Unable to open the material asset.";
        return false;
    }

    try {
        nlohmann::json document =
            nlohmann::json::parse(input, nullptr, false);
        if (document.is_discarded() ||
            !document.is_object() ||
            document.value("schema", std::string{}) != "vcr.asset" ||
            !document.contains("version") ||
            !document["version"].is_number_unsigned() ||
            document["version"].get<std::uint64_t>() != 1U ||
            document.value("kind", std::string{}) != "material" ||
            !document.contains("name") ||
            !document["name"].is_string() ||
            !document.contains("pbr") ||
            !document["pbr"].is_object()) {
            error = "Material schema, kind, or version is invalid.";
            return false;
        }
        const std::string& name =
            document["name"].get_ref<const std::string&>();
        if (name.size() >= state.name.size() ||
            name.find('\0') != std::string::npos) {
            error = "Material name exceeds 255 UTF-8 bytes.";
            return false;
        }
        const nlohmann::json& pbr = document["pbr"];
        if (!pbr.contains("base_color") ||
            !pbr["base_color"].is_array() ||
            pbr["base_color"].size() != 4U ||
            !pbr.contains("metallic") ||
            !pbr.contains("roughness") ||
            !pbr.contains("ambient_occlusion")) {
            error = "Material PBR schema is invalid.";
            return false;
        }

        PbrMaterial material{};
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            if (!read_material_factor(
                    pbr["base_color"][channel],
                    0.0F,
                    1.0F,
                    material.base_color[
                        static_cast<glm::length_t>(channel)])) {
                error = "Material base color is invalid.";
                return false;
            }
        }
        if (!read_material_factor(
                pbr["metallic"],
                0.0F,
                1.0F,
                material.metallic) ||
            !read_material_factor(
                pbr["roughness"],
                minimum_pbr_roughness,
                1.0F,
                material.roughness) ||
            !read_material_factor(
                pbr["ambient_occlusion"],
                0.0F,
                1.0F,
                material.ambient_occlusion)) {
            error = "Material PBR factors are invalid.";
            return false;
        }
        if (pbr.contains("ior") &&
            !read_material_factor(
                pbr["ior"], 1.0F, std::numeric_limits<float>::max(), material.ior)) {
            error = "Material IOR must be finite and from 1 through FLT_MAX.";
            return false;
        }
        if (pbr.contains("model")) {
            if (!pbr["model"].is_string()) {
                error = "Material model must be standard, unlit or subsurface.";
                return false;
            }
            const auto model = parse_material_model(
                pbr["model"].get_ref<const std::string&>());
            if (!model) {
                error = "Material model must be standard, unlit or subsurface.";
                return false;
            }
            material.model = *model;
        }
        if (pbr.contains("subsurface_profile")) {
            if (!pbr["subsurface_profile"].is_string()) {
                error = "Material subsurface profile must be none, narrow, medium, wide or foliage.";
                return false;
            }
            const auto profile = parse_subsurface_profile(
                pbr["subsurface_profile"].get_ref<const std::string&>());
            if (!profile) {
                error = "Material subsurface profile must be none, narrow, medium, wide or foliage.";
                return false;
            }
            material.subsurface_profile = *profile;
        }
        if (pbr.contains("subsurface_strength") && !read_material_factor(
                pbr["subsurface_strength"], 0.0F, 1.0F,
                material.subsurface_strength)) {
            error = "Material subsurface strength must be finite and from 0 through 1.";
            return false;
        }
        if (!document.contains("textures") ||
            !document["textures"].is_object()) {
            error = "Material texture references are invalid.";
            return false;
        }
        for (const auto& [semantic, reference] :
             document["textures"].items()) {
            constexpr std::array<std::string_view, 8U>
                supported_semantics{{
                    "base_color",
                    "normal",
                    "metallic",
                    "roughness",
                    "ambient_occlusion",
                    "emissive",
                    "metallic_roughness",
                    "displacement",
                }};
            if (std::ranges::find(
                    supported_semantics,
                    std::string_view(semantic)) ==
                supported_semantics.end()) {
                error =
                    "Material contains an unsupported texture semantic.";
                return false;
            }
            if (!reference.is_string()) {
                error =
                    "Material texture references must be strings.";
                return false;
            }
            const std::string& reference_text =
                reference.get_ref<const std::string&>();
            if (reference_text.empty() ||
                reference_text.size() > maximum_import_path_bytes ||
                reference_text.find('\0') != std::string::npos) {
                error =
                    "Material texture reference is empty or too long.";
                return false;
            }
            const std::filesystem::path texture_path =
                path_from_utf8(reference_text);
            if (path_escapes_asset_root(texture_path)) {
                error =
                    "Material texture reference escapes its asset.";
                return false;
            }
        }

        const bool has_displacement_texture =
            document["textures"].contains("displacement");
        MaterialTessellation tessellation{};
        if (document.contains("tessellation")) {
            const nlohmann::json& authored =
                document["tessellation"];
            if (!has_displacement_texture || !authored.is_object() ||
                !authored.contains("displacement_scale") ||
                !read_material_factor(
                    authored["displacement_scale"], 0.0F, 10'000.0F,
                    tessellation.displacement_scale) ||
                !authored.contains("displacement_center") ||
                !read_material_factor(
                    authored["displacement_center"], 0.0F, 1.0F,
                    tessellation.displacement_center) ||
                !authored.contains("target_edge_pixels") ||
                !read_material_factor(
                    authored["target_edge_pixels"], 1.0F, 1024.0F,
                    tessellation.target_edge_pixels) ||
                !authored.contains("maximum_factor") ||
                !read_material_factor(
                    authored["maximum_factor"], 1.0F, 64.0F,
                    tessellation.maximum_factor)) {
                error = "Material tessellation settings are invalid.";
                return false;
            }
            tessellation.enabled_flag = 1U;
            if (authored.contains("enabled")) {
                if (!authored["enabled"].is_boolean()) {
                    error = "Material tessellation enabled flag is invalid.";
                    return false;
                }
                tessellation.enabled_flag =
                    authored["enabled"].get<bool>() ? 1U : 0U;
            }
            if (authored.contains("up_facing_only")) {
                if (!authored["up_facing_only"].is_boolean()) {
                    error = "Material tessellation face filter is invalid.";
                    return false;
                }
                tessellation.up_facing_only =
                    authored["up_facing_only"].get<bool>() ? 1U : 0U;
            }
            if (authored.contains("normal_threshold") &&
                !read_material_factor(
                    authored["normal_threshold"], -1.0F, 1.0F,
                    tessellation.normal_threshold)) {
                error = "Material tessellation normal threshold is invalid.";
                return false;
            }
            if (authored.contains("uv_edge_fade") &&
                !read_material_factor(
                    authored["uv_edge_fade"], 0.0F, 0.5F,
                    tessellation.uv_edge_fade)) {
                error = "Material tessellation UV edge fade is invalid.";
                return false;
            }
        }

        MaterialAlphaMode alpha_mode = MaterialAlphaMode::opaque;
        float alpha_cutoff = 0.5F;
        bool double_sided = false;
        bool glass = false;
        bool packed_ao = false;
        if (document.contains("alpha_mode")) {
            if (!document["alpha_mode"].is_string()) {
                error = "Material alpha_mode must be opaque, mask, or blend.";
                return false;
            }
            const std::string& mode_text =
                document["alpha_mode"].get_ref<const std::string&>();
            if (mode_text == "opaque") {
                alpha_mode = MaterialAlphaMode::opaque;
            } else if (mode_text == "mask") {
                alpha_mode = MaterialAlphaMode::mask;
            } else if (mode_text == "blend") {
                alpha_mode = MaterialAlphaMode::blend;
                double_sided = true;
            } else {
                error = "Material alpha_mode must be opaque, mask, or blend.";
                return false;
            }
        }
        if (document.contains("glass")) {
            if (!document["glass"].is_boolean()) {
                error = "Material glass must be boolean.";
                return false;
            }
            glass = document["glass"].get<bool>();
            if (glass) {
                alpha_mode = MaterialAlphaMode::blend;
                double_sided = true;
            }
        }
        if (document.contains("alpha_cutoff") &&
            !read_material_factor(
                document["alpha_cutoff"], 0.0F, 1.0F, alpha_cutoff)) {
            error = "Material alpha cutoff must be finite and from 0 through 1.";
            return false;
        }
        if (document.contains("double_sided")) {
            if (!document["double_sided"].is_boolean()) {
                error = "Material double_sided must be boolean.";
                return false;
            }
            double_sided = document["double_sided"].get<bool>();
        }
        if (document.contains("occlusion_in_metallic_roughness")) {
            if (!document["occlusion_in_metallic_roughness"].is_boolean()) {
                error = "Material occlusion_in_metallic_roughness must be boolean.";
                return false;
            }
            packed_ao = document["occlusion_in_metallic_roughness"].get<bool>();
        }

        state.name.fill('\0');
        std::memcpy(state.name.data(), name.data(), name.size());
        state.document = std::move(document);
        state.relative_path = relative_path.lexically_normal();
        state.absolute_path = std::move(absolute_path);
        state.material = sanitize_pbr_material(material);
        state.tessellation = tessellation;
        state.alpha_mode = alpha_mode;
        state.alpha_cutoff = alpha_cutoff;
        state.double_sided = double_sided;
        state.glass = glass;
        state.packed_ao = packed_ao;
        state.has_displacement_texture = has_displacement_texture;
        state.loaded = true;
        state.dirty = false;
        state.error.clear();
        state.status =
            "Loaded " + path_to_utf8(state.relative_path);
        return true;
    } catch (const std::exception&) {
        error = "Material JSON contains an invalid value.";
        return false;
    }
}

enum class ExclusiveFileWriteResult : std::uint8_t {
    written,
    already_exists,
    failed,
};

class TemporaryMaterialPath final {
public:
    explicit TemporaryMaterialPath(
        std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryMaterialPath() {
        if (!retain_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    TemporaryMaterialPath(const TemporaryMaterialPath&) = delete;
    TemporaryMaterialPath& operator=(
        const TemporaryMaterialPath&) = delete;

    void retain() noexcept {
        retain_ = true;
    }

private:
    std::filesystem::path path_{};
    bool retain_ = false;
};

#if defined(_WIN32)
class UniqueMaterialFile final {
public:
    explicit UniqueMaterialFile(const HANDLE handle) noexcept
        : handle_(handle) {}

    ~UniqueMaterialFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    UniqueMaterialFile(const UniqueMaterialFile&) = delete;
    UniqueMaterialFile& operator=(
        const UniqueMaterialFile&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#else
class UniqueMaterialFile final {
public:
    explicit UniqueMaterialFile(const int descriptor) noexcept
        : descriptor_(descriptor) {}

    ~UniqueMaterialFile() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    UniqueMaterialFile(const UniqueMaterialFile&) = delete;
    UniqueMaterialFile& operator=(
        const UniqueMaterialFile&) = delete;

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_ = -1;
};
#endif

[[nodiscard]] ExclusiveFileWriteResult
write_exclusive_material_file(
    const std::filesystem::path& path,
    const std::string_view contents,
    const std::filesystem::perms permissions,
    std::string& error) {
#if defined(_WIN32)
    static_cast<void>(permissions);
    const HANDLE raw_handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0U,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (raw_handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS ||
            code == ERROR_ALREADY_EXISTS) {
            return ExclusiveFileWriteResult::already_exists;
        }
        error =
            "Unable to create the temporary material file "
            "(Win32 error " +
            std::to_string(code) + ").";
        return ExclusiveFileWriteResult::failed;
    }
    UniqueMaterialFile file(raw_handle);
    std::size_t offset = 0U;
    while (offset < contents.size()) {
        const std::size_t remaining = contents.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>(
                    std::numeric_limits<DWORD>::max())));
        DWORD written = 0U;
        if (WriteFile(
                file.get(),
                contents.data() + offset,
                chunk,
                &written,
                nullptr) == 0 ||
            written == 0U) {
            error =
                "Unable to write the complete temporary material file.";
            return ExclusiveFileWriteResult::failed;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (FlushFileBuffers(file.get()) == 0) {
        error = "Unable to flush the temporary material file.";
        return ExclusiveFileWriteResult::failed;
    }
#else
    const int descriptor = ::open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        0600);
    if (descriptor < 0) {
        if (errno == EEXIST) {
            return ExclusiveFileWriteResult::already_exists;
        }
        error =
            "Unable to create the temporary material file "
            "(errno " +
            std::to_string(errno) + ").";
        return ExclusiveFileWriteResult::failed;
    }
    UniqueMaterialFile file(descriptor);
    const mode_t native_permissions =
        static_cast<mode_t>(permissions) &
        static_cast<mode_t>(0777);
    if (::fchmod(file.get(), native_permissions) != 0) {
        error = "Unable to preserve material file permissions.";
        return ExclusiveFileWriteResult::failed;
    }
    std::size_t offset = 0U;
    while (offset < contents.size()) {
        const ssize_t written = ::write(
            file.get(),
            contents.data() + offset,
            contents.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            error =
                "Unable to write the complete temporary material file.";
            return ExclusiveFileWriteResult::failed;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(file.get()) != 0) {
        error = "Unable to flush the temporary material file.";
        return ExclusiveFileWriteResult::failed;
    }
#endif
    return ExclusiveFileWriteResult::written;
}

[[nodiscard]] bool replace_material_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH) == 0) {
        error =
            "Unable to replace the material file (Win32 error " +
            std::to_string(GetLastError()) +
            "). The complete temporary file was retained.";
        return false;
    }
    return true;
#else
    std::error_code filesystem_error;
    std::filesystem::rename(
        temporary, destination, filesystem_error);
    if (filesystem_error) {
        error =
            "Unable to replace the material file. The complete "
            "temporary file was retained.";
        return false;
    }
    return true;
#endif
}

[[nodiscard]] bool save_material_document(
    MaterialEditorUiState& state,
    const std::filesystem::path& asset_root,
    std::string& error) {
    error.clear();
    if (!state.loaded || state.absolute_path.empty()) {
        error = "No material is loaded.";
        return false;
    }
    std::filesystem::path revalidated_path;
    if (!resolve_material_path(
            asset_root,
            state.relative_path,
            revalidated_path,
            error) ||
        revalidated_path != state.absolute_path) {
        if (error.empty()) {
            error =
                "Material path changed after it was opened.";
        }
        return false;
    }
    std::error_code filesystem_error;
    const std::filesystem::perms material_permissions =
        std::filesystem::status(
            state.absolute_path, filesystem_error).permissions();
    if (filesystem_error ||
        material_permissions == std::filesystem::perms::unknown) {
        error = "Unable to read the material file permissions.";
        return false;
    }
    const std::string_view name(state.name.data());
    if (name.empty()) {
        error = "Material name cannot be empty.";
        return false;
    }

    try {
        const PbrMaterial material =
            sanitize_pbr_material(state.material);
        const auto readable_float = [](const float value) {
            constexpr double precision = 1'000'000.0;
            return std::round(static_cast<double>(value) * precision) /
                precision;
        };
        state.document["name"] = std::string(name);
        state.document["alpha_mode"] = std::string(to_token(state.alpha_mode));
        state.document["alpha_cutoff"] = readable_float(
            std::clamp(state.alpha_cutoff, 0.0F, 1.0F));
        state.document["double_sided"] = state.double_sided;
        if (state.glass) {
            state.document["glass"] = true;
        } else {
            state.document.erase("glass");
        }
        if (state.packed_ao) {
            state.document["occlusion_in_metallic_roughness"] = true;
        } else {
            state.document.erase("occlusion_in_metallic_roughness");
        }
        state.document["pbr"] = {
            {"base_color",
             {material.base_color.x,
              material.base_color.y,
              material.base_color.z,
              material.base_color.w}},
            {"metallic", material.metallic},
            {"roughness", material.roughness},
            {"ambient_occlusion", material.ambient_occlusion},
            {"ior", material.ior},
            {"model", to_token(material.model)},
            {"subsurface_profile", to_token(material.subsurface_profile)},
            {"subsurface_strength", material.subsurface_strength},
        };
        if (state.has_displacement_texture && material.model != MaterialModel::gpu_grass) {
            state.document["tessellation"] = {
                {"enabled", state.tessellation.enabled_flag != 0U},
                {"displacement_scale", readable_float(
                    state.tessellation.displacement_scale)},
                {"displacement_center", readable_float(
                    state.tessellation.displacement_center)},
                {"target_edge_pixels", readable_float(
                    state.tessellation.target_edge_pixels)},
                {"maximum_factor", readable_float(
                    state.tessellation.maximum_factor)},
                {"up_facing_only", state.tessellation.up_facing_only != 0U},
                {"normal_threshold", readable_float(
                    state.tessellation.normal_threshold)},
                {"uv_edge_fade", readable_float(
                    state.tessellation.uv_edge_fade)},
            };
        } else {
            state.document.erase("tessellation");
        }
        const std::string serialized =
            state.document.dump(2) + '\n';
        if (serialized.size() > maximum_material_file_bytes) {
            error =
                "Serialized material exceeds the 1 MiB safety limit.";
            return false;
        }

        std::filesystem::path temporary;
        const auto unique_value =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        for (std::uint32_t attempt = 0U;
             attempt < 32U;
             ++attempt) {
            temporary = state.absolute_path;
            temporary +=
                ".tmp." + std::to_string(unique_value) +
                "." + std::to_string(attempt);
            const ExclusiveFileWriteResult written =
                write_exclusive_material_file(
                    temporary,
                    serialized,
                    material_permissions,
                    error);
            if (written == ExclusiveFileWriteResult::written) {
                break;
            }
            if (written == ExclusiveFileWriteResult::failed) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            temporary.clear();
        }
        if (temporary.empty()) {
            error =
                "Unable to reserve a unique temporary material path.";
            return false;
        }
        TemporaryMaterialPath temporary_lifetime(temporary);
        if (!replace_material_file(
                temporary, state.absolute_path, error)) {
            temporary_lifetime.retain();
            return false;
        }
        state.material = material;
        state.dirty = false;
        state.error.clear();
        state.status =
            "Saved " + path_to_utf8(state.relative_path);
        return true;
    } catch (const std::exception&) {
        error = "Unable to encode the material as valid JSON.";
        return false;
    }
}

[[nodiscard]] bool point_inside_asset_drop_region(
    const SDL_DropEvent& event,
    const ExternalAssetDropState& state) {
    if (!state.region_valid ||
        event.windowID != state.window_id) {
        return false;
    }
    SDL_Window* const dropped_window =
        SDL_GetWindowFromID(event.windowID);
    if (dropped_window == nullptr) {
        return false;
    }
    const bool global_coordinates =
        (ImGui::GetIO().ConfigFlags &
         ImGuiConfigFlags_ViewportsEnable) != 0;
    int window_x = 0;
    int window_y = 0;
    if (global_coordinates &&
        !SDL_GetWindowPosition(
            dropped_window, &window_x, &window_y)) {
        return false;
    }
    const ImVec2 point{
        event.x +
            (global_coordinates
                ? static_cast<float>(window_x)
                : 0.0F),
        event.y +
            (global_coordinates
                ? static_cast<float>(window_y)
                : 0.0F),
    };
    return point.x >= state.minimum.x &&
        point.x <= state.maximum.x &&
        point.y >= state.minimum.y &&
        point.y <= state.maximum.y;
}

[[nodiscard]] SDL_Window* viewport_sdl_window(
    const ImGuiViewport* const viewport) noexcept {
    if (viewport == nullptr || viewport->PlatformHandle == nullptr) {
        return nullptr;
    }
    const auto encoded = reinterpret_cast<std::uintptr_t>(
        viewport->PlatformHandle);
    if (encoded <=
        static_cast<std::uintptr_t>(
            std::numeric_limits<SDL_WindowID>::max())) {
        if (SDL_Window* const by_id =
                SDL_GetWindowFromID(
                    static_cast<SDL_WindowID>(encoded))) {
            return by_id;
        }
    }
    return static_cast<SDL_Window*>(viewport->PlatformHandle);
}

} // namespace

EditorUi::EditorUi() = default;

EditorUi::~EditorUi() noexcept {
    shutdown();
}

void EditorUi::initialize(
    VulkanContext& context,
    SDL_Window* const main_window,
    Renderer& renderer,
    AudioEngine* const audio_engine,
    InputActionSystem* const input_actions,
    const std::string_view explicit_project_path,
    GameplaySession* const gameplay_session,
    AnimatorRuntimeSystem* const edit_animator_preview) {
    if (context_ != nullptr || main_window == nullptr) {
        fatal("Editor UI was initialized with invalid state.");
    }
    context_ = &context;
    main_window_ = main_window;
    audio_engine_ = audio_engine;
    input_actions_ = input_actions;
    gameplay_session_ = gameplay_session;
    edit_animator_preview_ = edit_animator_preview;
    show_skeleton_wireframe_ = true;
    show_spline_wireframe_ = true;
    animation_gameplay_panel_open_ = true;
    ui_localization_panel_open_ = true;
    sequencer_panel_open_ = true;
    ui_loc_status_.clear();
    save_slot_path_buf_.fill('\0');
    constexpr std::string_view default_save_slot =
        "saves/quicksave.vcrsave.json";
    std::memcpy(
        save_slot_path_buf_.data(),
        default_save_slot.data(),
        default_save_slot.size());
    pending_quick_save_path_.reset();
    pending_quick_load_path_.reset();
    save_load_status_.clear();
    save_load_error_.clear();
    skeleton_wireframe_scratch_.clear();
    import_worker_ = std::make_unique<AssetImportWorker>();
    import_ui_ = std::make_unique<AssetImportUiState>();
    external_drop_ = std::make_unique<ExternalAssetDropState>();
    material_editor_ =
        std::make_unique<MaterialEditorUiState>();
    audio_input_ui_ =
        std::make_unique<AudioInputUiState>();
    sequencer_ui_ =
        std::make_unique<SequencerEditorUiState>();
    constexpr std::string_view default_scatter_name{"Plant"};
    std::memcpy(
        scatter_name_prefix_.data(),
        default_scatter_name.data(),
        default_scatter_name.size());
    undo_history_.reserve(maximum_history_entries);
    redo_history_.reserve(maximum_history_entries);
    scatter_created_.reserve(maximum_scatter_instances);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    const bool platform_is_supported =
#if defined(_WIN32)
        true;
#else
        [] {
            const char* const video_driver =
                SDL_GetCurrentVideoDriver();
            return video_driver != nullptr &&
                std::string_view(video_driver) == "x11";
        }();
#endif
    platform_windows_enabled_ =
        platform_is_supported &&
        context.queue_families().graphics ==
            context.queue_families().present;
    if (platform_windows_enabled_) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    ui_kit_.initialize();

    if (!ImGui_ImplSDL3_InitForVulkan(main_window)) {
        fatal("Unable to initialize Dear ImGui SDL3 backend.");
    }
    initialize_vulkan_backend(renderer);
    register_scene_texture(renderer);

    hierarchy_rows_.reserve(max_scene_nodes);
    if (const auto asset_root =
            find_runtime_asset_root(explicit_project_path)) {
        project_root_ = asset_root->project_root;
        project_name_ = asset_root->project_name;
        if (asset_browser_.initialize(
                asset_root->path, asset_error_)) {
            asset_import_enabled_ =
                asset_root->project_owned;
            scene_path_ =
                asset_browser_.root() /
                asset_root->startup_scene_relative_path;
            scene_file_name_ =
                scene_path_.filename().string();
            scene_file_status_ =
                "Game scene; Save updates assets/" +
                asset_root->startup_scene_relative_path;
            if (!asset_import_enabled_) {
                asset_error_ =
                    "Packaged assets are browse-only. Open the editor "
                    "from a project checkout to import assets.";
            }
        }
    } else {
        asset_error_ =
            "Unable to locate a project or packaged assets directory.";
    }
}

void EditorUi::shutdown() noexcept {
    if (context_ == nullptr) {
        return;
    }
    if (import_worker_) {
        import_worker_->stop();
    }
    const VkResult idle = vkDeviceWaitIdle(context_->device());
    if (idle != VK_SUCCESS) {
        std::cerr
            << "Editor UI shutdown vkDeviceWaitIdle failed: "
            << static_cast<int>(idle) << '\n';
    }
    unregister_scene_texture();
    shutdown_vulkan_backend();
    ui_kit_.shutdown();
    if (ImGui::GetCurrentContext() != nullptr) {
        if (ImGui::GetIO().BackendPlatformUserData != nullptr) {
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext();
    }
    context_ = nullptr;
    main_window_ = nullptr;
    audio_engine_ = nullptr;
    input_actions_ = nullptr;
    gameplay_session_ = nullptr;
    edit_animator_preview_ = nullptr;
    pending_quick_save_path_.reset();
    pending_quick_load_path_.reset();
    save_load_status_.clear();
    save_load_error_.clear();
    skeleton_wireframe_scratch_.clear();
    imgui_color_format_ = VK_FORMAT_UNDEFINED;
    imgui_rendering_info_.pColorAttachmentFormats = nullptr;
    known_viewport_generation_ = 0U;
    known_swapchain_generation_ = 0U;
    known_swapchain_format_ = VK_FORMAT_UNDEFINED;
    known_swapchain_image_count_ = 0U;
    platform_windows_enabled_ = false;
    default_layout_initialized_ = false;
    reset_layout_requested_ = false;
    wants_quit_ = false;
    quit_requested_ = false;
    quit_confirmation_requested_ = false;
    save_requested_ = false;
    open_requested_ = false;
    open_confirmation_requested_ = false;
    import_dialog_requested_ = false;
    scene_replaced_ = false;
    startup_camera_requested_ = false;
    saved_material_update_.reset();
    saved_content_revision_ = 0U;
    saved_structure_revision_ = 0U;
    selected_asset_.reset();
    sequencer_ui_.reset();
    audio_input_ui_.reset();
    material_editor_.reset();
    external_drop_.reset();
    import_ui_.reset();
    import_worker_.reset();
    asset_import_enabled_ = false;
    undo_history_.clear();
    redo_history_.clear();
    selected_node_ = {};
    pending_delete_ = {};
    pending_duplicate_ = {};
    delete_confirmation_ = {};
    pending_reparent_child_ = {};
    pending_reparent_parent_ = {};
    edit_scene_snapshot_.reset();
    inspector_edit_snapshot_.reset();
    gizmo_edit_snapshot_.reset();
    focus_selection_requested_ = false;
    scatter_created_.clear();
    project_root_.clear();
    project_name_.clear();
    pending_open_scene_path_.clear();
}

void EditorUi::process_event(const SDL_Event& event) {
    if (context_ != nullptr) {
        if (audio_input_ui_ != nullptr &&
            audio_input_ui_->open &&
            audio_input_ui_->capturing_action.has_value() &&
            input_actions_ != nullptr) {
            const InputAction action = *audio_input_ui_->capturing_action;
            ActionBinding edited = input_actions_->binding(action);
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.key == SDLK_ESCAPE) {
                    audio_input_ui_->capturing_action.reset();
                    audio_input_ui_->capture_mode =
                        AudioInputUiState::CaptureMode::none;
                    audio_input_ui_->status = "Cancelled rebind.";
                    return;
                }
                if (event.key.key == SDLK_BACKSPACE ||
                    event.key.key == SDLK_DELETE) {
                    if (audio_input_ui_->capture_mode ==
                        AudioInputUiState::CaptureMode::primary_key) {
                        edited.primary_key = InputKey::none;
                    } else if (audio_input_ui_->capture_mode ==
                               AudioInputUiState::CaptureMode::secondary_key) {
                        edited.secondary_key = InputKey::none;
                    } else if (audio_input_ui_->capture_mode ==
                               AudioInputUiState::CaptureMode::gamepad_button) {
                        edited.gamepad_button = GamepadButton::none;
                    }
                    input_actions_->set_binding(action, edited);
                    audio_input_ui_->capturing_action.reset();
                    audio_input_ui_->capture_mode =
                        AudioInputUiState::CaptureMode::none;
                    audio_input_ui_->status =
                        "Cleared binding for " +
                        std::string(input_action_display_name(action)) + ".";
                    return;
                }
                if (audio_input_ui_->capture_mode ==
                        AudioInputUiState::CaptureMode::primary_key ||
                    audio_input_ui_->capture_mode ==
                        AudioInputUiState::CaptureMode::secondary_key) {
                    const InputKey mapped =
                        input_key_from_sdl_keycode(event.key.key);
                    if (mapped != InputKey::none) {
                        if (audio_input_ui_->capture_mode ==
                            AudioInputUiState::CaptureMode::primary_key) {
                            edited.primary_key = mapped;
                        } else {
                            edited.secondary_key = mapped;
                        }
                        input_actions_->set_binding(action, edited);
                        audio_input_ui_->capturing_action.reset();
                        audio_input_ui_->capture_mode =
                            AudioInputUiState::CaptureMode::none;
                        audio_input_ui_->status =
                            "Bound " +
                            std::string(input_action_display_name(action)) +
                            " to " + std::string(to_token(mapped)) + ".";
                        return;
                    }
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                       (audio_input_ui_->capture_mode ==
                            AudioInputUiState::CaptureMode::primary_key ||
                        audio_input_ui_->capture_mode ==
                            AudioInputUiState::CaptureMode::secondary_key)) {
                const InputKey mapped =
                    input_key_from_sdl_mouse_button(event.button.button);
                if (mapped != InputKey::none) {
                    if (audio_input_ui_->capture_mode ==
                        AudioInputUiState::CaptureMode::primary_key) {
                        edited.primary_key = mapped;
                    } else {
                        edited.secondary_key = mapped;
                    }
                    input_actions_->set_binding(action, edited);
                    audio_input_ui_->capturing_action.reset();
                    audio_input_ui_->capture_mode =
                        AudioInputUiState::CaptureMode::none;
                    audio_input_ui_->status =
                        "Bound " +
                        std::string(input_action_display_name(action)) +
                        " to " + std::string(to_token(mapped)) + ".";
                    return;
                }
            }
        }
        const bool ui_wants_cursor =
            gameplay_session_ != nullptr &&
            gameplay_session_->active() &&
            gameplay_session_->game_ui().wants_mouse_cursor();
        const bool ui_handles_esc =
            gameplay_session_ != nullptr &&
            gameplay_session_->active() &&
            gameplay_session_->game_ui().handles_escape_key();
        if (game_mouse_window_id_ != 0U) {
            if ((event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                 event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) &&
                event.window.windowID == game_mouse_window_id_) {
                set_game_mouse_capture(nullptr);
            } else if (event.type == SDL_EVENT_KEY_DOWN &&
                       event.key.key == SDLK_ESCAPE) {
                set_game_mouse_capture(nullptr);
                if (ui_handles_esc &&
                    play_state_ == EditorPlayState::playing &&
                    input_actions_ != nullptr) {
                    input_actions_->set_key_down(InputKey::escape, true);
                }
                return;
            } else if (event.type == SDL_EVENT_MOUSE_MOTION &&
                       event.motion.windowID == game_mouse_window_id_) {
                pending_game_mouse_delta_x_ += event.motion.xrel;
                pending_game_mouse_delta_y_ += event.motion.yrel;
                if (play_state_ == EditorPlayState::playing &&
                    input_actions_ != nullptr) {
                    input_actions_->add_mouse_delta(
                        event.motion.xrel, event.motion.yrel);
                }
            }
        }
        const bool allow_unfocused_ui_input =
            ui_wants_cursor &&
            (ImGui::GetCurrentContext() == nullptr ||
             !ImGui::GetIO().WantTextInput);
        if ((game_mouse_window_id_ != 0U || allow_unfocused_ui_input) &&
            play_state_ == EditorPlayState::playing &&
            input_actions_ != nullptr) {
            if ((event.type == SDL_EVENT_KEY_DOWN ||
                 event.type == SDL_EVENT_KEY_UP) &&
                !event.key.repeat) {
                const InputKey mapped =
                    input_key_from_sdl_keycode(event.key.key);
                if (mapped != InputKey::none) {
                    input_actions_->set_key_down(
                        mapped, event.type == SDL_EVENT_KEY_DOWN);
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                const InputKey mapped =
                    input_key_from_sdl_mouse_button(event.button.button);
                if (mapped != InputKey::none) {
                    input_actions_->set_key_down(
                        mapped,
                        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                }
            }
        }
        static_cast<void>(ImGui_ImplSDL3_ProcessEvent(&event));
        if (event.type == SDL_EVENT_KEY_DOWN &&
            !event.key.repeat) {
            if (event.key.key == SDLK_F5 &&
                (play_state_ == EditorPlayState::edit ||
                 (event.key.mod & SDL_KMOD_SHIFT) != 0)) {
                toggle_play_requested_ = true;
            } else if (event.key.key == SDLK_F6) {
                toggle_external_window_requested_ = true;
            }
        }
        if (external_drop_ != nullptr &&
            (event.type == SDL_EVENT_DROP_BEGIN ||
             event.type == SDL_EVENT_DROP_POSITION ||
             event.type == SDL_EVENT_DROP_FILE ||
             event.type == SDL_EVENT_DROP_COMPLETE)) {
            if (play_state_ != EditorPlayState::edit ||
                !asset_import_enabled_) {
                external_drop_->sources.clear();
                external_drop_->batch_active = false;
                external_drop_->hover = false;
                asset_error_ =
                    !asset_import_enabled_
                    ? "Packaged assets are browse-only; import requires "
                      "a project checkout."
                    : "Asset import is available only in Edit mode.";
                return;
            }
            if (event.type == SDL_EVENT_DROP_BEGIN) {
                external_drop_->sources.clear();
                external_drop_->batch_active = true;
                external_drop_->hover = false;
                return;
            }
            if (event.type == SDL_EVENT_DROP_POSITION) {
                external_drop_->hover =
                    point_inside_asset_drop_region(
                        event.drop, *external_drop_);
                return;
            }
            if (event.type == SDL_EVENT_DROP_FILE) {
                if (!external_drop_->batch_active) {
                    external_drop_->sources.clear();
                    external_drop_->batch_active = true;
                }
                external_drop_->hover =
                    point_inside_asset_drop_region(
                        event.drop, *external_drop_);
                if (!external_drop_->hover ||
                    event.drop.data == nullptr) {
                    return;
                }
                if (external_drop_->sources.size() >=
                    maximum_import_sources) {
                    asset_error_ =
                        "A dropped import batch is limited to 128 files.";
                    return;
                }
                const std::string_view path_text(event.drop.data);
                if (path_text.empty() ||
                    path_text.size() >
                        maximum_import_path_bytes) {
                    asset_error_ =
                        "A dropped asset path is empty or too long.";
                    return;
                }
                external_drop_->sources.push_back(
                    path_from_utf8(path_text));
                return;
            }
            if (event.type == SDL_EVENT_DROP_COMPLETE) {
                external_drop_->batch_active = false;
                external_drop_->hover = false;
                if (!external_drop_->sources.empty()) {
                    queue_import_sources(
                        std::move(external_drop_->sources));
                    external_drop_->sources.clear();
                }
            }
        }
    }
}

void EditorUi::synchronize_renderer(Renderer& renderer) {
    if (context_ == nullptr) {
        return;
    }
    if (known_swapchain_generation_ !=
        renderer.swapchain_generation()) {
        // Renderer swapchain recreation already establishes device idle
        // before retiring backend-visible formats and image descriptors.
        const VkFormat format =
            renderer.swapchain().color_format();
        const std::uint32_t image_count =
            static_cast<std::uint32_t>(
                renderer.swapchain().images().size());
        if (known_swapchain_format_ != format ||
            known_swapchain_image_count_ != image_count) {
            // The Vulkan multi-viewport shutdown destroys platform viewport
            // data as well, so both backends must be restarted together.
            unregister_scene_texture();
            shutdown_vulkan_backend();
            ImGui_ImplSDL3_Shutdown();
            if (!ImGui_ImplSDL3_InitForVulkan(main_window_)) {
                fatal(
                    "Unable to restart Dear ImGui SDL3 backend.");
            }
            initialize_vulkan_backend(renderer);
            register_scene_texture(renderer);
            return;
        }
        known_swapchain_generation_ =
            renderer.swapchain_generation();
    }
    if (known_viewport_generation_ !=
        renderer.editor_viewport_generation()) {
        // prepare_editor_viewport() waited before replacing the sampled image.
        unregister_scene_texture();
        register_scene_texture(renderer);
    }
}

bool EditorUi::select_node(
    SceneDocument& scene,
    const SceneNodeId id) noexcept {
    if (id && !scene.valid(id)) {
        return false;
    }
    selected_node_ = id;
    name_buffer_node_ = {};
    inspector_edit_snapshot_.reset();
    gizmo_edit_snapshot_.reset();
    std::uint32_t traversed = 0U;
    for (SceneNodeId ancestor = id ? scene.parent(id) : SceneNodeId{};
         ancestor && traversed < max_scene_nodes;
         ancestor = scene.parent(ancestor), ++traversed) {
        scene.set_expanded(ancestor, true);
    }
    hierarchy_revision_ = 0U;
    return true;
}

bool EditorUi::duplicate_selection(
    SceneDocument& scene,
    std::uint32_t& duplicated_nodes,
    std::string& error) {
    stop_sequencer_preview(scene);
    if (play_state_ != EditorPlayState::edit ||
        !scene.valid(selected_node_)) {
        duplicated_nodes = 0U;
        error = "Select a valid scene node in Edit mode before duplicating.";
        return false;
    }
    SceneDocument snapshot = scene;
    const SceneNodeId duplicate = duplicate_scene_subtree(
        scene, selected_node_, duplicated_nodes, error);
    if (!duplicate) {
        return false;
    }
    push_undo_snapshot(std::move(snapshot));
    static_cast<void>(select_node(scene, duplicate));
    return true;
}

bool EditorUi::focus_selection(
    const SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera) const noexcept {
    if (!scene.valid(selected_node_)) {
        return false;
    }
    glm::vec3 target{};
    float distance = 4.0F;
    if (const auto bounds = renderer.editor_node_bounds(selected_node_)) {
        target = bounds->center;
        distance = std::clamp(
            glm::length(bounds->extent) * 2.6F,
            2.5F,
            250.0F);
    } else if (const auto world =
                   scene_world_matrix(scene, selected_node_)) {
        target = glm::vec3((*world)[3]);
    } else {
        return false;
    }
    camera.focus(target, distance);
    renderer.request_exposure_reset();
    return true;
}

void EditorUi::begin_frame(
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera,
    const PerformanceSnapshot& performance,
    const std::chrono::steady_clock::time_point now) {
    play_display_fps_ = static_cast<float>(performance.average_fps);
    if (game_mouse_window_id_ != 0U) {
        const SDL_Window* const captured =
            SDL_GetWindowFromID(game_mouse_window_id_);
        if (captured == nullptr || SDL_GetKeyboardFocus() != captured) {
            set_game_mouse_capture(nullptr);
        }
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    camera_input_ = {};
    external_viewport_hovered_ = false;
    external_viewport_focused_ = false;
    process_import_pipeline();
    if (!pending_model_path_.empty() && !renderer.assets_pending()) {
        auto path = std::move(pending_model_path_);
        auto name = std::move(pending_model_name_);
        pending_model_path_.clear();
        pending_model_name_.clear();
        const auto previous_selection = selected_node_;
        selected_node_ = scene.valid(pending_model_parent_) ? pending_model_parent_ : SceneNodeId{};
        if (!add_model_asset_to_scene(scene, renderer, path, name)) {
            selected_node_ = previous_selection;
        }
    }
    if (saved_content_revision_ == 0U) {
        saved_content_revision_ = scene.content_revision();
        saved_structure_revision_ = scene.structure_revision();
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (play_state_ == EditorPlayState::edit &&
        io.KeyCtrl &&
        !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            save_requested_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            open_requested_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_I, false) &&
            !import_activity_pending()) {
            import_dialog_requested_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            static_cast<void>(undo_scene(scene));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            static_cast<void>(redo_scene(scene));
        }
    }

    if (toggle_play_requested_) {
        if (play_state_ == EditorPlayState::edit) {
            stop_sequencer_preview(scene, &renderer, &camera);
            start_play(scene);
        } else {
            stop_play(scene);
        }
        toggle_play_requested_ = false;
    }
    if (toggle_external_window_requested_ &&
        platform_windows_enabled_ &&
        play_state_ != EditorPlayState::edit) {
        set_game_mouse_capture(nullptr);
        external_game_window_open_ =
            !external_game_window_open_;
    }
    toggle_external_window_requested_ = false;

    const ImGuiDockNodeFlags dock_flags =
        ImGuiDockNodeFlags_PassthruCentralNode;
    const ImGuiViewport* const main_viewport =
        ImGui::GetMainViewport();
    const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(
        0U, ImGui::GetMainViewport(), dock_flags);
    if (!default_layout_initialized_ ||
        reset_layout_requested_) {
        ImGuiDockNode* const node =
            ImGui::DockBuilderGetNode(dockspace_id);
        if (node != nullptr &&
            (node->IsEmpty() || reset_layout_requested_)) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(
                dockspace_id,
                ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(
                dockspace_id, main_viewport->WorkSize);

            ImGuiID center = dockspace_id;
            ImGuiID left = 0U;
            ImGuiID right = 0U;
            ImGuiID bottom = 0U;
            ImGuiID top = 0U;
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Left,
                0.22F,
                &left,
                &center);
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Right,
                0.32F,
                &right,
                &center);
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Down,
                0.30F,
                &bottom,
                &center);
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Up,
                0.16F,
                &top,
                &center);
            if (ImGuiDockNode* const toolbar_node =
                    ImGui::DockBuilderGetNode(top);
                toolbar_node != nullptr) {
                toolbar_node->LocalFlags |=
                    ImGuiDockNodeFlags_NoTabBar |
                    ImGuiDockNodeFlags_NoWindowMenuButton |
                    ImGuiDockNodeFlags_NoCloseButton;
            }
            ImGui::DockBuilderDockWindow("Toolbar", top);
            ImGui::DockBuilderDockWindow(
                "Scene Hierarchy", left);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Material Editor", right);
            ImGui::DockBuilderDockWindow("Renderer", right);
            ImGui::DockBuilderDockWindow("Landscape", right);
            ImGui::DockBuilderDockWindow("Audio & Input", right);
            ImGui::DockBuilderDockWindow("Animation & Gameplay", right);
            ImGui::DockBuilderDockWindow("UI & Localization", right);
            ImGui::DockBuilderDockWindow("Assets", bottom);
            ImGui::DockBuilderDockWindow("Sequencer", bottom);
            ImGui::DockBuilderDockWindow("Scene View", center);
            ImGui::DockBuilderFinish(dockspace_id);
        }
        default_layout_initialized_ = true;
        reset_layout_requested_ = false;
    }
    build_main_menu(scene, renderer, camera);
    build_toolbar(scene, renderer, camera);
    build_hierarchy(scene);
    build_inspector(scene, renderer, camera);
    build_viewport(scene, renderer, camera, now);
    build_assets(scene, renderer);
    build_landscape(scene, renderer, camera);
    if (import_dialog_requested_) {
        queue_import_sources({});
        import_dialog_requested_ = false;
    }
    build_import_window();
    build_import_results();
    build_material_editor(renderer);
    build_audio_input_panel();
    build_animation_gameplay_panel(scene, renderer);
    build_ui_localization_panel(scene, renderer);
    build_sequencer_panel(scene, renderer, camera);
    build_performance(scene, renderer, performance);
    build_external_game_window(now);
    process_file_commands(scene, renderer, camera);
    apply_deferred_scene_commands(scene);
    update_camera_input();
}

ImDrawData* EditorUi::end_frame() noexcept {
    ImGui::Render();
    return ImGui::GetDrawData();
}

void EditorUi::finish_platform_windows(const bool render) {
    if (!platform_windows_enabled_) {
        return;
    }
    ImGui::UpdatePlatformWindows();
    if (render) {
        ImGui::RenderPlatformWindowsDefault();
    }
}

std::optional<VkExtent2D> EditorUi::consume_viewport_resize(
    const std::chrono::steady_clock::time_point now) noexcept {
    constexpr auto debounce = std::chrono::milliseconds(140);
    if (same_extent(
            pending_viewport_extent_,
            requested_viewport_extent_) ||
        viewport_extent_changed_at_.time_since_epoch().count() == 0 ||
        now - viewport_extent_changed_at_ < debounce) {
        return std::nullopt;
    }
    requested_viewport_extent_ = pending_viewport_extent_;
    return requested_viewport_extent_;
}

void EditorUi::initialize_vulkan_backend(Renderer& renderer) {
    imgui_color_format_ = renderer.swapchain().color_format();
    imgui_rendering_info_.pColorAttachmentFormats =
        &imgui_color_format_;
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = context_->instance();
    init_info.PhysicalDevice = context_->physical_device();
    init_info.Device = context_->device();
    init_info.QueueFamily = context_->queue_families().graphics;
    init_info.Queue = context_->graphics_queue();
    init_info.DescriptorPoolSize = 512U;
    init_info.MinImageCount = 2U;
    init_info.ImageCount = static_cast<std::uint32_t>(
        renderer.swapchain().images().size());
    init_info.PipelineInfoMain.MSAASamples =
        VK_SAMPLE_COUNT_1_BIT;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo =
        imgui_rendering_info_;
    init_info.PipelineInfoForViewports.MSAASamples =
        VK_SAMPLE_COUNT_1_BIT;
    init_info.PipelineInfoForViewports.PipelineRenderingCreateInfo =
        imgui_rendering_info_;
    init_info.UseDynamicRendering = true;
    init_info.CheckVkResultFn = check_imgui_vk_result;
    init_info.MinAllocationSize = 1024U * 1024U;
    if (!ImGui_ImplVulkan_Init(&init_info)) {
        fatal("Unable to initialize Dear ImGui Vulkan backend.");
    }
    vulkan_backend_initialized_ = true;
    known_swapchain_generation_ = renderer.swapchain_generation();
    known_swapchain_format_ = imgui_color_format_;
    known_swapchain_image_count_ =
        static_cast<std::uint32_t>(
            renderer.swapchain().images().size());
}

void EditorUi::shutdown_vulkan_backend() noexcept {
    set_game_mouse_capture(nullptr);
    // Init may fail after publishing backend data but before returning true.
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplVulkan_Shutdown();
    }
    vulkan_backend_initialized_ = false;
}

void EditorUi::register_scene_texture(Renderer& renderer) {
    scene_texture_ = ImGui_ImplVulkan_AddTexture(
        renderer.editor_viewport_image_view(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (scene_texture_ == VK_NULL_HANDLE) {
        fatal("Unable to register the editor scene texture.");
    }
    known_viewport_generation_ =
        renderer.editor_viewport_generation();
}

void EditorUi::unregister_scene_texture() noexcept {
    if (scene_texture_ != VK_NULL_HANDLE &&
        vulkan_backend_initialized_) {
        ImGui_ImplVulkan_RemoveTexture(scene_texture_);
    }
    scene_texture_ = VK_NULL_HANDLE;
}

void EditorUi::build_main_menu(
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        const bool editable =
            play_state_ == EditorPlayState::edit;
        if (ImGui::MenuItem(
                "Save scene",
                "Ctrl+S",
                false,
                editable)) {
            save_requested_ = true;
        }
        if (ImGui::MenuItem(
                "Open scene",
                "Ctrl+O",
                false,
                editable)) {
            pending_open_scene_path_.clear();
            open_requested_ = true;
        }
        if (ImGui::BeginMenu("Project scenes", editable)) {
            const std::filesystem::path scenes_dir =
                asset_browser_.root() / "scenes";
            std::error_code fs_ec;
            bool any_scene = false;
            if (!asset_browser_.root().empty() &&
                std::filesystem::is_directory(scenes_dir, fs_ec) && !fs_ec) {
                std::vector<std::filesystem::path> scene_files;
                for (const auto& entry :
                     std::filesystem::directory_iterator(scenes_dir, fs_ec)) {
                    if (fs_ec) {
                        break;
                    }
                    std::error_code item_ec;
                    if (entry.is_regular_file(item_ec) && !item_ec &&
                        entry.path().extension() == ".json") {
                        scene_files.push_back(entry.path());
                    }
                }
                std::sort(scene_files.begin(), scene_files.end());
                for (const std::filesystem::path& candidate : scene_files) {
                    any_scene = true;
                    const std::string label = candidate.filename().string();
                    const bool is_current = candidate == scene_path_;
                    if (ImGui::MenuItem(label.c_str(), nullptr, is_current)) {
                        pending_open_scene_path_ = candidate;
                        open_requested_ = true;
                    }
                }
            }
            if (!any_scene) {
                ImGui::TextDisabled("No saved scenes in assets/scenes/");
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(
                "Import assets...",
                "Ctrl+I",
                false,
                editable && !import_activity_pending() &&
                    asset_import_enabled_ &&
                    import_worker_ != nullptr &&
                    !import_worker_->failed())) {
            import_dialog_requested_ = true;
        }
        ImGui::Separator();
        if (!project_root_.empty()) {
            if (ImGui::MenuItem("Build / Update Game Executable")) {
                BundleProjectRuntimeResult bundle_res{};
                std::string bundle_err;
                if (bundle_project_runtime(
                        project_root_, bundle_res, bundle_err)) {
                    scene_file_status_ =
                        "Built standalone game: " +
                        bundle_res.executable_path.filename().string();
                } else {
                    scene_file_status_ = "Game build failed: " + bundle_err;
                }
            }
            if (ImGui::MenuItem("Project Icon Utility...")) {
#if defined(_WIN32)
                constexpr std::string_view icon_tool_exe_name{
                    "vulkan_icon_tool.exe"};
#else
                constexpr std::string_view icon_tool_exe_name{
                    "vulkan_icon_tool"};
#endif
                if (const auto tool_exe =
                        find_sibling_executable(icon_tool_exe_name)) {
                    const std::u8string u8_proj = project_root_.u8string();
                    std::string proj_utf8(u8_proj.size(), '\0');
                    std::transform(
                        u8_proj.begin(),
                        u8_proj.end(),
                        proj_utf8.begin(),
                        [](const char8_t ch) {
                            return static_cast<char>(ch);
                        });
                    const std::vector<std::string> args{
                        "--project",
                        std::move(proj_utf8),
                    };
                    std::string launch_error;
                    if (!launch_detached_executable(
                            *tool_exe, args, project_root_, launch_error)) {
                        scene_file_status_ =
                            "Icon Utility launch failed: " + launch_error;
                    }
                } else {
                    scene_file_status_ =
                        "Icon Utility executable was not found next to editor.";
                }
            }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Project Manager...")) {
#if defined(_WIN32)
            constexpr std::string_view pm_exe_name{"vulkan_project_manager.exe"};
#else
            constexpr std::string_view pm_exe_name{"vulkan_project_manager"};
#endif
            if (const auto pm_exe = find_sibling_executable(pm_exe_name)) {
                std::string launch_error;
                if (!launch_detached_executable(
                        *pm_exe, {}, pm_exe->parent_path(), launch_error)) {
                    scene_file_status_ =
                        "Project Manager launch failed: " + launch_error;
                }
            } else {
                scene_file_status_ =
                    "Project Manager executable was not found next to editor.";
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            quit_requested_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool editable = play_state_ == EditorPlayState::edit;
        if (ImGui::MenuItem(
                "Undo",
                "Ctrl+Z",
                false,
                editable && can_undo())) {
            static_cast<void>(undo_scene(scene));
        }
        if (ImGui::MenuItem(
                "Redo",
                "Ctrl+Y",
                false,
                editable && can_redo())) {
            static_cast<void>(redo_scene(scene));
        }
        ImGui::Separator();
        const bool has_selection = scene.valid(selected_node_);
        if (ImGui::MenuItem(
                "Duplicate selected",
                "Ctrl+D",
                false,
                editable && has_selection)) {
            pending_duplicate_ = selected_node_;
        }
        if (ImGui::MenuItem(
                "Delete selected",
                "Delete",
                false,
                editable && has_selection)) {
            pending_delete_ = selected_node_;
        }
        if (ImGui::MenuItem(
                "Focus selected",
                "F",
                false,
                has_selection)) {
            focus_selection_requested_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Scene")) {
        auto prepare_scene_edit = [&]() {
            stop_sequencer_preview(scene, &renderer, &camera);
        };
        const SceneNodeId parent =
            scene.valid(selected_node_)
            ? selected_node_
            : SceneNodeId{};
        if (ImGui::MenuItem(
                "Add folder",
                nullptr,
                false,
                play_state_ == EditorPlayState::edit)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            selected_node_ = scene.create_node(
                "Folder", SceneNodeKind::folder, parent);
        }
        if (ImGui::MenuItem(
                "Add cube",
                nullptr,
                false,
                play_state_ == EditorPlayState::edit)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            const SceneNodeId cube = scene.create_node(
                "Cube", SceneNodeKind::cube, parent);
            if (auto node = scene.node(cube)) {
                node->get().transform.translation =
                    glm::vec3(0.0F, 0.0F, -5.0F);
                scene.mark_content_changed();
            }
            selected_node_ = cube;
        }
        if (ImGui::MenuItem("Add camera", nullptr, false,
                            play_state_ == EditorPlayState::edit)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            const SceneNodeId cam_node = scene.create_node(
                "Camera", SceneNodeKind::camera, parent);
            if (auto node = scene.node(cam_node)) {
                node->get().transform.translation = glm::vec3(0.0F, 2.0F, -5.0F);
                scene.mark_content_changed();
            }
            selected_node_ = cam_node;
        }
        if (ImGui::MenuItem("Add ocean", nullptr, false,
                            play_state_ == EditorPlayState::edit)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            selected_node_ = scene.create_node("Ocean", SceneNodeKind::ocean, parent);
            if (auto node = scene.node(selected_node_)) {
                node->get().transform.translation.y = -2.0F;
                scene.mark_content_changed();
            }
        }
        const bool can_add_light =
            play_state_ == EditorPlayState::edit &&
            scene.local_light_count() < maximum_local_lights;
        if (ImGui::MenuItem("Add point light", nullptr, false, can_add_light)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            const SceneNodeId light = scene.create_node(
                "Point Light", SceneNodeKind::point_light, parent);
            if (auto node = scene.node(light)) {
                node->get().transform.translation = glm::vec3(0.0F, 2.0F, -5.0F);
                scene.mark_content_changed();
            }
            selected_node_ = light;
        }
        if (ImGui::MenuItem("Add spot light", nullptr, false, can_add_light)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            const SceneNodeId light = scene.create_node(
                "Spot Light", SceneNodeKind::spot_light, parent);
            if (auto node = scene.node(light)) {
                node->get().transform.translation = glm::vec3(0.0F, 2.0F, -5.0F);
                scene.mark_content_changed();
            }
            selected_node_ = light;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(
                "Add audio source",
                nullptr,
                false,
                play_state_ == EditorPlayState::edit)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            const SceneNodeId emitter = scene.create_node(
                "Audio Emitter", SceneNodeKind::folder, parent);
            if (emitter) {
                if (auto node = scene.node(emitter)) {
                    node->get().transform.translation =
                        glm::vec3(0.0F, 1.5F, -4.0F);
                }
                AudioSourceComponent source{};
                if (selected_asset_ &&
                    (selected_asset_->kind == AssetEntryKind::audio ||
                     selected_asset_->kind == AssetEntryKind::sound_cue) &&
                    valid_audio_asset_reference(
                        selected_asset_->relative_display)) {
                    static_cast<void>(
                        source.set_path(selected_asset_->relative_display));
                }
                static_cast<void>(scene.set_audio_source(emitter, source));
                selected_node_ = emitter;
            }
        }
        if (ImGui::MenuItem(
                "Add audio listener",
                nullptr,
                false,
                play_state_ == EditorPlayState::edit)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            const SceneNodeId listener = scene.create_node(
                "Audio Listener", SceneNodeKind::folder, parent);
            if (listener) {
                static_cast<void>(scene.set_audio_listener(
                    listener, AudioListenerComponent{}));
                selected_node_ = listener;
            }
        }
        if (ImGui::MenuItem(
                "Add reverb zone",
                nullptr,
                false,
                play_state_ == EditorPlayState::edit)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            const SceneNodeId zone = scene.create_node(
                "Reverb Zone", SceneNodeKind::folder, parent);
            if (zone) {
                static_cast<void>(scene.set_reverb_zone(
                    zone, AudioReverbZoneComponent{}));
                selected_node_ = zone;
            }
        }
        if (ImGui::MenuItem(
                "Add 3D spline",
                nullptr,
                false,
                play_state_ == EditorPlayState::edit)) {
            prepare_scene_edit();
            checkpoint_scene(scene);
            const SceneNodeId spline_node = scene.create_node(
                "Spline Path", SceneNodeKind::folder, parent);
            if (spline_node) {
                SplineComponent def_spline{};
                def_spline.basis = SplineBasis::catmull_rom;
                def_spline.closed_loop = false;
                def_spline.tension = 0.5F;
                def_spline.point_count = 4U;
                def_spline.points[0].position = glm::vec3{0.0F, 0.0F, 0.0F};
                def_spline.points[1].position = glm::vec3{4.0F, 1.0F, -4.0F};
                def_spline.points[2].position = glm::vec3{8.0F, 1.0F, -2.0F};
                def_spline.points[3].position = glm::vec3{12.0F, 0.0F, -6.0F};
                def_spline = sanitize_spline_component(def_spline);
                static_cast<void>(scene.set_spline(spline_node, def_spline));
                selected_node_ = spline_node;
                hierarchy_revision_ = 0U;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Assets")) {
        if (ImGui::MenuItem(
                "Import...",
                "Ctrl+I",
                false,
                play_state_ == EditorPlayState::edit &&
                    !import_activity_pending() &&
                    asset_import_enabled_ &&
                    import_worker_ != nullptr &&
                    !import_worker_->failed())) {
            import_dialog_requested_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        bool material_open =
            material_editor_ != nullptr &&
            material_editor_->open;
        if (ImGui::MenuItem(
                "Material Editor",
                nullptr,
                material_open)) {
            if (material_editor_ != nullptr) {
                if (!material_open) {
                    material_editor_->open = true;
                } else if (material_editor_->dirty) {
                    material_editor_->open = true;
                    material_editor_->
                        close_confirmation_requested = true;
                } else {
                    material_editor_->open = false;
                }
            }
        }
        const bool audio_input_open =
            audio_input_ui_ != nullptr && audio_input_ui_->open;
        if (ImGui::MenuItem(
                "Audio & Input",
                nullptr,
                audio_input_open) &&
            audio_input_ui_ != nullptr) {
            audio_input_ui_->open = !audio_input_open;
        }
        if (ImGui::MenuItem(
                "Animation & Gameplay",
                nullptr,
                animation_gameplay_panel_open_)) {
            animation_gameplay_panel_open_ = !animation_gameplay_panel_open_;
        }
        if (ImGui::MenuItem(
                "UI & Localization",
                nullptr,
                ui_localization_panel_open_)) {
            ui_localization_panel_open_ = !ui_localization_panel_open_;
        }
        if (ImGui::MenuItem(
                "Sequencer",
                nullptr,
                sequencer_panel_open_)) {
            sequencer_panel_open_ = !sequencer_panel_open_;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset editor layout")) {
            reset_layout_requested_ = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void EditorUi::build_toolbar(
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera) {
    ImGui::SetNextWindowSize(
        ImVec2(840.0F, 66.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(
            "Toolbar",
            nullptr,
            ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    ui_kit_.brand_lockup();
    if (!project_name_.empty()) {
        ImGui::SameLine(0.0F, 10.0F);
        ui_kit_.badge(project_name_.c_str(), EditorTone::primary);
    }
    ImGui::SameLine(0.0F, 15.0F);
    ImGui::AlignTextToFramePadding();

    const bool editable = play_state_ == EditorPlayState::edit;
    if (ui_kit_.icon_button(
            "SaveScene",
            EditorIcon::save,
            editable
                ? "Save scene  Ctrl+S"
                : "Scene editing is locked during Play",
            EditorButtonStyle::quiet,
            editable)) {
        save_requested_ = true;
    }
    ImGui::SameLine(0.0F, 5.0F);
    const bool can_import =
        editable &&
        !import_activity_pending() &&
        asset_import_enabled_ &&
        import_worker_ != nullptr &&
        !import_worker_->failed();
    if (ui_kit_.icon_button(
            "ImportAssets",
            EditorIcon::import_asset,
            can_import
                ? "Import assets  Ctrl+I"
                : "Asset import is currently unavailable",
            EditorButtonStyle::quiet,
            can_import)) {
        import_dialog_requested_ = true;
    }
    ImGui::SameLine(0.0F, 12.0F);

    if (play_state_ == EditorPlayState::edit) {
        const bool can_play = !import_activity_pending();
        if (ui_kit_.action_button(
                "Play",
                EditorIcon::play,
                "Play",
                EditorButtonStyle::primary,
                can_play)) {
            stop_sequencer_preview(scene, &renderer, &camera);
            start_play(scene);
        }
        if (!can_play &&
            ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "Close or finish asset import before Play.");
        }
    } else {
        if (ui_kit_.action_button(
                "Stop",
                EditorIcon::stop,
                "Stop",
                EditorButtonStyle::primary)) {
            stop_play(scene);
        }
        ImGui::SameLine();
        if (play_state_ == EditorPlayState::playing) {
            if (ui_kit_.action_button(
                    "Pause",
                    EditorIcon::pause,
                    "Pause")) {
                set_game_mouse_capture(nullptr);
                play_state_ = EditorPlayState::paused;
            }
        } else {
            if (ui_kit_.action_button(
                    "Resume",
                    EditorIcon::resume,
                    "Resume",
                    EditorButtonStyle::secondary,
                    true,
                    true)) {
                play_state_ = EditorPlayState::playing;
                game_mouse_capture_requested_ = true;
            }
        }
    }
    ImGui::SameLine();
    const bool can_open_external =
        platform_windows_enabled_ &&
        play_state_ != EditorPlayState::edit;
    if (ui_kit_.icon_button(
            "ExternalGameWindow",
            EditorIcon::external_window,
            external_game_window_open_
                ? "Close separate game window  F6"
                : "Open game in a separate window  F6",
            EditorButtonStyle::secondary,
            can_open_external,
            external_game_window_open_)) {
        set_game_mouse_capture(nullptr);
        external_game_window_open_ =
            !external_game_window_open_;
    }
    if (!platform_windows_enabled_ &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Separate platform windows require a shared graphics/present "
            "queue and a supported desktop backend.");
    }

    ImGui::SameLine(0.0F, 14.0F);
    ui_kit_.badge(
        play_state_name(play_state_),
        play_state_ == EditorPlayState::edit
            ? EditorTone::neutral
            : play_state_ == EditorPlayState::playing
            ? EditorTone::success
            : EditorTone::warning);
    ImGui::SameLine(0.0F, 6.0F);
    const bool dirty = scene_is_dirty(scene);
    ui_kit_.badge(
        dirty ? "UNSAVED" : "SAVED",
        dirty ? EditorTone::warning : EditorTone::success);
    if (!scene_file_status_.empty() &&
        ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", scene_file_status_.c_str());
    }
    ImGui::End();
}

void EditorUi::build_hierarchy(SceneDocument& scene) {
    ImGui::SetNextWindowSize(
        ImVec2(310.0F, 570.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene Hierarchy")) {
        ImGui::End();
        return;
    }
    ui_kit_.panel_header(
        EditorIcon::folder,
        "SCENE HIERARCHY",
        "Objects, collections and visibility");
    ImGui::SetNextItemWidth(-1.0F);
    static_cast<void>(ImGui::InputTextWithHint(
        "##HierarchyFilter",
        "Search objects...",
        hierarchy_filter_.data(),
        hierarchy_filter_.size()));
    if (ImGui::SmallButton("Expand all")) {
        std::vector<SceneHierarchyRow> all_rows;
        all_rows.reserve(scene.size());
        scene.build_hierarchy_rows(
            all_rows, {}, SceneHierarchyScope::all);
        for (const SceneHierarchyRow& row : all_rows) {
            if (row.has_children) {
                scene.set_expanded(row.id, true);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Collapse all")) {
        std::vector<SceneHierarchyRow> all_rows;
        all_rows.reserve(scene.size());
        scene.build_hierarchy_rows(
            all_rows, {}, SceneHierarchyScope::all);
        for (const SceneHierarchyRow& row : all_rows) {
            if (row.has_children) {
                scene.set_expanded(row.id, false);
            }
        }
    }
    const bool filter_changed =
        hierarchy_filter_ != previous_hierarchy_filter_;
    if (hierarchy_revision_ != scene.structure_revision() ||
        filter_changed) {
        scene.build_hierarchy_rows(
            hierarchy_rows_, hierarchy_filter_.data());
        hierarchy_revision_ = scene.structure_revision();
        previous_hierarchy_filter_ = hierarchy_filter_;
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(hierarchy_rows_.size()));
    while (clipper.Step()) {
        for (int row_index = clipper.DisplayStart;
             row_index < clipper.DisplayEnd;
             ++row_index) {
            const SceneHierarchyRow row =
                hierarchy_rows_[static_cast<std::size_t>(row_index)];
            const auto node_reference = scene.node(row.id);
            if (!node_reference) {
                continue;
            }
            SceneNode& node = node_reference->get();
            ImGui::PushID(static_cast<int>(row.id.index));
            ImGui::PushID(static_cast<int>(row.id.generation));
            const bool selected = selected_node_ == row.id;
            const EditorTreeRowResult row_result =
                ui_kit_.hierarchy_row(
                    "Node",
                    node_icon(node.kind),
                    node.name.c_str(),
                    row.depth,
                    row.has_children &&
                        hierarchy_filter_[0] == '\0',
                    node.expanded,
                    selected,
                    node.visible);
            if (row_result.selected) {
                static_cast<void>(select_node(scene, row.id));
            }
            if (row_result.expansion_toggled) {
                scene.set_expanded(row.id, !node.expanded);
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(
                    "VCR_SCENE_NODE",
                    &row.id,
                    sizeof(row.id));
                ImGui::TextUnformatted(node.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(
                            "VCR_SCENE_NODE");
                    payload != nullptr &&
                    payload->DataSize ==
                        static_cast<int>(sizeof(SceneNodeId))) {
                    const auto* dragged =
                        static_cast<const SceneNodeId*>(payload->Data);
                    pending_reparent_child_ = *dragged;
                    pending_reparent_parent_ = row.id;
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(
                        "Duplicate",
                        "Ctrl+D",
                        false,
                        play_state_ == EditorPlayState::edit)) {
                    pending_duplicate_ = row.id;
                }
                if (ImGui::MenuItem("Focus", "F")) {
                    static_cast<void>(select_node(scene, row.id));
                    focus_selection_requested_ = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                        "Delete",
                        nullptr,
                        false,
                        play_state_ == EditorPlayState::edit)) {
                    delete_confirmation_ = row.id;
                    delete_popup_requested_ = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    if (delete_popup_requested_) {
        ImGui::OpenPopup("Delete subtree?");
        delete_popup_requested_ = false;
    }
    if (ImGui::BeginPopupModal(
            "Delete subtree?",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto candidate = scene.node(delete_confirmation_);
        if (candidate) {
            ImGui::Text(
                "Delete \"%s\" and all of its children?",
                candidate->get().name.c_str());
            ImGui::TextDisabled(
                "The deleted subtree can be restored with Undo.");
            if (ImGui::Button("Delete")) {
                pending_delete_ = delete_confirmation_;
                delete_confirmation_ = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                delete_confirmation_ = {};
                ImGui::CloseCurrentPopup();
            }
        } else {
            delete_confirmation_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (delete_confirmation_ &&
        !ImGui::IsPopupOpen("Delete subtree?")) {
        delete_confirmation_ = {};
    }
    char hierarchy_status[96]{};
    std::snprintf(
        hierarchy_status,
        sizeof(hierarchy_status),
        "%u OBJECTS  /  %zu VISIBLE",
        scene.size(),
        hierarchy_rows_.size());
    ui_kit_.badge(hierarchy_status, EditorTone::neutral);
    ImGui::End();
}

namespace {

[[nodiscard]] bool edit_clip_name_field(
    const char* const combo_id,
    const char* const input_id,
    std::array<char, 128U>& buffer,
    const std::vector<AnimationClip>* const clips) {
    bool modified = false;
    if (clips != nullptr && !clips->empty()) {
        const char* preview =
            buffer[0] != '\0' ? buffer.data() : "(select clip)";
        if (ImGui::BeginCombo(combo_id, preview)) {
            for (const AnimationClip& clip : *clips) {
                const bool selected =
                    std::string_view(buffer.data()) == clip.name;
                if (ImGui::Selectable(clip.name.c_str(), selected)) {
                    std::array<char, 128U> next{};
                    const std::size_t n =
                        std::min(clip.name.size(), next.size() - 1U);
                    std::memcpy(next.data(), clip.name.data(), n);
                    buffer = next;
                    modified = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    if (ImGui::InputText(input_id, buffer.data(), buffer.size())) {
        modified = true;
    }
    return modified;
}

[[nodiscard]] bool edit_bone_name_field(
    const char* const combo_id,
    const char* const input_id,
    std::array<char, 64U>& buffer,
    const CookedSkeletonAsset* const skeleton) {
    bool modified = false;
    if (skeleton != nullptr && !skeleton->bones.empty()) {
        const char* preview =
            buffer[0] != '\0' ? buffer.data() : "(none)";
        if (ImGui::BeginCombo(combo_id, preview)) {
            if (ImGui::Selectable("(none)", buffer[0] == '\0')) {
                buffer.fill('\0');
                modified = true;
            }
            for (const SkeletonBone& bone : skeleton->bones) {
                const bool selected =
                    std::string_view(buffer.data()) == bone.name;
                if (ImGui::Selectable(bone.name.c_str(), selected)) {
                    std::array<char, 64U> next{};
                    const std::size_t n =
                        std::min(bone.name.size(), next.size() - 1U);
                    std::memcpy(next.data(), bone.name.data(), n);
                    buffer = next;
                    modified = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    if (ImGui::InputText(input_id, buffer.data(), buffer.size())) {
        modified = true;
    }
    return modified;
}

} // namespace

void EditorUi::build_inspector(
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera) {
    ImGui::SetNextWindowSize(
        ImVec2(340.0F, 570.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }
    const bool preview_locked = sequencer_preview_active();
    const auto selected = scene.node(selected_node_);
    if (!selected) {
        ui_kit_.panel_header(
            EditorIcon::eye,
            "INSPECTOR",
            "Properties and components");
        if (preview_locked) {
            ui_kit_.badge("SEQUENCER PREVIEW ACTIVE", EditorTone::warning);
            if (ui_kit_.action_button(
                    "StopPreviewEmptyInspector",
                    EditorIcon::stop,
                    "Stop Sequencer Preview",
                    EditorButtonStyle::secondary,
                    true)) {
                stop_sequencer_preview(scene, &renderer, &camera);
            }
        }
        ui_kit_.empty_state(
            EditorIcon::cube,
            "Nothing selected",
            "Choose an object in Scene Hierarchy");
        ImGui::End();
        return;
    }
    SceneNode& node = selected->get();
    const SceneNode node_before_edit = node;
    ui_kit_.panel_header(
        node_icon(node.kind),
        node.name.c_str(),
        to_string(node.kind).data());

    if (preview_locked) {
        ui_kit_.badge("SEQUENCER PREVIEW ACTIVE", EditorTone::warning);
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop Sequencer Preview")) {
            stop_sequencer_preview(scene, &renderer, &camera);
        }
    }

    const bool editable =
        play_state_ == EditorPlayState::edit && !sequencer_preview_active();
    ImGui::BeginDisabled(!editable);
    if (name_buffer_node_ != selected_node_) {
        selected_name_buffer_.fill('\0');
        const std::size_t copy_size = std::min(
            node.name.size(), selected_name_buffer_.size() - 1U);
        std::memcpy(
            selected_name_buffer_.data(),
            node.name.data(),
            copy_size);
        name_buffer_node_ = selected_node_;
    }
    bool changed = false;
    if (ui_kit_.begin_property_grid("IdentityProperties")) {
        ui_kit_.property_row("Name");
        if (ImGui::InputText(
                "##Name",
                selected_name_buffer_.data(),
                selected_name_buffer_.size())) {
            if (!inspector_edit_snapshot_) {
                // InputText mutates the temporary buffer first, so the full
                // document can still be captured before rename_node advances
                // the structure revision. This keeps dirty state correct
                // after undoing a rename.
                inspector_edit_snapshot_ = scene;
            }
            if (scene.rename_node(
                    selected_node_, selected_name_buffer_.data())) {
                hierarchy_revision_ = 0U;
            }
        }
        ui_kit_.property_row("Type");
        ui_kit_.badge(
            to_string(node.kind).data(),
            EditorTone::info);
        if (const auto node_uuid = scene.uuid(selected_node_)) {
            const auto uuid_chars = format_scene_uuid_chars(*node_uuid);
            ui_kit_.property_row("UUID");
            ImGui::TextDisabled("%.8s...", uuid_chars.data());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", uuid_chars.data());
            }
            ImGui::SameLine();
            ImGui::EndDisabled();
            if (ImGui::SmallButton("Copy##NodeUuid")) {
                ImGui::SetClipboardText(uuid_chars.data());
            }
            ImGui::BeginDisabled(!editable);
        }
        ui_kit_.property_row("Visible");
        changed |= ImGui::Checkbox(
            "##Visible",
            &node.visible);
        ImGui::SameLine();
        ImGui::TextDisabled(
            node.visible ? "Rendered" : "Hidden");
        if (node.kind == SceneNodeKind::model) {
            ui_kit_.property_row("Asset");
            ImGui::TextWrapped("%s", node.asset_path.c_str());
        }
        ui_kit_.end_property_grid();
    }

    const bool has_transform = true;
    if (has_transform) {
        ui_kit_.section_header(
            EditorIcon::cube,
            "Transform");
        if (ui_kit_.begin_property_grid("TransformProperties")) {
            changed |= ui_kit_.drag_vec3(
                "Position",
                "Position",
                &node.transform.translation.x,
                0.05F,
                0.0F);
            changed |= ui_kit_.drag_vec3(
                "Rotation",
                "Rotation",
                &node.transform.rotation_degrees.x,
                0.25F,
                0.0F);
            changed |= ui_kit_.drag_vec3(
                "Scale",
                "Scale",
                &node.transform.scale.x,
                0.02F,
                1.0F,
                0.0001F,
                1000.0F);
            ui_kit_.end_property_grid();
        }
    }
    if (node.kind == SceneNodeKind::cube ||
        node.kind == SceneNodeKind::ground) {
        ui_kit_.section_header(
            EditorIcon::material,
            "PBR Material");
        if (ui_kit_.begin_property_grid("MaterialProperties")) {
            ui_kit_.property_row("Base color");
            changed |= ImGui::ColorEdit3(
                "##BaseColor",
                &node.material.base_color.x);
            ui_kit_.property_row("Metallic");
            changed |= ImGui::SliderFloat(
                "##Metallic",
                &node.material.metallic,
                0.0F,
                1.0F,
                "%.2f");
            ui_kit_.property_row("Roughness");
            changed |= ImGui::SliderFloat(
                "##Roughness",
                &node.material.roughness,
                minimum_pbr_roughness,
                1.0F,
                "%.2f");
            ui_kit_.property_row("Occlusion");
            changed |= ImGui::SliderFloat(
                "##AmbientOcclusion",
                &node.material.ambient_occlusion,
                0.0F,
                1.0F,
                "%.2f");
            ui_kit_.property_row("IOR");
            changed |= ImGui::DragFloat(
                "##Ior", &node.material.ior, 0.01F,
                1.0F, std::numeric_limits<float>::max(), "%.6g",
                ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat);
            ui_kit_.property_row("Shading model");
            if (ImGui::BeginCombo("##MaterialModel", to_token(node.material.model).data())) {
                for (const auto model : {
                         MaterialModel::standard, MaterialModel::unlit,
                         MaterialModel::subsurface}) {
                    if (ImGui::Selectable(to_token(model).data(), node.material.model == model)) {
                        changed |= node.material.model != model;
                        node.material.model = model;
                        if (model == MaterialModel::subsurface) {
                            if (node.material.subsurface_profile == SubsurfaceProfile::none) {
                                node.material.subsurface_profile = SubsurfaceProfile::medium;
                            }
                            if (node.material.subsurface_strength <= 0.0F) {
                                node.material.subsurface_strength = 0.65F;
                            }
                        }
                    }
                }
                ImGui::EndCombo();
            }
            if (node.material.model == MaterialModel::subsurface) {
                ui_kit_.property_row("SSS profile");
                if (ImGui::BeginCombo(
                        "##SubsurfaceProfile", to_token(node.material.subsurface_profile).data())) {
                    for (const auto profile : {
                             SubsurfaceProfile::none, SubsurfaceProfile::narrow,
                             SubsurfaceProfile::medium, SubsurfaceProfile::wide,
                             SubsurfaceProfile::foliage}) {
                        if (ImGui::Selectable(
                                to_token(profile).data(),
                                node.material.subsurface_profile == profile)) {
                            changed |= node.material.subsurface_profile != profile;
                            node.material.subsurface_profile = profile;
                        }
                    }
                    ImGui::EndCombo();
                }
                ui_kit_.property_row("SSS strength");
                changed |= ImGui::SliderFloat(
                    "##SubsurfaceStrength", &node.material.subsurface_strength,
                    0.0F, 1.0F, "%.2f");
            }
            ui_kit_.end_property_grid();
        }
    }
    if (node.kind == SceneNodeKind::ocean) {
        ui_kit_.section_header(EditorIcon::ground, "Ocean");
        if (ui_kit_.begin_property_grid("OceanProperties")) {
            ui_kit_.property_row("Wave height (m)");
            changed |= ImGui::SliderFloat("##WaveHeight", &node.ocean.wave_height,
                0.0F, 10.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ui_kit_.property_row("Wave speed");
            changed |= ImGui::SliderFloat("##WaveSpeed", &node.ocean.wave_speed,
                0.0F, 10.0F, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
            ui_kit_.property_row("Wave size (m)");
            changed |= ImGui::SliderFloat("##WaveLength", &node.ocean.wave_length,
                1.0F, 200.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ui_kit_.property_row("Direction");
            changed |= ImGui::SliderFloat("##WaveDirection", &node.ocean.wave_direction_degrees,
                0.0F, 360.0F, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Wave travel: 0 deg = +X, 90 deg = +Z.");
            ui_kit_.property_row("Water color");
            changed |= ImGui::ColorEdit3("##OceanColor", node.ocean.color.data());
            ui_kit_.property_row("Optical depth (m)");
            changed |= ImGui::DragFloat("##OceanDepth", &node.ocean.depth, 0.1F,
                0.0F, 1000.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Water thickness for color absorption and transparency.");
            ui_kit_.property_row("Transparency");
            changed |= ImGui::SliderFloat("##OceanTransparency", &node.ocean.transparency,
                0.0F, 1.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("0 = opaque. Depth reduces transmission; grazing angles reflect the sky.");
            ui_kit_.property_row("Defaults");
            if (ImGui::Button("Reset ocean")) {
                node.ocean = OceanSettings{};
                changed = true;
            }
            ui_kit_.end_property_grid();
        }
    }
    if (node.kind == SceneNodeKind::point_light ||
        node.kind == SceneNodeKind::spot_light) {
        ui_kit_.section_header(EditorIcon::light, "Punctual Light");
        if (ui_kit_.begin_property_grid("PunctualLightProperties")) {
            ui_kit_.property_row("Linear color");
            changed |= ImGui::ColorEdit3("##LightColor", &node.light.color.x);
            ui_kit_.property_row("Intensity (cd)");
            changed |= ImGui::DragFloat(
                "##LightIntensity", &node.light.intensity_candela, 1.0F,
                0.0F, 1'000'000.0F, "%.3g", ImGuiSliderFlags_AlwaysClamp);
            ui_kit_.property_row("Range (m)");
            changed |= ImGui::DragFloat(
                "##LightRange", &node.light.range, 0.05F,
                0.05F, 1'000.0F, "%.3g", ImGuiSliderFlags_AlwaysClamp);
            ui_kit_.property_row("Night only");
            changed |= ImGui::Checkbox("##LightNightOnly", &node.light.night_only);
            if (node.kind == SceneNodeKind::spot_light) {
                ui_kit_.property_row("Inner cone");
                changed |= ImGui::DragFloat(
                    "##SpotInner", &node.light.inner_cone_degrees, 0.1F,
                    0.0F, 89.9F, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Outer cone");
                changed |= ImGui::DragFloat(
                    "##SpotOuter", &node.light.outer_cone_degrees, 0.1F,
                    0.1F, 90.0F, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
            }
            ui_kit_.end_property_grid();
        }
    }
    {
        ui_kit_.section_header(EditorIcon::cube, "Collision");
        if (const auto attached_collider = scene.collider(selected_node_)) {
            ColliderComponent edited_collider = attached_collider->get();
            bool collider_changed = false;
            bool remove_collider_requested = false;
            if (ui_kit_.begin_property_grid("ColliderProperties")) {
                ui_kit_.property_row("Enabled");
                collider_changed |= ImGui::Checkbox(
                    "##ColliderEnabled", &edited_collider.enabled);
                ui_kit_.property_row("Trigger");
                collider_changed |= ImGui::Checkbox(
                    "##ColliderTrigger", &edited_collider.is_trigger);
                ImGui::SameLine();
                ImGui::TextDisabled(
                    edited_collider.is_trigger ? "Overlap zone" : "Solid");
                ui_kit_.property_row("Shape");
                if (ImGui::BeginCombo(
                        "##ColliderShape",
                        to_token(edited_collider.shape).data())) {
                    for (const auto candidate_shape : {
                             ColliderShape::auto_bounds,
                             ColliderShape::box,
                             ColliderShape::sphere,
                             ColliderShape::capsule,
                             ColliderShape::convex_hull,
                             ColliderShape::triangle_mesh}) {
                        if (ImGui::Selectable(
                                to_token(candidate_shape).data(),
                                edited_collider.shape == candidate_shape)) {
                            collider_changed |=
                                edited_collider.shape != candidate_shape;
                            edited_collider.shape = candidate_shape;
                        }
                    }
                    ImGui::EndCombo();
                }
                collider_changed |= ui_kit_.drag_vec3(
                    "Offset",
                    "ColliderOffset",
                    &edited_collider.center_offset.x,
                    0.05F,
                    0.0F);
                const bool uses_box_extents =
                    edited_collider.shape == ColliderShape::box ||
                    (edited_collider.shape == ColliderShape::auto_bounds &&
                     node.kind != SceneNodeKind::cube &&
                     node.kind != SceneNodeKind::ground &&
                     node.kind != SceneNodeKind::model);
                if (uses_box_extents) {
                    collider_changed |= ui_kit_.drag_vec3(
                        "Half extents",
                        "ColliderExtents",
                        &edited_collider.half_extents.x,
                        0.05F,
                        1.0F,
                        minimum_collider_dimension,
                        maximum_collider_dimension);
                } else if (edited_collider.shape == ColliderShape::sphere) {
                    ui_kit_.property_row("Radius (m)");
                    collider_changed |= ImGui::DragFloat(
                        "##ColliderRadius",
                        &edited_collider.radius,
                        0.02F,
                        minimum_collider_dimension,
                        maximum_collider_dimension,
                        "%.3f",
                        ImGuiSliderFlags_AlwaysClamp);
                } else if (edited_collider.shape == ColliderShape::capsule) {
                    ui_kit_.property_row("Radius (m)");
                    collider_changed |= ImGui::DragFloat(
                        "##ColliderRadius",
                        &edited_collider.radius,
                        0.02F,
                        minimum_collider_dimension,
                        maximum_collider_dimension,
                        "%.3f",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Half height (m)");
                    collider_changed |= ImGui::DragFloat(
                        "##ColliderHalfHeight",
                        &edited_collider.half_height,
                        0.02F,
                        0.0F,
                        maximum_collider_dimension,
                        "%.3f",
                        ImGuiSliderFlags_AlwaysClamp);
                }
                ui_kit_.property_row("Layer");
                collider_changed |= ImGui::InputScalar(
                    "##ColliderLayer",
                    ImGuiDataType_U32,
                    &edited_collider.collision_layer);
                ui_kit_.property_row("Mask (hex)");
                collider_changed |= ImGui::InputScalar(
                    "##ColliderMask",
                    ImGuiDataType_U32,
                    &edited_collider.collision_mask,
                    nullptr,
                    nullptr,
                    "%08X",
                    ImGuiInputTextFlags_CharsHexadecimal);
                ui_kit_.property_row("Component");
                if (ImGui::SmallButton("Remove collider")) {
                    remove_collider_requested = true;
                }
                ui_kit_.end_property_grid();
            }
            if (remove_collider_requested) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.remove_collider(selected_node_));
            } else if (collider_changed) {
                edited_collider = sanitize_collider_component(edited_collider);
                if (!inspector_edit_snapshot_) {
                    inspector_edit_snapshot_ = scene;
                    if (auto previous =
                            inspector_edit_snapshot_->node(selected_node_)) {
                        previous->get() = node_before_edit;
                    }
                }
                static_cast<void>(
                    scene.set_collider(selected_node_, edited_collider));
            }
        } else {
            const bool implicit_solid =
                node.kind == SceneNodeKind::cube ||
                node.kind == SceneNodeKind::ground ||
                node.kind == SceneNodeKind::model ||
                node.kind == SceneNodeKind::terrain;
            ui_kit_.status_text(
                implicit_solid
                    ? "Implicit solid bounds active. Add Collider to customize or turn into a trigger."
                    : "No collider attached.",
                EditorTone::neutral);
            if (ui_kit_.action_button(
                    "AddColliderButton",
                    EditorIcon::add,
                    "Add Collider",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                ColliderComponent initial{};
                if (node.kind == SceneNodeKind::folder) {
                    initial.shape = ColliderShape::box;
                }
                static_cast<void>(scene.set_collider(selected_node_, initial));
            }
        }
    }
    {
        ui_kit_.section_header(EditorIcon::play, "Rigid Body");
        if (const auto attached_rb = scene.rigid_body(selected_node_)) {
            RigidBodyComponent edited_rb = attached_rb->get();
            bool rb_changed = false;
            bool remove_rb_requested = false;
            if (ui_kit_.begin_property_grid("RigidBodyProperties")) {
                ui_kit_.property_row("Enabled");
                rb_changed |= ImGui::Checkbox(
                    "##RigidBodyEnabled", &edited_rb.enabled);
                ui_kit_.property_row("Motion");
                if (ImGui::BeginCombo(
                        "##RigidBodyMotion",
                        to_token(edited_rb.motion_type).data())) {
                    for (const auto candidate_motion : {
                             RigidBodyMotionType::dynamic,
                             RigidBodyMotionType::kinematic}) {
                        if (ImGui::Selectable(
                                to_token(candidate_motion).data(),
                                edited_rb.motion_type == candidate_motion)) {
                            rb_changed |=
                                edited_rb.motion_type != candidate_motion;
                            edited_rb.motion_type = candidate_motion;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (edited_rb.motion_type == RigidBodyMotionType::dynamic) {
                    ui_kit_.property_row("Mass (kg)");
                    rb_changed |= ImGui::DragFloat(
                        "##RigidBodyMass",
                        &edited_rb.mass,
                        0.1F,
                        minimum_rigid_body_mass,
                        maximum_rigid_body_mass,
                        "%.3f",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Gravity scale");
                    rb_changed |= ImGui::DragFloat(
                        "##RigidBodyGravityScale",
                        &edited_rb.gravity_scale,
                        0.05F,
                        -maximum_rigid_body_gravity_scale,
                        maximum_rigid_body_gravity_scale,
                        "%.2f",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Linear damping");
                    rb_changed |= ImGui::DragFloat(
                        "##RigidBodyLinearDamping",
                        &edited_rb.linear_damping,
                        0.01F,
                        0.0F,
                        maximum_rigid_body_damping,
                        "%.3f",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Angular damping");
                    rb_changed |= ImGui::DragFloat(
                        "##RigidBodyAngularDamping",
                        &edited_rb.angular_damping,
                        0.01F,
                        0.0F,
                        maximum_rigid_body_damping,
                        "%.3f",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Lock rotation");
                    rb_changed |= ImGui::Checkbox(
                        "##RigidBodyLockRotation", &edited_rb.lock_rotation);
                }
                ui_kit_.property_row("Friction");
                rb_changed |= ImGui::SliderFloat(
                    "##RigidBodyFriction",
                    &edited_rb.friction,
                    0.0F,
                    2.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Restitution");
                rb_changed |= ImGui::SliderFloat(
                    "##RigidBodyRestitution",
                    &edited_rb.restitution,
                    0.0F,
                    1.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Component");
                if (ImGui::SmallButton("Remove rigid body")) {
                    remove_rb_requested = true;
                }
                ui_kit_.end_property_grid();
            }
            if (remove_rb_requested) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.remove_rigid_body(selected_node_));
            } else if (rb_changed) {
                edited_rb = sanitize_rigid_body_component(edited_rb);
                if (!inspector_edit_snapshot_) {
                    inspector_edit_snapshot_ = scene;
                    if (auto previous =
                            inspector_edit_snapshot_->node(selected_node_)) {
                        previous->get() = node_before_edit;
                    }
                }
                static_cast<void>(
                    scene.set_rigid_body(selected_node_, edited_rb));
            }
        } else {
            ui_kit_.status_text(
                "Static in world. Add Rigid Body for dynamic simulation or kinematic platforms.",
                EditorTone::neutral);
            if (ui_kit_.action_button(
                    "AddRigidBodyButton",
                    EditorIcon::add,
                    "Add Rigid Body",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.set_rigid_body(
                    selected_node_, RigidBodyComponent{}));
            }
        }
    }
    {
        ui_kit_.section_header(EditorIcon::play, "Audio Source");
        if (const auto attached_src = scene.audio_source(selected_node_)) {
            AudioSourceComponent edited_src = attached_src->get();
            bool src_changed = false;
            bool remove_src_requested = false;
            if (ui_kit_.begin_property_grid("AudioSourceProperties")) {
                ui_kit_.property_row("Enabled");
                src_changed |= ImGui::Checkbox(
                    "##AudioSrcEnabled", &edited_src.enabled);
                ui_kit_.property_row("Asset / Cue");
                std::array<char, max_audio_path_bytes + 1U> path_buf{};
                std::memcpy(
                    path_buf.data(),
                    edited_src.asset_path.data(),
                    edited_src.asset_path.size());
                if (ImGui::InputTextWithHint(
                        "##AudioSrcAsset",
                        "audio/clip.vcraudio.json or .vcrcue.json",
                        path_buf.data(),
                        path_buf.size())) {
                    const std::string_view candidate(path_buf.data());
                    if (edited_src.set_path(candidate)) {
                        src_changed = true;
                    }
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload("VCR_ASSET_PATH");
                        payload != nullptr &&
                        payload->Data != nullptr &&
                        payload->DataSize > 1) {
                        const auto* bytes =
                            static_cast<const char*>(payload->Data);
                        const std::size_t sz =
                            static_cast<std::size_t>(payload->DataSize);
                        if (bytes[sz - 1U] == '\0') {
                            const std::string_view dropped{bytes, sz - 1U};
                            if (edited_src.set_path(dropped)) {
                                src_changed = true;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (selected_asset_ &&
                    (selected_asset_->kind == AssetEntryKind::audio ||
                     selected_asset_->kind == AssetEntryKind::sound_cue) &&
                    valid_audio_asset_reference(
                        selected_asset_->relative_display)) {
                    ui_kit_.property_row("Assign");
                    if (ImGui::SmallButton("Use selected asset")) {
                        if (edited_src.set_path(
                                selected_asset_->relative_display)) {
                            src_changed = true;
                        }
                    }
                }
                if (audio_engine_ != nullptr &&
                    !edited_src.path_view().empty()) {
                    ui_kit_.property_row("Preview");
                    ImGui::EndDisabled();
                    if (ImGui::SmallButton("Play")) {
                        std::string preview_err;
                        if (!audio_engine_->play_preview(
                                edited_src.path_view(),
                                preview_err)) {
                            asset_error_ = std::move(preview_err);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Stop")) {
                        audio_engine_->stop_preview();
                    }
                    ImGui::BeginDisabled(!editable);
                }
                ui_kit_.property_row("Bus");
                if (ImGui::BeginCombo(
                        "##AudioSrcBus", to_token(edited_src.bus).data())) {
                    for (const auto candidate_bus : {
                             AudioBusId::master,
                             AudioBusId::music,
                             AudioBusId::sfx,
                             AudioBusId::voice,
                             AudioBusId::ui}) {
                        if (ImGui::Selectable(
                                to_token(candidate_bus).data(),
                                edited_src.bus == candidate_bus)) {
                            src_changed |= edited_src.bus != candidate_bus;
                            edited_src.bus = candidate_bus;
                        }
                    }
                    ImGui::EndCombo();
                }
                ui_kit_.property_row("Spatial 3D");
                src_changed |= ImGui::Checkbox(
                    "##AudioSrcSpatial", &edited_src.spatial);
                ui_kit_.property_row("Loop");
                src_changed |= ImGui::Checkbox(
                    "##AudioSrcLoop", &edited_src.loop);
                ui_kit_.property_row("Play on start");
                src_changed |= ImGui::Checkbox(
                    "##AudioSrcPlayOnStart", &edited_src.play_on_start);
                ui_kit_.property_row("Streaming");
                src_changed |= ImGui::Checkbox(
                    "##AudioSrcStreaming", &edited_src.streaming);
                ui_kit_.property_row("Volume");
                src_changed |= ImGui::SliderFloat(
                    "##AudioSrcVolume",
                    &edited_src.volume,
                    0.0F,
                    2.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Pitch");
                src_changed |= ImGui::SliderFloat(
                    "##AudioSrcPitch",
                    &edited_src.pitch,
                    0.25F,
                    4.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Volume variance");
                src_changed |= ImGui::SliderFloat(
                    "##AudioSrcVolVar",
                    &edited_src.volume_variance,
                    0.0F,
                    1.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Pitch variance");
                src_changed |= ImGui::SliderFloat(
                    "##AudioSrcPitchVar",
                    &edited_src.pitch_variance,
                    0.0F,
                    1.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                if (edited_src.spatial) {
                    ui_kit_.property_row("Attenuation");
                    if (ImGui::BeginCombo(
                            "##AudioSrcAtten",
                            to_token(edited_src.attenuation).data())) {
                        for (const auto candidate_model : {
                                 AudioAttenuationModel::inverse_clamped,
                                 AudioAttenuationModel::linear,
                                 AudioAttenuationModel::exponential}) {
                            if (ImGui::Selectable(
                                    to_token(candidate_model).data(),
                                    edited_src.attenuation ==
                                        candidate_model)) {
                                src_changed |=
                                    edited_src.attenuation != candidate_model;
                                edited_src.attenuation = candidate_model;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ui_kit_.property_row("Min distance (m)");
                    src_changed |= ImGui::DragFloat(
                        "##AudioSrcMinDist",
                        &edited_src.min_distance,
                        0.1F,
                        0.05F,
                        10'000.0F,
                        "%.2f",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Max distance (m)");
                    src_changed |= ImGui::DragFloat(
                        "##AudioSrcMaxDist",
                        &edited_src.max_distance,
                        0.25F,
                        edited_src.min_distance,
                        100'000.0F,
                        "%.2f",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Rolloff");
                    src_changed |= ImGui::DragFloat(
                        "##AudioSrcRolloff",
                        &edited_src.rolloff,
                        0.05F,
                        0.01F,
                        16.0F,
                        "%.2f",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Doppler factor");
                    src_changed |= ImGui::SliderFloat(
                        "##AudioSrcDoppler",
                        &edited_src.doppler_factor,
                        0.0F,
                        5.0F,
                        "%.2f",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Occlusion");
                    src_changed |= ImGui::Checkbox(
                        "##AudioSrcOcclusion", &edited_src.occlusion);
                    ui_kit_.property_row("Inner cone");
                    src_changed |= ImGui::SliderFloat(
                        "##AudioSrcConeIn",
                        &edited_src.inner_cone_degrees,
                        0.0F,
                        360.0F,
                        "%.0f deg",
                        ImGuiSliderFlags_AlwaysClamp);
                    ui_kit_.property_row("Outer cone");
                    src_changed |= ImGui::SliderFloat(
                        "##AudioSrcConeOut",
                        &edited_src.outer_cone_degrees,
                        edited_src.inner_cone_degrees,
                        360.0F,
                        "%.0f deg",
                        ImGuiSliderFlags_AlwaysClamp);
                    if (edited_src.outer_cone_degrees < 359.9F) {
                        ui_kit_.property_row("Outer gain");
                        src_changed |= ImGui::SliderFloat(
                            "##AudioSrcConeGain",
                            &edited_src.outer_cone_gain,
                            0.0F,
                            1.0F,
                            "%.2f",
                            ImGuiSliderFlags_AlwaysClamp);
                    }
                }
                ui_kit_.property_row("Component");
                if (ImGui::SmallButton("Remove audio source")) {
                    remove_src_requested = true;
                }
                ui_kit_.end_property_grid();
            }
            if (remove_src_requested) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.remove_audio_source(selected_node_));
            } else if (src_changed) {
                edited_src = sanitize_audio_source_component(edited_src);
                if (!inspector_edit_snapshot_) {
                    inspector_edit_snapshot_ = scene;
                    if (auto previous =
                            inspector_edit_snapshot_->node(selected_node_)) {
                        previous->get() = node_before_edit;
                    }
                }
                static_cast<void>(
                    scene.set_audio_source(selected_node_, edited_src));
            }
        } else {
            ui_kit_.status_text(
                "No audio emitter attached.",
                EditorTone::neutral);
            if (ui_kit_.action_button(
                    "AddAudioSourceButton",
                    EditorIcon::add,
                    "Add Audio Source",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                AudioSourceComponent initial{};
                if (selected_asset_ &&
                    (selected_asset_->kind == AssetEntryKind::audio ||
                     selected_asset_->kind == AssetEntryKind::sound_cue) &&
                    valid_audio_asset_reference(
                        selected_asset_->relative_display)) {
                    static_cast<void>(
                        initial.set_path(selected_asset_->relative_display));
                }
                static_cast<void>(
                    scene.set_audio_source(selected_node_, initial));
            }
        }
    }
    {
        ui_kit_.section_header(EditorIcon::camera, "Audio Listener");
        if (const auto attached_listener =
                scene.audio_listener(selected_node_)) {
            AudioListenerComponent edited_listener = attached_listener->get();
            bool listener_changed = false;
            bool remove_listener_requested = false;
            if (ui_kit_.begin_property_grid("AudioListenerProperties")) {
                ui_kit_.property_row("Enabled");
                listener_changed |= ImGui::Checkbox(
                    "##AudioListenerEnabled", &edited_listener.enabled);
                ui_kit_.property_row("Volume scale");
                listener_changed |= ImGui::SliderFloat(
                    "##AudioListenerVol",
                    &edited_listener.volume_scale,
                    0.0F,
                    2.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Doppler scale");
                listener_changed |= ImGui::SliderFloat(
                    "##AudioListenerDoppler",
                    &edited_listener.doppler_scale,
                    0.0F,
                    5.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Component");
                if (ImGui::SmallButton("Remove audio listener")) {
                    remove_listener_requested = true;
                }
                ui_kit_.end_property_grid();
            }
            if (remove_listener_requested) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(
                    scene.remove_audio_listener(selected_node_));
            } else if (listener_changed) {
                edited_listener =
                    sanitize_audio_listener_component(edited_listener);
                if (!inspector_edit_snapshot_) {
                    inspector_edit_snapshot_ = scene;
                    if (auto previous =
                            inspector_edit_snapshot_->node(selected_node_)) {
                        previous->get() = node_before_edit;
                    }
                }
                static_cast<void>(scene.set_audio_listener(
                    selected_node_, edited_listener));
            }
        } else {
            if (ui_kit_.action_button(
                    "AddAudioListenerButton",
                    EditorIcon::add,
                    "Add Audio Listener",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.set_audio_listener(
                    selected_node_, AudioListenerComponent{}));
            }
        }
    }
    {
        ui_kit_.section_header(EditorIcon::environment, "Reverb Zone");
        if (const auto attached_zone = scene.reverb_zone(selected_node_)) {
            AudioReverbZoneComponent edited_zone = attached_zone->get();
            bool zone_changed = false;
            bool remove_zone_requested = false;
            if (ui_kit_.begin_property_grid("ReverbZoneProperties")) {
                ui_kit_.property_row("Enabled");
                zone_changed |= ImGui::Checkbox(
                    "##ReverbZoneEnabled", &edited_zone.enabled);
                ui_kit_.property_row("Inner radius (m)");
                zone_changed |= ImGui::DragFloat(
                    "##ReverbInner",
                    &edited_zone.inner_radius,
                    0.2F,
                    0.1F,
                    10'000.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Outer radius (m)");
                zone_changed |= ImGui::DragFloat(
                    "##ReverbOuter",
                    &edited_zone.outer_radius,
                    0.2F,
                    edited_zone.inner_radius,
                    100'000.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Wet level");
                zone_changed |= ImGui::SliderFloat(
                    "##ReverbWet",
                    &edited_zone.wet_level,
                    0.0F,
                    1.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Decay (s)");
                zone_changed |= ImGui::SliderFloat(
                    "##ReverbDecay",
                    &edited_zone.decay_seconds,
                    0.1F,
                    20.0F,
                    "%.2f s",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("HF damping");
                zone_changed |= ImGui::SliderFloat(
                    "##ReverbDamping",
                    &edited_zone.damping,
                    0.0F,
                    1.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
                ui_kit_.property_row("Component");
                if (ImGui::SmallButton("Remove reverb zone")) {
                    remove_zone_requested = true;
                }
                ui_kit_.end_property_grid();
            }
            if (remove_zone_requested) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.remove_reverb_zone(selected_node_));
            } else if (zone_changed) {
                edited_zone =
                    sanitize_audio_reverb_zone_component(edited_zone);
                if (!inspector_edit_snapshot_) {
                    inspector_edit_snapshot_ = scene;
                    if (auto previous =
                            inspector_edit_snapshot_->node(selected_node_)) {
                        previous->get() = node_before_edit;
                    }
                }
                static_cast<void>(
                    scene.set_reverb_zone(selected_node_, edited_zone));
            }
        } else {
            if (ui_kit_.action_button(
                    "AddReverbZoneButton",
                    EditorIcon::add,
                    "Add Reverb Zone",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.set_reverb_zone(
                    selected_node_, AudioReverbZoneComponent{}));
            }
        }
    }
    if (node.kind == SceneNodeKind::model ||
        scene.has_animator(selected_node_)) {
        ui_kit_.section_header(EditorIcon::play, "Animator");
        const CookedSkeletonAsset* const skeleton = !node.asset_path.empty()
            ? renderer.find_model_skeleton(node.asset_path)
            : nullptr;
        const std::vector<AnimationClip>* const clips = !node.asset_path.empty()
            ? renderer.find_model_animations(node.asset_path)
            : nullptr;
        if (skeleton != nullptr) {
            char skel_badge[64]{};
            std::snprintf(
                skel_badge,
                sizeof(skel_badge),
                "%zu BONES  %zu CLIPS",
                skeleton->bones.size(),
                clips != nullptr ? clips->size() : 0U);
            ui_kit_.badge(skel_badge, EditorTone::info);
        } else {
            ui_kit_.status_text(
                "No skeleton loaded on model; CPU/MCP values will be stored.",
                EditorTone::neutral);
        }

        if (const auto attached_anim = scene.animator(selected_node_)) {
            AnimatorComponent edited_anim = attached_anim->get();
            bool anim_changed = false;
            bool remove_anim_requested = false;
            if (ui_kit_.begin_property_grid("AnimatorProperties")) {
                ui_kit_.property_row("Blend mode");
                constexpr std::array<AnimatorBlendMode, 4U> kModes{
                    AnimatorBlendMode::single_clip,
                    AnimatorBlendMode::blend_space_1d,
                    AnimatorBlendMode::blend_space_2d,
                    AnimatorBlendMode::state_machine,
                };
                const auto mode_label =
                    [](const AnimatorBlendMode m) -> const char* {
                    switch (m) {
                    case AnimatorBlendMode::single_clip: return "Single Clip";
                    case AnimatorBlendMode::blend_space_1d:
                        return "Blend Space 1D";
                    case AnimatorBlendMode::blend_space_2d:
                        return "Blend Space 2D";
                    case AnimatorBlendMode::state_machine:
                        return "State Machine";
                    }
                    return "Single Clip";
                };
                if (ImGui::BeginCombo(
                        "##AnimatorBlendMode", mode_label(edited_anim.mode))) {
                    for (const AnimatorBlendMode m : kModes) {
                        const bool is_sel = edited_anim.mode == m;
                        if (ImGui::Selectable(mode_label(m), is_sel)) {
                            edited_anim.mode = m;
                            anim_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }

                ui_kit_.property_row("Primary clip");
                anim_changed |= edit_clip_name_field(
                    "##AnimatorClipCombo",
                    "##AnimatorClipInput",
                    edited_anim.clip_name,
                    clips);

                ui_kit_.property_row("Secondary clip");
                anim_changed |= edit_clip_name_field(
                    "##AnimatorSecondaryCombo",
                    "##AnimatorSecondaryInput",
                    edited_anim.secondary_clip_name,
                    clips);

                ui_kit_.property_row("Upper-body bone");
                anim_changed |= edit_bone_name_field(
                    "##AnimatorUpperBoneCombo",
                    "##AnimatorUpperBoneInput",
                    edited_anim.upper_body_root_bone,
                    skeleton);

                ui_kit_.property_row("Upper weight");
                anim_changed |= ImGui::SliderFloat(
                    "##AnimatorUpperWeight",
                    &edited_anim.upper_body_weight,
                    0.0F,
                    1.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);

                ui_kit_.property_row("Upper additive");
                anim_changed |= ImGui::Checkbox(
                    "##AnimatorUpperAdditive",
                    &edited_anim.upper_body_additive);

                ui_kit_.property_row("Playing / Loop");
                anim_changed |= ImGui::Checkbox(
                    "Playing##AnimatorPlaying", &edited_anim.playing);
                ImGui::SameLine();
                anim_changed |= ImGui::Checkbox(
                    "Loop##AnimatorLooping", &edited_anim.looping);

                ui_kit_.property_row("Playback speed");
                anim_changed |= ImGui::DragFloat(
                    "##AnimatorSpeed",
                    &edited_anim.playback_speed,
                    0.05F,
                    -10.0F,
                    10.0F,
                    "%.2fx");

                ui_kit_.property_row("Blend X / Y");
                anim_changed |= ImGui::DragFloat2(
                    "##AnimatorBlendParams",
                    &edited_anim.param_x,
                    0.05F,
                    -100.0F,
                    100.0F,
                    "%.2f");

                ui_kit_.property_row("Root motion");
                anim_changed |= ImGui::Checkbox(
                    "##AnimatorRootMotion",
                    &edited_anim.root_motion_enabled);

                ui_kit_.property_row("Foot IK");
                anim_changed |= ImGui::Checkbox(
                    "Enable##AnimatorFootIk", &edited_anim.foot_ik_enabled);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-FLT_MIN);
                anim_changed |= ImGui::SliderFloat(
                    "##AnimatorFootIkWeight",
                    &edited_anim.foot_ik_weight,
                    0.0F,
                    1.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);

                ui_kit_.property_row("Look-At IK");
                anim_changed |= ImGui::Checkbox(
                    "Enable##AnimatorLookAtIk", &edited_anim.look_at_ik_enabled);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-FLT_MIN);
                anim_changed |= ImGui::SliderFloat(
                    "##AnimatorLookAtWeight",
                    &edited_anim.look_at_ik_weight,
                    0.0F,
                    1.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);

                anim_changed |= ui_kit_.drag_vec3(
                    "##AnimatorLookTarget",
                    "Look target",
                    &edited_anim.look_at_target_world.x,
                    0.05F,
                    0.0F);

                ui_kit_.property_row("Component");
                if (ImGui::SmallButton("Remove animator")) {
                    remove_anim_requested = true;
                }
                ui_kit_.end_property_grid();
            }

            if (remove_anim_requested) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.remove_animator(selected_node_));
                if (edit_animator_preview_ != nullptr) {
                    edit_animator_preview_->clear_preview(selected_node_);
                }
                renderer.remove_skinned_model_pose(selected_node_);
            } else if (anim_changed) {
                edited_anim = sanitize_animator_component(edited_anim);
                if (!inspector_edit_snapshot_) {
                    inspector_edit_snapshot_ = scene;
                    if (auto previous =
                            inspector_edit_snapshot_->node(selected_node_)) {
                        previous->get() = node_before_edit;
                    }
                }
                static_cast<void>(
                    scene.set_animator(selected_node_, edited_anim));
            }

            if (editable &&
                edit_animator_preview_ != nullptr &&
                clips != nullptr &&
                !clips->empty()) {
                const AnimatorNodeRuntime* const preview_rt =
                    edit_animator_preview_->node_runtime(selected_node_);
                float clip_duration = 1.0F;
                for (const AnimationClip& clip : *clips) {
                    if (clip.name == edited_anim.clip_name_view() ||
                        (edited_anim.clip_name_view().empty() &&
                         &clip == &clips->front())) {
                        clip_duration = std::max(clip.duration_seconds, 0.05F);
                        break;
                    }
                }
                float scrub_t = preview_rt != nullptr
                    ? std::clamp(preview_rt->current_time, 0.0F, clip_duration)
                    : std::clamp(
                          edited_anim.start_time_seconds, 0.0F, clip_duration);
                const std::string_view active_clip =
                    !edited_anim.clip_name_view().empty()
                        ? edited_anim.clip_name_view()
                        : std::string_view(clips->front().name);

                if (ui_kit_.action_button(
                        "PreviewAnimPlayButton",
                        EditorIcon::play,
                        "Preview Play",
                        EditorButtonStyle::secondary,
                        editable)) {
                    edit_animator_preview_->set_preview(
                        selected_node_, active_clip, true, scrub_t);
                }
                ImGui::SameLine();
                if (ui_kit_.action_button(
                        "PreviewAnimPauseButton",
                        EditorIcon::pause,
                        "Pause",
                        EditorButtonStyle::secondary,
                        editable)) {
                    edit_animator_preview_->set_preview(
                        selected_node_, active_clip, false, scrub_t);
                }
                ImGui::SameLine();
                if (ui_kit_.action_button(
                        "PreviewAnimStopButton",
                        EditorIcon::stop,
                        "Stop",
                        EditorButtonStyle::secondary,
                        editable)) {
                    edit_animator_preview_->clear_preview(selected_node_);
                    renderer.remove_skinned_model_pose(selected_node_);
                }
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat(
                        "##PreviewScrub",
                        &scrub_t,
                        0.0F,
                        clip_duration,
                        "Time %.2fs",
                        ImGuiSliderFlags_AlwaysClamp)) {
                    edit_animator_preview_->set_preview(
                        selected_node_, active_clip, false, scrub_t);
                }
            }
        } else {
            if (ui_kit_.action_button(
                    "AddAnimatorButton",
                    EditorIcon::add,
                    "Add Animator",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                AnimatorComponent initial{};
                if (clips != nullptr && !clips->empty()) {
                    static_cast<void>(
                        initial.set_clip_name(clips->front().name));
                }
                static_cast<void>(scene.set_animator(selected_node_, initial));
            }
        }
    }
    {
        ui_kit_.section_header(EditorIcon::cube, "Gameplay");
        if (const auto attached_gp = scene.gameplay(selected_node_)) {
            GameplayComponent edited_gp = attached_gp->get();
            bool gp_changed = false;
            bool remove_gp_requested = false;
            if (ui_kit_.begin_property_grid("GameplayProperties")) {
                ui_kit_.property_row("Enabled");
                gp_changed |= ImGui::Checkbox(
                    "Active##GameplayEnabled", &edited_gp.enabled);
                ImGui::SameLine();
                gp_changed |= ImGui::Checkbox(
                    "Save state##GameplayPersist",
                    &edited_gp.persist_save_state);

                ui_kit_.property_row("Behavior");
                constexpr std::array<const char*, 10U> kBehaviors{
                    "patrol",
                    "collectible",
                    "health_pack",
                    "door",
                    "interactable",
                    "dialogue",
                    "trigger_zone",
                    "destructible",
                    "cutscene_trigger",
                    "spline_mover",
                };
                if (ImGui::BeginCombo(
                        "##GameplayBehaviorCombo",
                        edited_gp.behavior[0] != '\0'
                            ? edited_gp.behavior.data()
                            : "interactable")) {
                    for (const char* const b : kBehaviors) {
                        const bool is_sel =
                            std::string_view(edited_gp.behavior.data()) == b;
                        if (ImGui::Selectable(b, is_sel)) {
                            static_cast<void>(edited_gp.set_behavior(b));
                            gp_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                gp_changed |= ImGui::InputText(
                    "##GameplayBehaviorInput",
                    edited_gp.behavior.data(),
                    edited_gp.behavior.size());

                ui_kit_.property_row("Tag");
                gp_changed |= ImGui::InputText(
                    "##GameplayTagInput",
                    edited_gp.tag.data(),
                    edited_gp.tag.size());

                ui_kit_.property_row("Sound cue");
                gp_changed |= ImGui::InputText(
                    "##GameplaySoundCue",
                    edited_gp.on_trigger_sound_cue.data(),
                    edited_gp.on_trigger_sound_cue.size());

                ui_kit_.property_row("Health / Max");
                gp_changed |= ImGui::DragFloat2(
                    "##GameplayHealthPair",
                    &edited_gp.health,
                    1.0F,
                    0.0F,
                    1'000'000.0F,
                    "%.1f");

                ui_kit_.property_row("Move speed");
                gp_changed |= ImGui::DragFloat(
                    "##GameplayMoveSpeed",
                    &edited_gp.move_speed,
                    0.1F,
                    0.0F,
                    1'000.0F,
                    "%.2f m/s");

                ui_kit_.property_row("Interact radius");
                gp_changed |= ImGui::DragFloat(
                    "##GameplayInteractRadius",
                    &edited_gp.interaction_radius,
                    0.05F,
                    0.05F,
                    1'000.0F,
                    "%.2f m");

                gp_changed |= ui_kit_.drag_vec3(
                    "##GameplayPatrolA",
                    "Patrol A",
                    &edited_gp.patrol_point_a.x,
                    0.05F,
                    0.0F);
                gp_changed |= ui_kit_.drag_vec3(
                    "##GameplayPatrolB",
                    "Patrol B",
                    &edited_gp.patrol_point_b.x,
                    0.05F,
                    0.0F);

                ui_kit_.property_row("Target UUID");
                std::array<char, 40U> target_uuid_buf{};
                if (edited_gp.target_node_uuid) {
                    const std::string formatted =
                        format_scene_uuid(edited_gp.target_node_uuid);
                    const std::size_t n = std::min(
                        formatted.size(), target_uuid_buf.size() - 1U);
                    std::memcpy(target_uuid_buf.data(), formatted.data(), n);
                }
                if (ImGui::InputText(
                        "##GameplayTargetUuid",
                        target_uuid_buf.data(),
                        target_uuid_buf.size(),
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
                    const std::string_view typed(target_uuid_buf.data());
                    if (typed.empty()) {
                        edited_gp.target_node_uuid = {};
                        gp_changed = true;
                    } else if (const auto parsed = parse_scene_uuid(typed)) {
                        edited_gp.target_node_uuid = *parsed;
                        gp_changed = true;
                    }
                }
                if (edited_gp.target_node_uuid) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear##ClearTargetUuid")) {
                        edited_gp.target_node_uuid = {};
                        gp_changed = true;
                    }
                }

                ui_kit_.property_row("Params 0 / 1");
                gp_changed |= ImGui::DragFloat2(
                    "##GameplayCustomParams",
                    &edited_gp.custom_param_0,
                    0.1F,
                    -100'000.0F,
                    100'000.0F,
                    "%.2f");

                ui_kit_.property_row("Component");
                if (ImGui::SmallButton("Remove gameplay")) {
                    remove_gp_requested = true;
                }
                ui_kit_.end_property_grid();
            }

            if (remove_gp_requested) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.remove_gameplay(selected_node_));
            } else if (gp_changed) {
                edited_gp = sanitize_gameplay_component(edited_gp);
                if (!valid_gameplay_component(edited_gp)) {
                    edited_gp.on_trigger_sound_cue.fill('\0');
                }
                if (!inspector_edit_snapshot_) {
                    inspector_edit_snapshot_ = scene;
                    if (auto previous =
                            inspector_edit_snapshot_->node(selected_node_)) {
                        previous->get() = node_before_edit;
                    }
                }
                static_cast<void>(
                    scene.set_gameplay(selected_node_, edited_gp));
            }
        } else {
            if (ui_kit_.action_button(
                    "AddGameplayButton",
                    EditorIcon::add,
                    "Add Gameplay",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                GameplayComponent initial{};
                static_cast<void>(initial.set_behavior("interactable"));
                initial.patrol_point_a = node.transform.translation;
                initial.patrol_point_b =
                    node.transform.translation + glm::vec3(0.0F, 0.0F, 4.0F);
                static_cast<void>(scene.set_gameplay(selected_node_, initial));
            }
        }
    }
    {
        ui_kit_.section_header(EditorIcon::eye, "UI Marker");
        if (const auto existing_um = scene.ui_marker(selected_node_)) {
            UiMarkerComponent edited_um = existing_um->get();
            bool um_changed = false;
            bool remove_um_requested = false;
            if (ui_kit_.begin_property_grid("UiMarkerGrid")) {
                ui_kit_.property_row("Enabled");
                um_changed |= ImGui::Checkbox("##UiMarkerEnabled", &edited_um.enabled);

                ui_kit_.property_row("Kind");
                constexpr std::array<UiMarkerKind, 6U> kMarkerKinds{
                    UiMarkerKind::objective,
                    UiMarkerKind::waypoint,
                    UiMarkerKind::interact,
                    UiMarkerKind::speaker,
                    UiMarkerKind::hazard,
                    UiMarkerKind::info,
                };
                if (ImGui::BeginCombo("##UiMarkerKind", to_string(edited_um.kind).data())) {
                    for (const UiMarkerKind kind : kMarkerKinds) {
                        const bool is_sel = edited_um.kind == kind;
                        if (ImGui::Selectable(to_string(kind).data(), is_sel)) {
                            edited_um.kind = kind;
                            um_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }

                ui_kit_.property_row("Label");
                um_changed |= ImGui::InputText(
                    "##UiMarkerLabel",
                    edited_um.label.data(),
                    edited_um.label.size());

                ui_kit_.property_row("Subtitle text");
                um_changed |= ImGui::InputText(
                    "##UiMarkerSubtitle",
                    edited_um.subtitle_text.data(),
                    edited_um.subtitle_text.size());

                ui_kit_.property_row("Dialogue ID");
                um_changed |= ImGui::InputText(
                    "##UiMarkerDialogueId",
                    edited_um.dialogue_id.data(),
                    edited_um.dialogue_id.size());

                um_changed |= ui_kit_.drag_vec3(
                    "##UiMarkerOffset",
                    "World offset",
                    &edited_um.world_offset.x,
                    0.05F,
                    0.0F);

                ui_kit_.property_row("Max / Fade m");
                float distances[2]{edited_um.max_distance, edited_um.fade_start_distance};
                if (ImGui::DragFloat2("##UiMarkerDist", distances, 0.5F, 0.0F, 5000.0F, "%.1f m")) {
                    edited_um.max_distance = distances[0];
                    edited_um.fade_start_distance = distances[1];
                    um_changed = true;
                }

                ui_kit_.property_row("Clamp to edge");
                um_changed |= ImGui::Checkbox("##UiMarkerClamp", &edited_um.clamp_to_screen);

                ui_kit_.property_row("Show distance");
                um_changed |= ImGui::Checkbox("##UiMarkerShowDist", &edited_um.show_distance);

                ui_kit_.property_row("Show health");
                um_changed |= ImGui::Checkbox("##UiMarkerShowHp", &edited_um.show_health_bar);

                ui_kit_.property_row("Color");
                float rgba[4]{
                    static_cast<float>(edited_um.color_rgba8 & 0xFFU) / 255.0F,
                    static_cast<float>((edited_um.color_rgba8 >> 8U) & 0xFFU) / 255.0F,
                    static_cast<float>((edited_um.color_rgba8 >> 16U) & 0xFFU) / 255.0F,
                    static_cast<float>((edited_um.color_rgba8 >> 24U) & 0xFFU) / 255.0F,
                };
                if (ImGui::ColorEdit4("##UiMarkerColor", rgba)) {
                    const auto to_u8 = [](const float v) noexcept -> std::uint8_t {
                        return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0F), 0L, 255L));
                    };
                    edited_um.color_rgba8 = pack_rgba8(
                        to_u8(rgba[0]), to_u8(rgba[1]), to_u8(rgba[2]), to_u8(rgba[3]));
                    um_changed = true;
                }

                ui_kit_.property_row("Component");
                if (ImGui::SmallButton("Remove UI marker")) {
                    remove_um_requested = true;
                }
                ui_kit_.end_property_grid();
            }

            if (remove_um_requested) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.remove_ui_marker(selected_node_));
            } else if (um_changed) {
                edited_um = sanitize_ui_marker_component(edited_um);
                if (!inspector_edit_snapshot_) {
                    inspector_edit_snapshot_ = scene;
                    if (auto previous =
                            inspector_edit_snapshot_->node(selected_node_)) {
                        previous->get() = node_before_edit;
                    }
                }
                static_cast<void>(scene.set_ui_marker(selected_node_, edited_um));
            }
        } else {
            if (ui_kit_.action_button(
                    "AddUiMarkerButton",
                    EditorIcon::add,
                    "Add UI Marker",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                UiMarkerComponent initial{};
                static_cast<void>(initial.set_label(node.name));
                static_cast<void>(scene.set_ui_marker(selected_node_, initial));
            }
        }
    }
    {
        ui_kit_.section_header(EditorIcon::camera, "3D Spline");
        if (const auto attached_spline = scene.spline(selected_node_)) {
            SplineComponent edited_spline = attached_spline->get();
            bool spline_changed = false;
            bool remove_spline_requested = false;
            if (ui_kit_.begin_property_grid("SplineProperties")) {
                ui_kit_.property_row("Basis");
                if (ImGui::BeginCombo(
                        "##SplineBasisCombo",
                        to_token(edited_spline.basis).data())) {
                    for (const SplineBasis b : {
                             SplineBasis::linear,
                             SplineBasis::catmull_rom,
                             SplineBasis::cubic_hermite}) {
                        const bool is_sel = edited_spline.basis == b;
                        if (ImGui::Selectable(to_token(b).data(), is_sel)) {
                            edited_spline.basis = b;
                            spline_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }

                ui_kit_.property_row("Closed loop");
                spline_changed |= ImGui::Checkbox(
                    "##SplineClosedLoop", &edited_spline.closed_loop);

                ui_kit_.property_row("Tension");
                spline_changed |= ImGui::SliderFloat(
                    "##SplineTension",
                    &edited_spline.tension,
                    0.0F,
                    1.0F,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);

                const glm::mat4 spline_world =
                    scene_world_matrix(scene, selected_node_)
                        .value_or(glm::mat4{1.0F});
                const SplineArcLengthTable arc =
                    build_spline_arc_table(edited_spline, spline_world);
                ui_kit_.property_row("Length / Points");
                ImGui::Text(
                    "%.2f m  (%u / %zu)",
                    arc.total_length,
                    edited_spline.point_count,
                    max_spline_control_points);

                ui_kit_.property_row("Component");
                if (ImGui::SmallButton("Remove 3D Spline")) {
                    remove_spline_requested = true;
                }
                ui_kit_.end_property_grid();
            }

            if (ImGui::SmallButton("Add Point##SplineAddPt") &&
                edited_spline.point_count < max_spline_control_points) {
                const std::uint32_t count = edited_spline.point_count;
                SplineControlPoint new_pt{};
                if (count >= 2U) {
                    const glm::vec3 d =
                        edited_spline.points[count - 1U].position -
                        edited_spline.points[count - 2U].position;
                    new_pt.position =
                        edited_spline.points[count - 1U].position +
                        (glm::dot(d, d) > 1.0e-4F
                             ? d
                             : glm::vec3{2.0F, 0.0F, 0.0F});
                } else {
                    new_pt.position =
                        glm::vec3{static_cast<float>(count) * 4.0F, 0.0F, 0.0F};
                }
                edited_spline.points[count] = new_pt;
                edited_spline.point_count = count + 1U;
                spline_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove Last##SplineRemoveLast") &&
                edited_spline.point_count > 2U) {
                edited_spline.point_count -= 1U;
                spline_changed = true;
            }

            for (std::uint32_t i = 0U; i < edited_spline.point_count; ++i) {
                ImGui::PushID(static_cast<int>(i));
                std::array<char, 32U> hdr{};
                std::snprintf(hdr.data(), hdr.size(), "Point %u", i);
                if (ImGui::TreeNodeEx(
                        hdr.data(),
                        i < 2U ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                    spline_changed |= ui_kit_.drag_vec3(
                        "##SplinePtPos",
                        "Position",
                        &edited_spline.points[i].position.x,
                        0.05F,
                        0.0F);
                    if (edited_spline.basis == SplineBasis::cubic_hermite) {
                        spline_changed |= ui_kit_.drag_vec3(
                            "##SplinePtTin",
                            "Tangent In",
                            &edited_spline.points[i].tangent_in.x,
                            0.05F,
                            0.0F);
                        spline_changed |= ui_kit_.drag_vec3(
                            "##SplinePtTout",
                            "Tangent Out",
                            &edited_spline.points[i].tangent_out.x,
                            0.05F,
                            0.0F);
                    }
                    ImGui::SetNextItemWidth(110.0F);
                    spline_changed |= ImGui::DragFloat(
                        "Roll (deg)##SplinePtRoll",
                        &edited_spline.points[i].roll_degrees,
                        0.5F,
                        -180.0F,
                        180.0F,
                        "%.1f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0F);
                    spline_changed |= ImGui::DragFloat(
                        "Speed##SplinePtSpeed",
                        &edited_spline.points[i].speed_scale,
                        0.02F,
                        0.01F,
                        20.0F,
                        "%.2f");
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            if (remove_spline_requested) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(scene.remove_spline(selected_node_));
            } else if (spline_changed) {
                edited_spline = sanitize_spline_component(edited_spline);
                if (!inspector_edit_snapshot_) {
                    inspector_edit_snapshot_ = scene;
                    if (auto previous =
                            inspector_edit_snapshot_->node(selected_node_)) {
                        previous->get() = node_before_edit;
                    }
                }
                static_cast<void>(
                    scene.set_spline(selected_node_, edited_spline));
            }
        } else {
            if (ui_kit_.action_button(
                    "AddSplineButton",
                    EditorIcon::add,
                    "Add 3D Spline",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                SplineComponent initial{};
                initial.basis = SplineBasis::catmull_rom;
                initial.point_count = 3U;
                initial.points[0].position = glm::vec3{0.0F, 0.0F, 0.0F};
                initial.points[1].position = glm::vec3{4.0F, 0.5F, -3.0F};
                initial.points[2].position = glm::vec3{8.0F, 0.0F, -6.0F};
                initial = sanitize_spline_component(initial);
                static_cast<void>(scene.set_spline(selected_node_, initial));
            }
        }
    }
    {
        ui_kit_.section_header(EditorIcon::model, "Prefab");
        const SceneNodeId enclosing_root =
            scene.enclosing_prefab_root(selected_node_);
        if (enclosing_root) {
            if (const auto inst = scene.prefab_instance(enclosing_root)) {
                ImGui::TextWrapped(
                    "Source: %s", inst->get().prefab_path.data());
            }
            if (ui_kit_.action_button(
                    "UnpackPrefabButton",
                    EditorIcon::revert,
                    "Unpack Prefab",
                    EditorButtonStyle::secondary,
                    editable)) {
                if (inspector_edit_snapshot_) {
                    push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                    inspector_edit_snapshot_.reset();
                }
                checkpoint_scene(scene);
                static_cast<void>(
                    scene.unpack_prefab_instance(enclosing_root));
                scene_file_status_ = "Unpacked prefab instance.";
            }
            ImGui::SameLine();
        }
        if (ui_kit_.action_button(
                "SaveNodeAsPrefabButton",
                EditorIcon::save,
                "Save as Prefab",
                EditorButtonStyle::secondary,
                editable && asset_import_enabled_)) {
            if (inspector_edit_snapshot_) {
                push_undo_snapshot(std::move(*inspector_edit_snapshot_));
                inspector_edit_snapshot_.reset();
            }
            std::string saved_path;
            std::string prefab_error;
            if (!save_node_as_prefab(
                    scene,
                    selected_node_,
                    {},
                    true,
                    true,
                    saved_path,
                    prefab_error)) {
                asset_error_ = std::move(prefab_error);
            }
        }
    }
    if (changed && !inspector_edit_snapshot_) {
        inspector_edit_snapshot_ = scene;
        if (auto previous =
                inspector_edit_snapshot_->node(selected_node_)) {
            previous->get() = node_before_edit;
        }
    }
    if (changed) {
        constexpr float minimum_scale = 0.0001F;
        node.transform.scale.x =
            std::max(node.transform.scale.x, minimum_scale);
        node.transform.scale.y =
            std::max(node.transform.scale.y, minimum_scale);
        node.transform.scale.z =
            std::max(node.transform.scale.z, minimum_scale);
        node.material =
            sanitize_pbr_material(node.material);
        node.light.color = glm::clamp(
            node.light.color, glm::vec3(0.0F), glm::vec3(1.0F));
        node.light.intensity_candela = std::clamp(
            node.light.intensity_candela, 0.0F, 1'000'000.0F);
        node.light.range = std::clamp(node.light.range, 0.05F, 1'000.0F);
        if (node.kind == SceneNodeKind::spot_light) {
            node.light.inner_cone_degrees = std::clamp(
                node.light.inner_cone_degrees, 0.0F, 89.9F);
            node.light.outer_cone_degrees = std::clamp(
                node.light.outer_cone_degrees,
                node.light.inner_cone_degrees + 0.1F,
                90.0F);
        }
        scene.mark_content_changed();
    }
    if (inspector_edit_snapshot_ && !ImGui::IsAnyItemActive()) {
        push_undo_snapshot(std::move(*inspector_edit_snapshot_));
        inspector_edit_snapshot_.reset();
    }
    if (!has_transform) {
        ui_kit_.status_text(
            node.kind == SceneNodeKind::camera
                ? "Scene View currently uses the editor fly camera."
                : "This component currently exposes visibility only.",
            EditorTone::neutral);
    }
    ImGui::EndDisabled();
    if (!editable) {
        ui_kit_.badge(
            "AUTHORING LOCKED DURING PLAY",
            EditorTone::warning);
    }
    ImGui::End();
}

namespace {

void draw_collider_wireframe(
    ImDrawList* const draw_list,
    const SceneDocument& scene,
    const Renderer& renderer,
    const SceneNodeId node_id,
    const glm::mat4& view_projection,
    const ImVec2 image_minimum,
    const float image_width,
    const float image_height) noexcept {
    if (draw_list == nullptr || !scene.valid(node_id)) {
        return;
    }
    const auto attached = scene.collider(node_id);
    if (!attached || !attached->get().enabled) {
        return;
    }
    const auto node_opt = scene.node(node_id);
    const auto world_opt = scene_world_matrix(scene, node_id);
    if (!node_opt || !world_opt) {
        return;
    }
    const ColliderComponent& col = attached->get();
    const SceneNodeKind kind = node_opt->get().kind;
    const glm::mat4& world = *world_opt;

    const ImU32 wire_color = col.is_trigger
        ? IM_COL32(255, 204, 64, 230)
        : IM_COL32(72, 232, 128, 230);

    const auto project_point = [&](const glm::vec3 world_pt,
                                   ImVec2& out) noexcept -> bool {
        const glm::vec4 clip = view_projection * glm::vec4(world_pt, 1.0F);
        if (!std::isfinite(clip.w) || clip.w <= 0.01F) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = ImVec2{
            image_minimum.x + (ndc.x * 0.5F + 0.5F) * image_width,
            image_minimum.y + (0.5F - ndc.y * 0.5F) * image_height,
        };
        return true;
    };

    const auto draw_segment = [&](const glm::vec3 a,
                                  const glm::vec3 b) noexcept {
        ImVec2 sa{};
        ImVec2 sb{};
        if (project_point(a, sa) && project_point(b, sb)) {
            draw_list->AddLine(sa, sb, wire_color, 1.75F);
        }
    };

    const glm::vec3 c0(world[0]);
    const glm::vec3 c1(world[1]);
    const glm::vec3 c2(world[2]);
    const glm::vec3 sx_sy_sz{
        glm::length(c0),
        glm::length(c1),
        glm::length(c2),
    };
    if (sx_sy_sz.x <= 1.0e-4F ||
        sx_sy_sz.y <= 1.0e-4F ||
        sx_sy_sz.z <= 1.0e-4F) {
        return;
    }
    const glm::vec3 ax0 = c0 / sx_sy_sz.x;
    const glm::vec3 ax1 = c1 / sx_sy_sz.y;
    const glm::vec3 ax2 = c2 / sx_sy_sz.z;
    const glm::vec3 center =
        glm::vec3(world[3]) +
        c0 * col.center_offset.x +
        c1 * col.center_offset.y +
        c2 * col.center_offset.z;

    const auto draw_obb = [&](const glm::vec3 c,
                              const glm::vec3 u0,
                              const glm::vec3 u1,
                              const glm::vec3 u2,
                              const glm::vec3 ext) noexcept {
        std::array<glm::vec3, 8> corners{};
        for (int idx = 0; idx < 8; ++idx) {
            const float sx = (idx & 1) != 0 ? 1.0F : -1.0F;
            const float sy = (idx & 2) != 0 ? 1.0F : -1.0F;
            const float sz = (idx & 4) != 0 ? 1.0F : -1.0F;
            corners[static_cast<std::size_t>(idx)] =
                c + u0 * (sx * ext.x) + u1 * (sy * ext.y) + u2 * (sz * ext.z);
        }
        constexpr std::array<std::pair<std::size_t, std::size_t>, 12> edges{{
            {0U, 1U}, {2U, 3U}, {4U, 5U}, {6U, 7U},
            {0U, 2U}, {1U, 3U}, {4U, 6U}, {5U, 7U},
            {0U, 4U}, {1U, 5U}, {2U, 6U}, {3U, 7U},
        }};
        for (const auto [i0, i1] : edges) {
            draw_segment(corners[i0], corners[i1]);
        }
    };

    constexpr int ring_segments = 24;
    constexpr float two_pi = 6.28318530718F;
    const auto draw_circle = [&](const glm::vec3 ring_center,
                                 const glm::vec3 u,
                                 const glm::vec3 v,
                                 const float r) noexcept {
        for (int s = 0; s < ring_segments; ++s) {
            const float a0 =
                two_pi * static_cast<float>(s) / static_cast<float>(ring_segments);
            const float a1 =
                two_pi * static_cast<float>(s + 1) / static_cast<float>(ring_segments);
            draw_segment(
                ring_center + (u * std::cos(a0) + v * std::sin(a0)) * r,
                ring_center + (u * std::cos(a1) + v * std::sin(a1)) * r);
        }
    };

    if (col.shape == ColliderShape::sphere) {
        const float r = col.radius *
            std::max({sx_sy_sz.x, sx_sy_sz.y, sx_sy_sz.z});
        draw_circle(center, ax0, ax1, r);
        draw_circle(center, ax0, ax2, r);
        draw_circle(center, ax1, ax2, r);
        return;
    }

    if (col.shape == ColliderShape::capsule) {
        const float r = col.radius * std::max(sx_sy_sz.x, sx_sy_sz.z);
        const float hh = col.half_height * sx_sy_sz.y;
        const glm::vec3 top = center + ax1 * hh;
        const glm::vec3 bot = center - ax1 * hh;
        draw_circle(top, ax0, ax2, r);
        draw_circle(bot, ax0, ax2, r);
        draw_segment(top + ax0 * r, bot + ax0 * r);
        draw_segment(top - ax0 * r, bot - ax0 * r);
        draw_segment(top + ax2 * r, bot + ax2 * r);
        draw_segment(top - ax2 * r, bot - ax2 * r);
        return;
    }

    if (kind == SceneNodeKind::model) {
        const CookedModelCollision* const cooked =
            renderer.editor_model_collision(node_opt->get().asset_path);
        const glm::vec3 world_offset =
            c0 * col.center_offset.x +
            c1 * col.center_offset.y +
            c2 * col.center_offset.z;

        if (col.shape == ColliderShape::triangle_mesh &&
            cooked != nullptr &&
            cooked->triangle_indices.size() >= 3U) {
            const std::size_t tri_count =
                cooked->triangle_indices.size() / 3U;
            constexpr std::size_t max_drawn_tris = 256U;
            const std::size_t stride =
                std::max<std::size_t>(1U, tri_count / max_drawn_tris);
            for (std::size_t t = 0U; t < tri_count; t += stride) {
                const auto i0 = cooked->triangle_indices[t * 3U + 0U];
                const auto i1 = cooked->triangle_indices[t * 3U + 1U];
                const auto i2 = cooked->triangle_indices[t * 3U + 2U];
                if (i0 >= cooked->triangle_positions.size() ||
                    i1 >= cooked->triangle_positions.size() ||
                    i2 >= cooked->triangle_positions.size()) {
                    continue;
                }
                const auto xf = [&](const std::array<float, 3>& p) noexcept {
                    return glm::vec3(
                               world * glm::vec4(p[0], p[1], p[2], 1.0F)) +
                           world_offset;
                };
                const glm::vec3 v0 = xf(cooked->triangle_positions[i0]);
                const glm::vec3 v1 = xf(cooked->triangle_positions[i1]);
                const glm::vec3 v2 = xf(cooked->triangle_positions[i2]);
                draw_segment(v0, v1);
                draw_segment(v1, v2);
                draw_segment(v2, v0);
            }
            return;
        }

        if (col.shape == ColliderShape::convex_hull &&
            cooked != nullptr &&
            cooked->convex_hull_vertices.size() >= 4U) {
            const std::size_t n = cooked->convex_hull_vertices.size();
            for (std::size_t i = 0U; i < n; ++i) {
                const auto& pa = cooked->convex_hull_vertices[i];
                const auto& pb = cooked->convex_hull_vertices[(i + 1U) % n];
                const glm::vec3 wa =
                    glm::vec3(world * glm::vec4(pa[0], pa[1], pa[2], 1.0F)) +
                    world_offset;
                const glm::vec3 wb =
                    glm::vec3(world * glm::vec4(pb[0], pb[1], pb[2], 1.0F)) +
                    world_offset;
                draw_segment(wa, wb);
            }
            const glm::vec3 lc{
                cooked->local_bounds.center[0],
                cooked->local_bounds.center[1],
                cooked->local_bounds.center[2],
            };
            const glm::vec3 le{
                cooked->local_bounds.half_extents[0],
                cooked->local_bounds.half_extents[1],
                cooked->local_bounds.half_extents[2],
            };
            draw_obb(
                glm::vec3(world * glm::vec4(lc, 1.0F)) + world_offset,
                ax0,
                ax1,
                ax2,
                le * sx_sy_sz);
            return;
        }

        if (col.shape == ColliderShape::auto_bounds) {
            if (cooked != nullptr && !cooked->submesh_boxes.empty()) {
                for (const CookedCollisionBox& sb : cooked->submesh_boxes) {
                    const glm::vec3 lc{sb.center[0], sb.center[1], sb.center[2]};
                    const glm::vec3 le{
                        sb.half_extents[0],
                        sb.half_extents[1],
                        sb.half_extents[2],
                    };
                    draw_obb(
                        glm::vec3(world * glm::vec4(lc, 1.0F)) + world_offset,
                        ax0,
                        ax1,
                        ax2,
                        le * sx_sy_sz);
                }
                return;
            }
            if (const auto bounds = renderer.editor_node_bounds(node_id)) {
                draw_obb(
                    bounds->center + world_offset,
                    glm::vec3(1.0F, 0.0F, 0.0F),
                    glm::vec3(0.0F, 1.0F, 0.0F),
                    glm::vec3(0.0F, 0.0F, 1.0F),
                    bounds->extent);
                return;
            }
        }
    }

    if (col.shape == ColliderShape::auto_bounds &&
        kind == SceneNodeKind::ground) {
        draw_obb(
            center - ax1 * 0.25F,
            ax0,
            ax1,
            ax2,
            glm::vec3(sx_sy_sz.x, 0.25F, sx_sy_sz.z));
        return;
    }

    const glm::vec3 local_ext =
        (col.shape == ColliderShape::auto_bounds && kind == SceneNodeKind::cube)
            ? glm::vec3(1.0F)
            : col.half_extents;
    draw_obb(center, ax0, ax1, ax2, local_ext * sx_sy_sz);
}

void draw_audio_wireframe(
    ImDrawList* const draw_list,
    const SceneDocument& scene,
    const SceneNodeId node_id,
    const glm::mat4& view_projection,
    const ImVec2 image_minimum,
    const float image_width,
    const float image_height) noexcept {
    if (draw_list == nullptr || !scene.valid(node_id)) {
        return;
    }
    const auto world_opt = scene_world_matrix(scene, node_id);
    if (!world_opt) {
        return;
    }
    const glm::mat4& world = *world_opt;
    const glm::vec3 center = glm::vec3(world[3]);

    const auto project_point = [&](const glm::vec3 world_pt,
                                   ImVec2& out) noexcept -> bool {
        const glm::vec4 clip = view_projection * glm::vec4(world_pt, 1.0F);
        if (!std::isfinite(clip.w) || clip.w <= 0.01F) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = ImVec2{
            image_minimum.x + (ndc.x * 0.5F + 0.5F) * image_width,
            image_minimum.y + (0.5F - ndc.y * 0.5F) * image_height,
        };
        return true;
    };

    const auto draw_segment = [&](const glm::vec3 a,
                                  const glm::vec3 b,
                                  const ImU32 color) noexcept {
        ImVec2 sa{};
        ImVec2 sb{};
        if (project_point(a, sa) && project_point(b, sb)) {
            draw_list->AddLine(sa, sb, color, 1.5F);
        }
    };

    constexpr int ring_segments = 24;
    constexpr float two_pi = 6.28318530718F;
    const auto draw_sphere = [&](const float r, const ImU32 color) noexcept {
        if (r <= 1.0e-3F) {
            return;
        }
        const auto draw_circle = [&](const glm::vec3 u,
                                     const glm::vec3 v) noexcept {
            for (int s = 0; s < ring_segments; ++s) {
                const float a0 = two_pi * static_cast<float>(s) /
                    static_cast<float>(ring_segments);
                const float a1 = two_pi * static_cast<float>(s + 1) /
                    static_cast<float>(ring_segments);
                draw_segment(
                    center + (u * std::cos(a0) + v * std::sin(a0)) * r,
                    center + (u * std::cos(a1) + v * std::sin(a1)) * r,
                    color);
            }
        };
        draw_circle(glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
        draw_circle(glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F));
        draw_circle(glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F));
    };

    if (const auto src_opt = scene.audio_source(node_id);
        src_opt && src_opt->get().enabled && src_opt->get().spatial) {
        const AudioSourceComponent& src = src_opt->get();
        draw_sphere(src.min_distance, IM_COL32(72, 212, 255, 225));
        draw_sphere(src.max_distance, IM_COL32(52, 148, 224, 150));
        if (src.outer_cone_degrees < 359.9F) {
            glm::vec3 fwd = -glm::vec3(world[2]);
            const float len_sq = glm::dot(fwd, fwd);
            fwd = len_sq > 1.0e-6F
                ? (fwd / std::sqrt(len_sq))
                : glm::vec3(0.0F, 0.0F, -1.0F);
            const glm::vec3 ref = std::abs(fwd.y) < 0.95F
                ? glm::vec3(0.0F, 1.0F, 0.0F)
                : glm::vec3(1.0F, 0.0F, 0.0F);
            const glm::vec3 right = glm::normalize(glm::cross(fwd, ref));
            const glm::vec3 up = glm::normalize(glm::cross(right, fwd));
            const auto draw_cone = [&](const float deg, const ImU32 col) {
                const float half_rad = glm::radians(deg * 0.5F);
                const float cos_h = std::cos(half_rad);
                const float sin_h = std::sin(half_rad);
                const float cone_len = std::min(src.max_distance, 12.0F);
                for (int i = 0; i < 4; ++i) {
                    const float ang = two_pi * 0.25F * static_cast<float>(i);
                    const glm::vec3 radial =
                        right * std::cos(ang) + up * std::sin(ang);
                    const glm::vec3 ray_dir = fwd * cos_h + radial * sin_h;
                    draw_segment(center, center + ray_dir * cone_len, col);
                }
            };
            draw_cone(src.inner_cone_degrees, IM_COL32(120, 240, 255, 210));
            draw_cone(src.outer_cone_degrees, IM_COL32(72, 164, 240, 150));
        }
    }

    if (const auto zone_opt = scene.reverb_zone(node_id);
        zone_opt && zone_opt->get().enabled) {
        const AudioReverbZoneComponent& zone = zone_opt->get();
        draw_sphere(zone.inner_radius, IM_COL32(198, 118, 255, 220));
        draw_sphere(zone.outer_radius, IM_COL32(150, 84, 224, 150));
    }
}

void draw_skeleton_wireframe(
    ImDrawList* const draw_list,
    const SceneDocument& scene,
    const Renderer& renderer,
    const AnimatorRuntimeSystem* const runtime,
    const SceneNodeId selected_node,
    const glm::mat4& view_projection,
    const ImVec2 image_minimum,
    const float image_width,
    const float image_height,
    std::vector<SkeletonDebugSegment>& segments_scratch) noexcept {
    if (draw_list == nullptr || !scene.valid(selected_node) ||
        image_width <= 1.0F || image_height <= 1.0F ||
        runtime == nullptr) {
        return;
    }
    const auto node_opt = scene.node(selected_node);
    if (!node_opt || node_opt->get().kind != SceneNodeKind::model ||
        node_opt->get().asset_path.empty()) {
        return;
    }
    const CookedSkeletonAsset* const skeleton =
        renderer.find_model_skeleton(node_opt->get().asset_path);
    const auto world_opt = scene_world_matrix(scene, selected_node);
    if (skeleton == nullptr || skeleton->bones.empty() || !world_opt) {
        return;
    }
    const auto anim_comp = scene.animator(selected_node);
    segments_scratch.clear();
    runtime->collect_debug_skeleton_segments(
        selected_node,
        *world_opt,
        *skeleton,
        segments_scratch,
        anim_comp ? &anim_comp->get() : nullptr);

    const auto project_point = [&](const glm::vec3 world_pt,
                                   ImVec2& out_screen) noexcept {
        const glm::vec4 clip = view_projection * glm::vec4(world_pt, 1.0F);
        if (!std::isfinite(clip.w) || clip.w <= 0.01F) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out_screen = ImVec2(
            image_minimum.x + (ndc.x * 0.5F + 0.5F) * image_width,
            image_minimum.y + (0.5F - ndc.y * 0.5F) * image_height);
        return std::isfinite(out_screen.x) && std::isfinite(out_screen.y);
    };

    constexpr ImU32 bone_color = IM_COL32(86, 224, 180, 230);
    constexpr ImU32 joint_color = IM_COL32(220, 255, 240, 240);
    constexpr ImU32 ik_color = IM_COL32(255, 204, 64, 240);
    for (const SkeletonDebugSegment& seg : segments_scratch) {
        ImVec2 sa{};
        ImVec2 sb{};
        if (!project_point(seg.world_a, sa) ||
            !project_point(seg.world_b, sb)) {
            continue;
        }
        if (seg.is_ik_marker) {
            draw_list->AddLine(sa, sb, ik_color, 1.75F);
            draw_list->AddCircle(sb, 4.0F, ik_color, 12, 1.5F);
        } else {
            draw_list->AddLine(sa, sb, bone_color, 2.0F);
            draw_list->AddCircleFilled(sb, 2.5F, joint_color, 8);
        }
    }
}

void draw_spline_and_camera_wireframe(
    ImDrawList* const draw_list,
    const SceneDocument& scene,
    const SceneNodeId selected_node,
    const glm::mat4& view_projection,
    const ImVec2 image_minimum,
    const float image_width,
    const float image_height,
    std::vector<SceneWorldNode>& world_nodes_scratch,
    std::vector<std::uint32_t>& ancestors_scratch) noexcept {
    if (draw_list == nullptr ||
        image_width <= 1.0F ||
        image_height <= 1.0F) {
        return;
    }

    const auto project_point = [&](const glm::vec3 world_pt,
                                   ImVec2& out_screen) noexcept -> bool {
        const glm::vec4 clip = view_projection * glm::vec4(world_pt, 1.0F);
        if (!std::isfinite(clip.w) || clip.w <= 0.01F) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out_screen = ImVec2(
            image_minimum.x + (ndc.x * 0.5F + 0.5F) * image_width,
            image_minimum.y + (0.5F - ndc.y * 0.5F) * image_height);
        return std::isfinite(out_screen.x) && std::isfinite(out_screen.y);
    };

    const auto draw_segment = [&](const glm::vec3 a,
                                  const glm::vec3 b,
                                  const ImU32 color,
                                  const float thickness = 1.75F) noexcept {
        ImVec2 sa{};
        ImVec2 sb{};
        if (project_point(a, sa) && project_point(b, sb)) {
            draw_list->AddLine(sa, sb, color, thickness);
        }
    };

    const auto owners = scene.splines().owners();
    const auto values = scene.splines().values();
    for (std::size_t i = 0U; i < owners.size(); ++i) {
        const SceneNodeId spline_node = owners[i];
        const SplineComponent& spline = values[i];
        if (!scene.valid(spline_node) || spline.point_count < 2U) {
            continue;
        }
        const auto world_opt = scene_world_matrix(scene, spline_node);
        if (!world_opt) {
            continue;
        }
        const glm::mat4& world = *world_opt;
        const bool is_selected = spline_node == selected_node;
        const ImU32 curve_color = is_selected
            ? IM_COL32(255, 198, 64, 240)
            : IM_COL32(76, 198, 245, 195);
        const ImU32 pt_color = is_selected
            ? IM_COL32(255, 240, 160, 255)
            : IM_COL32(160, 228, 255, 210);
        const std::size_t segments = std::clamp<std::size_t>(
            static_cast<std::size_t>(spline.point_count) * 16U,
            24U,
            256U);
        SplineSample prev = evaluate_spline(spline, world, 0.0F);
        for (std::size_t s = 1U; s <= segments; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const SplineSample curr = evaluate_spline(spline, world, u);
            draw_segment(
                prev.position,
                curr.position,
                curve_color,
                is_selected ? 2.4F : 1.6F);
            prev = curr;
        }
        for (std::uint32_t p = 0U; p < spline.point_count; ++p) {
            const glm::vec3 wp = glm::vec3(
                world * glm::vec4(spline.points[p].position, 1.0F));
            ImVec2 sp{};
            if (project_point(wp, sp)) {
                draw_list->AddCircleFilled(
                    sp, is_selected ? 4.2F : 3.0F, pt_color, 10);
            }
            if (spline.basis == SplineBasis::cubic_hermite && is_selected) {
                const glm::mat3 linear_mat(world);
                draw_segment(
                    wp,
                    wp + linear_mat * spline.points[p].tangent_out * 0.35F,
                    IM_COL32(255, 118, 96, 220),
                    1.2F);
                draw_segment(
                    wp,
                    wp - linear_mat * spline.points[p].tangent_in * 0.35F,
                    IM_COL32(128, 220, 140, 200),
                    1.2F);
            }
        }
    }

    world_nodes_scratch.clear();
    ancestors_scratch.clear();
    scene.build_world_nodes(world_nodes_scratch, ancestors_scratch);
    for (const SceneWorldNode& wn : world_nodes_scratch) {
        if (wn.kind != SceneNodeKind::camera || !wn.world_visible) {
            continue;
        }
        const bool is_selected = wn.id == selected_node;
        const ImU32 cam_color = is_selected
            ? IM_COL32(255, 224, 96, 240)
            : IM_COL32(150, 210, 235, 175);
        const glm::vec3 origin = glm::vec3(wn.world_matrix[3]);
        const auto safe_norm = [](const glm::vec3 v, const glm::vec3 fb) noexcept {
            const float ls = glm::dot(v, v);
            return ls > 1.0e-6F ? v / std::sqrt(ls) : fb;
        };
        const glm::vec3 right = safe_norm(
            glm::vec3(wn.world_matrix[0]), glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::vec3 up = safe_norm(
            glm::vec3(wn.world_matrix[1]), glm::vec3(0.0F, 1.0F, 0.0F));
        const glm::vec3 fwd = safe_norm(
            -glm::vec3(wn.world_matrix[2]), glm::vec3(0.0F, 0.0F, -1.0F));

        constexpr float depth = 0.9F;
        constexpr float hw = 0.45F;
        constexpr float hh = 0.28F;
        const glm::vec3 c = origin + fwd * depth;
        const glm::vec3 tl = c - right * hw + up * hh;
        const glm::vec3 tr = c + right * hw + up * hh;
        const glm::vec3 br = c + right * hw - up * hh;
        const glm::vec3 bl = c - right * hw - up * hh;
        draw_segment(origin, tl, cam_color, is_selected ? 2.0F : 1.3F);
        draw_segment(origin, tr, cam_color, is_selected ? 2.0F : 1.3F);
        draw_segment(origin, br, cam_color, is_selected ? 2.0F : 1.3F);
        draw_segment(origin, bl, cam_color, is_selected ? 2.0F : 1.3F);
        draw_segment(tl, tr, cam_color, is_selected ? 2.0F : 1.3F);
        draw_segment(tr, br, cam_color, is_selected ? 2.0F : 1.3F);
        draw_segment(br, bl, cam_color, is_selected ? 2.0F : 1.3F);
        draw_segment(bl, tl, cam_color, is_selected ? 2.0F : 1.3F);
        // Up triangle indicator above frustum top edge.
        const glm::vec3 top_apex = c + up * (hh + 0.16F);
        draw_segment(tl * 0.7F + tr * 0.3F, top_apex, cam_color, 1.3F);
        draw_segment(tr * 0.7F + tl * 0.3F, top_apex, cam_color, 1.3F);
    }
}

} // namespace

void EditorUi::build_viewport(
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera,
    const std::chrono::steady_clock::time_point now) {
    ImGui::SetNextWindowSize(
        ImVec2(900.0F, 620.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(
            "Scene View",
            nullptr,
            ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    const bool editable = play_state_ == EditorPlayState::edit;
    viewport_focused_ = ImGui::IsWindowFocused(
        ImGuiFocusedFlags_RootAndChildWindows);
    const ImGuiIO& io = ImGui::GetIO();
    const bool viewport_shortcuts =
        editable && viewport_focused_ && !io.WantTextInput &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (focus_selection_requested_) {
        static_cast<void>(focus_selection(scene, renderer, camera));
        focus_selection_requested_ = false;
    }
    if (viewport_shortcuts) {
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            gizmo_operation_ = EditorGizmoOperation::translate;
        }
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            gizmo_operation_ = EditorGizmoOperation::rotate;
        }
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            gizmo_operation_ = EditorGizmoOperation::scale;
        }
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            static_cast<void>(focus_selection(scene, renderer, camera));
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
            std::uint32_t duplicated_nodes = 0U;
            std::string duplicate_error;
            if (!duplicate_selection(
                    scene, duplicated_nodes, duplicate_error)) {
                asset_error_ = std::move(duplicate_error);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
            scene.valid(selected_node_)) {
            pending_delete_ = selected_node_;
        }
    }

    ImGui::BeginDisabled(!editable);
    if (ImGui::RadioButton(
            "W  Move",
            gizmo_operation_ == EditorGizmoOperation::translate)) {
        gizmo_operation_ = EditorGizmoOperation::translate;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "E  Rotate",
            gizmo_operation_ == EditorGizmoOperation::rotate)) {
        gizmo_operation_ = EditorGizmoOperation::rotate;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "R  Scale",
            gizmo_operation_ == EditorGizmoOperation::scale)) {
        gizmo_operation_ = EditorGizmoOperation::scale;
    }
    ImGui::SameLine(0.0F, 18.0F);
    ImGui::SetNextItemWidth(82.0F);
    const char* const spaces[] = {"World", "Local"};
    int space_index = gizmo_space_ == EditorGizmoSpace::world ? 0 : 1;
    if (ImGui::Combo(
            "##GizmoSpace", &space_index, spaces,
            static_cast<int>(std::size(spaces)))) {
        gizmo_space_ = space_index == 0
            ? EditorGizmoSpace::world
            : EditorGizmoSpace::local;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Transform orientation");
    }
    ImGui::SameLine();
    static_cast<void>(ImGui::Checkbox("Snap", &gizmo_snap_enabled_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(76.0F);
    float* snap_value = &gizmo_translation_snap_;
    const char* snap_format = "%.2f";
    float snap_speed = 0.05F;
    if (gizmo_operation_ == EditorGizmoOperation::rotate) {
        snap_value = &gizmo_rotation_snap_;
        snap_format = "%.0f deg";
        snap_speed = 1.0F;
    } else if (gizmo_operation_ == EditorGizmoOperation::scale) {
        snap_value = &gizmo_scale_snap_;
        snap_format = "%.2f";
        snap_speed = 0.01F;
    }
    static_cast<void>(ImGui::DragFloat(
        "##GizmoSnap", snap_value, snap_speed, 0.01F, 100.0F,
        snap_format, ImGuiSliderFlags_AlwaysClamp));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Snap step. Hold Ctrl for temporary snapping.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0F, 14.0F);
    static_cast<void>(ImGui::Checkbox("Skeleton", &show_skeleton_wireframe_));
    ImGui::SameLine(0.0F, 10.0F);
    static_cast<void>(ImGui::Checkbox("Splines", &show_spline_wireframe_));

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float dpi = std::max(
        ImGui::GetWindowViewport()->DpiScale, 0.25F);
    const std::uint32_t width = static_cast<std::uint32_t>(
        std::max(available.x * dpi, 64.0F));
    const std::uint32_t height = static_cast<std::uint32_t>(
        std::max(available.y * dpi, 64.0F));
    if (!external_game_window_open_ ||
        play_state_ == EditorPlayState::edit) {
        request_viewport_extent(
            VkExtent2D{width, height}, now);
    }
    ImGui::Image(
        imgui_texture_id(scene_texture_),
        available,
        ImVec2(0.0F, 0.0F),
        ImVec2(1.0F, 1.0F));
    const bool viewport_clicked =
        ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool viewport_hovered_now = ImGui::IsItemHovered();
    if (!external_game_window_open_) {
        update_play_viewport_mouse(
            ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax(),
            viewport_clicked,
            viewport_hovered_now);
    }
    if (!external_game_window_open_ || viewport_clicked) {
        update_game_mouse_capture(viewport_clicked);
    }
    const bool viewport_activated =
        viewport_clicked ||
        ImGui::IsItemClicked(ImGuiMouseButton_Right);
    viewport_hovered_ = ImGui::IsItemHovered();
    if (viewport_activated) {
        // Dear ImGui focuses windows implicitly only for LMB clicks.
        ImGui::SetWindowFocus();
        viewport_focused_ = true;
    }
    const ImVec2 image_minimum = ImGui::GetItemRectMin();
    const ImVec2 image_maximum = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(
        image_minimum,
        image_maximum,
        ImGui::ColorConvertFloat4ToU32(
            EditorUiKit::border_color()),
        3.0F,
        0,
        1.0F);
    ui_kit_.viewport_badge(
        ImVec2(
            image_minimum.x + 10.0F,
            image_minimum.y + 10.0F),
        play_state_ == EditorPlayState::edit
            ? "EDIT CAMERA"
            : play_state_ == EditorPlayState::playing
            ? "GAME RUNNING"
            : "GAME PAUSED",
        play_state_ == EditorPlayState::edit
            ? EditorTone::info
            : play_state_ == EditorPlayState::playing
            ? EditorTone::success
            : EditorTone::warning);
    char resolution_label[64]{};
    std::snprintf(
        resolution_label,
        sizeof(resolution_label),
        "%u x %u",
        renderer.editor_viewport_extent().width,
        renderer.editor_viewport_extent().height);
    const ImVec2 resolution_size =
        ImGui::CalcTextSize(resolution_label);
    ui_kit_.viewport_badge(
        ImVec2(
            image_maximum.x - resolution_size.x - 39.0F,
            image_minimum.y + 10.0F),
        resolution_label,
        EditorTone::neutral);
    ui_kit_.viewport_badge(
        ImVec2(
            image_minimum.x + 10.0F,
            image_maximum.y - 34.0F),
        play_state_ == EditorPlayState::edit
            ? "LMB SELECT  /  RMB + WASD  /  F FOCUS"
            : game_mouse_window_id_ != 0U
            ? "WASD MOVE  /  SHIFT RUN  /  SPACE JUMP  /  ESC RELEASE"
            : "LMB CAPTURE MOUSE  /  WASD MOVE  /  SPACE JUMP",
        EditorTone::purple);

    const float image_width = image_maximum.x - image_minimum.x;
    const float image_height = image_maximum.y - image_minimum.y;
    const float viewport_fov = renderer.vertical_fov_degrees();
    const glm::mat4 view = camera.view_matrix();
    const glm::mat4 projection = editor_projection(
        image_width, image_height, viewport_fov);
    if (const auto bounds = renderer.editor_node_bounds(selected_node_)) {
        ImVec2 selection_minimum(
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::infinity());
        ImVec2 selection_maximum(
            -std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity());
        std::uint32_t projected_corners = 0U;
        for (const glm::vec3 corner : bounds_corners(*bounds)) {
            const glm::vec4 clip =
                projection * view * glm::vec4(corner, 1.0F);
            if (!std::isfinite(clip.w) || clip.w <= 0.0F) {
                continue;
            }
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            const ImVec2 screen{
                image_minimum.x + (ndc.x * 0.5F + 0.5F) * image_width,
                image_minimum.y + (0.5F - ndc.y * 0.5F) * image_height,
            };
            selection_minimum.x = std::min(selection_minimum.x, screen.x);
            selection_minimum.y = std::min(selection_minimum.y, screen.y);
            selection_maximum.x = std::max(selection_maximum.x, screen.x);
            selection_maximum.y = std::max(selection_maximum.y, screen.y);
            ++projected_corners;
        }
        if (projected_corners >= 2U) {
            ImGui::GetWindowDrawList()->AddRect(
                selection_minimum,
                selection_maximum,
                ImGui::ColorConvertFloat4ToU32(
                    EditorUiKit::color(EditorTone::info)),
                2.0F,
                0,
                1.5F);
        }
    }

    draw_collider_wireframe(
        ImGui::GetWindowDrawList(),
        scene,
        renderer,
        selected_node_,
        projection * view,
        image_minimum,
        image_width,
        image_height);
    draw_audio_wireframe(
        ImGui::GetWindowDrawList(),
        scene,
        selected_node_,
        projection * view,
        image_minimum,
        image_width,
        image_height);
    if (show_skeleton_wireframe_) {
        const AnimatorRuntimeSystem* const runtime =
            play_state_ != EditorPlayState::edit
                ? (gameplay_session_ != nullptr
                       ? &gameplay_session_->animator_runtime()
                       : nullptr)
                : edit_animator_preview_;
        draw_skeleton_wireframe(
            ImGui::GetWindowDrawList(),
            scene,
            renderer,
            runtime,
            selected_node_,
            projection * view,
            image_minimum,
            image_width,
            image_height,
            skeleton_wireframe_scratch_);
    }
    if (sequencer_ui_ != nullptr &&
        image_width > 1.0F &&
        image_height > 1.0F) {
        sequencer_ui_->eval_scratch.viewport_aspect =
            image_width / image_height;
        if (sequencer_ui_->preview_active) {
            sequencer_ui_->last_eval_frame.letterbox_bar_fraction =
                compute_letterbox_bar_fraction(
                    sequencer_ui_->eval_scratch.viewport_aspect,
                    sequencer_ui_->sequence.letterbox_aspect);
        }
    }
    if (show_spline_wireframe_ && sequencer_ui_ != nullptr) {
        draw_spline_and_camera_wireframe(
            ImGui::GetWindowDrawList(),
            scene,
            selected_node_,
            projection * view,
            image_minimum,
            image_width,
            image_height,
            sequencer_ui_->wireframe_world_nodes,
            sequencer_ui_->wireframe_ancestors);
    }

    if (play_state_ == EditorPlayState::edit &&
        sequencer_preview_active() &&
        sequencer_ui_ != nullptr) {
        ImDrawList* const vp_draw = ImGui::GetWindowDrawList();
        const CutsceneFrameResult& frame = sequencer_ui_->last_eval_frame;
        vp_draw->PushClipRect(image_minimum, image_maximum, true);
        if (frame.letterbox_bar_fraction > 0.001F) {
            const float bar_h = image_height * frame.letterbox_bar_fraction;
            vp_draw->AddRectFilled(
                image_minimum,
                ImVec2(image_maximum.x, image_minimum.y + bar_h),
                IM_COL32(0, 0, 0, 255));
            vp_draw->AddRectFilled(
                ImVec2(image_minimum.x, image_maximum.y - bar_h),
                image_maximum,
                IM_COL32(0, 0, 0, 255));
        }
        if (!frame.active_subtitle_text.empty()) {
            const auto resolve_sub = [&](const std::string_view key_or_text) {
                return gameplay_session_ != nullptr
                    ? gameplay_session_->localization().resolve(key_or_text)
                    : key_or_text;
            };
            const std::string_view speaker =
                resolve_sub(frame.active_subtitle_speaker);
            const std::string_view text =
                resolve_sub(frame.active_subtitle_text);
            std::array<char, 512U> sub_line{};
            if (!speaker.empty()) {
                std::snprintf(
                    sub_line.data(),
                    sub_line.size(),
                    "%.*s: %.*s",
                    static_cast<int>(speaker.size()),
                    speaker.data(),
                    static_cast<int>(text.size()),
                    text.data());
            } else {
                std::snprintf(
                    sub_line.data(),
                    sub_line.size(),
                    "%.*s",
                    static_cast<int>(text.size()),
                    text.data());
            }
            const ImVec2 txt_size = ImGui::CalcTextSize(sub_line.data());
            const float bar_h = image_height * frame.letterbox_bar_fraction;
            const ImVec2 txt_pos(
                image_minimum.x + std::max(8.0F, (image_width - txt_size.x) * 0.5F),
                image_maximum.y - std::max(bar_h, 12.0F) - txt_size.y - 14.0F);
            vp_draw->AddRectFilled(
                ImVec2(txt_pos.x - 10.0F, txt_pos.y - 6.0F),
                ImVec2(txt_pos.x + txt_size.x + 10.0F, txt_pos.y + txt_size.y + 6.0F),
                IM_COL32(10, 12, 16, 200),
                6.0F);
            const std::uint32_t rgba = frame.active_subtitle_color;
            const ImU32 sub_col = IM_COL32(
                (rgba >> 24U) & 0xFFU,
                (rgba >> 16U) & 0xFFU,
                (rgba >> 8U) & 0xFFU,
                rgba & 0xFFU);
            vp_draw->AddText(txt_pos, sub_col, sub_line.data());
        }
        vp_draw->PopClipRect();
    }

    bool gizmo_over = false;
    bool gizmo_using = false;
    if (editable && !sequencer_preview_active() && scene.valid(selected_node_)) {
        if (auto world = scene_world_matrix(scene, selected_node_)) {
            ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
            ImGuizmo::SetRect(
                image_minimum.x,
                image_minimum.y,
                image_width,
                image_height);
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::Enable(true);
            ImGuizmo::PushID(static_cast<int>(selected_node_.index));
            std::array<float, 3U> translation_snap{
                gizmo_translation_snap_,
                gizmo_translation_snap_,
                gizmo_translation_snap_,
            };
            const bool snap_active = gizmo_snap_enabled_ || io.KeyCtrl;
            const float* snap = nullptr;
            if (snap_active) {
                snap = gizmo_operation_ == EditorGizmoOperation::translate
                    ? translation_snap.data()
                    : gizmo_operation_ == EditorGizmoOperation::rotate
                    ? &gizmo_rotation_snap_
                    : &gizmo_scale_snap_;
            }
            const bool manipulated = ImGuizmo::Manipulate(
                glm::value_ptr(view),
                glm::value_ptr(projection),
                imguizmo_operation(gizmo_operation_),
                imguizmo_space(gizmo_space_),
                glm::value_ptr(*world),
                nullptr,
                snap);
            gizmo_over = ImGuizmo::IsOver();
            gizmo_using = ImGuizmo::IsUsing();
            ImGuizmo::PopID();
            if (manipulated) {
                const auto local = scene_local_transform_from_world(
                    scene, selected_node_, *world);
                if (local) {
                    const auto selected = scene.node(selected_node_);
                    if (selected && selected->get().transform != *local) {
                        if (!gizmo_edit_snapshot_) {
                            gizmo_edit_snapshot_ = scene;
                        }
                        selected->get().transform = *local;
                        scene.mark_content_changed();
                    }
                } else {
                    asset_error_ =
                        "The gizmo result cannot be represented as a finite "
                        "positive-scale local transform.";
                }
            }
        }
    }
    if (gizmo_edit_snapshot_ && !gizmo_using) {
        push_undo_snapshot(std::move(*gizmo_edit_snapshot_));
        gizmo_edit_snapshot_.reset();
    }

    if (viewport_clicked && editable && !gizmo_over && !gizmo_using) {
        const auto ray = viewport_ray(
            io.MousePos, image_minimum, image_maximum, camera, viewport_fov);
        if (ray) {
            const auto picked = renderer.pick_editor_scene(
                ray->origin, ray->direction);
            static_cast<void>(select_node(
                scene, picked.value_or(SceneNodeId{})));
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(
                    "VCR_ASSET_PATH");
            payload != nullptr) {
            if (payload->Data == nullptr ||
                payload->DataSize <= 1 ||
                static_cast<std::size_t>(payload->DataSize) >
                    max_scene_asset_path_bytes + 1U) {
                asset_error_ =
                    "Dropped asset path is empty or exceeds "
                    "the scene path limit.";
            } else {
                const auto* bytes =
                    static_cast<const char*>(payload->Data);
                const std::size_t size =
                    static_cast<std::size_t>(
                        payload->DataSize);
                if (bytes[size - 1U] != '\0') {
                    asset_error_ =
                        "Dropped asset path is not terminated.";
                } else {
                    const std::string_view relative_path{
                        bytes, size - 1U};
                    static_cast<void>(
                        add_model_asset_to_scene(
                            scene, renderer, relative_path, {}));
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();
}

void EditorUi::build_external_game_window(
    const std::chrono::steady_clock::time_point now) {
    if (!external_game_window_open_ ||
        play_state_ == EditorPlayState::edit ||
        !platform_windows_enabled_) {
        external_game_window_open_ = false;
        return;
    }
    ImGuiWindowClass window_class{};
    window_class.ViewportFlagsOverrideSet =
        ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&window_class);
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(
            main->Pos.x + 80.0F,
            main->Pos.y + 80.0F),
        ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(
        ImVec2(1280.0F, 720.0F), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(
            "Game##External",
            &external_game_window_open_,
            ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float dpi = std::max(
            ImGui::GetWindowViewport()->DpiScale, 0.25F);
        const std::uint32_t width =
            static_cast<std::uint32_t>(
                std::max(available.x * dpi, 64.0F));
        const std::uint32_t height =
            static_cast<std::uint32_t>(
                std::max(available.y * dpi, 64.0F));
        request_viewport_extent(
            VkExtent2D{width, height}, now);
        ImGui::Image(
            imgui_texture_id(scene_texture_),
            available,
            ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F));
        const bool ext_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const bool ext_hovered = ImGui::IsItemHovered();
        update_play_viewport_mouse(
            ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax(),
            ext_clicked,
            ext_hovered);
        update_game_mouse_capture(ext_clicked);
        const bool external_viewport_activated =
            ext_clicked ||
            ImGui::IsItemClicked(ImGuiMouseButton_Right);
        external_viewport_hovered_ = ext_hovered;
        if (external_viewport_activated) {
            ImGui::SetWindowFocus();
        }
        external_viewport_focused_ = ImGui::IsWindowFocused(
            ImGuiFocusedFlags_RootAndChildWindows);
    }
    ImGui::End();
}

void EditorUi::build_assets(
    SceneDocument& scene,
    Renderer& renderer) {
    if (external_drop_ != nullptr) {
        external_drop_->region_valid = false;
    }
    ImGui::SetNextWindowSize(
        ImVec2(720.0F, 270.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Assets")) {
        ImGui::End();
        return;
    }
    if (ui_kit_.icon_button(
            "AssetsUp",
            EditorIcon::folder,
            "Go to parent folder")) {
        if (asset_browser_.navigate_up(asset_error_)) {
            selected_asset_.reset();
        }
    }
    ImGui::SameLine(0.0F, 5.0F);
    if (ui_kit_.icon_button(
            "AssetsRefresh",
            EditorIcon::refresh,
            "Refresh current folder")) {
        if (asset_browser_.refresh(asset_error_)) {
            selected_asset_.reset();
        }
    }
    ImGui::SameLine();
    const bool can_import =
        play_state_ == EditorPlayState::edit &&
        !import_activity_pending() &&
        asset_import_enabled_ &&
        import_worker_ != nullptr &&
        !import_worker_->failed();
    if (ui_kit_.action_button(
            "AssetsImport",
            EditorIcon::import_asset,
            "Import",
            EditorButtonStyle::secondary,
            can_import)) {
        import_dialog_requested_ = true;
    }
    if (!can_import &&
        ImGui::IsItemHovered(
            ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            !asset_import_enabled_
                ? "Import requires a writable project checkout; "
                  "packaged assets are browse-only."
                : (import_worker_ != nullptr &&
                   import_worker_->failed()
                    ? "The import worker stopped after an internal "
                      "error; restart the editor."
                : (play_state_ == EditorPlayState::edit
                    ? "Wait for the current import."
                    : "Asset import is available only in Edit mode.")));
    }
    ImGui::SameLine();
    const bool can_add_selected_model =
        play_state_ == EditorPlayState::edit &&
        selected_asset_.has_value() &&
        (selected_asset_->kind == AssetEntryKind::model ||
         selected_asset_->kind == AssetEntryKind::prefab ||
         selected_asset_->kind == AssetEntryKind::audio ||
         selected_asset_->kind == AssetEntryKind::sound_cue);
    if (ui_kit_.action_button(
            "AddSelectedAsset",
            EditorIcon::add,
            "Add to Scene",
            EditorButtonStyle::primary,
            can_add_selected_model)) {
        static_cast<void>(add_model_asset_to_scene(
            scene,
            renderer,
            selected_asset_->relative_display,
            selected_asset_->display_name));
    }
    if (!can_add_selected_model &&
        ImGui::IsItemHovered(
            ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Select a .vcrasset.json model, .vcrprefab.json prefab, audio clip, or .vcrcue.json Sound Cue.");
    }
    if (audio_engine_ != nullptr) {
        const bool selected_audio =
            selected_asset_.has_value() &&
            (selected_asset_->kind == AssetEntryKind::audio ||
             selected_asset_->kind == AssetEntryKind::sound_cue);
        const bool preview_active = audio_engine_->is_preview_playing();
        if (selected_audio || preview_active) {
            ImGui::SameLine();
            if (preview_active) {
                if (ui_kit_.action_button(
                        "StopAssetPreview",
                        EditorIcon::stop,
                        "Stop Preview",
                        EditorButtonStyle::secondary,
                        true)) {
                    audio_engine_->stop_preview();
                }
            } else if (ui_kit_.action_button(
                           "PlayAssetPreview",
                           EditorIcon::play,
                           "Preview",
                           EditorButtonStyle::secondary,
                           selected_audio)) {
                std::string preview_err;
                if (!audio_engine_->play_preview(
                        selected_asset_->relative_display, preview_err)) {
                    asset_error_ = std::move(preview_err);
                } else {
                    asset_error_.clear();
                }
            }
        }
    }
    if (asset_import_enabled_ && play_state_ == EditorPlayState::edit) {
        ImGui::SameLine();
        if (ui_kit_.action_button(
                "CreateSoundCueAsset",
                EditorIcon::add,
                "Sound Cue",
                EditorButtonStyle::secondary,
                selected_asset_.has_value() &&
                    selected_asset_->kind == AssetEntryKind::audio)) {
            SoundCueAsset cue{};
            cue.name = selected_asset_->display_name;
            cue.bus = AudioBusId::sfx;
            cue.avoid_repeat = true;
            cue.volume_min = 0.92F;
            cue.volume_max = 1.0F;
            cue.pitch_min = 0.95F;
            cue.pitch_max = 1.05F;
            cue.entries.push_back(SoundCueClipEntry{
                .asset_path = selected_asset_->relative_display,
                .weight = 1.0F,
                .volume_scale = 1.0F,
                .pitch_scale = 1.0F,
            });
            const std::string cue_rel_path =
                make_default_sound_cue_relative_path(cue.name);
            std::string cue_err;
            if (save_sound_cue_file(
                    cue, asset_browser_.root(), cue_rel_path, cue_err)) {
                refresh_asset_browser();
                asset_error_.clear();
                scene_file_status_ = "Created Sound Cue: " + cue_rel_path;
            } else {
                asset_error_ = std::move(cue_err);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "Select an audio clip to create a randomized .vcrcue.json Sound Cue.");
        }
    }
    ImGui::SameLine();
    ui_kit_.badge(
        asset_browser_.current_directory_display().data(),
        EditorTone::neutral);
    if (!asset_error_.empty()) {
        ui_kit_.status_text(
            asset_error_.c_str(),
            EditorTone::danger);
    }
    if (asset_browser_.truncated()) {
        ImGui::TextColored(
            EditorUiKit::color(EditorTone::warning),
            "Directory contains more than %zu entries; "
            "the bounded view is truncated.",
            AssetBrowser::maximum_entries);
    }
    const auto entries = asset_browser_.entries();
    const std::size_t entry_count = entries.size();
    ImGui::BeginChild(
        "AssetList",
        ImVec2(0.0F, -34.0F),
        ImGuiChildFlags_Borders);
    if (external_drop_ != nullptr) {
        external_drop_->minimum = ImGui::GetWindowPos();
        const ImVec2 child_size = ImGui::GetWindowSize();
        external_drop_->maximum = ImVec2{
            external_drop_->minimum.x + child_size.x,
            external_drop_->minimum.y + child_size.y,
        };
        const ImGuiViewport* const viewport =
            ImGui::GetWindowViewport();
        SDL_Window* const viewport_window =
            viewport_sdl_window(viewport);
        external_drop_->window_id =
            viewport_window != nullptr
            ? SDL_GetWindowID(viewport_window)
            : 0U;
        external_drop_->region_valid =
            external_drop_->window_id != 0U;
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(entry_count));
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart;
             index < clipper.DisplayEnd;
             ++index) {
            const AssetEntry& entry =
                entries[static_cast<std::size_t>(index)];
            ImGui::PushID(index);
            const EditorAssetRowResult row_result =
                ui_kit_.asset_row(
                    "Asset",
                    asset_icon(entry.kind),
                    entry.display_name.c_str(),
                    asset_kind_name(entry.kind),
                    selected_asset_ &&
                        selected_asset_->relative_path ==
                            entry.relative_path);
            if (row_result.activated) {
                selected_asset_ = entry;
                if (row_result.double_clicked &&
                    entry.kind == AssetEntryKind::directory) {
                    pending_asset_directory_ =
                        entry.relative_path;
                } else if (row_result.double_clicked &&
                           entry.kind ==
                               AssetEntryKind::material) {
                    open_material_editor(entry.relative_path);
                } else if (row_result.double_clicked &&
                           (entry.kind ==
                                AssetEntryKind::model ||
                            entry.kind ==
                                AssetEntryKind::prefab)) {
                    static_cast<void>(add_model_asset_to_scene(
                        scene,
                        renderer,
                        entry.relative_display,
                        entry.display_name));
                } else if (row_result.double_clicked &&
                           entry.kind == AssetEntryKind::cutscene) {
                    open_cutscene_in_sequencer(scene, entry.relative_display);
                } else if (row_result.double_clicked &&
                           (entry.kind == AssetEntryKind::audio ||
                            entry.kind == AssetEntryKind::sound_cue) &&
                           audio_engine_ != nullptr) {
                    std::string preview_err;
                    if (!audio_engine_->play_preview(
                            entry.relative_display, preview_err)) {
                        asset_error_ = std::move(preview_err);
                    } else {
                        asset_error_.clear();
                    }
                }
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(
                    "VCR_ASSET_PATH",
                    entry.relative_display.c_str(),
                    entry.relative_display.size() + 1U);
                ImGui::TextUnformatted(
                    entry.relative_display.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (entry.kind == AssetEntryKind::material &&
                    ImGui::MenuItem("Open in Material Editor")) {
                    open_material_editor(entry.relative_path);
                }
                if (entry.kind == AssetEntryKind::cutscene &&
                    ImGui::MenuItem("Open in Sequencer")) {
                    open_cutscene_in_sequencer(scene, entry.relative_display);
                }
                if ((entry.kind == AssetEntryKind::audio ||
                     entry.kind == AssetEntryKind::sound_cue) &&
                    audio_engine_ != nullptr &&
                    ImGui::MenuItem("Preview Audio")) {
                    std::string preview_err;
                    if (!audio_engine_->play_preview(
                            entry.relative_display, preview_err)) {
                        asset_error_ = std::move(preview_err);
                    } else {
                        asset_error_.clear();
                    }
                }
                if ((entry.kind == AssetEntryKind::model ||
                     entry.kind == AssetEntryKind::prefab ||
                     entry.kind == AssetEntryKind::audio ||
                     entry.kind == AssetEntryKind::sound_cue) &&
                    ImGui::MenuItem("Add to Scene")) {
                    static_cast<void>(add_model_asset_to_scene(
                        scene,
                        renderer,
                        entry.relative_display,
                        entry.display_name));
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if (external_drop_ != nullptr && external_drop_->hover) {
        ImGui::GetWindowDrawList()->AddRect(
            external_drop_->minimum,
            external_drop_->maximum,
            ImGui::ColorConvertFloat4ToU32(
                EditorUiKit::color(EditorTone::primary)),
            2.0F,
            0,
            3.0F);
        const char* const message =
            "Drop files to import into this folder";
        const ImVec2 text_size = ImGui::CalcTextSize(message);
        const ImVec2 center{
            (external_drop_->minimum.x +
             external_drop_->maximum.x) *
                0.5F,
            (external_drop_->minimum.y +
             external_drop_->maximum.y) *
                0.5F,
        };
        ImGui::GetWindowDrawList()->AddText(
            ImVec2{
                center.x - text_size.x * 0.5F,
                center.y - text_size.y * 0.5F,
            },
            ImGui::ColorConvertFloat4ToU32(
                EditorUiKit::text_color()),
            message);
    }
    ImGui::EndChild();
    if (pending_asset_directory_) {
        if (asset_browser_.navigate_to(
                *pending_asset_directory_, asset_error_)) {
            selected_asset_.reset();
        }
        pending_asset_directory_.reset();
    }
    char asset_status[96]{};
    std::snprintf(
        asset_status,
        sizeof(asset_status),
        "%zu ITEMS  /  CACHE HIGH-WATER %zu",
        entry_count,
        asset_browser_.high_water_mark());
    ui_kit_.badge(asset_status, EditorTone::neutral);
    ImGui::SameLine();
    if (selected_asset_) {
        ui_kit_.badge(
            asset_kind_name(selected_asset_->kind),
            EditorTone::info);
        ImGui::SameLine();
        ImGui::Text(
            "%s  /  %llu bytes",
            selected_asset_->relative_display.c_str(),
            static_cast<unsigned long long>(
                selected_asset_->size));
    } else {
        ImGui::TextDisabled("Select an asset for details.");
    }
    if (import_worker_ != nullptr && import_worker_->busy()) {
        ImGui::SameLine();
        ui_kit_.badge("IMPORTING", EditorTone::info);
    }
    ImGui::End();
}

void EditorUi::build_landscape(
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera) {
    ImGui::SetNextWindowSize(
        ImVec2(360.0F, 470.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Landscape")) {
        ImGui::End();
        return;
    }
    ui_kit_.panel_header(
        EditorIcon::terrain,
        "LANDSCAPE",
        "Flat terrain and deterministic vegetation scatter");
    const bool editable = play_state_ == EditorPlayState::edit;
    ImGui::BeginDisabled(!editable);

    ui_kit_.section_header(EditorIcon::ground, "Flat terrain");
    if (ui_kit_.begin_property_grid("FlatTerrainProperties")) {
        ui_kit_.property_row("Size X/Z");
        static_cast<void>(ImGui::DragFloat2(
            "##FlatTerrainSize",
            flat_ground_size_,
            0.5F,
            1.0F,
            8192.0F,
            "%.1f"));
        ui_kit_.property_row("Height");
        static_cast<void>(ImGui::DragFloat(
            "##FlatTerrainHeight",
            &flat_ground_height_,
            0.1F,
            -4096.0F,
            4096.0F,
            "%.2f"));
        ui_kit_.end_property_grid();
    }
    if (ui_kit_.action_button(
            "CreateFlatTerrain",
            EditorIcon::add,
            "Create flat terrain",
            EditorButtonStyle::primary,
            editable)) {
        stop_sequencer_preview(scene, &renderer, &camera);
        SceneNodeId parent{};
        if (const auto selected = scene.node(selected_node_);
            selected && selected->get().kind == SceneNodeKind::folder) {
            parent = selected_node_;
        }
        SceneDocument before = scene;
        std::string error;
        const SceneNodeId ground = create_flat_ground(
            scene,
            FlatGroundOptions{
                .parent = parent,
                .height = flat_ground_height_,
                .width = flat_ground_size_[0],
                .depth = flat_ground_size_[1],
            },
            error);
        if (ground) {
            push_undo_snapshot(std::move(before));
            selected_node_ = ground;
            hierarchy_revision_ = 0U;
            scene_file_status_ = "Created flat terrain.";
        } else {
            asset_error_ = std::move(error);
        }
    }

    ui_kit_.section_header(EditorIcon::model, "Vegetation scatter");
    const bool selected_model = selected_asset_.has_value() &&
        selected_asset_->kind == AssetEntryKind::model &&
        valid_scene_asset_path(selected_asset_->relative_display);
    if (selected_model) {
        ImGui::TextWrapped(
            "Asset: %s", selected_asset_->relative_display.c_str());
    } else {
        ui_kit_.status_text(
            "Select an imported *.vcrasset.json model in Assets.",
            EditorTone::neutral);
    }
    if (ui_kit_.begin_property_grid("ScatterProperties")) {
        ui_kit_.property_row("Name prefix");
        static_cast<void>(ImGui::InputText(
            "##ScatterName",
            scatter_name_prefix_.data(),
            scatter_name_prefix_.size()));
        ui_kit_.property_row("Count");
        static_cast<void>(ImGui::DragScalar(
            "##ScatterCount",
            ImGuiDataType_U32,
            &scatter_count_,
            1.0F,
            nullptr,
            nullptr,
            "%u"));
        scatter_count_ = std::clamp(
            scatter_count_, 1U, maximum_scatter_instances);
        ui_kit_.property_row("Seed");
        static_cast<void>(ImGui::InputScalar(
            "##ScatterSeed", ImGuiDataType_U32, &scatter_seed_));
        ui_kit_.property_row("Center X/Y/Z");
        static_cast<void>(ImGui::DragFloat3(
            "##ScatterCenter", scatter_center_, 0.1F));
        ui_kit_.property_row("Area X/Z");
        static_cast<void>(ImGui::DragFloat2(
            "##ScatterArea",
            scatter_size_,
            0.5F,
            0.1F,
            8192.0F,
            "%.1f"));
        ui_kit_.property_row("Scale min/max");
        static_cast<void>(ImGui::DragFloat2(
            "##ScatterScale",
            scatter_scale_,
            0.01F,
            0.01F,
            100.0F,
            "%.2f"));
        ui_kit_.end_property_grid();
    }
    const bool can_scatter = editable && selected_model &&
        scatter_scale_[0] <= scatter_scale_[1];
    if (ui_kit_.action_button(
            "ScatterVegetation",
            EditorIcon::add,
            "Scatter selected model",
            EditorButtonStyle::secondary,
            can_scatter) &&
        selected_asset_) {
        SceneTransform base_transform{};
        std::string error;
        if (!renderer.prepare_editor_model(
                asset_browser_.root(),
                selected_asset_->relative_display,
                base_transform,
                error)) {
            asset_error_ = error.empty()
                ? "Model preparation started; retry scatter when it completes."
                : std::move(error);
        } else {
            stop_sequencer_preview(scene, &renderer, &camera);
            SceneNodeId parent{};
            if (const auto selected = scene.node(selected_node_);
                selected && selected->get().kind == SceneNodeKind::folder) {
                parent = selected_node_;
            }
            SceneDocument before = scene;
            const bool scattered = scatter_model_instances(
                scene,
                ModelScatterOptions{
                    .asset_path = selected_asset_->relative_display,
                    .name_prefix = scatter_name_prefix_.data(),
                    .parent = parent,
                    .base_transform = base_transform,
                    .center_x = scatter_center_[0],
                    .height = scatter_center_[1],
                    .center_z = scatter_center_[2],
                    .width = scatter_size_[0],
                    .depth = scatter_size_[1],
                    .minimum_scale = scatter_scale_[0],
                    .maximum_scale = scatter_scale_[1],
                    .count = scatter_count_,
                    .seed = scatter_seed_,
                },
                scatter_created_,
                error);
            if (scattered) {
                push_undo_snapshot(std::move(before));
                hierarchy_revision_ = 0U;
                scene_file_status_ =
                    "Scattered " + std::to_string(scatter_created_.size()) +
                    " model instances.";
            } else {
                asset_error_ = std::move(error);
            }
        }
    }
    ImGui::EndDisabled();
    if (!editable) {
        ui_kit_.badge("LOCKED DURING PLAY", EditorTone::warning);
    }
    ImGui::End();
}

void EditorUi::refresh_asset_browser() noexcept {
    std::string ignored_error;
    if (asset_browser_.refresh(ignored_error) && selected_asset_) {
        bool still_present = false;
        for (const AssetEntry& entry : asset_browser_.entries()) {
            if (entry.relative_path == selected_asset_->relative_path) {
                still_present = true;
                break;
            }
        }
        if (!still_present) {
            selected_asset_.reset();
        }
    }
}

bool EditorUi::save_node_as_prefab(
    SceneDocument& scene,
    const SceneNodeId root_id,
    const std::string_view custom_relative_path,
    const bool zero_root_translation,
    const bool attach_instance,
    std::string& saved_relative_path,
    std::string& error) {
    error.clear();
    saved_relative_path.clear();
    if (play_state_ != EditorPlayState::edit) {
        error = "Prefabs can be saved only in Edit mode.";
        return false;
    }
    const auto node_opt = scene.node(root_id);
    if (!node_opt) {
        error = "Select a valid scene node before saving a prefab.";
        return false;
    }
    if (asset_browser_.root().empty()) {
        error = "The configured asset root is unavailable.";
        return false;
    }

    std::string target_path;
    if (!custom_relative_path.empty()) {
        target_path = std::string(custom_relative_path);
    } else if (const auto existing_inst = scene.prefab_instance(root_id);
               existing_inst &&
               existing_inst->get().is_prefab_root &&
               valid_prefab_asset_path(existing_inst->get().path_view())) {
        target_path = std::string(existing_inst->get().path_view());
    } else {
        target_path = make_default_prefab_relative_path(node_opt->get().name);
    }

    if (!save_prefab_file(
            scene,
            root_id,
            asset_browser_.root(),
            target_path,
            error,
            PrefabSaveOptions{
                .zero_root_translation = zero_root_translation,
            })) {
        return false;
    }

    if (attach_instance) {
        checkpoint_scene(scene);
        std::vector<SceneHierarchyRow> rows;
        rows.reserve(scene.size());
        scene.build_hierarchy_rows(rows, {}, SceneHierarchyScope::all);
        bool in_subtree = false;
        std::uint32_t root_depth = 0U;
        std::optional<std::uint32_t> nested_prefab_depth{};
        for (const SceneHierarchyRow& row : rows) {
            if (!in_subtree) {
                if (row.id == root_id) {
                    in_subtree = true;
                    root_depth = row.depth;
                    if (const auto u = scene.uuid(row.id)) {
                        PrefabInstanceComponent inst{};
                        if (inst.set_path(target_path)) {
                            inst.source_node_uuid = *u;
                            inst.is_prefab_root = true;
                            static_cast<void>(
                                scene.set_prefab_instance(row.id, inst));
                        }
                    }
                }
                continue;
            }
            if (row.depth <= root_depth) {
                break;
            }
            if (nested_prefab_depth && row.depth > *nested_prefab_depth) {
                continue;
            }
            nested_prefab_depth.reset();
            if (const auto child_inst = scene.prefab_instance(row.id);
                child_inst && child_inst->get().is_prefab_root) {
                nested_prefab_depth = row.depth;
                continue;
            }
            if (const auto u = scene.uuid(row.id)) {
                PrefabInstanceComponent inst{};
                if (inst.set_path(target_path)) {
                    inst.source_node_uuid = *u;
                    inst.is_prefab_root = false;
                    static_cast<void>(
                        scene.set_prefab_instance(row.id, inst));
                }
            }
        }
    }

    refresh_asset_browser();
    asset_error_.clear();
    saved_relative_path = target_path;
    scene_file_status_ = "Saved prefab: " + target_path;
    return true;
}

bool EditorUi::instantiate_prefab_in_scene(
    SceneDocument& scene,
    Renderer& renderer,
    const std::string_view relative_path,
    const PrefabInstantiateOptions& options,
    PrefabInstantiateResult& result,
    std::string& error) {
    error.clear();
    result = {};
    if (play_state_ != EditorPlayState::edit) {
        error = "Prefabs can be instantiated only in Edit mode.";
        return false;
    }
    if (asset_browser_.root().empty()) {
        error = "The configured asset root is unavailable.";
        return false;
    }

    SceneDocument snapshot = scene;
    if (!instantiate_prefab_file(
            scene,
            asset_browser_.root(),
            relative_path,
            options,
            result,
            error)) {
        return false;
    }

    std::vector<SceneHierarchyRow> rows;
    rows.reserve(scene.size());
    scene.build_hierarchy_rows(rows, {}, SceneHierarchyScope::all);
    bool in_subtree = false;
    std::uint32_t root_depth = 0U;
    for (const SceneHierarchyRow& row : rows) {
        if (!in_subtree) {
            if (row.id == result.root_id) {
                in_subtree = true;
                root_depth = row.depth;
            } else {
                continue;
            }
        } else if (row.depth <= root_depth) {
            break;
        }
        if (const auto created_node = scene.node(row.id);
            created_node && created_node->get().kind == SceneNodeKind::model &&
            valid_scene_asset_path(created_node->get().asset_path)) {
            SceneTransform unused_transform{};
            std::string prep_error;
            static_cast<void>(renderer.prepare_editor_model(
                asset_browser_.root(),
                created_node->get().asset_path,
                unused_transform,
                prep_error));
        }
    }

    push_undo_snapshot(std::move(snapshot));
    static_cast<void>(select_node(scene, result.root_id));
    asset_error_.clear();
    scene_file_status_ = "Instantiated prefab: " + std::string(relative_path);
    return true;
}

bool EditorUi::add_model_asset_to_scene(
    SceneDocument& scene,
    Renderer& renderer,
    const std::string_view relative_path,
    const std::string_view display_name) {
    if (!pending_model_path_.empty()) {
        asset_error_ = "A model is already being prepared. Wait for it to finish.";
        return false;
    }
    if (play_state_ != EditorPlayState::edit) {
        asset_error_ =
            "Models can be added to the scene only in Edit mode.";
        return false;
    }
    if (valid_prefab_asset_path(relative_path)) {
        stop_sequencer_preview(scene, &renderer, nullptr);
        SceneNodeId parent{};
        if (const auto selected = scene.node(selected_node_);
            selected && selected->get().kind == SceneNodeKind::folder) {
            parent = selected_node_;
        }
        PrefabInstantiateResult prefab_result{};
        std::string prefab_error;
        if (!instantiate_prefab_in_scene(
                scene,
                renderer,
                relative_path,
                PrefabInstantiateOptions{.parent = parent},
                prefab_result,
                prefab_error)) {
            asset_error_ = std::move(prefab_error);
            return false;
        }
        return true;
    }
    if (!relative_path.empty() &&
        valid_audio_asset_reference(relative_path)) {
        if (relative_path.ends_with(".vcrcue.json")) {
            SoundCueAsset validated_cue{};
            if (!load_sound_cue_file(
                    asset_browser_.root(),
                    relative_path,
                    validated_cue,
                    asset_error_)) {
                return false;
            }
        } else {
            std::filesystem::path resolved_audio{};
            std::filesystem::path source_rel{};
            std::string resolved_name{};
            bool stream_recommended = false;
            if (!resolve_sandboxed_audio_file(
                    asset_browser_.root(),
                    relative_path,
                    resolved_audio,
                    source_rel,
                    resolved_name,
                    stream_recommended,
                    asset_error_)) {
                return false;
            }
        }
        std::string_view emitter_name = display_name;
        if (emitter_name.empty()) {
            const std::size_t sep = relative_path.find_last_of('/');
            emitter_name = relative_path.substr(
                sep == std::string_view::npos ? 0U : sep + 1U);
        }
        for (const std::string_view suffix : {
                 std::string_view{".vcraudio.json"},
                 std::string_view{".vcrcue.json"},
                 std::string_view{".wav"},
                 std::string_view{".ogg"},
                 std::string_view{".flac"}}) {
            if (emitter_name.ends_with(suffix)) {
                emitter_name.remove_suffix(suffix.size());
                break;
            }
        }
        if (emitter_name.empty() ||
            emitter_name.size() > max_scene_node_name_bytes) {
            emitter_name = "Audio Emitter";
        }
        stop_sequencer_preview(scene, &renderer, nullptr);
        SceneNodeId parent{};
        if (const auto selected = scene.node(selected_node_);
            selected && selected->get().kind == SceneNodeKind::folder) {
            parent = selected_node_;
        }
        checkpoint_scene(scene);
        const SceneNodeId emitter = scene.create_node(
            std::string(emitter_name), SceneNodeKind::folder, parent);
        if (!emitter) {
            asset_error_ =
                "Unable to create the audio emitter node; scene capacity may be exhausted.";
            return false;
        }
        AudioSourceComponent src{};
        static_cast<void>(src.set_path(relative_path));
        static_cast<void>(scene.set_audio_source(emitter, src));
        selected_node_ = emitter;
        name_buffer_node_ = {};
        asset_error_.clear();
        scene_file_status_ =
            "Added audio source to scene: " + std::string(relative_path);
        return true;
    }
    if (!valid_scene_asset_path(relative_path)) {
        asset_error_ =
            "Only an imported .vcrasset.json model can be added "
            "to the scene.";
        return false;
    }
    stop_sequencer_preview(scene, &renderer, nullptr);

    try {
        std::error_code filesystem_error;
        const std::filesystem::path canonical_root =
            std::filesystem::weakly_canonical(
                asset_browser_.root(), filesystem_error);
        if (filesystem_error || canonical_root.empty()) {
            asset_error_ =
                "The configured asset root is unavailable.";
            return false;
        }
        const std::filesystem::path relative =
            path_from_utf8(relative_path);
        std::filesystem::path component_path = canonical_root;
        for (const auto& component : relative) {
            component_path /= component;
            const auto status =
                std::filesystem::symlink_status(
                    component_path, filesystem_error);
            if (filesystem_error) {
                asset_error_ =
                    "Unable to inspect the selected model path.";
                return false;
            }
            if (std::filesystem::is_symlink(status)) {
                asset_error_ =
                    "Symbolic links are not followed for scene models.";
                return false;
            }
        }
        const std::filesystem::path canonical_asset =
            std::filesystem::weakly_canonical(
                canonical_root / relative, filesystem_error);
        if (filesystem_error) {
            asset_error_ =
                "The selected model asset is unavailable.";
            return false;
        }
        const std::filesystem::path rooted_relative =
            std::filesystem::relative(
                canonical_asset,
                canonical_root,
                filesystem_error);
        if (filesystem_error ||
            path_escapes_asset_root(rooted_relative) ||
            !std::filesystem::is_regular_file(
                canonical_asset, filesystem_error) ||
            filesystem_error) {
            asset_error_ =
                "The selected model is outside Assets or inaccessible.";
            return false;
        }

        constexpr std::string_view manifest_suffix =
            ".vcrasset.json";
        std::string_view name = display_name;
        if (name.empty()) {
            const std::size_t separator =
                relative_path.find_last_of('/');
            name = relative_path.substr(
                separator == std::string_view::npos
                    ? 0U
                    : separator + 1U);
        }
        if (name.ends_with(manifest_suffix)) {
            name.remove_suffix(manifest_suffix.size());
        }
        if (name.empty() ||
            name.size() > max_scene_node_name_bytes ||
            name.find('\0') != std::string_view::npos) {
            name = "Model";
        }

        SceneTransform recommended_transform{};
        if (!renderer.prepare_editor_model(
                asset_browser_.root(),
                relative_path,
                recommended_transform,
                asset_error_)) {
            if (asset_error_.empty() && renderer.assets_pending()) {
                pending_model_path_ = relative_path;
                pending_model_name_ = name;
                pending_model_parent_ = selected_node_;
                scene_file_status_ = "Preparing model...";
                return true;
            }
            return false;
        }

        SceneNodeId parent{};
        if (const auto selected = scene.node(selected_node_);
            selected &&
            selected->get().kind == SceneNodeKind::folder) {
            parent = selected_node_;
        }
        checkpoint_scene(scene);
        const SceneNodeId model = scene.create_node(
            std::string(name), SceneNodeKind::model, parent);
        if (!model) {
            asset_error_ =
                "Unable to create the model node; "
                "the scene capacity may be exhausted.";
            return false;
        }
        const auto node = scene.node(model);
        if (!node) {
            static_cast<void>(scene.destroy_subtree(model));
            asset_error_ =
                "The new model node became invalid.";
            return false;
        }
        node->get().asset_path = std::string(relative_path);
        // Store the source-space centering explicitly in the scene so
        // reloading the document preserves the exact placement.
        node->get().transform = recommended_transform;
        scene.mark_content_changed();
        selected_node_ = model;
        name_buffer_node_ = {};
        asset_error_.clear();
        scene_file_status_ =
            "Added model to scene: " +
            std::string(relative_path);
        return true;
    } catch (const std::exception&) {
        asset_error_ =
            "Unable to add the model because its path is invalid.";
        return false;
    }
}

void EditorUi::process_import_pipeline() {
    if (import_worker_ == nullptr || import_ui_ == nullptr) {
        return;
    }
    std::vector<AssetImportUiState::Completion> completed;
    if (!import_worker_->drain(completed)) {
        return;
    }

    import_ui_->completions = std::move(completed);
    import_ui_->results_window_open = true;
    std::size_t succeeded = 0U;
    for (const auto& completion : import_ui_->completions) {
        if (completion.result) {
            ++succeeded;
        } else if (completion.error) {
            std::cerr << "Asset import failed: '" << path_to_utf8(completion.source)
                      << "': " << completion.error->message
                      << " [" << path_to_utf8(completion.error->path) << "]\n";
        }
    }
    import_ui_->status =
        "Import completed: " + std::to_string(succeeded) +
        " succeeded, " +
        std::to_string(
            import_ui_->completions.size() - succeeded) +
        " failed.";
    if (asset_browser_.refresh(asset_error_)) {
        selected_asset_.reset();
    }
}

void EditorUi::queue_import_sources(
    std::vector<std::filesystem::path> sources) {
    if (import_ui_ == nullptr) {
        return;
    }
    if (play_state_ != EditorPlayState::edit ||
        !asset_import_enabled_) {
        asset_error_ =
            !asset_import_enabled_
            ? "Packaged assets are browse-only; import requires "
              "a project checkout."
            : "Asset import is available only in Edit mode.";
        return;
    }
    if (import_worker_ == nullptr || import_worker_->failed()) {
        asset_error_ =
            "The asset import worker is unavailable; restart the editor.";
        return;
    }
    if (sources.size() > maximum_import_sources) {
        sources.resize(maximum_import_sources);
        import_ui_->status =
            "The import batch was truncated to 128 source files.";
    } else {
        import_ui_->status.clear();
    }
    import_ui_->sources = std::move(sources);
    import_ui_->destination_directory =
        asset_browser_.current_directory();
    import_ui_->source_path.fill('\0');
    import_ui_->import_popup_requested = true;
}

bool EditorUi::import_activity_pending() const noexcept {
    return (import_worker_ != nullptr && import_worker_->busy()) ||
        (import_ui_ != nullptr &&
         (import_ui_->import_popup_requested ||
          import_ui_->import_popup_open));
}

void EditorUi::build_import_window() {
    if (import_ui_ == nullptr || import_worker_ == nullptr) {
        return;
    }
    if (import_ui_->import_popup_requested) {
        ImGui::OpenPopup("Import Assets");
        import_ui_->import_popup_requested = false;
    }

    import_ui_->import_popup_open = ImGui::BeginPopupModal(
        "Import Assets",
        nullptr,
        ImGuiWindowFlags_AlwaysAutoResize);
    if (!import_ui_->import_popup_open) {
        return;
    }

    ui_kit_.panel_header(
        EditorIcon::import_asset,
        "IMPORT ASSETS",
        "FBX, OBJ, glTF/GLB, DAE, PLY, STL, 3DS, images and audio (.wav/.ogg/.flac)");
    ui_kit_.badge(
        path_to_utf8(
            import_ui_->destination_directory).c_str(),
        EditorTone::neutral);
    ImGui::SetNextItemWidth(620.0F);
    static_cast<void>(ImGui::InputTextWithHint(
        "##SourcePath",
        "Absolute source path...",
        import_ui_->source_path.data(),
        import_ui_->source_path.size()));
    ImGui::SameLine();
    if (ui_kit_.icon_button(
            "AddImportSource",
            EditorIcon::add,
            "Add source path")) {
        const std::string_view source_text(
            import_ui_->source_path.data());
        if (source_text.empty()) {
            import_ui_->status = "Enter a source file path.";
        } else if (import_ui_->sources.size() >=
                   maximum_import_sources) {
            import_ui_->status =
                "An import batch is limited to 128 source files.";
        } else {
            const std::filesystem::path source =
                path_from_utf8(source_text);
            const bool duplicate = std::ranges::any_of(
                import_ui_->sources,
                [&source](const std::filesystem::path& existing) {
                    return existing == source;
                });
            if (!duplicate) {
                import_ui_->sources.push_back(source);
            }
            import_ui_->source_path.fill('\0');
            import_ui_->status.clear();
        }
    }
    ImGui::TextDisabled(
        "Paste a path here, or drop files directly into the Assets list.");

    ImGui::BeginChild(
        "ImportSourceList",
        ImVec2(720.0F, 220.0F),
        ImGuiChildFlags_Borders);
    std::optional<std::size_t> remove_source;
    for (std::size_t index = 0U;
         index < import_ui_->sources.size();
         ++index) {
        const std::filesystem::path& source =
            import_ui_->sources[index];
        ImGui::PushID(static_cast<int>(index));
        const bool supported = asset_source_is_supported(source);
        ui_kit_.badge(
            supported ? "READY" : "UNSUPPORTED",
            supported
                ? EditorTone::success
                : EditorTone::danger);
        ImGui::SameLine();
        ImGui::TextWrapped(
            "%s", path_to_utf8(source).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            remove_source = index;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (remove_source) {
        import_ui_->sources.erase(
            import_ui_->sources.begin() +
            static_cast<std::ptrdiff_t>(*remove_source));
    }

    const bool all_sources_supported =
        !import_ui_->sources.empty() &&
        std::ranges::all_of(
            import_ui_->sources,
            [](const std::filesystem::path& source) {
                return asset_source_is_supported(source);
            });
    const bool can_submit =
        play_state_ == EditorPlayState::edit &&
        asset_import_enabled_ &&
        !import_worker_->failed() &&
        !import_worker_->busy() &&
        all_sources_supported;
    if (ui_kit_.action_button(
            "SubmitImport",
            EditorIcon::import_asset,
            "Import",
            EditorButtonStyle::primary,
            can_submit)) {
        AssetImportWorker::Batch batch{
            .sources = import_ui_->sources,
            .options =
                ImportAssetOptions{
                    .asset_root = asset_browser_.root(),
                    .destination_directory =
                        import_ui_->destination_directory,
                },
        };
        if (import_worker_->submit(std::move(batch))) {
            import_ui_->status =
                "Import queued. Source parsing runs outside "
                "the render thread.";
            import_ui_->sources.clear();
            import_ui_->import_popup_open = false;
            ImGui::CloseCurrentPopup();
        } else {
            import_ui_->status =
                import_worker_->failed()
                ? "The asset import worker stopped after an internal "
                  "error. Restart the editor before importing again."
                : "The bounded import queue is full.";
        }
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "CancelImport",
            EditorIcon::stop,
            "Cancel",
            EditorButtonStyle::quiet)) {
        import_ui_->sources.clear();
        import_ui_->status.clear();
        import_ui_->import_popup_open = false;
        ImGui::CloseCurrentPopup();
    }
    if (!import_ui_->status.empty()) {
        ui_kit_.status_text(
            import_ui_->status.c_str(),
            can_submit
                ? EditorTone::neutral
                : EditorTone::warning);
    }
    ImGui::EndPopup();
}

void EditorUi::build_import_results() {
    if (import_ui_ == nullptr ||
        !import_ui_->results_window_open) {
        return;
    }
    ImGui::SetNextWindowSize(
        ImVec2(680.0F, 420.0F), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(
            "Import Results",
            &import_ui_->results_window_open)) {
        ui_kit_.panel_header(
            EditorIcon::import_asset,
            "IMPORT RESULTS",
            "Cooked assets and diagnostics");
        ui_kit_.status_text(
            import_ui_->status.c_str(),
            EditorTone::info);
        for (std::size_t completion_index = 0U;
             completion_index < import_ui_->completions.size();
             ++completion_index) {
            const auto& completion =
                import_ui_->completions[completion_index];
            ImGui::PushID(
                static_cast<int>(completion_index));
            ImGui::SeparatorText(
                path_to_utf8(
                    completion.source.filename()).c_str());
            ImGui::TextWrapped(
                "Source: %s",
                path_to_utf8(completion.source).c_str());
            if (completion.error) {
                ImGui::TextColored(
                    EditorUiKit::color(EditorTone::danger),
                    "Failed: %s",
                    completion.error->message.c_str());
                if (!completion.error->path.empty()) {
                    ImGui::TextWrapped(
                        "Path: %s",
                        path_to_utf8(
                            completion.error->path).c_str());
                }
                ImGui::PopID();
                continue;
            }
            if (!completion.result) {
                ImGui::TextDisabled("No import result.");
                ImGui::PopID();
                continue;
            }
            ImGui::TextWrapped(
                "Primary: %s",
                path_to_utf8(
                    completion.result->primary_asset).c_str());
            for (std::size_t file_index = 0U;
                 file_index < completion.result->files.size();
                 ++file_index) {
                const ImportedAssetFile& file =
                    completion.result->files[file_index];
                ImGui::PushID(static_cast<int>(file_index));
                ImGui::BulletText(
                    "%s: %s",
                    imported_asset_kind_name(file.kind),
                    path_to_utf8(
                        file.relative_path).c_str());
                if (file.kind == ImportedAssetKind::material) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Open")) {
                        open_material_editor(file.relative_path);
                    }
                }
                ImGui::PopID();
            }
            for (const std::string& warning :
                 completion.result->warnings) {
                ImGui::TextColored(
                    EditorUiKit::color(EditorTone::warning),
                    "Warning: %s",
                    warning.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void EditorUi::open_material_editor(
    const std::filesystem::path& relative_path) {
    if (material_editor_ == nullptr) {
        return;
    }
    if (material_editor_->loaded &&
        material_editor_->relative_path == relative_path) {
        material_editor_->open = true;
        return;
    }
    if (material_editor_->loaded &&
        material_editor_->dirty &&
        material_editor_->relative_path != relative_path) {
        material_editor_->open = true;
        material_editor_->error =
            "Save or revert the current material before opening another.";
        return;
    }
    std::string error;
    if (!load_material_document(
            asset_browser_.root(),
            relative_path,
            *material_editor_,
            error)) {
        material_editor_->loaded = false;
        material_editor_->error =
            "Unable to open material: " + error;
    }
    material_editor_->open = true;
}

bool EditorUi::inspect_material(
    const std::filesystem::path& relative_path,
    MaterialEditorSnapshot& snapshot,
    std::string& error) const {
    MaterialEditorUiState state;
    if (!load_material_document(
            asset_browser_.root(), relative_path, state, error)) {
        return false;
    }
    snapshot = make_material_snapshot(state);
    return true;
}

bool EditorUi::set_material_tessellation(
    const std::filesystem::path& relative_path,
    MaterialTessellation tessellation,
    MaterialEditorSnapshot& snapshot,
    std::string& error) {
    if (play_state_ != EditorPlayState::edit) {
        error = "Material authoring is locked during Play.";
        return false;
    }
    if (material_editor_ != nullptr && material_editor_->dirty &&
        material_editor_->relative_path == relative_path.lexically_normal()) {
        error = "The material has unsaved edits in Material Editor.";
        return false;
    }
    if (tessellation.enabled_flag > 1U ||
        tessellation.up_facing_only > 1U ||
        !std::isfinite(tessellation.displacement_scale) ||
        tessellation.displacement_scale < 0.0F ||
        tessellation.displacement_scale > 10'000.0F ||
        !std::isfinite(tessellation.displacement_center) ||
        tessellation.displacement_center < 0.0F ||
        tessellation.displacement_center > 1.0F ||
        !std::isfinite(tessellation.target_edge_pixels) ||
        tessellation.target_edge_pixels < 1.0F ||
        tessellation.target_edge_pixels > 1024.0F ||
        !std::isfinite(tessellation.maximum_factor) ||
        tessellation.maximum_factor < 1.0F ||
        tessellation.maximum_factor > 64.0F ||
        !std::isfinite(tessellation.normal_threshold) ||
        tessellation.normal_threshold < -1.0F ||
        tessellation.normal_threshold > 1.0F ||
        !std::isfinite(tessellation.uv_edge_fade) ||
        tessellation.uv_edge_fade < 0.0F ||
        tessellation.uv_edge_fade > 0.5F) {
        error = "Material tessellation settings are outside supported ranges.";
        return false;
    }

    MaterialEditorUiState state;
    if (!load_material_document(
            asset_browser_.root(), relative_path, state, error)) {
        return false;
    }
    if (!state.has_displacement_texture) {
        error = "A displacement texture is required before tessellation can be configured.";
        return false;
    }
    state.tessellation = tessellation;
    state.dirty = true;
    if (!save_material_document(state, asset_browser_.root(), error)) {
        return false;
    }
    snapshot = make_material_snapshot(state);
    saved_material_update_ = make_saved_material_update(state);
    if (material_editor_ != nullptr && material_editor_->loaded &&
        material_editor_->relative_path == state.relative_path) {
        const bool was_open = material_editor_->open;
        *material_editor_ = std::move(state);
        material_editor_->open = was_open;
    }
    return true;
}

void EditorUi::build_material_editor(Renderer& renderer) {
    if (material_editor_ == nullptr ||
        !material_editor_->open) {
        return;
    }
    ImGui::SetNextWindowSize(
        ImVec2(430.0F, 520.0F), ImGuiCond_FirstUseEver);
    bool keep_open = material_editor_->open;
    if (ImGui::Begin("Material Editor", &keep_open)) {
        ui_kit_.panel_header(
            EditorIcon::material,
            "MATERIAL EDITOR",
            "PBR textures and adaptive tessellation");
        if (!material_editor_->loaded) {
            ui_kit_.empty_state(
                EditorIcon::material,
                "No material loaded",
                "Open a material from Asset Library");
            if (!material_editor_->error.empty()) {
                ui_kit_.status_text(
                    material_editor_->error.c_str(),
                    EditorTone::danger);
            }
        } else {
            ui_kit_.badge(
                material_editor_->dirty ? "UNSAVED" : "SAVED",
                material_editor_->dirty
                    ? EditorTone::warning
                    : EditorTone::success);
            ImGui::SameLine();
            ImGui::Text(
                "%s",
                path_to_utf8(
                    material_editor_->relative_path).c_str());
            const bool editable =
                play_state_ == EditorPlayState::edit;
            ImGui::BeginDisabled(!editable);
            bool changed = false;
            if (ui_kit_.begin_property_grid(
                    "MaterialEditorProperties")) {
                ui_kit_.property_row("Name");
                changed |= ImGui::InputText(
                    "##MaterialName",
                    material_editor_->name.data(),
                    material_editor_->name.size());
                ui_kit_.property_row("Base color");
                changed |= ImGui::ColorEdit4(
                    "##MaterialBaseColor",
                    &material_editor_->material.base_color.x);
                ui_kit_.property_row("Metallic");
                changed |= ImGui::SliderFloat(
                    "##MaterialMetallic",
                    &material_editor_->material.metallic,
                    0.0F,
                    1.0F,
                    "%.2f");
                ui_kit_.property_row("Roughness");
                changed |= ImGui::SliderFloat(
                    "##MaterialRoughness",
                    &material_editor_->material.roughness,
                    minimum_pbr_roughness,
                    1.0F,
                    "%.2f");
                ui_kit_.property_row("Occlusion");
                changed |= ImGui::SliderFloat(
                    "##MaterialOcclusion",
                    &material_editor_->material.ambient_occlusion,
                    0.0F,
                    1.0F,
                    "%.2f");
                ui_kit_.property_row("IOR");
                changed |= ImGui::DragFloat(
                    "##MaterialIor", &material_editor_->material.ior, 0.01F,
                    1.0F, std::numeric_limits<float>::max(), "%.6g",
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat);
                ui_kit_.property_row("Shading model");
                ImGui::BeginDisabled(material_editor_->material.model == MaterialModel::gpu_grass);
                if (ImGui::BeginCombo(
                        "##MaterialModel", to_token(material_editor_->material.model).data())) {
                    for (const auto model : {
                             MaterialModel::standard,
                             MaterialModel::unlit,
                             MaterialModel::subsurface}) {
                        if (ImGui::Selectable(
                                to_token(model).data(), material_editor_->material.model == model)) {
                            changed |= material_editor_->material.model != model;
                            material_editor_->material.model = model;
                            if (model == MaterialModel::subsurface) {
                                if (material_editor_->material.subsurface_profile ==
                                    SubsurfaceProfile::none) {
                                    const bool thin_foliage =
                                        (material_editor_->document.contains("double_sided") &&
                                         material_editor_->document["double_sided"].is_boolean() &&
                                         material_editor_->document["double_sided"].get<bool>()) ||
                                        (material_editor_->document.contains("alpha_mode") &&
                                         material_editor_->document["alpha_mode"].is_string() &&
                                         material_editor_->document["alpha_mode"] == "mask");
                                    material_editor_->material.subsurface_profile = thin_foliage
                                        ? SubsurfaceProfile::foliage
                                        : SubsurfaceProfile::medium;
                                }
                                if (material_editor_->material.subsurface_strength <= 0.0F) {
                                    material_editor_->material.subsurface_strength = 0.70F;
                                }
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (material_editor_->material.model == MaterialModel::subsurface) {
                    ui_kit_.property_row("SSS profile");
                    if (ImGui::BeginCombo(
                            "##MaterialSubsurfaceProfile",
                            to_token(material_editor_->material.subsurface_profile).data())) {
                        for (const auto profile : {
                                 SubsurfaceProfile::none,
                                 SubsurfaceProfile::narrow,
                                 SubsurfaceProfile::medium,
                                 SubsurfaceProfile::wide,
                                 SubsurfaceProfile::foliage}) {
                            if (ImGui::Selectable(
                                    to_token(profile).data(),
                                    material_editor_->material.subsurface_profile == profile)) {
                                changed |= material_editor_->material.subsurface_profile != profile;
                                material_editor_->material.subsurface_profile = profile;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ui_kit_.property_row("SSS strength");
                    changed |= ImGui::SliderFloat(
                        "##MaterialSubsurfaceStrength",
                        &material_editor_->material.subsurface_strength,
                        0.0F,
                        1.0F,
                        "%.2f");
                }
                ui_kit_.property_row("Alpha mode");
                const bool gpu_grass =
                    material_editor_->material.model == MaterialModel::gpu_grass;
                ImGui::BeginDisabled(gpu_grass);
                if (ImGui::BeginCombo(
                        "##MaterialAlphaMode",
                        to_token(material_editor_->alpha_mode).data())) {
                    for (const auto alpha_mode : {
                             MaterialAlphaMode::opaque,
                             MaterialAlphaMode::mask,
                             MaterialAlphaMode::blend}) {
                        if (ImGui::Selectable(
                                to_token(alpha_mode).data(),
                                material_editor_->alpha_mode == alpha_mode)) {
                            if (material_editor_->alpha_mode != alpha_mode) {
                                material_editor_->alpha_mode = alpha_mode;
                                if (alpha_mode == MaterialAlphaMode::blend) {
                                    material_editor_->double_sided = true;
                                } else {
                                    material_editor_->glass = false;
                                }
                                if (alpha_mode == MaterialAlphaMode::mask) {
                                    material_editor_->tessellation.enabled_flag = 0U;
                                }
                                changed = true;
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                if (material_editor_->alpha_mode == MaterialAlphaMode::mask) {
                    ui_kit_.property_row("Alpha cutoff");
                    changed |= ImGui::SliderFloat(
                        "##MaterialAlphaCutoff",
                        &material_editor_->alpha_cutoff,
                        0.0F,
                        1.0F,
                        "%.2f");
                }
                ui_kit_.property_row("Glass dielectric");
                if (ImGui::Checkbox(
                        "##MaterialGlass", &material_editor_->glass)) {
                    if (material_editor_->glass) {
                        material_editor_->alpha_mode = MaterialAlphaMode::blend;
                        material_editor_->double_sided = true;
                    }
                    changed = true;
                }
                ImGui::EndDisabled();
                ui_kit_.property_row("Double sided");
                changed |= ImGui::Checkbox(
                    "##MaterialDoubleSided", &material_editor_->double_sided);
                ui_kit_.property_row("AO in M/R texture");
                changed |= ImGui::Checkbox(
                    "##MaterialPackedAo", &material_editor_->packed_ao);
                ui_kit_.end_property_grid();
            }
            ui_kit_.section_header(
                EditorIcon::environment,
                "Tessellation");
            ui_kit_.badge(
                renderer.tessellation_supported()
                    ? "GPU SUPPORTED" : "GPU FALLBACK",
                renderer.tessellation_supported()
                    ? EditorTone::success : EditorTone::warning);
            if (!material_editor_->has_displacement_texture) {
                ImGui::SameLine();
                ui_kit_.badge("NO HEIGHT MAP", EditorTone::warning);
            }
            ImGui::BeginDisabled(
                !material_editor_->has_displacement_texture ||
                material_editor_->material.model == MaterialModel::gpu_grass ||
                material_editor_->alpha_mode == MaterialAlphaMode::mask);
            if (ui_kit_.begin_property_grid(
                    "MaterialTessellationProperties")) {
                bool enabled =
                    material_editor_->tessellation.enabled_flag != 0U;
                ui_kit_.property_row("Enabled");
                if (ImGui::Checkbox("##TessellationEnabled", &enabled)) {
                    material_editor_->tessellation.enabled_flag =
                        enabled ? 1U : 0U;
                    changed = true;
                }
                ui_kit_.property_row("Displacement (m)");
                changed |= ImGui::SliderFloat(
                    "##TessellationScale",
                    &material_editor_->tessellation.displacement_scale,
                    0.0F, 0.5F, "%.3f m");
                ui_kit_.property_row("Height midpoint");
                changed |= ImGui::SliderFloat(
                    "##TessellationCenter",
                    &material_editor_->tessellation.displacement_center,
                    0.0F, 1.0F, "%.3f");
                ui_kit_.property_row("Target edge");
                changed |= ImGui::SliderFloat(
                    "##TessellationEdgePixels",
                    &material_editor_->tessellation.target_edge_pixels,
                    2.0F, 128.0F, "%.1f px");
                ui_kit_.property_row("Maximum factor");
                changed |= ImGui::SliderFloat(
                    "##TessellationMaximumFactor",
                    &material_editor_->tessellation.maximum_factor,
                    1.0F, 64.0F, "%.0f");
                bool up_facing_only =
                    material_editor_->tessellation.up_facing_only != 0U;
                ui_kit_.property_row("Up-facing only");
                if (ImGui::Checkbox(
                        "##TessellationUpFacingOnly",
                        &up_facing_only)) {
                    material_editor_->tessellation.up_facing_only =
                        up_facing_only ? 1U : 0U;
                    changed = true;
                }
                ui_kit_.property_row("Normal threshold");
                changed |= ImGui::SliderFloat(
                    "##TessellationNormalThreshold",
                    &material_editor_->tessellation.normal_threshold,
                    -1.0F, 1.0F, "%.2f");
                ui_kit_.property_row("UV edge fade");
                changed |= ImGui::SliderFloat(
                    "##TessellationUvEdgeFade",
                    &material_editor_->tessellation.uv_edge_fade,
                    0.0F, 0.25F, "%.3f");
                ui_kit_.end_property_grid();
            }
            ImGui::EndDisabled();
            ImGui::TextWrapped(
                "Displacement follows the height map and is faded from "
                "vertical faces when Up-facing only is enabled. UV edge fade "
                "fades the outer UV bounds of the whole material surface, "
                "including tiled textures. Lower Target edge gives finer geometry; "
                "Height midpoint shifts the surface without adding relief.");
            if (changed) {
                material_editor_->material =
                    sanitize_pbr_material(
                        material_editor_->material);
                material_editor_->dirty = true;
                material_editor_->status =
                    "Unsaved material changes.";
                saved_material_update_ = make_saved_material_update(*material_editor_);
            }

            if (ui_kit_.action_button(
                    "SaveMaterial",
                    EditorIcon::save,
                    "Save",
                    EditorButtonStyle::primary)) {
                std::string error;
                if (!save_material_document(
                        *material_editor_,
                        asset_browser_.root(),
                        error)) {
                    material_editor_->error =
                        "Save failed: " + error;
                } else {
                    saved_material_update_ =
                        make_saved_material_update(*material_editor_);
                }
            }
            ImGui::SameLine();
            if (ui_kit_.action_button(
                    "RevertMaterial",
                    EditorIcon::revert,
                    "Revert",
                    EditorButtonStyle::secondary)) {
                const std::filesystem::path relative =
                    material_editor_->relative_path;
                std::string error;
                if (!load_material_document(
                        asset_browser_.root(),
                        relative,
                        *material_editor_,
                        error)) {
                    material_editor_->error =
                        "Revert failed: " + error;
                } else {
                    saved_material_update_ =
                        make_saved_material_update(*material_editor_);
                }
            }
            ImGui::EndDisabled();
            if (!editable) {
                ui_kit_.badge(
                    "LOCKED DURING PLAY",
                    EditorTone::warning);
            }

            ui_kit_.section_header(
                EditorIcon::environment,
                "Texture References");
            const nlohmann::json& textures =
                material_editor_->document.at("textures");
            if (textures.empty()) {
                ui_kit_.empty_state(
                    EditorIcon::environment,
                    "No textures",
                    "This material uses factors only");
            } else {
                for (const auto& [semantic, reference] :
                     textures.items()) {
                    if (ui_kit_.begin_property_grid(
                            semantic.c_str())) {
                        ui_kit_.property_row(
                            semantic.c_str());
                        ImGui::TextWrapped(
                            "%s",
                            reference.get_ref<
                                const std::string&>().c_str());
                        ui_kit_.end_property_grid();
                    }
                }
            }
            if (!material_editor_->error.empty()) {
                ui_kit_.status_text(
                    material_editor_->error.c_str(),
                    EditorTone::danger);
            } else if (!material_editor_->status.empty()) {
                ui_kit_.status_text(
                    material_editor_->status.c_str(),
                    material_editor_->dirty
                        ? EditorTone::warning
                        : EditorTone::success);
            }
        }
    }
    ImGui::End();

    if (!keep_open) {
        if (material_editor_->dirty) {
            material_editor_->open = true;
            material_editor_->close_confirmation_requested = true;
        } else {
            material_editor_->open = false;
        }
    }
    if (material_editor_->close_confirmation_requested) {
        ImGui::OpenPopup("Discard material changes?");
        material_editor_->close_confirmation_requested = false;
    }
    if (ImGui::BeginPopupModal(
            "Discard material changes?",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "The material has unsaved changes.");
        if (ImGui::Button("Save and close")) {
            std::string error;
            if (save_material_document(
                    *material_editor_,
                    asset_browser_.root(),
                    error)) {
                saved_material_update_ =
                    make_saved_material_update(*material_editor_);
                material_editor_->open = false;
                ImGui::CloseCurrentPopup();
            } else {
                material_editor_->error =
                    "Save failed: " + error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            const std::filesystem::path relative =
                material_editor_->relative_path;
            std::string error;
            if (load_material_document(
                    asset_browser_.root(),
                    relative,
                    *material_editor_,
                    error)) {
                material_editor_->open = false;
                ImGui::CloseCurrentPopup();
            } else {
                material_editor_->error =
                    "Discard failed: " + error;
                material_editor_->open = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            material_editor_->open = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorUi::build_audio_input_panel() {
    if (audio_input_ui_ == nullptr || !audio_input_ui_->open) {
        return;
    }
    ImGui::SetNextWindowSize(
        ImVec2(420.0F, 560.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio & Input", &audio_input_ui_->open)) {
        ImGui::End();
        return;
    }
    ui_kit_.panel_header(
        EditorIcon::play,
        "AUDIO & INPUT",
        "5-bus mixer, brickwall limiter, and rebindable input actions");

    ui_kit_.section_header(EditorIcon::play, "Audio Mixer");
    if (audio_engine_ != nullptr && audio_engine_->initialized()) {
        const AudioEngineTelemetry telem = audio_engine_->telemetry();
        ui_kit_.badge(
            telem.hardware_output_active ? "WASAPI 48 kHz" : "HEADLESS MIXER",
            telem.hardware_output_active
                ? EditorTone::success
                : EditorTone::warning);
        ImGui::SameLine();
        char voice_badge[64]{};
        std::snprintf(
            voice_badge,
            sizeof(voice_badge),
            "VOICES %u/%zu  STREAMS %u/%zu",
            telem.active_voice_count,
            AudioEngine::max_voices,
            telem.active_stream_count,
            AudioEngine::max_stream_slots);
        ui_kit_.badge(voice_badge, EditorTone::info);
        ImGui::SameLine();
        char limiter_badge[48]{};
        if (telem.master_limiter_gain < 0.995F) {
            const float gr_db =
                20.0F * std::log10(std::max(telem.master_limiter_gain, 1.0e-4F));
            std::snprintf(
                limiter_badge,
                sizeof(limiter_badge),
                "LIMITER %.1f dB",
                gr_db);
            ui_kit_.badge(limiter_badge, EditorTone::warning);
        } else {
            ui_kit_.badge("LIMITER IDLE", EditorTone::neutral);
        }

        const double resident_mb =
            static_cast<double>(telem.resident_pcm_bytes) /
            (1024.0 * 1024.0);
        const double peak_mb =
            static_cast<double>(telem.resident_pcm_high_water_bytes) /
            (1024.0 * 1024.0);
        ImGui::TextDisabled(
            "Clip cache: %zu clips (%.2f MB / %.2f MB high-water)",
            telem.resident_clip_count,
            resident_mb,
            peak_mb);

        if (audio_engine_->is_preview_playing()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop Preview")) {
                audio_engine_->stop_preview();
            }
        }

        constexpr std::array<std::pair<AudioBusId, const char*>, audio_bus_count>
            bus_labels{{
                {AudioBusId::master, "Master"},
                {AudioBusId::music, "Music"},
                {AudioBusId::sfx, "SFX"},
                {AudioBusId::voice, "Voice"},
                {AudioBusId::ui, "UI"},
            }};
        if (ImGui::BeginTable(
                "AudioBusMixerTable",
                4,
                ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(
                "Bus", ImGuiTableColumnFlags_WidthFixed, 64.0F);
            ImGui::TableSetupColumn(
                "Mute", ImGuiTableColumnFlags_WidthFixed, 42.0F);
            ImGui::TableSetupColumn(
                "Gain", ImGuiTableColumnFlags_WidthStretch, 1.2F);
            ImGui::TableSetupColumn(
                "Peak L/R", ImGuiTableColumnFlags_WidthStretch, 0.9F);
            ImGui::TableHeadersRow();

            for (std::size_t idx = 0U; idx < bus_labels.size(); ++idx) {
                const auto [bus_id, label] = bus_labels[idx];
                const AudioBusSnapshot& snap = telem.buses[idx];
                ImGui::PushID(static_cast<int>(idx));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);

                ImGui::TableSetColumnIndex(1);
                bool muted = snap.muted;
                if (ImGui::Checkbox("##Mute", &muted)) {
                    audio_engine_->set_bus_muted(bus_id, muted);
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::SetNextItemWidth(-FLT_MIN);
                float volume = snap.volume;
                if (ImGui::SliderFloat(
                        "##Gain",
                        &volume,
                        0.0F,
                        2.0F,
                        "%.2f",
                        ImGuiSliderFlags_AlwaysClamp)) {
                    audio_engine_->set_bus_volume(bus_id, volume);
                }

                ImGui::TableSetColumnIndex(3);
                const float peak =
                    std::clamp(std::max(snap.peak_left, snap.peak_right), 0.0F, 1.0F);
                char peak_label[32]{};
                std::snprintf(
                    peak_label,
                    sizeof(peak_label),
                    "%.2f (%u)",
                    std::max(snap.peak_left, snap.peak_right),
                    snap.active_voices);
                ImGui::ProgressBar(peak, ImVec2(-FLT_MIN, 0.0F), peak_label);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else {
        ui_kit_.status_text(
            "Audio engine is unavailable in this session.",
            EditorTone::warning);
    }

    ImGui::Spacing();
    ui_kit_.section_header(EditorIcon::camera, "Input Actions & Gamepad");
    if (input_actions_ != nullptr) {
        if (audio_input_ui_->capturing_action.has_value() &&
            audio_input_ui_->capture_mode ==
                AudioInputUiState::CaptureMode::gamepad_button) {
            const GamepadButton pressed_button =
                input_actions_->consume_last_pressed_gamepad_button();
            if (pressed_button != GamepadButton::none) {
                const InputAction action = *audio_input_ui_->capturing_action;
                ActionBinding edited = input_actions_->binding(action);
                edited.gamepad_button = pressed_button;
                input_actions_->set_binding(action, edited);
                audio_input_ui_->capturing_action.reset();
                audio_input_ui_->capture_mode =
                    AudioInputUiState::CaptureMode::none;
                audio_input_ui_->status =
                    "Bound " + std::string(input_action_display_name(action)) +
                    " to gamepad " + std::string(to_token(pressed_button)) + ".";
            }
        } else {
            static_cast<void>(
                input_actions_->consume_last_pressed_gamepad_button());
        }

        const GamepadState& gp = input_actions_->gamepad_state();
        ui_kit_.badge(
            gp.connected ? "GAMEPAD CONNECTED" : "NO CONTROLLER",
            gp.connected ? EditorTone::success : EditorTone::neutral);
        ImGui::SameLine();
        ImGui::TextDisabled(
            "LS (%.2f, %.2f)  RS (%.2f, %.2f)",
            gp.left_stick.x,
            gp.left_stick.y,
            gp.right_stick.x,
            gp.right_stick.y);

        if (ui_kit_.begin_property_grid("InputGamepadProperties")) {
            ui_kit_.property_row("Context");
            const InputContext current_ctx = input_actions_->context();
            if (ImGui::BeginCombo(
                    "##InputContextCombo", to_token(current_ctx).data())) {
                for (const InputContext candidate : {
                         InputContext::gameplay,
                         InputContext::secondary,
                         InputContext::ui,
                         InputContext::cutscene}) {
                    if (ImGui::Selectable(
                            to_token(candidate).data(),
                            current_ctx == candidate)) {
                        input_actions_->set_context(candidate);
                    }
                }
                ImGui::EndCombo();
            }

            GamepadConfig gp_cfg = input_actions_->gamepad_config();
            bool cfg_changed = false;
            ui_kit_.property_row("Deadzone inner");
            cfg_changed |= ImGui::SliderFloat(
                "##GpInnerDz",
                &gp_cfg.stick_inner_deadzone,
                0.0F,
                0.6F,
                "%.2f",
                ImGuiSliderFlags_AlwaysClamp);
            ui_kit_.property_row("Deadzone outer");
            cfg_changed |= ImGui::SliderFloat(
                "##GpOuterDz",
                &gp_cfg.stick_outer_deadzone,
                std::max(0.65F, gp_cfg.stick_inner_deadzone + 0.05F),
                1.0F,
                "%.2f",
                ImGuiSliderFlags_AlwaysClamp);
            ui_kit_.property_row("Look speed");
            cfg_changed |= ImGui::SliderFloat(
                "##GpLookSpeed",
                &gp_cfg.look_speed_pixels_per_sec,
                120.0F,
                2400.0F,
                "%.0f px/s",
                ImGuiSliderFlags_AlwaysClamp);
            ui_kit_.property_row("Invert look Y");
            cfg_changed |= ImGui::Checkbox(
                "##GpInvertY", &gp_cfg.invert_look_y);
            ui_kit_.property_row("Vibration");
            cfg_changed |= ImGui::Checkbox(
                "##GpVibration", &gp_cfg.vibration_enabled);
            if (cfg_changed) {
                input_actions_->set_gamepad_config(gp_cfg);
            }
            ui_kit_.property_row("Rumble test");
            if (ImGui::SmallButton("Pulse Rumble")) {
                input_actions_->trigger_rumble(0.65F, 0.85F, 0.35F);
            }
            ui_kit_.end_property_grid();
        }

        if (audio_input_ui_->capturing_action.has_value()) {
            const std::string prompt =
                "Listening for " +
                std::string(input_action_display_name(
                    *audio_input_ui_->capturing_action)) +
                " (Esc to cancel, Backspace/Del to clear)...";
            ui_kit_.status_text(prompt.c_str(), EditorTone::info);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) {
                audio_input_ui_->capturing_action.reset();
                audio_input_ui_->capture_mode =
                    AudioInputUiState::CaptureMode::none;
            }
        }

        std::array<BindingConflict, 16U> conflicts{};
        const std::size_t conflict_count =
            input_actions_->detect_conflicts(conflicts);
        if (conflict_count > 0U) {
            const BindingConflict& first = conflicts[0];
            const std::string shared_label =
                first.shared_key != InputKey::none
                    ? std::string(to_token(first.shared_key))
                    : std::string(to_token(first.shared_gamepad));
            const std::string warning_msg =
                "Binding conflict (" + std::to_string(conflict_count) + "): " +
                std::string(input_action_display_name(first.first)) + " & " +
                std::string(input_action_display_name(first.second)) +
                " share '" + shared_label + "' in " +
                std::string(to_token(first.shared_context)) + ".";
            ui_kit_.status_text(warning_msg.c_str(), EditorTone::warning);
        }

        if (!audio_input_ui_->error.empty()) {
            ui_kit_.status_text(
                audio_input_ui_->error.c_str(), EditorTone::danger);
        } else if (!audio_input_ui_->status.empty()) {
            ui_kit_.status_text(
                audio_input_ui_->status.c_str(), EditorTone::neutral);
        }

        if (ImGui::Button("Save Bindings")) {
            std::string save_err;
            const std::filesystem::path bindings_path =
                asset_browser_.root() / "config" / "input_bindings.json";
            if (save_input_bindings_file(
                    bindings_path, *input_actions_, save_err)) {
                audio_input_ui_->error.clear();
                audio_input_ui_->status =
                    "Saved config/input_bindings.json.";
            } else {
                audio_input_ui_->error = std::move(save_err);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Defaults")) {
            input_actions_->reset_bindings_to_default();
            audio_input_ui_->capturing_action.reset();
            audio_input_ui_->capture_mode =
                AudioInputUiState::CaptureMode::none;
            audio_input_ui_->error.clear();
            audio_input_ui_->status = "Restored default bindings.";
        }

        if (ImGui::BeginTable(
                "InputBindingsTable",
                4,
                ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.2F);
            ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch, 0.9F);
            ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthStretch, 0.9F);
            ImGui::TableSetupColumn("Gamepad", ImGuiTableColumnFlags_WidthStretch, 0.95F);
            ImGui::TableHeadersRow();

            for (std::size_t idx = 0U; idx < input_action_count; ++idx) {
                const auto action = static_cast<InputAction>(idx);
                const ActionBinding bind = input_actions_->binding(action);
                ImGui::PushID(static_cast<int>(idx));
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(input_action_display_name(action).data());

                const auto rebind_button =
                    [&](const char* const id_suffix,
                        const std::string_view token,
                        const AudioInputUiState::CaptureMode mode,
                        const char* const listening_text) {
                        const bool is_listening =
                            audio_input_ui_->capturing_action == action &&
                            audio_input_ui_->capture_mode == mode;
                        std::string btn_label = is_listening
                            ? std::string(listening_text)
                            : std::string(token);
                        btn_label += id_suffix;
                        if (ImGui::Button(
                                btn_label.c_str(), ImVec2(-FLT_MIN, 0.0F))) {
                            audio_input_ui_->capturing_action = action;
                            audio_input_ui_->capture_mode = mode;
                            static_cast<void>(
                                input_actions_
                                    ->consume_last_pressed_gamepad_button());
                        }
                    };

                ImGui::TableSetColumnIndex(1);
                rebind_button(
                    "##PrimaryBind",
                    to_token(bind.primary_key),
                    AudioInputUiState::CaptureMode::primary_key,
                    "[Key...]");

                ImGui::TableSetColumnIndex(2);
                rebind_button(
                    "##SecondaryBind",
                    to_token(bind.secondary_key),
                    AudioInputUiState::CaptureMode::secondary_key,
                    "[Key...]");

                ImGui::TableSetColumnIndex(3);
                rebind_button(
                    "##GamepadBind",
                    to_token(bind.gamepad_button),
                    AudioInputUiState::CaptureMode::gamepad_button,
                    "[Pad...]");

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void EditorUi::build_animation_gameplay_panel(
    SceneDocument& scene,
    Renderer& renderer) {
    if (!animation_gameplay_panel_open_) {
        return;
    }
    ImGui::SetNextWindowSize(
        ImVec2(420.0F, 560.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(
            "Animation & Gameplay", &animation_gameplay_panel_open_)) {
        ImGui::End();
        return;
    }
    ui_kit_.panel_header(
        EditorIcon::play,
        "ANIMATION & GAMEPLAY",
        "Skeletal runtime, gameplay behaviors, variables, and save/load");

    const bool play_active =
        play_state_ != EditorPlayState::edit &&
        gameplay_session_ != nullptr &&
        gameplay_session_->active();
    const AnimatorRuntimeSystem* const anim_rt = play_active
        ? &gameplay_session_->animator_runtime()
        : edit_animator_preview_;

    char anim_count_str[24]{};
    char gp_count_str[24]{};
    char skinned_count_str[24]{};
    char hp_str[32]{};
    std::snprintf(
        anim_count_str,
        sizeof(anim_count_str),
        "%u",
        scene.animators().size());
    std::snprintf(
        gp_count_str,
        sizeof(gp_count_str),
        "%u",
        scene.gameplays().size());
    std::snprintf(
        skinned_count_str,
        sizeof(skinned_count_str),
        "%u",
        renderer.skinning_active_node_count());
    if (play_active) {
        const GameplayWorld& gw = gameplay_session_->gameplay_world();
        std::snprintf(
            hp_str,
            sizeof(hp_str),
            "%.0f / %.0f",
            gw.player_health(),
            gw.player_max_health());
    } else {
        std::snprintf(hp_str, sizeof(hp_str), "Edit");
    }

    if (ImGui::BeginTable(
            "AnimGameplayMetrics",
            4,
            ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ui_kit_.metric_card(
            "AnimCountCard", "ANIMATORS", anim_count_str, EditorTone::info);
        ImGui::TableNextColumn();
        ui_kit_.metric_card(
            "SkinnedCard",
            "SKINNED",
            skinned_count_str,
            EditorTone::success);
        ImGui::TableNextColumn();
        ui_kit_.metric_card(
            "GameplayCountCard", "GAMEPLAY", gp_count_str, EditorTone::primary);
        ImGui::TableNextColumn();
        ui_kit_.metric_card(
            "PlayerHpCard", "PLAYER HP", hp_str, EditorTone::neutral);
        ImGui::EndTable();
    }

    ui_kit_.section_header(EditorIcon::save, "Save / Load Game State");
    ui_kit_.badge(
        play_active ? "F5 QUICKSAVE / F9 QUICKLOAD" : "START PLAY FOR SAVE/LOAD",
        play_active ? EditorTone::success : EditorTone::neutral);
    ImGui::SetNextItemWidth(-FLT_MIN);
    static_cast<void>(ImGui::InputText(
        "##SaveSlotPathInput",
        save_slot_path_buf_.data(),
        save_slot_path_buf_.size()));

    if (ui_kit_.action_button(
            "SaveGameStateButton",
            EditorIcon::save,
            "Save Game",
            EditorButtonStyle::secondary,
            play_active)) {
        pending_quick_save_path_ = std::string(save_slot_path_buf_.data());
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "LoadGameStateButton",
            EditorIcon::folder,
            "Load Game",
            EditorButtonStyle::secondary,
            play_active)) {
        pending_quick_load_path_ = std::string(save_slot_path_buf_.data());
    }
    if (!save_load_error_.empty()) {
        ui_kit_.status_text(save_load_error_.c_str(), EditorTone::danger);
    } else if (!save_load_status_.empty()) {
        ui_kit_.status_text(save_load_status_.c_str(), EditorTone::success);
    }

    ui_kit_.section_header(EditorIcon::cube, "Gameplay Runtime");
    if (play_active) {
        const GameplayWorld& gw = gameplay_session_->gameplay_world();
        char entity_badge[48]{};
        std::snprintf(
            entity_badge,
            sizeof(entity_badge),
            "%zu ENTITIES",
            gw.entity_count());
        ui_kit_.badge(entity_badge, EditorTone::info);

        const auto vars = gw.variables();
        if (!vars.empty()) {
            if (ImGui::BeginTable(
                    "GameplayVariablesTable",
                    2,
                    ImGuiTableFlags_BordersInnerH |
                        ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Variable", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 90.0F);
                ImGui::TableHeadersRow();
                for (const auto& [var_name, var_value] : vars) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(var_name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.2f", var_value);
                }
                ImGui::EndTable();
            }
        } else {
            ui_kit_.status_text(
                "No gameplay variables recorded yet.", EditorTone::neutral);
        }

        if (gw.event_log_count() > 0U) {
            ui_kit_.section_header(EditorIcon::play, "Gameplay Events");
            for (std::size_t i = 0U; i < gw.event_log_count(); ++i) {
                const std::string_view entry = gw.event_log_entry(i);
                ui_kit_.status_text(
                    std::string(entry).c_str(), EditorTone::neutral);
            }
        }
    } else {
        ui_kit_.status_text(
            "Enter Play (F5) to simulate patrol, triggers, doors, pickups, and Save/Load.",
            EditorTone::neutral);
    }

    ui_kit_.section_header(EditorIcon::play, "Animation Events");
    if (anim_rt != nullptr && anim_rt->event_log_count() > 0U) {
        for (std::size_t i = 0U; i < anim_rt->event_log_count(); ++i) {
            const std::string_view entry = anim_rt->event_log_entry(i);
            ui_kit_.status_text(
                std::string(entry).c_str(), EditorTone::neutral);
        }
    } else {
        ui_kit_.status_text(
            "No recent animation events.", EditorTone::neutral);
    }

    ImGui::End();
}

void EditorUi::build_ui_localization_panel(
    SceneDocument& /*scene*/,
    Renderer& /*renderer*/) {
    if (!ui_localization_panel_open_) {
        return;
    }
    ImGui::SetNextWindowSize(
        ImVec2(420.0F, 520.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UI & Localization", &ui_localization_panel_open_)) {
        ImGui::End();
        return;
    }
    ui_kit_.panel_header(
        EditorIcon::eye,
        "UI & LOCALIZATION",
        "HUD layout, resolution scaling, language switcher, and dialogues");

    if (gameplay_session_ == nullptr) {
        ui_kit_.status_text(
            "Gameplay session host is unavailable.", EditorTone::warning);
        ImGui::End();
        return;
    }

    ui_kit_.section_header(EditorIcon::eye, "Game UI Config");
    GameUiConfig cfg = gameplay_session_->game_ui().config();
    bool cfg_changed = false;
    if (ui_kit_.begin_property_grid("GameUiConfigGrid")) {
        ui_kit_.property_row("Start main menu");
        cfg_changed |= ImGui::Checkbox("##UiStartMainMenu", &cfg.start_in_main_menu);

        ui_kit_.property_row("Auto pause (Esc)");
        cfg_changed |= ImGui::Checkbox("##UiAutoPause", &cfg.auto_pause_enabled);

        ui_kit_.property_row("Default HUD");
        cfg_changed |= ImGui::Checkbox("##UiShowHud", &cfg.show_default_hud);

        ui_kit_.property_row("Crosshair");
        cfg_changed |= ImGui::Checkbox("##UiShowCrosshair", &cfg.show_crosshair);

        ui_kit_.property_row("Show FPS");
        cfg_changed |= ImGui::Checkbox("##UiShowFps", &cfg.show_fps);

        ui_kit_.property_row("Subtitles");
        cfg_changed |= ImGui::Checkbox("##UiShowSubtitles", &cfg.show_subtitles);

        ui_kit_.property_row("World markers");
        cfg_changed |= ImGui::Checkbox("##UiShowMarkers", &cfg.show_world_markers);

        ui_kit_.property_row("Marker dialogue");
        cfg_changed |= ImGui::Checkbox("##UiAutoDialogue", &cfg.auto_trigger_marker_dialogue);

        ui_kit_.property_row("Scale mode");
        constexpr std::array<const char*, 4U> kScaleLabels{
            "Fit (Reference 1080p)",
            "Match Height",
            "Match Width",
            "Fixed Pixels",
        };
        const std::size_t mode_idx =
            std::min<std::size_t>(static_cast<std::size_t>(cfg.scale_config.mode), 3U);
        if (ImGui::BeginCombo("##UiScaleMode", kScaleLabels[mode_idx])) {
            for (std::size_t i = 0U; i < kScaleLabels.size(); ++i) {
                if (ImGui::Selectable(kScaleLabels[i], mode_idx == i)) {
                    cfg.scale_config.mode = static_cast<UiScaleMode>(i);
                    cfg_changed = true;
                }
            }
            ImGui::EndCombo();
        }

        ui_kit_.property_row("UI scale");
        cfg_changed |= ImGui::DragFloat(
            "##UiUserScale", &cfg.scale_config.user_scale, 0.02F, 0.50F, 2.50F, "%.2fx");

        ui_kit_.property_row("Safe area %");
        float safe_pct = cfg.scale_config.safe_area_fraction * 100.0F;
        if (ImGui::DragFloat("##UiSafeArea", &safe_pct, 0.25F, 0.0F, 12.0F, "%.1f%%")) {
            cfg.scale_config.safe_area_fraction = safe_pct * 0.01F;
            cfg_changed = true;
        }

        auto edit_cfg_string = [](const char* const id, std::string& str) {
            std::array<char, 128U> buf{};
            const std::size_t n = std::min(str.size(), buf.size() - 1U);
            if (n > 0U) {
                std::memcpy(buf.data(), str.data(), n);
            }
            if (ImGui::InputText(id, buf.data(), buf.size())) {
                str.assign(buf.data());
                return true;
            }
            return false;
        };
        ui_kit_.property_row("Menu title");
        cfg_changed |= edit_cfg_string("##UiTitleText", cfg.title_text);

        ui_kit_.property_row("Menu subtitle");
        cfg_changed |= edit_cfg_string("##UiSubtitleText", cfg.subtitle_text);

        ui_kit_.end_property_grid();
    }
    if (cfg_changed) {
        gameplay_session_->configure_game_ui(cfg);
    }

    ui_kit_.section_header(EditorIcon::refresh, "Localization & Dialogues");
    LocalizationSystem& loc = gameplay_session_->localization();
    const auto locales = loc.available_locales();
    const std::string active_loc(loc.locale());
    std::size_t active_entry_count = 0U;
    if (ImGui::BeginCombo("Active locale", active_loc.c_str())) {
        for (const auto& info : locales) {
            const bool selected = (info.locale == active_loc);
            if (selected) {
                active_entry_count = info.entry_count;
            }
            std::string display = info.locale;
            if (!info.display_name.empty()) {
                display += " (";
                display += info.display_name;
                display += ")";
            }
            if (ImGui::Selectable(display.c_str(), selected)) {
                static_cast<void>(loc.set_locale(info.locale));
                ui_loc_status_ = "Switched locale to " + info.locale + ".";
            }
        }
        ImGui::EndCombo();
    } else {
        for (const auto& info : locales) {
            if (info.locale == active_loc) {
                active_entry_count = info.entry_count;
                break;
            }
        }
    }
    ImGui::Text(
        "Entries in '%s': %zu",
        active_loc.c_str(),
        active_entry_count);

    if (ImGui::Button("Reload localization & dialogues")) {
        loc.reset_to_builtins();
        std::string err;
        const std::filesystem::path loc_dir = asset_root() / "localization";
        const std::size_t loaded_loc =
            std::filesystem::exists(loc_dir) ? loc.load_directory(loc_dir, &err) : 0U;
        if (!active_loc.empty()) {
            static_cast<void>(loc.set_locale(active_loc));
        }
        gameplay_session_->game_ui().clear_dialogues();
        const std::filesystem::path dlg_dir = asset_root() / "dialogues";
        const std::size_t loaded_dlg = std::filesystem::exists(dlg_dir)
            ? gameplay_session_->game_ui().load_dialogue_directory(dlg_dir, &err)
            : 0U;
        ui_loc_status_ = "Loaded " + std::to_string(loaded_loc) +
                         " locale files, " + std::to_string(loaded_dlg) + " dialogue files.";
        if (!err.empty()) {
            ui_loc_status_ += " Note: " + err;
        }
    }

    ui_kit_.section_header(EditorIcon::play, "Play UI Test Actions");
    const bool playing_now = gameplay_session_->active();
    ImGui::BeginDisabled(!playing_now);
    ImGui::InputText("Speaker", ui_test_speaker_buf_.data(), ui_test_speaker_buf_.size());
    ImGui::InputText("Text / Key", ui_test_text_buf_.data(), ui_test_text_buf_.size());
    ImGui::InputText("Dialogue ID", ui_test_dialogue_id_buf_.data(), ui_test_dialogue_id_buf_.size());
    if (ImGui::Button("Push subtitle")) {
        gameplay_session_->game_ui().push_subtitle(
            ui_test_speaker_buf_.data(), ui_test_text_buf_.data(), 4.0F);
    }
    ImGui::SameLine();
    if (ImGui::Button("Show toast")) {
        gameplay_session_->game_ui().show_toast(ui_test_text_buf_.data(), 3.0F);
    }
    ImGui::SameLine();
    if (ImGui::Button("Start dialogue")) {
        if (ui_test_dialogue_id_buf_[0] != '\0') {
            static_cast<void>(gameplay_session_->game_ui().start_dialogue(
                ui_test_dialogue_id_buf_.data()));
        } else {
            static_cast<void>(gameplay_session_->game_ui().start_inline_dialogue(
                ui_test_speaker_buf_.data(), ui_test_text_buf_.data()));
        }
    }
    if (ImGui::Button("Screen: In-Game")) {
        gameplay_session_->game_ui().open_screen(GameUiScreen::in_game);
    }
    ImGui::SameLine();
    if (ImGui::Button("Screen: Pause")) {
        gameplay_session_->game_ui().open_screen(GameUiScreen::pause_menu);
    }
    ImGui::SameLine();
    if (ImGui::Button("Screen: Settings")) {
        gameplay_session_->game_ui().open_screen(GameUiScreen::settings);
    }
    ImGui::SameLine();
    if (ImGui::Button("Screen: Main Menu")) {
        gameplay_session_->game_ui().open_screen(GameUiScreen::main_menu);
    }
    ImGui::EndDisabled();

    if (!ui_loc_status_.empty()) {
        ui_kit_.status_text(ui_loc_status_.c_str(), EditorTone::neutral);
    }
    ImGui::End();
}

namespace {

[[nodiscard]] std::vector<SequencerKeyframe>& camera_cut_sub_curve(
    CameraCutClip& clip,
    const SequencerEditorUiState::CameraSubCurve sub) noexcept {
    switch (sub) {
    case SequencerEditorUiState::CameraSubCurve::fov: return clip.fov_keys;
    case SequencerEditorUiState::CameraSubCurve::roll: return clip.roll_keys;
    case SequencerEditorUiState::CameraSubCurve::exposure: return clip.exposure_keys;
    case SequencerEditorUiState::CameraSubCurve::focus_distance:
        return clip.focus_distance_keys;
    case SequencerEditorUiState::CameraSubCurve::rail_u: return clip.rail_u_keys;
    }
    return clip.fov_keys;
}

[[nodiscard]] const char* camera_sub_curve_label(
    const SequencerEditorUiState::CameraSubCurve sub) noexcept {
    switch (sub) {
    case SequencerEditorUiState::CameraSubCurve::fov: return "FOV (deg)";
    case SequencerEditorUiState::CameraSubCurve::roll: return "Roll (deg)";
    case SequencerEditorUiState::CameraSubCurve::exposure: return "Exposure (EV)";
    case SequencerEditorUiState::CameraSubCurve::focus_distance:
        return "Focus Dist (m)";
    case SequencerEditorUiState::CameraSubCurve::rail_u: return "Rail U (0..1)";
    }
    return "FOV (deg)";
}

[[nodiscard]] std::vector<SequencerKeyframe>* resolve_active_curve(
    SequencerEditorUiState& st,
    float& out_default_value) noexcept {
    out_default_value = 0.0F;
    if (st.selection_kind == SequencerEditorUiState::SelectionKind::property_track &&
        st.selected_track_index < st.sequence.property_tracks.size()) {
        return &st.sequence.property_tracks[st.selected_track_index].keys;
    }
    if (st.selection_kind == SequencerEditorUiState::SelectionKind::camera_cut &&
        st.selected_track_index < st.sequence.camera_cuts.size()) {
        CameraCutClip& clip = st.sequence.camera_cuts[st.selected_track_index];
        switch (st.camera_sub_curve) {
        case SequencerEditorUiState::CameraSubCurve::fov:
            out_default_value = clip.default_fov_degrees;
            break;
        case SequencerEditorUiState::CameraSubCurve::exposure:
            out_default_value = clip.default_exposure_ev;
            break;
        case SequencerEditorUiState::CameraSubCurve::focus_distance:
            out_default_value = clip.default_focus_distance;
            break;
        case SequencerEditorUiState::CameraSubCurve::roll:
            out_default_value = 0.0F;
            break;
        case SequencerEditorUiState::CameraSubCurve::rail_u:
            out_default_value = clip.rail_start_u;
            break;
        }
        return &camera_cut_sub_curve(clip, st.camera_sub_curve);
    }
    return nullptr;
}

void scan_cutscene_files(
    const std::filesystem::path& asset_root,
    std::vector<std::string>& out_files) {
    out_files.clear();
    if (asset_root.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(asset_root, ec) || ec) {
        return;
    }
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(
             asset_root,
             std::filesystem::directory_options::skip_permission_denied,
             ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec) {
            continue;
        }
        std::error_code rel_ec;
        const std::filesystem::path rel =
            std::filesystem::relative(entry.path(), asset_root, rel_ec);
        if (rel_ec) {
            continue;
        }
        const std::string rel_utf8 = rel.generic_string();
        if (!rel_utf8.empty() && rel_utf8.ends_with(".vcrseq.json")) {
            out_files.push_back(rel_utf8);
        }
    }
    std::sort(out_files.begin(), out_files.end());
}

void draw_node_uuid_binder(
    const char* const id_suffix,
    const char* const label,
    SceneUuid& uuid_field,
    const SceneDocument& scene,
    const SceneNodeId selected_node,
    bool& modified) {
    ImGui::PushID(id_suffix);
    std::string desc = "(none)";
    if (uuid_field) {
        const SceneNodeId bound = scene.find_by_uuid(uuid_field);
        if (const auto nr = scene.node(bound)) {
            desc = nr->get().name + " [" +
                format_scene_uuid(uuid_field).substr(0U, 8U) + "]";
        } else {
            desc = format_scene_uuid(uuid_field);
        }
    }
    ImGui::Text("%s: %s", label, desc.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Bind Selected") && scene.valid(selected_node)) {
        if (const auto u = scene.uuid(selected_node)) {
            uuid_field = *u;
            modified = true;
        }
    }
    if (uuid_field) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            uuid_field = {};
            modified = true;
        }
    }
    ImGui::PopID();
}

} // namespace

bool EditorUi::sequencer_preview_active() const noexcept {
    return sequencer_ui_ != nullptr && sequencer_ui_->preview_active;
}

bool EditorUi::sequencer_locks_edit_camera() const noexcept {
    return sequencer_ui_ != nullptr &&
        sequencer_ui_->preview_active &&
        sequencer_ui_->lock_camera;
}

SequencerStatusSnapshot EditorUi::sequencer_status() const {
    SequencerStatusSnapshot snap{};
    if (sequencer_ui_ == nullptr) {
        return snap;
    }
    snap.has_sequence = sequencer_ui_->has_sequence;
    snap.relative_path = sequencer_ui_->relative_path;
    snap.sequence_id = sequencer_ui_->sequence.id;
    snap.title = sequencer_ui_->sequence.title;
    snap.dirty = sequencer_ui_->dirty;
    snap.preview_active = sequencer_ui_->preview_active;
    snap.preview_playing = sequencer_ui_->preview_playing;
    snap.lock_camera = sequencer_ui_->lock_camera;
    snap.looping = sequencer_ui_->sequence.looping;
    snap.playhead_seconds = sequencer_ui_->playhead_seconds;
    snap.duration_seconds = sequencer_ui_->sequence.duration_seconds;
    snap.frame_rate = sequencer_ui_->sequence.frame_rate;
    snap.camera_cut_count = sequencer_ui_->sequence.camera_cuts.size();
    snap.camera_shake_count = sequencer_ui_->sequence.camera_shakes.size();
    snap.property_track_count = sequencer_ui_->sequence.property_tracks.size();
    snap.animation_clip_count = sequencer_ui_->sequence.animation_clips.size();
    snap.audio_clip_count = sequencer_ui_->sequence.audio_clips.size();
    snap.subtitle_count = sequencer_ui_->sequence.subtitles.size();
    snap.event_key_count = sequencer_ui_->sequence.event_keys.size();
    return snap;
}

void EditorUi::stop_sequencer_preview(
    SceneDocument& scene,
    Renderer* const renderer,
    FlyCamera* const camera) noexcept {
    if (sequencer_ui_ == nullptr) {
        return;
    }
    if (edit_animator_preview_ != nullptr) {
        for (const SceneNodeId id : sequencer_ui_->previewed_anim_nodes) {
            edit_animator_preview_->clear_preview(id);
            if (renderer != nullptr) {
                renderer->remove_skinned_model_pose(id);
            }
        }
    }
    sequencer_ui_->previewed_anim_nodes.clear();
    if (sequencer_ui_->baseline_scene) {
        scene = std::move(*sequencer_ui_->baseline_scene);
        sequencer_ui_->baseline_scene.reset();
        scene_replaced_ = true;
        hierarchy_revision_ = 0U;
        inspector_edit_snapshot_.reset();
        gizmo_edit_snapshot_.reset();
        if (!scene.valid(selected_node_)) {
            selected_node_ = {};
            name_buffer_node_ = {};
        }
    }
    if (sequencer_ui_->baseline_camera && camera != nullptr) {
        *camera = *sequencer_ui_->baseline_camera;
    }
    sequencer_ui_->baseline_camera.reset();
    if (renderer != nullptr) {
        renderer->set_vertical_fov_degrees(camera_vertical_fov);
        renderer->set_camera_exposure_offset_ev(0.0F);
        renderer->set_camera_focus_state(CameraFocusState{});
        renderer->notify_camera_cut();
    }
    sequencer_ui_->preview_active = false;
    sequencer_ui_->preview_playing = false;
    sequencer_ui_->needs_preview_eval = false;
    sequencer_ui_->last_eval_frame = {};
    sequencer_ui_->eval_scratch.reset_frame_state();
}

void EditorUi::update_sequencer_edit_preview(
    const float delta_seconds,
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera) {
    if (sequencer_ui_ == nullptr ||
        play_state_ != EditorPlayState::edit ||
        !sequencer_ui_->preview_active) {
        return;
    }
    if (!sequencer_ui_->baseline_scene) {
        sequencer_ui_->baseline_scene = scene;
        sequencer_ui_->baseline_camera = camera;
        sequencer_ui_->needs_preview_eval = true;
    }
    const float duration =
        std::max(sequencer_ui_->sequence.duration_seconds, 0.05F);
    if (sequencer_ui_->preview_playing && !sequencer_ui_->dragging_playhead) {
        sequencer_ui_->playhead_seconds += std::max(delta_seconds, 0.0F);
        if (sequencer_ui_->playhead_seconds >= duration) {
            if (sequencer_ui_->sequence.looping) {
                sequencer_ui_->playhead_seconds =
                    std::fmod(sequencer_ui_->playhead_seconds, duration);
            } else {
                sequencer_ui_->playhead_seconds = duration;
                sequencer_ui_->preview_playing = false;
            }
        }
        sequencer_ui_->needs_preview_eval = true;
    }
    sequencer_ui_->playhead_seconds =
        std::clamp(sequencer_ui_->playhead_seconds, 0.0F, duration);
    if (!sequencer_ui_->needs_preview_eval) {
        return;
    }

    if (edit_animator_preview_ != nullptr) {
        for (const SceneNodeId prev_id : sequencer_ui_->previewed_anim_nodes) {
            edit_animator_preview_->clear_preview(prev_id);
            renderer.remove_skinned_model_pose(prev_id);
        }
    }
    sequencer_ui_->previewed_anim_nodes.clear();

    scene = *sequencer_ui_->baseline_scene;
    sequencer_ui_->last_eval_frame = evaluate_cutscene_at_time(
        sequencer_ui_->sequence,
        *sequencer_ui_->baseline_scene,
        sequencer_ui_->playhead_seconds,
        sequencer_ui_->eval_scratch);

    bool any_scene_mutation = false;
    for (const CutsceneEvaluatedNodePose& pose :
         sequencer_ui_->eval_scratch.nodes) {
        SceneNodeId id = pose.node_id;
        if (!scene.valid(id) && pose.node_uuid) {
            id = scene.find_by_uuid(pose.node_uuid);
        }
        if (!scene.valid(id)) {
            continue;
        }
        if (auto node_ref = scene.node(id)) {
            SceneNode& n = node_ref->get();
            if (pose.transform_modified) {
                n.transform = pose.transform;
                any_scene_mutation = true;
            }
            if (pose.visibility_modified) {
                n.visible = pose.visible;
                any_scene_mutation = true;
            }
            if (pose.light_modified) {
                n.light = pose.light;
                any_scene_mutation = true;
            }
            if (pose.material_modified) {
                n.material = pose.material;
                any_scene_mutation = true;
            }
        }
        if (pose.animator_modified) {
            static_cast<void>(scene.set_animator(id, pose.animator));
            any_scene_mutation = true;
        }
        if (edit_animator_preview_ != nullptr &&
            pose.has_anim_clip &&
            !pose.active_anim_clip_name.empty()) {
            edit_animator_preview_->set_preview(
                id,
                pose.active_anim_clip_name,
                false,
                pose.anim_eval_time,
                pose.animator.looping);
            sequencer_ui_->previewed_anim_nodes.push_back(id);
        }
    }
    if (any_scene_mutation) {
        scene.mark_content_changed();
    }

    if (sequencer_ui_->last_eval_frame.has_camera &&
        sequencer_ui_->lock_camera) {
        pose_to_fly_camera(sequencer_ui_->last_eval_frame.camera_pose, camera);
        renderer.set_vertical_fov_degrees(
            sequencer_ui_->last_eval_frame.camera_pose.vertical_fov_degrees);
        renderer.set_camera_exposure_offset_ev(
            sequencer_ui_->last_eval_frame.camera_pose.exposure_offset_ev);
        renderer.set_camera_focus_state(
            sequencer_ui_->last_eval_frame.camera_pose.focus);
        if (sequencer_ui_->last_eval_frame.camera_cut_discontinuity) {
            renderer.notify_camera_cut();
        }
    } else if (!sequencer_ui_->lock_camera) {
        renderer.set_vertical_fov_degrees(camera_vertical_fov);
        renderer.set_camera_exposure_offset_ev(0.0F);
        renderer.set_camera_focus_state(CameraFocusState{});
    }
    sequencer_ui_->needs_preview_eval = false;
}

void EditorUi::open_cutscene_in_sequencer(
    SceneDocument& scene,
    const std::string_view relative_path) {
    if (sequencer_ui_ == nullptr) {
        return;
    }
    sequencer_panel_open_ = true;
    std::string err;
    if (!mcp_sequencer_open(scene, relative_path, err)) {
        sequencer_ui_->error = std::move(err);
    }
}

bool EditorUi::mcp_sequencer_new(
    SceneDocument& scene,
    const std::string_view id,
    const std::string_view title,
    const float duration_seconds,
    const float frame_rate,
    const bool looping,
    std::string& error) {
    error.clear();
    if (sequencer_ui_ == nullptr) {
        error = "Sequencer UI state is unavailable.";
        return false;
    }
    if (!std::isfinite(duration_seconds) || duration_seconds < 0.25F ||
        duration_seconds > 3600.0F) {
        error = "Cutscene duration_seconds must be within [0.25, 3600.0].";
        return false;
    }
    if (!std::isfinite(frame_rate) || frame_rate < 1.0F || frame_rate > 240.0F) {
        error = "Cutscene frame_rate must be within [1.0, 240.0].";
        return false;
    }
    stop_sequencer_preview(scene);
    sequencer_panel_open_ = true;
    sequencer_ui_->reset_to_default(
        id, title, duration_seconds, frame_rate, looping);
    sequencer_ui_->dirty = true;
    sequencer_ui_->status =
        "Created new sequence '" + sequencer_ui_->sequence.id + "'.";
    sequencer_ui_->error.clear();
    return true;
}

bool EditorUi::mcp_sequencer_open(
    SceneDocument& scene,
    const std::string_view relative_path,
    std::string& error) {
    error.clear();
    if (sequencer_ui_ == nullptr) {
        error = "Sequencer UI state is unavailable.";
        return false;
    }
    CutsceneSequence loaded{};
    if (!load_cutscene_sequence_file(
            asset_browser_.root(), relative_path, loaded, error)) {
        return false;
    }
    stop_sequencer_preview(scene);
    sequencer_panel_open_ = true;
    sequencer_ui_->sequence = std::move(loaded);
    sequencer_ui_->relative_path = std::string(relative_path);
    sequencer_ui_->has_sequence = true;
    sequencer_ui_->dirty = false;
    sequencer_ui_->playhead_seconds = 0.0F;
    sequencer_ui_->timeline_scroll_seconds = 0.0F;
    sequencer_ui_->selection_kind = SequencerEditorUiState::SelectionKind::none;
    sequencer_ui_->selected_track_index = 0U;
    sequencer_ui_->selected_key_index = 0U;
    sequencer_ui_->eval_scratch.reset_frame_state();
    sequencer_ui_->last_eval_frame = {};
    sequencer_ui_->sync_buffers();
    sequencer_ui_->status = "Opened " + sequencer_ui_->relative_path;
    sequencer_ui_->error.clear();
    return true;
}

bool EditorUi::mcp_sequencer_save(
    const std::string_view custom_relative_path,
    std::string& out_saved_path,
    std::string& error) {
    error.clear();
    out_saved_path.clear();
    if (sequencer_ui_ == nullptr || !sequencer_ui_->has_sequence) {
        error = "No cutscene sequence is loaded in Sequencer.";
        return false;
    }
    std::string target = custom_relative_path.empty()
        ? sequencer_ui_->relative_path
        : std::string(custom_relative_path);
    if (target.empty()) {
        target = make_default_cutscene_relative_path(sequencer_ui_->sequence.id);
    }
    sequencer_ui_->sequence =
        sanitize_cutscene_sequence(std::move(sequencer_ui_->sequence));
    if (!save_cutscene_sequence_file(
            sequencer_ui_->sequence,
            asset_browser_.root(),
            target,
            error)) {
        return false;
    }
    sequencer_ui_->relative_path = target;
    sequencer_ui_->dirty = false;
    sequencer_ui_->sync_buffers();
    sequencer_ui_->files_scanned = false;
    refresh_asset_browser();
    out_saved_path = std::move(target);
    sequencer_ui_->status = "Saved " + out_saved_path;
    sequencer_ui_->error.clear();
    return true;
}

bool EditorUi::mcp_sequencer_set_playback(
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera,
    const bool active,
    const bool playing,
    const std::optional<float> scrub_time_seconds,
    const std::optional<bool> lock_camera,
    std::string& error) {
    error.clear();
    if (sequencer_ui_ == nullptr || !sequencer_ui_->has_sequence) {
        error = "No cutscene sequence is loaded in Sequencer.";
        return false;
    }
    if (play_state_ != EditorPlayState::edit) {
        error = "Sequencer preview control is only available in Edit mode.";
        return false;
    }
    if (!active) {
        stop_sequencer_preview(scene, &renderer, &camera);
        if (scrub_time_seconds) {
            if (!std::isfinite(*scrub_time_seconds)) {
                error = "scrub_time_seconds must be finite.";
                return false;
            }
            sequencer_ui_->playhead_seconds = std::clamp(
                *scrub_time_seconds,
                0.0F,
                std::max(sequencer_ui_->sequence.duration_seconds, 0.05F));
        }
        sequencer_ui_->status = "Sequencer preview stopped.";
        sequencer_ui_->error.clear();
        return true;
    }
    if (scrub_time_seconds && !std::isfinite(*scrub_time_seconds)) {
        error = "scrub_time_seconds must be finite.";
        return false;
    }
    if (!sequencer_ui_->preview_active) {
        sequencer_ui_->baseline_scene = scene;
        sequencer_ui_->baseline_camera = camera;
        sequencer_ui_->preview_active = true;
    }
    sequencer_ui_->preview_playing = playing;
    if (lock_camera) {
        const bool was_locked = sequencer_ui_->lock_camera;
        sequencer_ui_->lock_camera = *lock_camera;
        if (was_locked && !*lock_camera) {
            if (sequencer_ui_->baseline_camera) {
                camera = *sequencer_ui_->baseline_camera;
            }
            renderer.set_vertical_fov_degrees(camera_vertical_fov);
            renderer.set_camera_exposure_offset_ev(0.0F);
            renderer.set_camera_focus_state(CameraFocusState{});
        }
    }
    if (scrub_time_seconds) {
        sequencer_ui_->playhead_seconds = std::clamp(
            *scrub_time_seconds,
            0.0F,
            std::max(sequencer_ui_->sequence.duration_seconds, 0.05F));
        sequencer_ui_->eval_scratch.reset_frame_state();
    }
    sequencer_ui_->needs_preview_eval = true;
    update_sequencer_edit_preview(0.0F, scene, renderer, camera);
    sequencer_ui_->status = playing
        ? "Sequencer preview playing."
        : "Sequencer preview active.";
    sequencer_ui_->error.clear();
    return true;
}

void EditorUi::build_sequencer_panel(
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera) {
    if (sequencer_ui_ != nullptr &&
        !sequencer_ui_->pending_runtime_cutscene.empty() &&
        play_state_ == EditorPlayState::playing &&
        gameplay_session_ != nullptr &&
        gameplay_session_->active()) {
        gameplay_session_->gameplay_world().request_cutscene(
            sequencer_ui_->pending_runtime_cutscene);
        sequencer_ui_->pending_runtime_cutscene.clear();
    }
    if (!sequencer_panel_open_ || sequencer_ui_ == nullptr) {
        return;
    }
    SequencerEditorUiState& st = *sequencer_ui_;
    if (!st.files_scanned) {
        scan_cutscene_files(asset_browser_.root(), st.available_files);
        st.files_scanned = true;
    }

    ImGui::SetNextWindowSize(
        ImVec2(840.0F, 300.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sequencer", &sequencer_panel_open_)) {
        ImGui::End();
        return;
    }

    const bool can_edit = play_state_ == EditorPlayState::edit;
    CutsceneSequence& seq = st.sequence;

    // 1. File / Transport bar
    if (ImGui::SmallButton("New##SeqNew")) {
        stop_sequencer_preview(scene, &renderer, &camera);
        st.reset_to_default();
        st.dirty = true;
        st.status = "Created new sequence.";
        st.error.clear();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0F);
    if (ImGui::BeginCombo(
            "##SeqFilePicker",
            st.relative_path.empty()
                ? "(choose cutscene)"
                : st.relative_path.c_str())) {
        if (ImGui::Selectable("Refresh list...##SeqRefreshList", false)) {
            scan_cutscene_files(asset_browser_.root(), st.available_files);
        }
        ImGui::Separator();
        for (const std::string& rel : st.available_files) {
            const bool is_sel = rel == st.relative_path;
            if (ImGui::Selectable(rel.c_str(), is_sel)) {
                std::string err;
                if (!mcp_sequencer_open(scene, rel, err)) {
                    st.error = std::move(err);
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(210.0F);
    if (ImGui::InputText(
            "##SeqPathInput",
            st.path_buffer.data(),
            st.path_buffer.size())) {
        st.relative_path = st.path_buffer.data();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open##SeqOpenBtn")) {
        std::string err;
        if (!mcp_sequencer_open(scene, st.path_buffer.data(), err)) {
            st.error = std::move(err);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Save##SeqSaveBtn")) {
        std::string saved;
        std::string err;
        if (!mcp_sequencer_save(st.path_buffer.data(), saved, err)) {
            st.error = std::move(err);
        }
    }

    ImGui::SameLine(0.0F, 14.0F);
    ImGui::BeginDisabled(!can_edit);
    if (st.preview_playing) {
        if (ImGui::SmallButton("Pause##SeqPause")) {
            st.preview_playing = false;
        }
    } else {
        if (ImGui::SmallButton("Play##SeqPlay")) {
            if (!st.preview_active) {
                st.baseline_scene = scene;
                st.baseline_camera = camera;
                st.preview_active = true;
            }
            if (st.playhead_seconds >= seq.duration_seconds - 1.0e-3F) {
                st.playhead_seconds = 0.0F;
                st.eval_scratch.reset_frame_state();
            }
            st.preview_playing = true;
            st.needs_preview_eval = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop##SeqStop")) {
        stop_sequencer_preview(scene, &renderer, &camera);
        st.playhead_seconds = 0.0F;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::SmallButton("Play Runtime##SeqPlayGame")) {
        std::string saved_rel;
        std::string save_err;
        if (mcp_sequencer_save(st.path_buffer.data(), saved_rel, save_err)) {
            if (play_state_ == EditorPlayState::edit) {
                stop_sequencer_preview(scene, &renderer, &camera);
                st.pending_runtime_cutscene = saved_rel;
                start_play(scene);
            } else if (gameplay_session_ != nullptr &&
                       gameplay_session_->active()) {
                gameplay_session_->gameplay_world().request_cutscene(saved_rel);
            }
        } else {
            st.error = std::move(save_err);
        }
    }

    // Row 2: Scrub, Duration, FPS, Flags
    bool seq_edited = false;
    const float duration = std::max(seq.duration_seconds, 0.25F);
    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::SliderFloat(
            "##SeqPlayhead",
            &st.playhead_seconds,
            0.0F,
            duration,
            "%.2fs",
            ImGuiSliderFlags_AlwaysClamp) &&
        can_edit) {
        if (!st.preview_active) {
            st.baseline_scene = scene;
            st.baseline_camera = camera;
            st.preview_active = true;
        }
        st.preview_playing = false;
        st.needs_preview_eval = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(76.0F);
    if (ImGui::DragFloat(
            "Dur##SeqDur",
            &seq.duration_seconds,
            0.1F,
            0.25F,
            3600.0F,
            "%.2fs",
            ImGuiSliderFlags_AlwaysClamp)) {
        st.playhead_seconds =
            std::min(st.playhead_seconds, seq.duration_seconds);
        seq_edited = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(62.0F);
    if (ImGui::DragFloat(
            "FPS##SeqFps",
            &seq.frame_rate,
            0.5F,
            1.0F,
            240.0F,
            "%.0f",
            ImGuiSliderFlags_AlwaysClamp)) {
        seq_edited = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop##SeqLoop", &seq.looping)) {
        seq_edited = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Lock Cam##SeqLockCam", &st.lock_camera) &&
        st.preview_active) {
        if (!st.lock_camera) {
            if (st.baseline_camera) {
                camera = *st.baseline_camera;
            }
            renderer.set_vertical_fov_degrees(camera_vertical_fov);
            renderer.set_camera_exposure_offset_ev(0.0F);
            renderer.set_camera_focus_state(CameraFocusState{});
        } else {
            st.needs_preview_eval = true;
        }
    }
    ImGui::SameLine();
    static_cast<void>(ImGui::Checkbox("Curves##SeqCurves", &st.show_curve_editor));
    ImGui::SameLine();
    static_cast<void>(ImGui::Checkbox("Settings##SeqSettings", &st.show_settings));
    if (st.dirty) {
        ImGui::SameLine();
        ui_kit_.badge("UNSAVED", EditorTone::warning);
    }

    if (st.show_settings) {
        ImGui::Separator();
        ImGui::SetNextItemWidth(160.0F);
        if (ImGui::InputText("ID##SeqId", st.id_buffer.data(), st.id_buffer.size())) {
            seq.id = st.id_buffer.data();
            seq_edited = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0F);
        if (ImGui::InputText(
                "Title##SeqTitle", st.title_buffer.data(), st.title_buffer.size())) {
            seq.title = st.title_buffer.data();
            seq_edited = true;
        }
        ImGui::SameLine();
        seq_edited |= ImGui::Checkbox("Skippable##SeqSkip", &seq.skippable);
        ImGui::SameLine();
        seq_edited |= ImGui::Checkbox("Lock Input##SeqLockIn", &seq.lock_player_input);
        ImGui::SameLine();
        seq_edited |=
            ImGui::Checkbox("Pause Sim##SeqPauseSim", &seq.pause_gameplay_simulation);
        ImGui::SameLine();
        seq_edited |= ImGui::Checkbox("Hide HUD##SeqHideHud", &seq.hide_hud);
        ImGui::SetNextItemWidth(90.0F);
        seq_edited |= ImGui::DragFloat(
            "Letterbox##SeqLb", &seq.letterbox_aspect, 0.05F, 0.0F, 4.0F, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0F);
        seq_edited |= ImGui::DragFloat(
            "Blend In##SeqBI", &seq.blend_in_seconds, 0.05F, 0.0F, 10.0F, "%.2fs");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0F);
        seq_edited |= ImGui::DragFloat(
            "Blend Out##SeqBO", &seq.blend_out_seconds, 0.05F, 0.0F, 10.0F, "%.2fs");
    }

    // Track creation buttons
    ImGui::Separator();
    const SceneUuid selected_uuid =
        scene.valid(selected_node_)
            ? scene.uuid(selected_node_).value_or(SceneUuid{})
            : SceneUuid{};
    if (ImGui::SmallButton("+ Cam Cut") &&
        seq.camera_cuts.size() < maximum_cutscene_clips) {
        CameraCutClip clip{};
        clip.start_time = st.playhead_seconds;
        clip.end_time = std::min(seq.duration_seconds, st.playhead_seconds + 2.5F);
        if (clip.end_time <= clip.start_time + 0.05F) {
            clip.end_time = clip.start_time + 0.5F;
        }
        clip.camera_node_uuid = selected_uuid;
        seq.camera_cuts.push_back(std::move(clip));
        st.selection_kind = SequencerEditorUiState::SelectionKind::camera_cut;
        st.selected_track_index = seq.camera_cuts.size() - 1U;
        seq_edited = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Shake") &&
        seq.camera_shakes.size() < maximum_cutscene_clips) {
        CameraShakeClip shake{};
        shake.start_time = st.playhead_seconds;
        shake.duration = 0.6F;
        seq.camera_shakes.push_back(shake);
        st.selection_kind = SequencerEditorUiState::SelectionKind::camera_shake;
        st.selected_track_index = seq.camera_shakes.size() - 1U;
        seq_edited = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Property") &&
        seq.property_tracks.size() < maximum_cutscene_tracks) {
        SequencerPropertyTrack tr{};
        tr.target_node_uuid = selected_uuid;
        tr.keys.push_back(SequencerKeyframe{.time = 0.0F, .value = 0.0F});
        tr.keys.push_back(SequencerKeyframe{
            .time = std::max(0.5F, seq.duration_seconds), .value = 1.0F});
        seq.property_tracks.push_back(std::move(tr));
        st.selection_kind =
            SequencerEditorUiState::SelectionKind::property_track;
        st.selected_track_index = seq.property_tracks.size() - 1U;
        st.selected_key_index = 0U;
        seq_edited = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Anim") &&
        seq.animation_clips.size() < maximum_cutscene_clips) {
        SequencerAnimationClip ac{};
        ac.target_node_uuid = selected_uuid;
        ac.start_time = st.playhead_seconds;
        ac.end_time = std::min(seq.duration_seconds, st.playhead_seconds + 2.0F);
        if (ac.end_time <= ac.start_time + 0.05F) {
            ac.end_time = ac.start_time + 0.5F;
        }
        seq.animation_clips.push_back(std::move(ac));
        st.selection_kind =
            SequencerEditorUiState::SelectionKind::animation_clip;
        st.selected_track_index = seq.animation_clips.size() - 1U;
        seq_edited = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Audio") &&
        seq.audio_clips.size() < maximum_cutscene_clips) {
        SequencerAudioClip au{};
        au.start_time = st.playhead_seconds;
        au.spatial_node_uuid = selected_uuid;
        seq.audio_clips.push_back(std::move(au));
        st.selection_kind = SequencerEditorUiState::SelectionKind::audio_clip;
        st.selected_track_index = seq.audio_clips.size() - 1U;
        seq_edited = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Subtitle") &&
        seq.subtitles.size() < maximum_cutscene_clips) {
        SequencerSubtitleClip sub{};
        sub.start_time = st.playhead_seconds;
        sub.end_time = std::min(seq.duration_seconds, st.playhead_seconds + 2.5F);
        if (sub.end_time <= sub.start_time + 0.05F) {
            sub.end_time = sub.start_time + 0.5F;
        }
        sub.speaker_key = "Speaker";
        sub.text_key = "Subtitle text";
        seq.subtitles.push_back(std::move(sub));
        st.selection_kind = SequencerEditorUiState::SelectionKind::subtitle;
        st.selected_track_index = seq.subtitles.size() - 1U;
        seq_edited = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Event") &&
        seq.event_keys.size() < maximum_cutscene_clips) {
        SequencerEventKey ev{};
        ev.time = st.playhead_seconds;
        ev.event_name = "event";
        ev.target_node_uuid = selected_uuid;
        seq.event_keys.push_back(std::move(ev));
        st.selection_kind = SequencerEditorUiState::SelectionKind::event_key;
        st.selected_track_index = seq.event_keys.size() - 1U;
        seq_edited = true;
    }
    ImGui::SameLine(0.0F, 14.0F);
    ImGui::SetNextItemWidth(90.0F);
    static_cast<void>(ImGui::SliderFloat(
        "Zoom##SeqZoom",
        &st.timeline_zoom,
        0.25F,
        8.0F,
        "%.2fx",
        ImGuiSliderFlags_AlwaysClamp));

    if (!st.error.empty()) {
        ui_kit_.status_text(st.error.c_str(), EditorTone::danger);
    } else if (!st.status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.status.c_str());
    }

    // 2. Main Split: Timeline/Curve on left, Track Inspector on right
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float inspector_width = std::clamp(avail.x * 0.34F, 260.0F, 420.0F);
    const float canvas_width = std::max(avail.x - inspector_width - 8.0F, 220.0F);
    const float body_height = std::max(avail.y, 130.0F);

    if (ImGui::BeginChild(
            "##SeqTimelineArea", ImVec2(canvas_width, body_height), true)) {
        const float px_per_sec =
            std::max((canvas_width - 140.0F) / std::max(duration, 0.25F), 16.0F) *
            st.timeline_zoom;
        const float label_col_w = 130.0F;
        const float ruler_h = 22.0F;
        const float row_h = 22.0F;

        const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        const float timeline_x0 = canvas_pos.x + label_col_w;
        const float timeline_w = std::max(
            ImGui::GetContentRegionAvail().x - label_col_w, 60.0F);
        const auto time_to_x = [&](const float t) noexcept {
            return timeline_x0 +
                (t - st.timeline_scroll_seconds) * px_per_sec;
        };
        const auto x_to_time = [&](const float x) noexcept {
            return std::clamp(
                st.timeline_scroll_seconds + (x - timeline_x0) / px_per_sec,
                0.0F,
                duration);
        };

        // Ruler interaction
        ImGui::SetCursorScreenPos(ImVec2(timeline_x0, canvas_pos.y));
        static_cast<void>(ImGui::InvisibleButton(
            "##SeqRulerHit", ImVec2(timeline_w, ruler_h)));
        if (ImGui::IsItemActive() &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            can_edit) {
            st.dragging_playhead = true;
            st.playhead_seconds = x_to_time(ImGui::GetIO().MousePos.x);
            if (!st.preview_active) {
                st.baseline_scene = scene;
                st.baseline_camera = camera;
                st.preview_active = true;
            }
            st.preview_playing = false;
            st.needs_preview_eval = true;
        } else if (st.dragging_playhead &&
                   !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            st.dragging_playhead = false;
        }

        ImDrawList* const dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(
            canvas_pos,
            ImVec2(canvas_pos.x + label_col_w + timeline_w, canvas_pos.y + ruler_h),
            IM_COL32(34, 38, 46, 255));
        dl->PushClipRect(
            ImVec2(timeline_x0, canvas_pos.y),
            ImVec2(timeline_x0 + timeline_w, canvas_pos.y + body_height),
            true);
        const float step_sec = st.timeline_zoom >= 3.0F
            ? 0.25F
            : (st.timeline_zoom >= 1.5F ? 0.5F : 1.0F);
        for (float t = 0.0F; t <= duration + 1.0e-4F; t += step_sec) {
            const float tx = time_to_x(t);
            if (tx < timeline_x0 - 2.0F || tx > timeline_x0 + timeline_w + 2.0F) {
                continue;
            }
            dl->AddLine(
                ImVec2(tx, canvas_pos.y + ruler_h - 7.0F),
                ImVec2(tx, canvas_pos.y + ruler_h),
                IM_COL32(150, 165, 185, 190));
            std::array<char, 16U> tick_buf{};
            std::snprintf(tick_buf.data(), tick_buf.size(), "%.1fs", t);
            dl->AddText(
                ImVec2(tx + 2.0F, canvas_pos.y + 3.0F),
                IM_COL32(190, 200, 215, 220),
                tick_buf.data());
        }
        dl->PopClipRect();

        if (!st.show_curve_editor) {
            // Track rows
            std::size_t row_idx = 0U;
            const auto draw_bar_row = [&](const char* const row_label,
                                          const float t0,
                                          const float t1,
                                          const ImU32 bar_col,
                                          const SequencerEditorUiState::SelectionKind kind,
                                          const std::size_t idx) {
                const float y0 = canvas_pos.y + ruler_h +
                    static_cast<float>(row_idx) * row_h;
                const float y1 = y0 + row_h - 2.0F;
                const bool is_sel =
                    st.selection_kind == kind && st.selected_track_index == idx;
                dl->AddRectFilled(
                    ImVec2(canvas_pos.x, y0),
                    ImVec2(canvas_pos.x + label_col_w - 2.0F, y1),
                    is_sel ? IM_COL32(58, 74, 102, 230) : IM_COL32(30, 34, 42, 210),
                    3.0F);
                dl->AddText(
                    ImVec2(canvas_pos.x + 6.0F, y0 + 3.0F),
                    IM_COL32(230, 235, 245, 240),
                    row_label);

                dl->PushClipRect(
                    ImVec2(timeline_x0, y0),
                    ImVec2(timeline_x0 + timeline_w, y1),
                    true);
                dl->AddRectFilled(
                    ImVec2(timeline_x0, y0),
                    ImVec2(timeline_x0 + timeline_w, y1),
                    (row_idx % 2U) == 0U
                        ? IM_COL32(24, 27, 34, 210)
                        : IM_COL32(28, 31, 39, 210));
                const float bx0 = time_to_x(t0);
                const float bx1 = std::max(bx0 + 6.0F, time_to_x(t1));
                dl->AddRectFilled(
                    ImVec2(bx0, y0 + 2.0F),
                    ImVec2(bx1, y1 - 2.0F),
                    bar_col,
                    3.0F);
                if (is_sel) {
                    dl->AddRect(
                        ImVec2(bx0, y0 + 2.0F),
                        ImVec2(bx1, y1 - 2.0F),
                        IM_COL32(255, 230, 120, 255),
                        3.0F,
                        0,
                        1.5F);
                }
                dl->PopClipRect();

                ImGui::SetCursorScreenPos(ImVec2(canvas_pos.x, y0));
                ImGui::PushID(static_cast<int>(row_idx));
                if (ImGui::InvisibleButton(
                        "##SeqRowHit",
                        ImVec2(label_col_w + timeline_w, row_h))) {
                    st.selection_kind = kind;
                    st.selected_track_index = idx;
                    st.selected_key_index = 0U;
                }
                ImGui::PopID();
                ++row_idx;
            };

            for (std::size_t i = 0U; i < seq.camera_cuts.size(); ++i) {
                std::array<char, 40U> lbl{};
                std::snprintf(lbl.data(), lbl.size(), "Cam Cut #%zu", i);
                draw_bar_row(
                    lbl.data(),
                    seq.camera_cuts[i].start_time,
                    seq.camera_cuts[i].end_time,
                    IM_COL32(72, 160, 232, 225),
                    SequencerEditorUiState::SelectionKind::camera_cut,
                    i);
            }
            for (std::size_t i = 0U; i < seq.camera_shakes.size(); ++i) {
                std::array<char, 40U> lbl{};
                std::snprintf(lbl.data(), lbl.size(), "Cam Shake #%zu", i);
                draw_bar_row(
                    lbl.data(),
                    seq.camera_shakes[i].start_time,
                    seq.camera_shakes[i].start_time + seq.camera_shakes[i].duration,
                    IM_COL32(230, 130, 72, 225),
                    SequencerEditorUiState::SelectionKind::camera_shake,
                    i);
            }
            for (std::size_t i = 0U; i < seq.property_tracks.size(); ++i) {
                const SequencerPropertyTrack& tr = seq.property_tracks[i];
                const float y0 = canvas_pos.y + ruler_h +
                    static_cast<float>(row_idx) * row_h;
                const float y1 = y0 + row_h - 2.0F;
                const bool is_sel =
                    st.selection_kind ==
                        SequencerEditorUiState::SelectionKind::property_track &&
                    st.selected_track_index == i;
                dl->AddRectFilled(
                    ImVec2(canvas_pos.x, y0),
                    ImVec2(canvas_pos.x + label_col_w - 2.0F, y1),
                    is_sel ? IM_COL32(58, 74, 102, 230) : IM_COL32(30, 34, 42, 210),
                    3.0F);
                dl->AddText(
                    ImVec2(canvas_pos.x + 6.0F, y0 + 3.0F),
                    IM_COL32(220, 238, 215, 240),
                    to_token(tr.property).data());
                dl->PushClipRect(
                    ImVec2(timeline_x0, y0),
                    ImVec2(timeline_x0 + timeline_w, y1),
                    true);
                dl->AddRectFilled(
                    ImVec2(timeline_x0, y0),
                    ImVec2(timeline_x0 + timeline_w, y1),
                    (row_idx % 2U) == 0U
                        ? IM_COL32(24, 27, 34, 210)
                        : IM_COL32(28, 31, 39, 210));
                for (std::size_t k = 0U; k < tr.keys.size(); ++k) {
                    const float kx = time_to_x(tr.keys[k].time);
                    const float ky = (y0 + y1) * 0.5F;
                    const bool key_sel = is_sel && st.selected_key_index == k;
                    dl->AddQuadFilled(
                        ImVec2(kx, ky - 5.0F),
                        ImVec2(kx + 5.0F, ky),
                        ImVec2(kx, ky + 5.0F),
                        ImVec2(kx - 5.0F, ky),
                        key_sel
                            ? IM_COL32(255, 232, 96, 255)
                            : IM_COL32(118, 224, 148, 240));
                }
                dl->PopClipRect();
                ImGui::SetCursorScreenPos(ImVec2(canvas_pos.x, y0));
                ImGui::PushID(static_cast<int>(row_idx));
                if (ImGui::InvisibleButton(
                        "##SeqRowPropHit",
                        ImVec2(label_col_w + timeline_w, row_h))) {
                    st.selection_kind =
                        SequencerEditorUiState::SelectionKind::property_track;
                    st.selected_track_index = i;
                    st.selected_key_index = 0U;
                }
                ImGui::PopID();
                ++row_idx;
            }
            for (std::size_t i = 0U; i < seq.animation_clips.size(); ++i) {
                draw_bar_row(
                    seq.animation_clips[i].clip_name.empty()
                        ? "Anim Clip"
                        : seq.animation_clips[i].clip_name.c_str(),
                    seq.animation_clips[i].start_time,
                    seq.animation_clips[i].end_time,
                    IM_COL32(168, 120, 232, 225),
                    SequencerEditorUiState::SelectionKind::animation_clip,
                    i);
            }
            for (std::size_t i = 0U; i < seq.audio_clips.size(); ++i) {
                std::array<char, 40U> lbl{};
                std::snprintf(lbl.data(), lbl.size(), "Audio #%zu", i);
                draw_bar_row(
                    lbl.data(),
                    seq.audio_clips[i].start_time,
                    seq.audio_clips[i].start_time + 1.0F,
                    IM_COL32(72, 205, 195, 225),
                    SequencerEditorUiState::SelectionKind::audio_clip,
                    i);
            }
            for (std::size_t i = 0U; i < seq.subtitles.size(); ++i) {
                std::array<char, 40U> lbl{};
                std::snprintf(lbl.data(), lbl.size(), "Subtitle #%zu", i);
                draw_bar_row(
                    lbl.data(),
                    seq.subtitles[i].start_time,
                    seq.subtitles[i].end_time,
                    IM_COL32(215, 198, 92, 225),
                    SequencerEditorUiState::SelectionKind::subtitle,
                    i);
            }
            for (std::size_t i = 0U; i < seq.event_keys.size(); ++i) {
                draw_bar_row(
                    seq.event_keys[i].event_name.empty()
                        ? "Event"
                        : seq.event_keys[i].event_name.c_str(),
                    seq.event_keys[i].time,
                    seq.event_keys[i].time + 0.15F,
                    IM_COL32(238, 102, 136, 225),
                    SequencerEditorUiState::SelectionKind::event_key,
                    i);
            }
        } else {
            // Curve Editor
            float default_val = 0.0F;
            std::vector<SequencerKeyframe>* const keys =
                resolve_active_curve(st, default_val);
            const ImVec2 plot_min(
                timeline_x0, canvas_pos.y + ruler_h + 4.0F);
            const ImVec2 plot_max(
                timeline_x0 + timeline_w,
                canvas_pos.y + body_height - 16.0F);
            dl->PushClipRect(plot_min, plot_max, true);
            dl->AddRectFilled(plot_min, plot_max, IM_COL32(20, 23, 29, 255));
            dl->AddRect(plot_min, plot_max, IM_COL32(62, 70, 84, 255));
            if (keys == nullptr || plot_max.y <= plot_min.y + 20.0F) {
                dl->AddText(
                    ImVec2(plot_min.x + 12.0F, plot_min.y + 12.0F),
                    IM_COL32(180, 190, 205, 220),
                    "Select a Camera Cut or Property Track to edit its keyframe curve.");
            } else {
                float min_v = default_val - 1.0F;
                float max_v = default_val + 1.0F;
                for (const SequencerKeyframe& k : *keys) {
                    min_v = std::min(min_v, k.value);
                    max_v = std::max(max_v, k.value);
                }
                const float span_v = std::max(max_v - min_v, 0.25F);
                min_v -= span_v * 0.15F;
                max_v += span_v * 0.15F;
                const auto val_to_y = [&](const float v) noexcept {
                    const float n = (v - min_v) / std::max(max_v - min_v, 1.0e-4F);
                    return plot_max.y - n * (plot_max.y - plot_min.y);
                };
                constexpr std::size_t plot_samples = 96U;
                for (std::size_t s = 1U; s <= plot_samples; ++s) {
                    const float t0 = duration *
                        static_cast<float>(s - 1U) /
                        static_cast<float>(plot_samples);
                    const float t1 = duration *
                        static_cast<float>(s) /
                        static_cast<float>(plot_samples);
                    const float v0 = evaluate_sequencer_curve(
                        *keys, t0, default_val);
                    const float v1 = evaluate_sequencer_curve(
                        *keys, t1, default_val);
                    dl->AddLine(
                        ImVec2(time_to_x(t0), val_to_y(v0)),
                        ImVec2(time_to_x(t1), val_to_y(v1)),
                        IM_COL32(92, 204, 255, 240),
                        2.0F);
                }
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const bool click_plot = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                for (std::size_t k = 0U; k < keys->size(); ++k) {
                    const ImVec2 kp(
                        time_to_x((*keys)[k].time),
                        val_to_y((*keys)[k].value));
                    const bool is_ksel = st.selected_key_index == k;
                    dl->AddCircleFilled(
                        kp,
                        is_ksel ? 5.5F : 4.0F,
                        is_ksel
                            ? IM_COL32(255, 224, 80, 255)
                            : IM_COL32(130, 235, 160, 240),
                        12);
                    if (click_plot) {
                        const float dx = mouse.x - kp.x;
                        const float dy = mouse.y - kp.y;
                        if (dx * dx + dy * dy <= 64.0F) {
                            st.selected_key_index = k;
                        }
                    }
                }
            }
            dl->PopClipRect();
        }

        // Playhead vertical line
        const float px = time_to_x(st.playhead_seconds);
        if (px >= timeline_x0 && px <= timeline_x0 + timeline_w) {
            dl->AddLine(
                ImVec2(px, canvas_pos.y),
                ImVec2(px, canvas_pos.y + body_height),
                IM_COL32(255, 76, 76, 245),
                2.0F);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild(
            "##SeqTrackInspector",
            ImVec2(inspector_width, body_height),
            true)) {
        using SK = SequencerEditorUiState::SelectionKind;
        if (st.selection_kind == SK::camera_cut &&
            st.selected_track_index < seq.camera_cuts.size()) {
            CameraCutClip& clip = seq.camera_cuts[st.selected_track_index];
            ImGui::Text("Camera Cut #%zu", st.selected_track_index);
            seq_edited |= ImGui::DragFloat("Start##CCStart", &clip.start_time, 0.05F, 0.0F, duration);
            seq_edited |= ImGui::DragFloat("End##CCEnd", &clip.end_time, 0.05F, 0.05F, duration);
            draw_node_uuid_binder("CCCam", "Camera Node", clip.camera_node_uuid, scene, selected_node_, seq_edited);
            draw_node_uuid_binder("CCLook", "Look-At Node", clip.look_at_target_uuid, scene, selected_node_, seq_edited);
            draw_node_uuid_binder("CCRail", "Rail Spline", clip.rail_spline_uuid, scene, selected_node_, seq_edited);
            seq_edited |= ImGui::DragFloat("Default FOV##CCDefFov", &clip.default_fov_degrees, 0.5F, minimum_camera_vertical_fov, maximum_camera_vertical_fov);
            seq_edited |= ImGui::DragFloat("Blend In##CCBI", &clip.blend_in_seconds, 0.05F, 0.0F, 10.0F);
            seq_edited |= ImGui::Checkbox("DoF##CCDof", &clip.dof_enabled);
            ImGui::SameLine();
            seq_edited |= ImGui::Checkbox("Auto Focus##CCAF", &clip.auto_focus_look_at);
            ImGui::SetNextItemWidth(140.0F);
            if (ImGui::BeginCombo("Curve##CCSubCurve", camera_sub_curve_label(st.camera_sub_curve))) {
                for (const auto sub : {
                         SequencerEditorUiState::CameraSubCurve::fov,
                         SequencerEditorUiState::CameraSubCurve::roll,
                         SequencerEditorUiState::CameraSubCurve::exposure,
                         SequencerEditorUiState::CameraSubCurve::focus_distance,
                         SequencerEditorUiState::CameraSubCurve::rail_u}) {
                    if (ImGui::Selectable(camera_sub_curve_label(sub), st.camera_sub_curve == sub)) {
                        st.camera_sub_curve = sub;
                        st.selected_key_index = 0U;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::SmallButton("Delete Cut##DelCC")) {
                seq.camera_cuts.erase(seq.camera_cuts.begin() + static_cast<std::ptrdiff_t>(st.selected_track_index));
                st.selection_kind = SK::none;
                seq_edited = true;
            }
        } else if (st.selection_kind == SK::camera_shake &&
                   st.selected_track_index < seq.camera_shakes.size()) {
            CameraShakeClip& sh = seq.camera_shakes[st.selected_track_index];
            ImGui::Text("Camera Shake #%zu", st.selected_track_index);
            seq_edited |= ImGui::DragFloat("Start##ShStart", &sh.start_time, 0.05F, 0.0F, duration);
            seq_edited |= ImGui::DragFloat("Duration##ShDur", &sh.duration, 0.05F, 0.05F, 30.0F);
            seq_edited |= ImGui::DragFloat3(
                "Pos Amp##ShPos", &sh.params.amplitude_position.x, 0.005F, 0.0F, 5.0F);
            seq_edited |= ImGui::DragFloat3(
                "Rot Amp##ShRot", &sh.params.amplitude_rotation_degrees.x, 0.1F, 0.0F, 45.0F);
            seq_edited |= ImGui::DragFloat(
                "FOV Amp##ShFov", &sh.params.amplitude_fov_degrees, 0.1F, 0.0F, 30.0F);
            seq_edited |= ImGui::DragFloat("Frequency##ShFreq", &sh.params.frequency_hz, 0.5F, 0.5F, 60.0F);
            if (ImGui::SmallButton("Delete Shake##DelSh")) {
                seq.camera_shakes.erase(seq.camera_shakes.begin() + static_cast<std::ptrdiff_t>(st.selected_track_index));
                st.selection_kind = SK::none;
                seq_edited = true;
            }
        } else if (st.selection_kind == SK::property_track &&
                   st.selected_track_index < seq.property_tracks.size()) {
            SequencerPropertyTrack& tr = seq.property_tracks[st.selected_track_index];
            ImGui::Text("Property Track #%zu", st.selected_track_index);
            draw_node_uuid_binder("PTNode", "Target Node", tr.target_node_uuid, scene, selected_node_, seq_edited);
            draw_node_uuid_binder("PTSpline", "Ref Spline", tr.reference_spline_uuid, scene, selected_node_, seq_edited);
            if (ImGui::BeginCombo("Property##PTProp", to_token(tr.property).data())) {
                for (std::uint8_t pi = 0U; pi <= 23U; ++pi) {
                    const auto tgt = static_cast<SequencerPropertyTarget>(pi);
                    if (ImGui::Selectable(to_token(tgt).data(), tr.property == tgt)) {
                        tr.property = tgt;
                        seq_edited = true;
                    }
                }
                ImGui::EndCombo();
            }
            seq_edited |= ImGui::Checkbox("Muted##PTMute", &tr.muted);
            if (ImGui::SmallButton("Delete Track##DelPT")) {
                seq.property_tracks.erase(seq.property_tracks.begin() + static_cast<std::ptrdiff_t>(st.selected_track_index));
                st.selection_kind = SK::none;
                seq_edited = true;
            }
        } else if (st.selection_kind == SK::animation_clip &&
                   st.selected_track_index < seq.animation_clips.size()) {
            SequencerAnimationClip& ac = seq.animation_clips[st.selected_track_index];
            ImGui::Text("Animation Clip #%zu", st.selected_track_index);
            draw_node_uuid_binder("ACNode", "Target Node", ac.target_node_uuid, scene, selected_node_, seq_edited);
            std::array<char, 96U> ac_buf{};
            std::memcpy(ac_buf.data(), ac.clip_name.data(), std::min(ac.clip_name.size(), ac_buf.size() - 1U));
            if (ImGui::InputText("Clip Name##ACName", ac_buf.data(), ac_buf.size())) {
                ac.clip_name = ac_buf.data();
                seq_edited = true;
            }
            seq_edited |= ImGui::DragFloat("Start##ACStart", &ac.start_time, 0.05F, 0.0F, duration);
            seq_edited |= ImGui::DragFloat("End##ACEnd", &ac.end_time, 0.05F, 0.05F, duration);
            seq_edited |= ImGui::DragFloat("Speed##ACSpd", &ac.playback_speed, 0.05F, 0.0F, 10.0F);
            seq_edited |= ImGui::Checkbox("Loop##ACLoop", &ac.looping);
            if (ImGui::SmallButton("Delete Anim##DelAC")) {
                seq.animation_clips.erase(seq.animation_clips.begin() + static_cast<std::ptrdiff_t>(st.selected_track_index));
                st.selection_kind = SK::none;
                seq_edited = true;
            }
        } else if (st.selection_kind == SK::audio_clip &&
                   st.selected_track_index < seq.audio_clips.size()) {
            SequencerAudioClip& au = seq.audio_clips[st.selected_track_index];
            ImGui::Text("Audio Clip #%zu", st.selected_track_index);
            std::array<char, 160U> au_buf{};
            std::memcpy(au_buf.data(), au.audio_asset_path.data(), std::min(au.audio_asset_path.size(), au_buf.size() - 1U));
            if (ImGui::InputText("Asset##AuPath", au_buf.data(), au_buf.size())) {
                au.audio_asset_path = au_buf.data();
                seq_edited = true;
            }
            draw_node_uuid_binder("AuNode", "Spatial Node", au.spatial_node_uuid, scene, selected_node_, seq_edited);
            seq_edited |= ImGui::DragFloat("Start##AuStart", &au.start_time, 0.05F, 0.0F, duration);
            seq_edited |= ImGui::DragFloat("Volume##AuVol", &au.volume, 0.02F, 0.0F, 2.0F);
            seq_edited |= ImGui::Checkbox("Stop on End##AuStop", &au.stop_on_cutscene_end);
            if (ImGui::SmallButton("Delete Audio##DelAu")) {
                seq.audio_clips.erase(seq.audio_clips.begin() + static_cast<std::ptrdiff_t>(st.selected_track_index));
                st.selection_kind = SK::none;
                seq_edited = true;
            }
        } else if (st.selection_kind == SK::subtitle &&
                   st.selected_track_index < seq.subtitles.size()) {
            SequencerSubtitleClip& sub = seq.subtitles[st.selected_track_index];
            ImGui::Text("Subtitle #%zu", st.selected_track_index);
            std::array<char, 96U> sp_buf{};
            std::array<char, 256U> tx_buf{};
            std::memcpy(sp_buf.data(), sub.speaker_key.data(), std::min(sub.speaker_key.size(), sp_buf.size() - 1U));
            std::memcpy(tx_buf.data(), sub.text_key.data(), std::min(sub.text_key.size(), tx_buf.size() - 1U));
            if (ImGui::InputText("Speaker##SubSp", sp_buf.data(), sp_buf.size())) {
                sub.speaker_key = sp_buf.data();
                seq_edited = true;
            }
            if (ImGui::InputText("Text##SubTx", tx_buf.data(), tx_buf.size())) {
                sub.text_key = tx_buf.data();
                seq_edited = true;
            }
            seq_edited |= ImGui::DragFloat("Start##SubStart", &sub.start_time, 0.05F, 0.0F, duration);
            seq_edited |= ImGui::DragFloat("End##SubEnd", &sub.end_time, 0.05F, 0.05F, duration);
            if (ImGui::SmallButton("Delete Sub##DelSub")) {
                seq.subtitles.erase(seq.subtitles.begin() + static_cast<std::ptrdiff_t>(st.selected_track_index));
                st.selection_kind = SK::none;
                seq_edited = true;
            }
        } else if (st.selection_kind == SK::event_key &&
                   st.selected_track_index < seq.event_keys.size()) {
            SequencerEventKey& ev = seq.event_keys[st.selected_track_index];
            ImGui::Text("Event Key #%zu", st.selected_track_index);
            std::array<char, 96U> ev_name{};
            std::array<char, 160U> ev_pay{};
            std::memcpy(ev_name.data(), ev.event_name.data(), std::min(ev.event_name.size(), ev_name.size() - 1U));
            std::memcpy(ev_pay.data(), ev.payload.data(), std::min(ev.payload.size(), ev_pay.size() - 1U));
            if (ImGui::InputText("Name##EvName", ev_name.data(), ev_name.size())) {
                ev.event_name = ev_name.data();
                seq_edited = true;
            }
            if (ImGui::InputText("Payload##EvPay", ev_pay.data(), ev_pay.size())) {
                ev.payload = ev_pay.data();
                seq_edited = true;
            }
            draw_node_uuid_binder("EvNode", "Target Node", ev.target_node_uuid, scene, selected_node_, seq_edited);
            seq_edited |= ImGui::DragFloat("Time##EvTime", &ev.time, 0.05F, 0.0F, duration);
            if (ImGui::SmallButton("Delete Event##DelEv")) {
                seq.event_keys.erase(seq.event_keys.begin() + static_cast<std::ptrdiff_t>(st.selected_track_index));
                st.selection_kind = SK::none;
                seq_edited = true;
            }
        } else {
            ImGui::TextDisabled("Select a track/clip on the timeline.");
        }

        // Keyframe controls for curve-backed tracks
        float dummy_def = 0.0F;
        if (std::vector<SequencerKeyframe>* const curve =
                resolve_active_curve(st, dummy_def);
            curve != nullptr) {
            ImGui::Separator();
            ImGui::Text("Keyframes (%zu)", curve->size());
            if (ImGui::SmallButton("+ Key @ Playhead") &&
                curve->size() < maximum_cutscene_keys_per_track) {
                const float v =
                    evaluate_sequencer_curve(*curve, st.playhead_seconds, dummy_def);
                curve->push_back(SequencerKeyframe{
                    .time = st.playhead_seconds,
                    .value = v,
                });
                sort_and_sanitize_keyframes(*curve, duration);
                seq_edited = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Auto Tangents")) {
                auto_compute_curve_tangents(*curve);
                seq_edited = true;
            }
            if (!curve->empty()) {
                st.selected_key_index =
                    std::min(st.selected_key_index, curve->size() - 1U);
                int k_idx = static_cast<int>(st.selected_key_index);
                if (ImGui::SliderInt(
                        "Key Index##KeyIdx",
                        &k_idx,
                        0,
                        static_cast<int>(curve->size() - 1U))) {
                    st.selected_key_index = static_cast<std::size_t>(k_idx);
                }
                SequencerKeyframe& kf = (*curve)[st.selected_key_index];
                bool key_time_edited = false;
                key_time_edited |= ImGui::DragFloat(
                    "Key Time##KeyT", &kf.time, 0.02F, 0.0F, duration);
                seq_edited |= ImGui::DragFloat(
                    "Key Value##KeyV", &kf.value, 0.05F);
                if (ImGui::BeginCombo("Interp##KeyInterp", to_token(kf.interp).data())) {
                    for (const CurveInterpMode m : {
                             CurveInterpMode::constant,
                             CurveInterpMode::linear,
                             CurveInterpMode::cubic_hermite,
                             CurveInterpMode::cubic_bezier}) {
                        if (ImGui::Selectable(to_token(m).data(), kf.interp == m)) {
                            kf.interp = m;
                            seq_edited = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (kf.interp == CurveInterpMode::cubic_hermite ||
                    kf.interp == CurveInterpMode::cubic_bezier) {
                    seq_edited |= ImGui::DragFloat2(
                        "Tangents##KeyTan", &kf.in_tangent, 0.05F);
                }
                if (kf.interp == CurveInterpMode::cubic_bezier) {
                    seq_edited |= ImGui::DragFloat2(
                        "Weights##KeyWgt", &kf.in_weight, 0.01F, 0.01F, 0.99F);
                }
                if (key_time_edited) {
                    sort_and_sanitize_keyframes(*curve, duration);
                    seq_edited = true;
                }
                if (ImGui::SmallButton("Delete Key##DelKey")) {
                    curve->erase(
                        curve->begin() +
                        static_cast<std::ptrdiff_t>(st.selected_key_index));
                    st.selected_key_index = 0U;
                    seq_edited = true;
                }
            }
        }
    }
    ImGui::EndChild();

    if (seq_edited) {
        st.dirty = true;
        st.needs_preview_eval = st.preview_active;
    }

    ImGui::End();
}

void EditorUi::build_performance(
    SceneDocument& scene,
    Renderer& renderer,
    const PerformanceSnapshot& performance) {
    ImGui::SetNextWindowSize(
        ImVec2(380.0F, 360.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Renderer")) {
        ImGui::End();
        return;
    }
    ui_kit_.panel_header(
        EditorIcon::renderer,
        "RENDERER",
        "Live frame health and output settings");

    char average_fps[32]{};
    char low_fps[32]{};
    char cpu_time[32]{};
    char gpu_time[32]{};
    std::snprintf(
        average_fps,
        sizeof(average_fps),
        "%.1f FPS",
        performance.average_fps);
    std::snprintf(
        low_fps,
        sizeof(low_fps),
        "%.1f FPS",
        performance.one_percent_low_fps);
    if (performance.average.cpu_frame_ms >= 0.0) {
        std::snprintf(
            cpu_time,
            sizeof(cpu_time),
            "%.3f ms",
            performance.average.cpu_frame_ms);
    } else {
        std::snprintf(cpu_time, sizeof(cpu_time), "N/A");
    }
    if (performance.average.gpu_frame_ms >= 0.0) {
        std::snprintf(
            gpu_time,
            sizeof(gpu_time),
            "%.3f ms",
            performance.average.gpu_frame_ms);
    } else {
        std::snprintf(gpu_time, sizeof(gpu_time), "N/A");
    }

    if (ImGui::BeginTable(
            "PerformanceCards",
            2,
            ImGuiTableFlags_SizingStretchSame |
                ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();
        static_cast<void>(ImGui::TableSetColumnIndex(0));
        ui_kit_.metric_card(
            "AverageFps",
            "AVERAGE",
            average_fps,
            EditorTone::success);
        static_cast<void>(ImGui::TableSetColumnIndex(1));
        ui_kit_.metric_card(
            "LowFps",
            "1% LOW",
            low_fps,
            EditorTone::info);
        ImGui::TableNextRow();
        static_cast<void>(ImGui::TableSetColumnIndex(0));
        ui_kit_.metric_card(
            "CpuTime",
            "CPU FRAME",
            cpu_time,
            EditorTone::warning);
        static_cast<void>(ImGui::TableSetColumnIndex(1));
        ui_kit_.metric_card(
            "GpuTime",
            "GPU FRAME",
            gpu_time,
            EditorTone::purple);
        ImGui::EndTable();
    }

    auto lighting = renderer.lighting_settings();
    if (ImGui::BeginCombo("Tone mapper", to_token(renderer.tone_mapper()).data())) {
        for (const auto mapper : {ToneMapper::reinhard, ToneMapper::gt7}) {
            if (ImGui::Selectable(to_token(mapper).data(), renderer.tone_mapper() == mapper) &&
                mapper != renderer.tone_mapper() && renderer.set_tone_mapper(mapper)) {
                checkpoint_scene(scene);
                scene.tone_mapper = mapper;
                scene.mark_content_changed();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Subsurface", to_token(renderer.subsurface_mode()).data())) {
        for (const auto mode : {SubsurfaceMode::off, SubsurfaceMode::full_resolution}) {
            if (ImGui::Selectable(to_token(mode).data(), renderer.subsurface_mode() == mode) &&
                mode != renderer.subsurface_mode() && renderer.set_subsurface_mode(mode)) {
                checkpoint_scene(scene);
                scene.subsurface = mode;
                scene.mark_content_changed();
            }
        }
        ImGui::EndCombo();
    }
    auto fog = renderer.fog_settings();
    ImGui::BeginDisabled(!renderer.fog_supported());
    bool fog_changed = ImGui::Checkbox("Atmospheric fog", &fog.enabled);
    if (fog.enabled) {
        fog_changed |= ImGui::ColorEdit3("Fog color", fog.color.data(),
            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        fog_changed |= ImGui::DragFloat("Fog density", &fog.density,
            0.0001F, 0.0F, 1.0F, "%.4f", ImGuiSliderFlags_AlwaysClamp);
        fog_changed |= ImGui::DragFloat("Fog base height", &fog.base_height,
            0.1F, -100'000.0F, 100'000.0F, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        fog_changed |= ImGui::DragFloat("Fog height falloff", &fog.height_falloff,
            0.001F, 0.0F, 10.0F, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        fog_changed |= ImGui::DragFloat("Fog start distance", &fog.start_distance,
            1.0F, 0.0F, std::nextafter(fog.maximum_distance, 0.0F),
            "%.1f", ImGuiSliderFlags_AlwaysClamp);
        fog_changed |= ImGui::DragFloat("Fog maximum distance", &fog.maximum_distance,
            1.0F, std::nextafter(fog.start_distance, 100'001.0F), 100'000.0F,
            "%.1f", ImGuiSliderFlags_AlwaysClamp);
        fog_changed |= ImGui::SliderFloat("Fog maximum opacity", &fog.maximum_opacity,
            0.0F, 1.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::EndDisabled();
    if (!renderer.fog_supported()) ImGui::TextDisabled("Atmospheric fog unavailable on this GPU");
    if (fog_changed) {
        for (auto& channel : fog.color) channel = std::clamp(channel, 0.0F, 16.0F);
        if (renderer.set_fog_settings(fog)) {
            checkpoint_scene(scene);
            scene.fog = fog;
            scene.mark_content_changed();
        }
    }
    auto motion_blur = renderer.motion_blur_settings();
    bool motion_blur_changed = false;
    if (ImGui::BeginCombo(
            "Motion blur", to_token(motion_blur.mode).data())) {
        for (const auto mode : {
                 MotionBlurMode::off, MotionBlurMode::camera,
                 MotionBlurMode::objects, MotionBlurMode::all}) {
            if (ImGui::Selectable(
                    to_token(mode).data(), motion_blur.mode == mode)) {
                motion_blur.mode = mode;
                motion_blur_changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "Depth-aware TileMax/NeighborMax blur. Active mode supports Off, SMAA 1x, or FSR Native AA.");
    }
    if (motion_blur.mode != MotionBlurMode::off) {
        if (ImGui::BeginCombo(
                "Motion blur quality", to_token(motion_blur.quality).data())) {
            for (const auto quality : {
                     MotionBlurQuality::low, MotionBlurQuality::high}) {
                if (ImGui::Selectable(
                        to_token(quality).data(), motion_blur.quality == quality)) {
                    motion_blur.quality = quality;
                    motion_blur_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo(
                "Motion blur shutter", to_token(motion_blur.shutter).data())) {
            for (const auto shutter : {
                     MotionBlurShutter::centered,
                     MotionBlurShutter::trailing}) {
                if (ImGui::Selectable(
                        to_token(shutter).data(), motion_blur.shutter == shutter)) {
                    motion_blur.shutter = shutter;
                    motion_blur_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        motion_blur_changed |= ImGui::SliderFloat(
            "Shutter angle", &motion_blur.shutter_angle_degrees,
            0.0F, 360.0F, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
        motion_blur_changed |= ImGui::SliderFloat(
            "Maximum blur radius", &motion_blur.maximum_radius_pixels,
            1.0F, 64.0F, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
        bool camera_fade = motion_blur.camera_fade_end >
            motion_blur.camera_fade_start;
        if (ImGui::Checkbox("Camera depth fade", &camera_fade)) {
            motion_blur.camera_fade_start = camera_fade ? 100.0F : 0.0F;
            motion_blur.camera_fade_end = camera_fade ? 500.0F : 0.0F;
            motion_blur_changed = true;
        }
        if (camera_fade) {
            motion_blur_changed |= ImGui::DragFloat(
                "Camera fade start", &motion_blur.camera_fade_start,
                1.0F, 0.0F, motion_blur.camera_fade_end - 0.01F,
                "%.1f m", ImGuiSliderFlags_AlwaysClamp);
            motion_blur_changed |= ImGui::DragFloat(
                "Camera fade end", &motion_blur.camera_fade_end,
                1.0F, motion_blur.camera_fade_start + 0.01F, 10'000.0F,
                "%.1f m", ImGuiSliderFlags_AlwaysClamp);
        }
    }
    if (motion_blur_changed) {
        static_cast<void>(renderer.set_motion_blur_settings(motion_blur));
    }
    auto exposure = renderer.exposure_settings();
    bool exposure_changed = false;
    if (ImGui::BeginCombo("Exposure", to_token(exposure.mode).data())) {
        for (const auto mode : {ExposureMode::fixed, ExposureMode::automatic}) {
            if (ImGui::Selectable(to_token(mode).data(), exposure.mode == mode)) {
                exposure.mode = mode;
                exposure_changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Exposure metering", to_token(exposure.metering).data())) {
        for (const auto metering : {ExposureMetering::uniform, ExposureMetering::center}) {
            if (ImGui::Selectable(to_token(metering).data(), exposure.metering == metering)) {
                exposure.metering = metering;
                exposure_changed = true;
            }
        }
        ImGui::EndCombo();
    }
    exposure_changed |= ImGui::DragFloat(
        "Exposure EV", &exposure.compensation_ev, 0.05F, -24.0F, 24.0F, "%.2f",
        ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat);
    if (exposure.mode == ExposureMode::automatic) {
        exposure_changed |= ImGui::DragFloat(
            "Exposure min EV", &exposure.minimum_ev, 0.05F, -24.0F,
            exposure.maximum_ev, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        exposure_changed |= ImGui::DragFloat(
            "Exposure max EV", &exposure.maximum_ev, 0.05F,
            exposure.minimum_ev, 24.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        exposure_changed |= ImGui::DragFloat(
            "Low percentile", &exposure.low_percentile, 0.001F, 0.0F,
            std::nextafter(exposure.high_percentile, 0.0F), "%.3f",
            ImGuiSliderFlags_AlwaysClamp);
        exposure_changed |= ImGui::DragFloat(
            "High percentile", &exposure.high_percentile, 0.001F,
            std::nextafter(exposure.low_percentile, 1.0F), 1.0F, "%.3f",
            ImGuiSliderFlags_AlwaysClamp);
        exposure_changed |= ImGui::DragFloat(
            "Brighten seconds", &exposure.brighten_seconds, 0.01F, 0.01F, 60.0F,
            "%.2f", ImGuiSliderFlags_AlwaysClamp);
        exposure_changed |= ImGui::DragFloat(
            "Darken seconds", &exposure.darken_seconds, 0.01F, 0.01F, 60.0F,
            "%.2f", ImGuiSliderFlags_AlwaysClamp);
    }
    if (exposure_changed && renderer.set_exposure_settings(exposure)) {
        scene.exposure = exposure;
        scene.mark_content_changed();
    }
    ImGui::TextDisabled(
        "Actual %s, %.3fx, target %.3fx, luminance %.4f",
        to_token(renderer.exposure_mode_actual()).data(),
        renderer.exposure_mode_actual() == ExposureMode::fixed
            ? fixed_exposure_multiplier(renderer.exposure_settings())
            : renderer.exposure_state().exposure,
        renderer.exposure_mode_actual() == ExposureMode::fixed
            ? fixed_exposure_multiplier(renderer.exposure_settings())
            : renderer.exposure_state().target_exposure,
        renderer.exposure_state().average_luminance);

    ColorGrading grading = renderer.color_grading();
    ColorGradingLutSize grading_size = renderer.color_grading_lut_size();
    bool grading_changed = false;
    if (ImGui::BeginCombo("Color grading", to_token(grading).data())) {
        for (const auto mode : {
                 ColorGrading::off, ColorGrading::trilinear,
                 ColorGrading::tetrahedral}) {
            if (ImGui::Selectable(to_token(mode).data(), grading == mode)) {
                grading = mode;
                grading_changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo(
            "Grading LUT size",
            grading_size == ColorGradingLutSize::size_64 ? "64" : "32")) {
        for (const auto size : {
                 ColorGradingLutSize::size_32, ColorGradingLutSize::size_64}) {
            const char* label = size == ColorGradingLutSize::size_64 ? "64" : "32";
            if (ImGui::Selectable(label, grading_size == size)) {
                grading_size = size;
                grading_changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (grading_changed) {
        static_cast<void>(renderer.set_color_grading(grading, grading_size));
    }
    ImGui::TextDisabled(
        "Actual %s, identity %u^3 RGBA16F",
        to_token(renderer.color_grading_actual()).data(),
        grading_lut_size(renderer.color_grading_lut_size()));
    if (ImGui::BeginCombo("Depth prepass", to_token(renderer.depth_prepass()).data())) {
        for (const auto prepass : {DepthPrepass::off, DepthPrepass::partial, DepthPrepass::full}) {
            if (ImGui::Selectable(to_token(prepass).data(), renderer.depth_prepass() == prepass))
                static_cast<void>(renderer.set_depth_prepass(prepass));
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Partial covers terrain and primitives; full also includes imported opaque models.");
    if (renderer.depth_prepass_actual() != renderer.depth_prepass()) {
        ImGui::TextDisabled("Actual %s (promoted for hardware RT)",
                            to_token(renderer.depth_prepass_actual()).data());
    }
    if (ImGui::BeginCombo("Model LOD fade", to_token(renderer.lod_fade_pattern()).data())) {
        for (const auto pattern : {LodFadePattern::off, LodFadePattern::bayer4, LodFadePattern::bayer8}) {
            if (ImGui::Selectable(to_token(pattern).data(), renderer.lod_fade_pattern() == pattern))
                static_cast<void>(renderer.set_lod_fade(pattern, renderer.lod_fade_seconds()));
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Ordered Bayer transitions for imported models; terrain LOD remains discrete.");
    float lod_fade_seconds = renderer.lod_fade_seconds();
    if (ImGui::DragFloat("LOD fade seconds", &lod_fade_seconds, 0.01F, 0.05F, 2.0F, "%.2f",
                        ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat)) {
        static_cast<void>(renderer.set_lod_fade(renderer.lod_fade_pattern(), lod_fade_seconds));
    }
    bool lighting_changed = ImGui::Checkbox("Dynamic day and night", &lighting.dynamic);
    lighting_changed |= ImGui::Checkbox("Cache individual shadow cascades", &lighting.cascade_cache);
    ImGui::Text("Shadow depth: %s (%s filter; startup setting)",
                renderer.shadow_depth_format().data(), renderer.shadow_linear_filter() ? "linear" : "nearest");
    std::array<char, 32> shadow_label{};
    std::snprintf(shadow_label.data(), shadow_label.size(), "%u x %u",
                  lighting.shadow_resolution, lighting.shadow_resolution);
    if (ImGui::BeginCombo("Shadow resolution", shadow_label.data())) {
        for (const auto resolution : shadow_resolutions) {
            std::snprintf(shadow_label.data(), shadow_label.size(), "%u x %u", resolution, resolution);
            ImGui::BeginDisabled(!renderer.supports_shadow_resolution(resolution));
            if (ImGui::Selectable(shadow_label.data(), lighting.shadow_resolution == resolution)) {
                lighting.shadow_resolution = resolution;
                lighting_changed = true;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Larger maps sharpen shadows and use more GPU memory and rendering time.");
    if (lighting.dynamic)
        ImGui::TextDisabled("4 cascades, %u x %u each", lighting.shadow_resolution / 2,
                            lighting.shadow_resolution / 2);
    lighting_changed |= ImGui::Checkbox("Advance time", &lighting.cycle);
    float hour = static_cast<float>(lighting.hour), day = static_cast<float>(lighting.day_seconds);
    if (ImGui::SliderFloat("Time of day", &hour, 0, 23.999F, "%.2f h")) {
        lighting.hour = hour;
        lighting_changed = true;
    }
    if (ImGui::SliderFloat("Day duration", &day, 10, 3600, "%.0f s")) {
        lighting.day_seconds = day;
        lighting_changed = true;
    }
    lighting_changed |=
        ImGui::SliderFloat("Sky light intensity", &lighting.sky_intensity, 0, 8, "%.2f");
    ImGui::BeginDisabled(!renderer.gi_provisioned() || !lighting.dynamic);
    if (ImGui::Checkbox("Voxel indirect lighting", &lighting.voxel_gi)) {
        lighting_changed = true;
        if (!lighting.voxel_gi && ray_tracing_uses_gi(lighting.ray_tracing)) {
            lighting.ray_tracing = lighting.ray_tracing == RayTracingMode::full
                ? RayTracingMode::shadows
                : RayTracingMode::off;
        }
    }
    lighting_changed |=
        ImGui::SliderFloat("Indirect intensity", &lighting.gi_strength, 0, 2, "%.2f");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!renderer.ray_tracing_supported() || !lighting.dynamic);
    if (ImGui::BeginCombo("Ray tracing", to_token(lighting.ray_tracing).data())) {
        for (const auto mode : {RayTracingMode::off, RayTracingMode::shadows,
                                RayTracingMode::gi, RayTracingMode::full}) {
            const bool needs_gi = ray_tracing_uses_gi(mode) && !renderer.gi_provisioned();
            ImGui::BeginDisabled(needs_gi);
            if (ImGui::Selectable(to_token(mode).data(), lighting.ray_tracing == mode)) {
                lighting.ray_tracing = mode;
                if (ray_tracing_uses_gi(mode) && renderer.gi_provisioned()) {
                    lighting.voxel_gi = true;
                }
                lighting_changed = true;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Inline VK_KHR_ray_query sun shadows and hybrid near-field RT + far-field voxel GI.");
    if (lighting_changed) {
        if (!lighting.dynamic) {
            lighting.voxel_gi = false;
            lighting.ray_tracing = RayTracingMode::off;
        }
        if (renderer.set_lighting_settings(lighting)) {
            checkpoint_scene(scene);
            scene.lighting = lighting;
            scene.mark_content_changed();
        }
    }
    ImGui::TextDisabled(
        "RT actual %s (%s), %u BLAS, %u TLAS instances, %.2f MiB AS, TLAS %.3f ms",
        to_token(renderer.ray_tracing_mode_actual()).data(),
        renderer.ray_tracing_supported()
            ? (renderer.ray_tracing_provisioned() ? "provisioned" : "supported")
            : "unavailable",
        renderer.lighting_statistics().blas_count,
        renderer.lighting_statistics().tlas_instances,
        static_cast<double>(renderer.lighting_statistics().acceleration_structure_bytes) /
            (1024.0 * 1024.0),
        performance.average.gpu_tlas_ms);
    ImGui::Text("GI geometry %.3f ms, lighting %.3f ms", performance.average.gpu_voxel_geometry_ms,
                performance.average.gpu_voxel_light_ms);
    ImGui::Text("GI pending regions: %u", renderer.lighting_statistics().pending_bricks);
    auto visibility = renderer.visibility_settings();
    bool visibility_changed = ImGui::Checkbox("Frustum culling", &visibility.frustum);
    visibility_changed |= ImGui::Checkbox("Occlusion culling", &visibility.occlusion);
    visibility_changed |= ImGui::Checkbox("Mesh and terrain LOD", &visibility.lod);
    visibility_changed |= ImGui::Checkbox("Dynamic object shadows", &visibility.dynamic_shadows);
    if (visibility.lod) {
        visibility_changed |= ImGui::SliderFloat("LOD error (pixels)", &visibility.lod_pixel_error,
                                                 .1F, 8.0F, "%.1f");
    }
    if (visibility_changed) {
        renderer.set_visibility_settings(visibility);
    }
    const auto &visible = renderer.visibility_statistics();
    ImGui::Text("Visible: %u / %u (%s)", visible.visible, visible.submitted,
                renderer.visibility_backend());
    ImGui::Text("Rejected: %u frustum, %u occlusion", visible.frustum_rejected,
                visible.occlusion_rejected);
    ImGui::Text("LOD instances: %u / %u / %u / %u", visible.lod_instances[0],
                visible.lod_instances[1], visible.lod_instances[2], visible.lod_instances[3]);
    ImGui::Text("GPU culling %.3f ms, shadows %.3f ms\nGPU postprocess %.3f ms, display %.3f ms, exposure %.3f ms, SSS %.3f ms, motion blur %.3f ms, depth prepass %.3f ms",
                performance.average.gpu_visibility_ms, performance.average.gpu_shadow_ms,
                performance.average.gpu_postprocess_ms, performance.average.gpu_display_ms,
                performance.average.gpu_exposure_ms,
                performance.average.gpu_subsurface_ms,
                performance.average.gpu_motion_blur_ms,
                performance.average.gpu_depth_prepass_ms);
    ui_kit_.section_header(EditorIcon::renderer, "Output Pipeline");
    if (ui_kit_.begin_property_grid("RendererProperties")) {
        ui_kit_.property_row("Present");
        ui_kit_.badge(
            Swapchain::present_mode_name(
                renderer.swapchain().present_mode()),
            EditorTone::info);
        ui_kit_.property_row("Anti-aliasing");
        ImGui::TextUnformatted(
            to_string(renderer.anti_aliasing().method).data());
        ui_kit_.property_row("Quality");
        ImGui::TextUnformatted(
            renderer.anti_aliasing_quality_name().data());
        ui_kit_.end_property_grid();
    }
    if (ui_kit_.action_button(
            "CycleAA",
            EditorIcon::refresh,
            "Cycle AA",
            EditorButtonStyle::secondary)) {
        renderer.cycle_anti_aliasing();
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "IncreaseAAQuality",
            EditorIcon::add,
            "Quality",
            EditorButtonStyle::secondary)) {
        renderer.adjust_anti_aliasing_quality(1);
    }
    ui_kit_.status_text(
        "Editor metrics are isolated from the benchmark executable.",
        EditorTone::neutral);
    ImGui::End();
}

void EditorUi::process_file_commands(
    SceneDocument& scene,
    Renderer& renderer,
    FlyCamera& camera) {
    if (save_requested_) {
        save_requested_ = false;
        if (play_state_ == EditorPlayState::edit) {
            stop_sequencer_preview(scene, &renderer, &camera);
            save_scene(scene);
        }
    }
    if (open_requested_) {
        open_requested_ = false;
        if (play_state_ == EditorPlayState::edit) {
            if (scene_is_dirty(scene)) {
                open_confirmation_requested_ = true;
            } else {
                stop_sequencer_preview(scene, &renderer, &camera);
                if (!pending_open_scene_path_.empty()) {
                    scene_path_ = std::exchange(pending_open_scene_path_, {});
                    scene_file_name_ = scene_path_.filename().string();
                }
                open_scene(scene);
            }
        } else {
            pending_open_scene_path_.clear();
        }
    }
    if (quit_requested_) {
        quit_requested_ = false;
        if (play_state_ != EditorPlayState::edit) {
            stop_play(scene);
        } else {
            stop_sequencer_preview(scene, &renderer, &camera);
        }
        const bool material_dirty =
            material_editor_ != nullptr &&
            material_editor_->dirty;
        if (scene_is_dirty(scene) || material_dirty) {
            quit_confirmation_requested_ = true;
        } else {
            wants_quit_ = true;
        }
    }

    if (open_confirmation_requested_) {
        ImGui::OpenPopup("Open saved scene?");
        open_confirmation_requested_ = false;
    }
    if (ImGui::BeginPopupModal(
            "Open saved scene?",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::filesystem::path& target_scene_path =
            pending_open_scene_path_.empty()
                ? scene_path_
                : pending_open_scene_path_;
        ImGui::TextUnformatted(
            "Discard the current unsaved changes and open:");
        ImGui::TextWrapped(
            "%s",
            target_scene_path.empty()
                ? "(scene path unavailable)"
                : target_scene_path.string().c_str());
        if (ImGui::Button("Discard and open")) {
            stop_sequencer_preview(scene, &renderer, &camera);
            if (!pending_open_scene_path_.empty()) {
                scene_path_ = std::exchange(pending_open_scene_path_, {});
                scene_file_name_ = scene_path_.filename().string();
            }
            open_scene(scene);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_open_scene_path_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (quit_confirmation_requested_) {
        ImGui::OpenPopup("Exit editor?");
        quit_confirmation_requested_ = false;
    }
    if (ImGui::BeginPopupModal(
            "Exit editor?",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "The editor has unsaved scene or material changes.");
        if (ImGui::Button("Save and exit")) {
            if (scene_is_dirty(scene)) {
                save_scene(scene);
            }
            bool material_saved = true;
            if (material_editor_ != nullptr &&
                material_editor_->dirty) {
                std::string error;
                material_saved = save_material_document(
                    *material_editor_,
                    asset_browser_.root(),
                    error);
                if (!material_saved) {
                    material_editor_->error =
                        "Save failed: " + error;
                    material_editor_->open = true;
                } else {
                    saved_material_update_ =
                        make_saved_material_update(*material_editor_);
                }
            }
            if (!scene_is_dirty(scene) && material_saved) {
                wants_quit_ = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard and exit")) {
            wants_quit_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        if (!scene_file_status_.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", scene_file_status_.c_str());
        }
        ImGui::EndPopup();
    }
}

void EditorUi::save_scene(SceneDocument& scene) {
    std::string error;
    static_cast<void>(save_scene_external(scene, error));
}

void EditorUi::open_scene(SceneDocument& scene) {
    stop_sequencer_preview(scene);
    if (scene_path_.empty()) {
        scene_file_status_ =
            "Open failed: scene path is unavailable.";
        return;
    }
    SceneDocument loaded;
    std::string error;
    if (!load_scene_file(scene_path_, loaded, error)) {
        scene_file_status_ = "Open failed: " + error;
        return;
    }
    scene = std::move(loaded);
    pending_model_path_.clear();
    pending_model_name_.clear();
    pending_model_parent_ = {};
    saved_content_revision_ = scene.content_revision();
    saved_structure_revision_ = scene.structure_revision();
    scene_file_status_ =
        "Opened " + scene_path_.string();
    scene_replaced_ = true;
    startup_camera_requested_ = true;
    selected_node_ = {};
    pending_delete_ = {};
    delete_confirmation_ = {};
    pending_reparent_child_ = {};
    pending_reparent_parent_ = {};
    edit_scene_snapshot_.reset();
    inspector_edit_snapshot_.reset();
    undo_history_.clear();
    redo_history_.clear();
    play_state_ = EditorPlayState::edit;
    external_game_window_open_ = false;
    hierarchy_revision_ = 0U;
    name_buffer_node_ = {};
}

bool EditorUi::scene_is_dirty(
    const SceneDocument& scene) const noexcept {
    if (saved_content_revision_ == 0U) {
        return false;
    }
    const SceneDocument& author_scene =
        (sequencer_ui_ != nullptr && sequencer_ui_->baseline_scene.has_value())
            ? *sequencer_ui_->baseline_scene
            : scene;
    return author_scene.content_revision() != saved_content_revision_ ||
        author_scene.structure_revision() !=
            saved_structure_revision_;
}

void EditorUi::push_undo_snapshot(SceneDocument snapshot) {
    if (undo_history_.size() == maximum_history_entries) {
        undo_history_.erase(undo_history_.begin());
    }
    undo_history_.push_back(std::move(snapshot));
    redo_history_.clear();
}

void EditorUi::checkpoint_scene(const SceneDocument& scene) {
    push_undo_snapshot(scene);
}

bool EditorUi::undo_scene(SceneDocument& scene) {
    stop_sequencer_preview(scene);
    if (play_state_ != EditorPlayState::edit || undo_history_.empty()) {
        return false;
    }
    if (redo_history_.size() == maximum_history_entries) {
        redo_history_.erase(redo_history_.begin());
    }
    redo_history_.push_back(std::move(scene));
    scene = std::move(undo_history_.back());
    undo_history_.pop_back();
    scene_replaced_ = true;
    inspector_edit_snapshot_.reset();
    selected_node_ = {};
    hierarchy_revision_ = 0U;
    name_buffer_node_ = {};
    scene_file_status_ = "Undid scene edit.";
    return true;
}

bool EditorUi::redo_scene(SceneDocument& scene) {
    stop_sequencer_preview(scene);
    if (play_state_ != EditorPlayState::edit || redo_history_.empty()) {
        return false;
    }
    if (undo_history_.size() == maximum_history_entries) {
        undo_history_.erase(undo_history_.begin());
    }
    undo_history_.push_back(std::move(scene));
    scene = std::move(redo_history_.back());
    redo_history_.pop_back();
    scene_replaced_ = true;
    inspector_edit_snapshot_.reset();
    selected_node_ = {};
    hierarchy_revision_ = 0U;
    name_buffer_node_ = {};
    scene_file_status_ = "Redid scene edit.";
    return true;
}

bool EditorUi::save_scene_external(
    SceneDocument& scene,
    std::string& error) {
    stop_sequencer_preview(scene);
    error.clear();
    if (play_state_ != EditorPlayState::edit) {
        error = "Scene save is available only in Edit mode.";
    } else if (scene_path_.empty()) {
        error = "Scene path is unavailable.";
    } else if (!save_scene_file(scene, scene_path_, error)) {
        // save_scene_file supplies a detailed error.
    } else {
        saved_content_revision_ = scene.content_revision();
        saved_structure_revision_ = scene.structure_revision();
        scene_file_status_ = "Saved " + scene_path_.string();
        return true;
    }
    scene_file_status_ = "Save failed: " + error;
    return false;
}

bool EditorUi::queue_import_external(
    const std::filesystem::path& source,
    const std::filesystem::path& destination_directory,
    std::string& error) {
    error.clear();
    if (play_state_ != EditorPlayState::edit) {
        error = "Asset import is available only in Edit mode.";
        return false;
    }
    if (!asset_import_enabled_) {
        error = "Asset import requires a writable project checkout.";
        return false;
    }
    if (import_worker_ == nullptr || import_worker_->failed()) {
        error = "The asset import worker is unavailable.";
        return false;
    }
    if (!asset_source_is_supported(source)) {
        error = "The source format is not supported by the asset importer.";
        return false;
    }
    AssetImportWorker::Batch batch{
        .sources = {source},
        .options = ImportAssetOptions{
            .asset_root = asset_browser_.root(),
            .destination_directory = destination_directory,
        },
    };
    if (!import_worker_->submit(std::move(batch))) {
        error = "The bounded asset import queue is full.";
        return false;
    }
    if (import_ui_ != nullptr) {
        import_ui_->status = "MCP asset import queued: " + source.string();
    }
    return true;
}

std::string_view EditorUi::asset_import_status() const noexcept {
    return import_ui_ != nullptr
        ? std::string_view(import_ui_->status)
        : std::string_view{};
}

void EditorUi::apply_deferred_scene_commands(
    SceneDocument& scene) {
    if (play_state_ == EditorPlayState::edit &&
        (scene.valid(pending_duplicate_) ||
         (pending_reparent_child_ && pending_reparent_parent_) ||
         pending_delete_)) {
        stop_sequencer_preview(scene);
    }
    if (play_state_ == EditorPlayState::edit &&
        scene.valid(pending_duplicate_)) {
        static_cast<void>(select_node(scene, pending_duplicate_));
        std::uint32_t duplicated_nodes = 0U;
        std::string duplicate_error;
        if (!duplicate_selection(
                scene, duplicated_nodes, duplicate_error)) {
            asset_error_ = std::move(duplicate_error);
        }
    }
    pending_duplicate_ = {};
    if (play_state_ == EditorPlayState::edit &&
        pending_reparent_child_ &&
        pending_reparent_parent_) {
        checkpoint_scene(scene);
        static_cast<void>(scene.reparent(
            pending_reparent_child_,
            pending_reparent_parent_));
    }
    pending_reparent_child_ = {};
    pending_reparent_parent_ = {};
    if (play_state_ == EditorPlayState::edit &&
        pending_delete_) {
        checkpoint_scene(scene);
        static_cast<void>(scene.destroy_subtree(pending_delete_));
        if (!scene.valid(selected_node_)) {
            selected_node_ = {};
        }
    }
    pending_delete_ = {};
}

void EditorUi::update_camera_input() {
    const float mouse_delta_x = std::exchange(pending_game_mouse_delta_x_, 0.0F);
    const float mouse_delta_y = std::exchange(pending_game_mouse_delta_y_, 0.0F);
    if (play_state_ != EditorPlayState::edit) {
        if (play_state_ != EditorPlayState::playing ||
            game_mouse_window_id_ == 0U) {
            return;
        }
        // Relative motion is independent of ImGui hover/absolute cursor position.
        camera_input_.mouse_delta_x = mouse_delta_x;
        camera_input_.mouse_delta_y = mouse_delta_y;
        int key_count = 0;
        const bool* const keys = SDL_GetKeyboardState(&key_count);
        const std::span<const bool> keyboard = keys != nullptr && key_count > 0
            ? std::span<const bool>(keys, static_cast<std::size_t>(key_count))
            : std::span<const bool>{};
        const auto key_down = [keyboard](const SDL_Scancode scancode) {
            const auto index = static_cast<std::size_t>(scancode);
            return index < keyboard.size() && keyboard[index];
        };
        camera_input_.forward = key_down(SDL_SCANCODE_W);
        camera_input_.backward = key_down(SDL_SCANCODE_S);
        camera_input_.left = key_down(SDL_SCANCODE_A);
        camera_input_.right = key_down(SDL_SCANCODE_D);
        camera_input_.up = key_down(SDL_SCANCODE_SPACE);
        camera_input_.boost =
            key_down(SDL_SCANCODE_LSHIFT) || key_down(SDL_SCANCODE_RSHIFT);
        return;
    }
    const bool viewport_active =
        (viewport_hovered_ && viewport_focused_) ||
        (external_viewport_hovered_ && external_viewport_focused_);
    if (!viewport_active) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const bool navigate =
        ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (navigate) {
        camera_input_.mouse_delta_x = io.MouseDelta.x;
        camera_input_.mouse_delta_y = io.MouseDelta.y;
    }
    if (!navigate) {
        return;
    }
    if (io.WantTextInput) {
        return;
    }
    camera_input_.forward = ImGui::IsKeyDown(ImGuiKey_W);
    camera_input_.backward = ImGui::IsKeyDown(ImGuiKey_S);
    camera_input_.left = ImGui::IsKeyDown(ImGuiKey_A);
    camera_input_.right = ImGui::IsKeyDown(ImGuiKey_D);
    camera_input_.up =
        ImGui::IsKeyDown(ImGuiKey_E) || ImGui::IsKeyDown(ImGuiKey_Space);
    camera_input_.down =
        ImGui::IsKeyDown(ImGuiKey_Q) ||
        ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
        ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    camera_input_.boost =
        ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
        ImGui::IsKeyDown(ImGuiKey_RightShift);
}

void EditorUi::set_game_mouse_capture(SDL_Window* const window) noexcept {
    const SDL_WindowID requested_id =
        window != nullptr ? SDL_GetWindowID(window) : 0U;
    game_mouse_capture_requested_ = false;
    if (requested_id == game_mouse_window_id_) {
        return;
    }
    constexpr ImGuiConfigFlags capture_flags =
        ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange |
        ImGuiConfigFlags_NavEnableKeyboard;
    ImGuiIO& io = ImGui::GetIO();
    if (game_mouse_window_id_ != 0U) {
        if (SDL_Window* const previous = SDL_GetWindowFromID(game_mouse_window_id_);
            previous != nullptr && !SDL_SetWindowRelativeMouseMode(previous, false)) {
            std::fprintf(stderr, "Unable to release game mouse: %s\n", SDL_GetError());
            return;
        }
        io.ConfigFlags = (io.ConfigFlags & ~capture_flags) | game_mouse_saved_config_;
        game_mouse_window_id_ = 0U;
    }
    camera_input_ = {};
    pending_game_mouse_delta_x_ = 0.0F;
    pending_game_mouse_delta_y_ = 0.0F;
    if (window == nullptr) {
        if (input_actions_ != nullptr) {
            input_actions_->release_all();
        }
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(window, true)) {
        std::fprintf(stderr, "Unable to capture game mouse: %s\n", SDL_GetError());
        return;
    }
    game_mouse_window_id_ = requested_id;
    game_mouse_saved_config_ = io.ConfigFlags & capture_flags;
    // SDL owns the hidden cursor while playing; ImGui must not show it or
    // activate editor controls with gameplay clicks/Space.
    io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableKeyboard) |
        ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::ClearActiveID();
}

void EditorUi::update_play_viewport_mouse(
    const ImVec2 rect_min,
    const ImVec2 rect_max,
    const bool clicked_left,
    const bool hovered) noexcept {
    if (play_state_ != EditorPlayState::playing ||
        gameplay_session_ == nullptr ||
        !gameplay_session_->active()) {
        play_mouse_pos_px_ = glm::vec2(-1.0F, -1.0F);
        play_mouse_left_down_ = false;
        play_mouse_left_pressed_ = false;
        prev_ui_wants_cursor_ = false;
        return;
    }
    const bool ui_wants_cursor =
        gameplay_session_->game_ui().wants_mouse_cursor();
    if (ui_wants_cursor && game_mouse_window_id_ != 0U) {
        set_game_mouse_capture(nullptr);
    }
    prev_ui_wants_cursor_ = ui_wants_cursor;

    const float rw = rect_max.x - rect_min.x;
    const float rh = rect_max.y - rect_min.y;
    if (ui_wants_cursor && rw > 1.0F && rh > 1.0F) {
        const ImVec2 mp = ImGui::GetMousePos();
        if (mp.x >= rect_min.x && mp.x <= rect_max.x &&
            mp.y >= rect_min.y && mp.y <= rect_max.y) {
            const float nx = (mp.x - rect_min.x) / rw;
            const float ny = (mp.y - rect_min.y) / rh;
            const float vp_w = static_cast<float>(std::max(1U, requested_viewport_extent_.width));
            const float vp_h = static_cast<float>(std::max(1U, requested_viewport_extent_.height));
            play_mouse_pos_px_ = glm::vec2(nx * vp_w, ny * vp_h);
        } else {
            play_mouse_pos_px_ = glm::vec2(-1.0F, -1.0F);
        }
        play_mouse_left_down_ = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (clicked_left || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
            play_mouse_left_pressed_ = true;
        }
    } else {
        play_mouse_pos_px_ = glm::vec2(-1.0F, -1.0F);
        play_mouse_left_down_ = false;
        play_mouse_left_pressed_ = false;
    }
}

void EditorUi::update_game_mouse_capture(const bool viewport_clicked) {
    if (gameplay_session_ != nullptr &&
        gameplay_session_->active() &&
        gameplay_session_->game_ui().wants_mouse_cursor()) {
        return;
    }
    if (play_state_ != EditorPlayState::playing ||
        game_mouse_window_id_ != 0U ||
        (!game_mouse_capture_requested_ && !viewport_clicked)) {
        return;
    }
    SDL_Window* const window = viewport_sdl_window(ImGui::GetWindowViewport());
    if (window == nullptr) {
        return; // A detached ImGui viewport may not have its SDL window yet.
    }
    ImGui::SetWindowFocus();
    if (SDL_GetKeyboardFocus() != window) {
        if (!SDL_RaiseWindow(window)) {
            std::fprintf(stderr, "Unable to focus game viewport: %s\n", SDL_GetError());
        }
        return;
    }
    set_game_mouse_capture(window);
}

GameplayFrameInput EditorUi::gameplay_input(
    const float delta_seconds) noexcept {
    if (play_state_ != EditorPlayState::playing) {
        return {};
    }
    const bool ui_wants_cursor =
        gameplay_session_ != nullptr &&
        gameplay_session_->active() &&
        gameplay_session_->game_ui().wants_mouse_cursor();
    const bool ui_handles_esc =
        gameplay_session_ != nullptr &&
        gameplay_session_->active() &&
        gameplay_session_->game_ui().handles_escape_key();
    const glm::vec2 mouse_px = play_mouse_pos_px_;
    const bool left_down = play_mouse_left_down_;
    const bool left_pressed = std::exchange(play_mouse_left_pressed_, false);
    const bool cutscene_active =
        gameplay_session_ != nullptr &&
        gameplay_session_->active() &&
        gameplay_session_->cutscene_player().active();
    const bool cutscene_captures =
        cutscene_active &&
        gameplay_session_->cutscene_player().captures_input();
    if (input_actions_ != nullptr) {
        if (ui_wants_cursor) {
            if (input_actions_->context() != InputContext::ui) {
                input_actions_->set_context(InputContext::ui);
            }
        } else if (cutscene_captures) {
            if (input_actions_->context() != InputContext::cutscene) {
                input_actions_->set_context(InputContext::cutscene);
            }
        } else if (input_actions_->context() == InputContext::ui ||
                   input_actions_->context() == InputContext::cutscene) {
            input_actions_->set_context(InputContext::gameplay);
        }
        input_actions_->begin_frame(delta_seconds);
        if (!ui_handles_esc && !cutscene_active &&
            input_actions_->action_pressed(InputAction::pause)) {
            set_game_mouse_capture(nullptr);
            play_state_ = EditorPlayState::paused;
            return {};
        }
        if (ui_wants_cursor) {
            return GameplayFrameInput{
                .mouse_pos_px = mouse_px,
                .mouse_left_down = left_down,
                .mouse_left_pressed = left_pressed,
                .display_fps = play_display_fps_,
            };
        }
        if (game_mouse_window_id_ == 0U &&
            !input_actions_->gamepad_state().connected) {
            return GameplayFrameInput{
                .display_fps = play_display_fps_,
            };
        }
        const glm::vec2 move = input_actions_->move_axis();
        const glm::vec2 look = input_actions_->look_delta();
        return GameplayFrameInput{
            .character = CharacterInput{
                .move_axis = move,
                .forward = move.y > 0.1F,
                .backward = move.y < -0.1F,
                .left = move.x < -0.1F,
                .right = move.x > 0.1F,
                .jump = input_actions_->action_down(InputAction::jump),
                .run = input_actions_->action_down(InputAction::run),
                .interact = input_actions_->action_pressed(InputAction::interact),
            },
            .mouse_delta_x = look.x,
            .mouse_delta_y = look.y,
            .mouse_pos_px = mouse_px,
            .mouse_left_down = left_down,
            .mouse_left_pressed = left_pressed,
            .display_fps = play_display_fps_,
        };
    }
    return GameplayFrameInput{
        .character = CharacterInput{
            .forward = camera_input_.forward,
            .backward = camera_input_.backward,
            .left = camera_input_.left,
            .right = camera_input_.right,
            .jump = camera_input_.up,
            .run = camera_input_.boost,
        },
        .mouse_delta_x = camera_input_.mouse_delta_x,
        .mouse_delta_y = camera_input_.mouse_delta_y,
        .mouse_pos_px = mouse_px,
        .mouse_left_down = left_down,
        .mouse_left_pressed = left_pressed,
        .display_fps = play_display_fps_,
    };
}

void EditorUi::start_play(SceneDocument& scene) {
    stop_sequencer_preview(scene);
    if (play_state_ != EditorPlayState::edit) {
        return;
    }
    if (import_activity_pending()) {
        asset_error_ =
            "Close or finish asset import before entering Play.";
        return;
    }
    if (input_actions_ != nullptr) {
        input_actions_->release_all();
        input_actions_->set_context(InputContext::gameplay);
    }
    edit_scene_snapshot_ = scene;
    play_state_ = EditorPlayState::playing;
    game_mouse_capture_requested_ = true;
}

void EditorUi::stop_play(SceneDocument& scene) {
    if (play_state_ == EditorPlayState::edit) {
        return;
    }
    set_game_mouse_capture(nullptr);
    if (audio_engine_ != nullptr) {
        audio_engine_->stop_gameplay_audio();
    }
    if (input_actions_ != nullptr) {
        input_actions_->stop_rumble();
        input_actions_->release_all();
    }
    if (edit_scene_snapshot_) {
        scene = std::move(*edit_scene_snapshot_);
        edit_scene_snapshot_.reset();
        scene_replaced_ = true;
    }
    play_state_ = EditorPlayState::edit;
    external_game_window_open_ = false;
    hierarchy_revision_ = 0U;
    selected_node_ = {};
}

void EditorUi::request_viewport_extent(
    const VkExtent2D extent,
    const std::chrono::steady_clock::time_point now) noexcept {
    if (same_extent(extent, pending_viewport_extent_)) {
        return;
    }
    pending_viewport_extent_ = extent;
    viewport_extent_changed_at_ = now;
}

} // namespace vcr
