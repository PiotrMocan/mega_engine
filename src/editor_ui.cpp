#include "editor_ui.hpp"

#include "asset_importer.hpp"
#include "editor_scene_tools.hpp"
#include "renderer.hpp"
#include "runtime_assets.hpp"
#include "scene_io.hpp"
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

struct MaterialEditorUiState {
    nlohmann::json document{};
    std::filesystem::path relative_path{};
    std::filesystem::path absolute_path{};
    std::array<char, 256U> name{};
    PbrMaterial material{};
    MaterialTessellation tessellation{};
    bool has_displacement_texture = false;
    std::string error{};
    std::string status{};
    bool open = false;
    bool loaded = false;
    bool dirty = false;
    bool close_confirmation_requested = false;
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
    const float height) noexcept {
    return glm::perspective(
        glm::radians(65.0F),
        width / std::max(height, 1.0F),
        0.1F,
        500.0F);
}

[[nodiscard]] std::optional<EditorViewportRay> viewport_ray(
    const ImVec2 mouse,
    const ImVec2 minimum,
    const ImVec2 maximum,
    const FlyCamera& camera) noexcept {
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
        editor_projection(width, height) * camera.view_matrix());
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
                error = "Material subsurface profile must be none, narrow, medium or wide.";
                return false;
            }
            const auto profile = parse_subsurface_profile(
                pbr["subsurface_profile"].get_ref<const std::string&>());
            if (!profile) {
                error = "Material subsurface profile must be none, narrow, medium or wide.";
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

        state.name.fill('\0');
        std::memcpy(state.name.data(), name.data(), name.size());
        state.document = std::move(document);
        state.relative_path = relative_path.lexically_normal();
        state.absolute_path = std::move(absolute_path);
        state.material = sanitize_pbr_material(material);
        state.tessellation = tessellation;
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
        if (state.has_displacement_texture) {
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
    Renderer& renderer) {
    if (context_ != nullptr || main_window == nullptr) {
        fatal("Editor UI was initialized with invalid state.");
    }
    context_ = &context;
    main_window_ = main_window;
    import_worker_ = std::make_unique<AssetImportWorker>();
    import_ui_ = std::make_unique<AssetImportUiState>();
    external_drop_ = std::make_unique<ExternalAssetDropState>();
    material_editor_ =
        std::make_unique<MaterialEditorUiState>();
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
    if (const auto asset_root = find_runtime_asset_root()) {
        if (asset_browser_.initialize(
                asset_root->path, asset_error_)) {
            asset_import_enabled_ =
                asset_root->project_owned;
            scene_path_ =
                asset_browser_.root() /
                canonical_scene_relative_path;
            scene_file_name_ =
                scene_path_.filename().string();
            scene_file_status_ =
                "Game scene; Save updates assets/scenes/editor_scene.json";
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
}

void EditorUi::process_event(const SDL_Event& event) {
    if (context_ != nullptr) {
        static_cast<void>(ImGui_ImplSDL3_ProcessEvent(&event));
        if (event.type == SDL_EVENT_KEY_DOWN &&
            !event.key.repeat) {
            if (event.key.key == SDLK_F5) {
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
            start_play(scene);
        } else {
            stop_play(scene);
        }
        toggle_play_requested_ = false;
    }
    if (toggle_external_window_requested_ &&
        platform_windows_enabled_ &&
        play_state_ != EditorPlayState::edit) {
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
            ImGui::DockBuilderDockWindow("Assets", bottom);
            ImGui::DockBuilderDockWindow("Scene View", center);
            ImGui::DockBuilderFinish(dockspace_id);
        }
        default_layout_initialized_ = true;
        reset_layout_requested_ = false;
    }
    build_main_menu(scene);
    build_toolbar(scene);
    build_hierarchy(scene);
    build_inspector(scene);
    build_viewport(scene, renderer, camera, now);
    build_assets(scene, renderer);
    build_landscape(scene, renderer);
    if (import_dialog_requested_) {
        queue_import_sources({});
        import_dialog_requested_ = false;
    }
    build_import_window();
    build_import_results();
    build_material_editor(renderer);
    build_performance(scene, renderer, performance);
    build_external_game_window(now);
    process_file_commands(scene);
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

void EditorUi::build_main_menu(SceneDocument& scene) {
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
            open_requested_ = true;
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
        const SceneNodeId parent =
            scene.valid(selected_node_)
            ? selected_node_
            : SceneNodeId{};
        if (ImGui::MenuItem(
                "Add folder",
                nullptr,
                false,
                play_state_ == EditorPlayState::edit)) {
            checkpoint_scene(scene);
            selected_node_ = scene.create_node(
                "Folder", SceneNodeKind::folder, parent);
        }
        if (ImGui::MenuItem(
                "Add cube",
                nullptr,
                false,
                play_state_ == EditorPlayState::edit)) {
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
        if (ImGui::MenuItem("Add ocean", nullptr, false,
                            play_state_ == EditorPlayState::edit)) {
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
            checkpoint_scene(scene);
            const SceneNodeId light = scene.create_node(
                "Spot Light", SceneNodeKind::spot_light, parent);
            if (auto node = scene.node(light)) {
                node->get().transform.translation = glm::vec3(0.0F, 2.0F, -5.0F);
                scene.mark_content_changed();
            }
            selected_node_ = light;
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
        ImGui::Separator();
        if (ImGui::MenuItem("Reset editor layout")) {
            reset_layout_requested_ = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void EditorUi::build_toolbar(SceneDocument& scene) {
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

void EditorUi::build_inspector(SceneDocument& scene) {
    ImGui::SetNextWindowSize(
        ImVec2(340.0F, 570.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }
    const auto selected = scene.node(selected_node_);
    if (!selected) {
        ui_kit_.panel_header(
            EditorIcon::eye,
            "INSPECTOR",
            "Properties and components");
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

    const bool editable = play_state_ == EditorPlayState::edit;
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
                             SubsurfaceProfile::medium, SubsurfaceProfile::wide}) {
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
        "LMB SELECT  /  RMB + WASD  /  F FOCUS",
        EditorTone::purple);

    const float image_width = image_maximum.x - image_minimum.x;
    const float image_height = image_maximum.y - image_minimum.y;
    const glm::mat4 view = camera.view_matrix();
    const glm::mat4 projection = editor_projection(
        image_width, image_height);
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

    bool gizmo_over = false;
    bool gizmo_using = false;
    if (editable && scene.valid(selected_node_)) {
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
            io.MousePos, image_minimum, image_maximum, camera);
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
        const bool external_viewport_activated =
            ImGui::IsItemClicked(ImGuiMouseButton_Left) ||
            ImGui::IsItemClicked(ImGuiMouseButton_Right);
        external_viewport_hovered_ = ImGui::IsItemHovered();
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
        selected_asset_->kind == AssetEntryKind::model;
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
            "Select an imported .vcrasset.json model.");
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
                           entry.kind ==
                               AssetEntryKind::model) {
                    static_cast<void>(add_model_asset_to_scene(
                        scene,
                        renderer,
                        entry.relative_display,
                        entry.display_name));
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
                if (entry.kind == AssetEntryKind::model &&
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
    Renderer& renderer) {
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
    if (!valid_scene_asset_path(relative_path)) {
        asset_error_ =
            "Only an imported .vcrasset.json model can be added "
            "to the scene.";
        return false;
    }

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
        "FBX, OBJ, glTF/GLB, DAE, PLY, STL, 3DS and images");
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
    snapshot = MaterialEditorSnapshot{
        .relative_path = state.relative_path.generic_string(),
        .name = std::string(state.name.data()),
        .displacement_texture = state.has_displacement_texture
            ? state.document["textures"]["displacement"].get<std::string>()
            : std::string{},
        .material = state.material,
        .tessellation = state.tessellation,
    };
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
    snapshot = MaterialEditorSnapshot{
        .relative_path = state.relative_path.generic_string(),
        .name = std::string(state.name.data()),
        .displacement_texture =
            state.document["textures"]["displacement"].get<std::string>(),
        .material = state.material,
        .tessellation = state.tessellation,
    };
    if (material_editor_ != nullptr && material_editor_->loaded &&
        material_editor_->relative_path == state.relative_path) {
        const bool was_open = material_editor_->open;
        *material_editor_ = std::move(state);
        material_editor_->open = was_open;
    }
    saved_material_update_ = SavedMaterialUpdate{
        .relative_path = snapshot.relative_path,
        .material = snapshot.material,
        .tessellation = snapshot.tessellation,
    };
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
                if (ImGui::BeginCombo(
                        "##MaterialModel", to_token(material_editor_->material.model).data())) {
                    for (const auto model : {MaterialModel::standard, MaterialModel::unlit}) {
                        if (ImGui::Selectable(
                                to_token(model).data(), material_editor_->material.model == model)) {
                            changed |= material_editor_->material.model != model;
                            material_editor_->material.model = model;
                        }
                    }
                    ImGui::EndCombo();
                }
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
                !material_editor_->has_displacement_texture);
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
                saved_material_update_ = SavedMaterialUpdate{
                    .relative_path =
                        material_editor_->relative_path.generic_string(),
                    .material = material_editor_->material,
                    .tessellation = material_editor_->tessellation,
                };
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
                        SavedMaterialUpdate{
                            .relative_path =
                                material_editor_->relative_path
                                    .generic_string(),
                            .material =
                                material_editor_->material,
                            .tessellation =
                                material_editor_->tessellation,
                        };
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
                    saved_material_update_ = SavedMaterialUpdate{
                        .relative_path =
                            material_editor_->relative_path.generic_string(),
                        .material = material_editor_->material,
                        .tessellation = material_editor_->tessellation,
                    };
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
                    SavedMaterialUpdate{
                        .relative_path =
                            material_editor_->relative_path
                                .generic_string(),
                        .material =
                            material_editor_->material,
                        .tessellation =
                            material_editor_->tessellation,
                    };
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
            if (ImGui::Selectable(to_token(mode).data(), renderer.subsurface_mode() == mode)) {
                static_cast<void>(renderer.set_subsurface_mode(mode));
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
    if (exposure_changed) static_cast<void>(renderer.set_exposure_settings(exposure));
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
    ImGui::BeginDisabled(!renderer.gi_provisioned() || !lighting.dynamic);
    lighting_changed |= ImGui::Checkbox("Voxel indirect lighting", &lighting.voxel_gi);
    lighting_changed |=
        ImGui::SliderFloat("Indirect intensity", &lighting.gi_strength, 0, 2, "%.2f");
    ImGui::EndDisabled();
    if (lighting_changed) {
        if (!lighting.dynamic)
            lighting.voxel_gi = false;
        if (renderer.set_lighting_settings(lighting)) {
            checkpoint_scene(scene);
            scene.lighting = lighting;
            scene.mark_content_changed();
        }
    }
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
    ImGui::Text("LOD instances: %u / %u / %u", visible.lod_instances[0], visible.lod_instances[1],
                visible.lod_instances[2]);
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

void EditorUi::process_file_commands(SceneDocument& scene) {
    if (save_requested_) {
        save_requested_ = false;
        if (play_state_ == EditorPlayState::edit) {
            save_scene(scene);
        }
    }
    if (open_requested_) {
        open_requested_ = false;
        if (play_state_ == EditorPlayState::edit) {
            if (scene_is_dirty(scene)) {
                open_confirmation_requested_ = true;
            } else {
                open_scene(scene);
            }
        }
    }
    if (quit_requested_) {
        quit_requested_ = false;
        if (play_state_ != EditorPlayState::edit) {
            stop_play(scene);
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
        ImGui::TextUnformatted(
            "Discard the current unsaved changes and open:");
        ImGui::TextWrapped(
            "%s",
            scene_path_.empty()
                ? "(scene path unavailable)"
                : scene_path_.string().c_str());
        if (ImGui::Button("Discard and open")) {
            open_scene(scene);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
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
                        SavedMaterialUpdate{
                            .relative_path =
                                material_editor_->relative_path
                                    .generic_string(),
                            .material =
                                material_editor_->material,
                            .tessellation =
                                material_editor_->tessellation,
                        };
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
    return saved_content_revision_ != 0U &&
        (scene.content_revision() != saved_content_revision_ ||
         scene.structure_revision() !=
             saved_structure_revision_);
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
    const bool navigate =
        ((viewport_hovered_ && viewport_focused_) ||
         (external_viewport_hovered_ &&
          external_viewport_focused_)) &&
        ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!navigate) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    camera_input_.mouse_delta_x = io.MouseDelta.x;
    camera_input_.mouse_delta_y = io.MouseDelta.y;
    camera_input_.forward = ImGui::IsKeyDown(ImGuiKey_W);
    camera_input_.backward = ImGui::IsKeyDown(ImGuiKey_S);
    camera_input_.left = ImGui::IsKeyDown(ImGuiKey_A);
    camera_input_.right = ImGui::IsKeyDown(ImGuiKey_D);
    camera_input_.up =
        ImGui::IsKeyDown(ImGuiKey_E) ||
        ImGui::IsKeyDown(ImGuiKey_Space);
    camera_input_.down =
        ImGui::IsKeyDown(ImGuiKey_Q) ||
        ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
        ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    camera_input_.boost =
        ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
        ImGui::IsKeyDown(ImGuiKey_RightShift);
}

void EditorUi::start_play(SceneDocument& scene) {
    if (play_state_ != EditorPlayState::edit) {
        return;
    }
    if (import_activity_pending()) {
        asset_error_ =
            "Close or finish asset import before entering Play.";
        return;
    }
    edit_scene_snapshot_ = scene;
    play_state_ = EditorPlayState::playing;
}

void EditorUi::stop_play(SceneDocument& scene) {
    if (play_state_ == EditorPlayState::edit) {
        return;
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
