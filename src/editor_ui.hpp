#pragma once

#include "asset_browser.hpp"
#include "editor_ui_kit.hpp"
#include "fly_camera.hpp"
#include "performance_metrics.hpp"
#include "scene_document.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vcr {

class Renderer;
class AssetImportWorker;
struct AssetImportUiState;
struct ExternalAssetDropState;
struct MaterialEditorUiState;

enum class EditorPlayState : std::uint8_t {
    edit,
    playing,
    paused,
};

enum class EditorGizmoOperation : std::uint8_t {
    translate,
    rotate,
    scale,
};

enum class EditorGizmoSpace : std::uint8_t {
    world,
    local,
};

struct SavedMaterialUpdate {
    std::string relative_path{};
    PbrMaterial material{};
};

class EditorUi final {
public:
    EditorUi();
    ~EditorUi() noexcept;

    EditorUi(const EditorUi&) = delete;
    EditorUi& operator=(const EditorUi&) = delete;

    void initialize(
        VulkanContext& context,
        SDL_Window* main_window,
        Renderer& renderer);
    void shutdown() noexcept;
    void process_event(const SDL_Event& event);
    void synchronize_renderer(Renderer& renderer);
    void request_quit() noexcept {
        quit_requested_ = true;
    }

    void begin_frame(
        SceneDocument& scene,
        Renderer& renderer,
        FlyCamera& camera,
        const PerformanceSnapshot& performance,
        std::chrono::steady_clock::time_point now);
    [[nodiscard]] ImDrawData* end_frame() noexcept;
    void finish_platform_windows(bool render);

    [[nodiscard]] std::optional<VkExtent2D>
        consume_viewport_resize(
            std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] FlyCameraInput camera_input() const noexcept {
        return camera_input_;
    }
    [[nodiscard]] bool wants_quit() const noexcept {
        return wants_quit_;
    }
    [[nodiscard]] bool separate_window_supported() const noexcept {
        return platform_windows_enabled_;
    }
    [[nodiscard]] EditorPlayState play_state() const noexcept {
        return play_state_;
    }
    [[nodiscard]] SceneNodeId selected_node() const noexcept {
        return selected_node_;
    }
    [[nodiscard]] EditorGizmoOperation gizmo_operation() const noexcept {
        return gizmo_operation_;
    }
    [[nodiscard]] EditorGizmoSpace gizmo_space() const noexcept {
        return gizmo_space_;
    }
    [[nodiscard]] bool gizmo_snap_enabled() const noexcept {
        return gizmo_snap_enabled_;
    }
    [[nodiscard]] bool select_node(
        SceneDocument& scene,
        SceneNodeId id) noexcept;
    [[nodiscard]] bool duplicate_selection(
        SceneDocument& scene,
        std::uint32_t& duplicated_nodes,
        std::string& error);
    [[nodiscard]] bool focus_selection(
        const SceneDocument& scene,
        Renderer& renderer,
        FlyCamera& camera) const noexcept;
    [[nodiscard]] bool external_game_window_open() const noexcept {
        return external_game_window_open_;
    }
    [[nodiscard]] bool consume_scene_replaced() noexcept {
        const bool replaced = scene_replaced_;
        scene_replaced_ = false;
        return replaced;
    }
    [[nodiscard]] bool consume_startup_camera_requested() noexcept {
        return std::exchange(startup_camera_requested_, false);
    }
    [[nodiscard]] std::optional<SavedMaterialUpdate>
        consume_saved_material_update() noexcept {
        return std::exchange(saved_material_update_, std::nullopt);
    }
    [[nodiscard]] const std::filesystem::path&
        asset_root() const noexcept {
        return asset_browser_.root();
    }
    void report_asset_error(std::string error) {
        asset_error_ = std::move(error);
    }
    [[nodiscard]] bool scene_is_dirty(
        const SceneDocument& scene) const noexcept;
    void checkpoint_scene(const SceneDocument& scene);
    [[nodiscard]] bool undo_scene(SceneDocument& scene);
    [[nodiscard]] bool redo_scene(SceneDocument& scene);
    [[nodiscard]] bool can_undo() const noexcept {
        return !undo_history_.empty();
    }
    [[nodiscard]] bool can_redo() const noexcept {
        return !redo_history_.empty();
    }
    [[nodiscard]] bool save_scene_external(
        SceneDocument& scene,
        std::string& error);
    [[nodiscard]] bool queue_import_external(
        const std::filesystem::path& source,
        const std::filesystem::path& destination_directory,
        std::string& error);
    [[nodiscard]] bool asset_import_pending() const noexcept {
        return import_activity_pending();
    }
    [[nodiscard]] std::string_view asset_import_status() const noexcept;

private:
    void initialize_vulkan_backend(Renderer& renderer);
    void shutdown_vulkan_backend() noexcept;
    void register_scene_texture(Renderer& renderer);
    void unregister_scene_texture() noexcept;
    void build_main_menu(SceneDocument& scene);
    void build_toolbar(SceneDocument& scene);
    void build_hierarchy(SceneDocument& scene);
    void build_inspector(SceneDocument& scene);
    void build_viewport(
        SceneDocument& scene,
        Renderer& renderer,
        FlyCamera& camera,
        std::chrono::steady_clock::time_point now);
    void build_external_game_window(
        std::chrono::steady_clock::time_point now);
    void build_assets(
        SceneDocument& scene,
        Renderer& renderer);
    void build_landscape(
        SceneDocument& scene,
        Renderer& renderer);
    void build_import_window();
    void build_import_results();
    void build_material_editor();
    void build_performance(
        SceneDocument& scene,
        Renderer& renderer,
        const PerformanceSnapshot& performance);
    void process_file_commands(SceneDocument& scene);
    void process_import_pipeline();
    void queue_import_sources(
        std::vector<std::filesystem::path> sources);
    void open_material_editor(
        const std::filesystem::path& relative_path);
    [[nodiscard]] bool add_model_asset_to_scene(
        SceneDocument& scene,
        Renderer& renderer,
        std::string_view relative_path,
        std::string_view display_name);
    [[nodiscard]] bool import_activity_pending() const noexcept;
    void save_scene(SceneDocument& scene);
    void open_scene(SceneDocument& scene);
    void push_undo_snapshot(SceneDocument snapshot);
    void apply_deferred_scene_commands(SceneDocument& scene);
    void update_camera_input();
    void start_play(SceneDocument& scene);
    void stop_play(SceneDocument& scene);
    void request_viewport_extent(
        VkExtent2D extent,
        std::chrono::steady_clock::time_point now) noexcept;

    // Non-owning observers. EditorApplication owns both objects and shuts
    // EditorUi down before either lifetime ends.
    VulkanContext* context_ = nullptr;
    SDL_Window* main_window_ = nullptr;
    VkDescriptorSet scene_texture_ = VK_NULL_HANDLE;
    VkFormat imgui_color_format_ = VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfo imgui_rendering_info_{
        .sType =
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0U,
        .colorAttachmentCount = 1U,
        .pColorAttachmentFormats = nullptr,
        .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    std::uint64_t known_viewport_generation_ = 0U;
    std::uint64_t known_swapchain_generation_ = 0U;
    VkFormat known_swapchain_format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t known_swapchain_image_count_ = 0U;
    bool vulkan_backend_initialized_ = false;
    bool platform_windows_enabled_ = false;
    bool default_layout_initialized_ = false;
    bool reset_layout_requested_ = false;
    bool wants_quit_ = false;
    bool quit_requested_ = false;
    bool quit_confirmation_requested_ = false;
    bool save_requested_ = false;
    bool open_requested_ = false;
    bool open_confirmation_requested_ = false;
    bool import_dialog_requested_ = false;
    bool scene_replaced_ = false;
    bool startup_camera_requested_ = false;
    std::optional<SavedMaterialUpdate> saved_material_update_{};

    SceneNodeId selected_node_{};
    SceneNodeId pending_delete_{};
    SceneNodeId pending_duplicate_{};
    SceneNodeId delete_confirmation_{};
    SceneNodeId pending_reparent_child_{};
    SceneNodeId pending_reparent_parent_{};
    std::optional<SceneDocument> edit_scene_snapshot_{};
    std::optional<SceneDocument> inspector_edit_snapshot_{};
    std::optional<SceneDocument> gizmo_edit_snapshot_{};
    static constexpr std::size_t maximum_history_entries = 32U;
    std::vector<SceneDocument> undo_history_{};
    std::vector<SceneDocument> redo_history_{};
    EditorPlayState play_state_ = EditorPlayState::edit;
    bool external_game_window_open_ = false;
    bool viewport_hovered_ = false;
    bool viewport_focused_ = false;
    bool external_viewport_hovered_ = false;
    bool external_viewport_focused_ = false;
    bool toggle_play_requested_ = false;
    bool toggle_external_window_requested_ = false;
    bool delete_popup_requested_ = false;
    bool focus_selection_requested_ = false;
    FlyCameraInput camera_input_{};
    EditorGizmoOperation gizmo_operation_ =
        EditorGizmoOperation::translate;
    EditorGizmoSpace gizmo_space_ = EditorGizmoSpace::world;
    bool gizmo_snap_enabled_ = false;
    float gizmo_translation_snap_ = 0.5F;
    float gizmo_rotation_snap_ = 15.0F;
    float gizmo_scale_snap_ = 0.1F;

    std::vector<SceneHierarchyRow> hierarchy_rows_{};
    std::uint64_t hierarchy_revision_ = 0U;
    std::array<char, 128> hierarchy_filter_{};
    std::array<char, 128> previous_hierarchy_filter_{};
    std::array<char, 256> selected_name_buffer_{};
    SceneNodeId name_buffer_node_{};

    VkExtent2D pending_viewport_extent_{1280U, 720U};
    VkExtent2D requested_viewport_extent_{1280U, 720U};
    std::chrono::steady_clock::time_point
        viewport_extent_changed_at_{};

    AssetBrowser asset_browser_{};
    std::unique_ptr<AssetImportWorker> import_worker_;
    std::unique_ptr<AssetImportUiState> import_ui_;
    std::unique_ptr<ExternalAssetDropState> external_drop_;
    std::unique_ptr<MaterialEditorUiState> material_editor_;
    std::string asset_error_{};
    std::string pending_model_path_{}, pending_model_name_{};
    SceneNodeId pending_model_parent_{};
    std::optional<std::filesystem::path>
        pending_asset_directory_{};
    std::optional<AssetEntry> selected_asset_{};
    std::array<char, 128> scatter_name_prefix_{};
    std::vector<SceneNodeId> scatter_created_{};
    std::uint32_t scatter_count_ = 100U;
    std::uint32_t scatter_seed_ = 1U;
    float scatter_center_[3]{0.0F, 0.0F, 0.0F};
    float scatter_size_[2]{32.0F, 32.0F};
    float scatter_scale_[2]{0.8F, 1.2F};
    float flat_ground_size_[2]{64.0F, 64.0F};
    float flat_ground_height_ = 0.0F;
    std::filesystem::path scene_path_{};
    std::string scene_file_name_{"editor_scene.json"};
    std::string scene_file_status_{};
    std::uint64_t saved_content_revision_ = 0U;
    std::uint64_t saved_structure_revision_ = 0U;
    bool asset_import_enabled_ = false;
    EditorUiKit ui_kit_{};
};

} // namespace vcr
