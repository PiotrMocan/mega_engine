#include "editor_ui.hpp"

#include "renderer.hpp"
#include "scene_io.hpp"
#include "vulkan_common.hpp"

#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <utility>

namespace vcr {
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

[[nodiscard]] const char* play_state_name(
    const EditorPlayState state) noexcept {
    switch (state) {
    case EditorPlayState::edit: return "EDIT";
    case EditorPlayState::playing: return "PLAYING";
    case EditorPlayState::paused: return "PAUSED";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::optional<std::filesystem::path>
find_project_asset_root() {
    const auto search_ancestors =
        [](std::filesystem::path candidate)
            -> std::optional<std::filesystem::path> {
            for (std::uint32_t depth = 0U;
                 depth < 16U && !candidate.empty();
                 ++depth) {
                std::error_code filesystem_error;
                const bool has_project_file =
                    std::filesystem::is_regular_file(
                        candidate / "CMakeLists.txt",
                        filesystem_error);
                filesystem_error.clear();
                const std::filesystem::path assets =
                    candidate / "assets";
                const bool has_assets =
                    std::filesystem::is_directory(
                        assets, filesystem_error);
                if (has_project_file && has_assets &&
                    !filesystem_error) {
                    return assets;
                }
                const std::filesystem::path parent =
                    candidate.parent_path();
                if (parent == candidate) {
                    break;
                }
                candidate = parent;
            }
            return std::nullopt;
        };

    std::error_code filesystem_error;
    const std::filesystem::path working_directory =
        std::filesystem::current_path(filesystem_error);
    if (!filesystem_error) {
        if (auto root = search_ancestors(working_directory)) {
            return root;
        }
    }
    const char* const base_path = SDL_GetBasePath();
    if (base_path != nullptr) {
        if (auto root = search_ancestors(
                std::filesystem::path(base_path))) {
            return root;
        }
        const std::filesystem::path packaged_assets =
            std::filesystem::path(base_path) / "assets";
        filesystem_error.clear();
        if (std::filesystem::is_directory(
                packaged_assets, filesystem_error) &&
            !filesystem_error) {
            return packaged_assets;
        }
    }
    return std::nullopt;
}

} // namespace

EditorUi::~EditorUi() {
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
    apply_nuklear_inspired_style();

    if (!ImGui_ImplSDL3_InitForVulkan(main_window)) {
        fatal("Unable to initialize Dear ImGui SDL3 backend.");
    }
    initialize_vulkan_backend(renderer);
    register_scene_texture(renderer);

    hierarchy_rows_.reserve(max_scene_nodes);
    if (const auto asset_root = find_project_asset_root()) {
        if (asset_browser_.initialize(
                *asset_root, asset_error_)) {
            scene_path_ =
                asset_browser_.root() /
                "scenes" /
                "editor_scene.json";
            scene_file_name_ =
                scene_path_.filename().string();
            scene_file_status_ =
                "Default scene; Save writes assets/scenes/editor_scene.json";
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
    const VkResult idle = vkDeviceWaitIdle(context_->device());
    if (idle != VK_SUCCESS) {
        std::cerr
            << "Editor UI shutdown vkDeviceWaitIdle failed: "
            << static_cast<int>(idle) << '\n';
    }
    unregister_scene_texture();
    shutdown_vulkan_backend();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
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
    wants_quit_ = false;
    quit_requested_ = false;
    quit_confirmation_requested_ = false;
    save_requested_ = false;
    open_requested_ = false;
    open_confirmation_requested_ = false;
    scene_replaced_ = false;
    saved_content_revision_ = 0U;
    saved_structure_revision_ = 0U;
    selected_asset_.reset();
}

void EditorUi::process_event(const SDL_Event& event) noexcept {
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

void EditorUi::begin_frame(
    SceneDocument& scene,
    Renderer& renderer,
    const PerformanceSnapshot& performance,
    const std::chrono::steady_clock::time_point now) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    camera_input_ = {};
    external_viewport_hovered_ = false;
    external_viewport_focused_ = false;
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
    if (!default_layout_initialized_) {
        ImGuiDockNode* const node =
            ImGui::DockBuilderGetNode(dockspace_id);
        if (node != nullptr && node->IsEmpty()) {
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
                0.20F,
                &left,
                &center);
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Right,
                0.24F,
                &right,
                &center);
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Down,
                0.28F,
                &bottom,
                &center);
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Up,
                0.09F,
                &top,
                &center);
            ImGui::DockBuilderDockWindow("Toolbar", top);
            ImGui::DockBuilderDockWindow(
                "Scene Hierarchy", left);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Renderer", right);
            ImGui::DockBuilderDockWindow("Assets", bottom);
            ImGui::DockBuilderDockWindow("Scene View", center);
            ImGui::DockBuilderFinish(dockspace_id);
        }
        default_layout_initialized_ = true;
    }
    build_main_menu(scene);
    build_toolbar(scene);
    build_hierarchy(scene);
    build_inspector(scene);
    build_viewport(renderer, now);
    build_assets();
    build_performance(renderer, performance);
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
    if (vulkan_backend_initialized_) {
        ImGui_ImplVulkan_Shutdown();
        vulkan_backend_initialized_ = false;
    }
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

void EditorUi::apply_nuklear_inspired_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 1.0F;
    style.ChildRounding = 1.0F;
    style.FrameRounding = 2.0F;
    style.PopupRounding = 2.0F;
    style.ScrollbarRounding = 2.0F;
    style.GrabRounding = 2.0F;
    style.TabRounding = 1.0F;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.WindowPadding = ImVec2(8.0F, 8.0F);
    style.FramePadding = ImVec2(7.0F, 4.0F);
    style.ItemSpacing = ImVec2(7.0F, 5.0F);
    style.IndentSpacing = 16.0F;
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.075F, 0.085F, 0.105F, 1.0F);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.065F, 0.075F, 0.095F, 1.0F);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.055F, 0.065F, 0.08F, 0.98F);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.19F, 0.23F, 0.27F, 1.0F);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.11F, 0.13F, 0.16F, 1.0F);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.16F, 0.22F, 0.25F, 1.0F);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.13F, 0.30F, 0.31F, 1.0F);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.055F, 0.065F, 0.08F, 1.0F);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.08F, 0.11F, 0.13F, 1.0F);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.10F, 0.25F, 0.27F, 1.0F);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.12F, 0.40F, 0.40F, 1.0F);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.10F, 0.52F, 0.49F, 1.0F);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.10F, 0.22F, 0.24F, 1.0F);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.11F, 0.43F, 0.42F, 1.0F);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.08F, 0.56F, 0.52F, 1.0F);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.28F, 0.93F, 0.82F, 1.0F);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.18F, 0.73F, 0.67F, 1.0F);
    style.Colors[ImGuiCol_TabSelected] =
        ImVec4(0.10F, 0.30F, 0.31F, 1.0F);
    if ((ImGui::GetIO().ConfigFlags &
         ImGuiConfigFlags_ViewportsEnable) != 0) {
        style.WindowRounding = 0.0F;
        style.Colors[ImGuiCol_WindowBg].w = 1.0F;
    }
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
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            quit_requested_ = true;
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
            selected_node_ = scene.create_node(
                "Folder", SceneNodeKind::folder, parent);
        }
        if (ImGui::MenuItem(
                "Add cube",
                nullptr,
                false,
                play_state_ == EditorPlayState::edit)) {
            const SceneNodeId cube = scene.create_node(
                "Cube", SceneNodeKind::cube, parent);
            if (auto node = scene.node(cube)) {
                node->get().transform.translation =
                    glm::vec3(0.0F, 0.0F, -5.0F);
                scene.mark_content_changed();
            }
            selected_node_ = cube;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void EditorUi::build_toolbar(SceneDocument& scene) {
    ImGui::SetNextWindowSize(
        ImVec2(540.0F, 56.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(
            "Toolbar",
            nullptr,
            ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    if (play_state_ == EditorPlayState::edit) {
        if (ImGui::Button("Play")) {
            start_play(scene);
        }
    } else {
        if (ImGui::Button("Stop")) {
            stop_play(scene);
        }
        ImGui::SameLine();
        if (play_state_ == EditorPlayState::playing) {
            if (ImGui::Button("Pause")) {
                play_state_ = EditorPlayState::paused;
            }
        } else if (ImGui::Button("Resume")) {
            play_state_ = EditorPlayState::playing;
        }
    }
    ImGui::SameLine();
    const bool can_open_external =
        platform_windows_enabled_ &&
        play_state_ != EditorPlayState::edit;
    ImGui::BeginDisabled(!can_open_external);
    if (ImGui::Button(
            external_game_window_open_
                ? "Close game window"
                : "Game in separate window")) {
        external_game_window_open_ =
            !external_game_window_open_;
    }
    ImGui::EndDisabled();
    if (!platform_windows_enabled_ &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Separate platform windows require a shared graphics/present "
            "queue and a supported desktop backend.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "[%s] F5 Play/Stop | F6 separate window",
        play_state_name(play_state_));
    ImGui::SameLine();
    ImGui::TextDisabled(
        "| %s%s",
        scene_is_dirty(scene) ? "* " : "",
        scene_path_.empty()
            ? "scene path unavailable"
            : scene_file_name_.c_str());
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
    ImGui::SetNextItemWidth(-1.0F);
    static_cast<void>(ImGui::InputText(
        "##HierarchyFilter",
        hierarchy_filter_.data(),
        hierarchy_filter_.size()));
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
            const float indent =
                static_cast<float>(
                    std::min(row.depth, 32U)) *
                ImGui::GetStyle().IndentSpacing;
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() + indent);
            if (row.has_children && hierarchy_filter_[0] == '\0') {
                if (ImGui::SmallButton(node.expanded ? "v" : ">")) {
                    scene.set_expanded(row.id, !node.expanded);
                }
            } else {
                ImGui::Dummy(ImVec2(
                    ImGui::GetFrameHeight(), 1.0F));
            }
            ImGui::SameLine();
            const bool selected = selected_node_ == row.id;
            if (ImGui::Selectable(
                    node.name.c_str(),
                    selected,
                    ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_node_ = row.id;
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
                "This editor stage does not have undo/redo.");
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
    ImGui::TextDisabled(
        "%u objects, %zu visible rows",
        scene.size(),
        hierarchy_rows_.size());
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
        ImGui::TextDisabled("Select an object in the hierarchy.");
        ImGui::End();
        return;
    }
    SceneNode& node = selected->get();
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
    if (ImGui::InputText(
            "Name",
            selected_name_buffer_.data(),
            selected_name_buffer_.size())) {
        static_cast<void>(scene.rename_node(
            selected_node_, selected_name_buffer_.data()));
        hierarchy_revision_ = 0U;
    }
    ImGui::Text("Type: %s", to_string(node.kind).data());
    bool changed = false;
    changed |= ImGui::Checkbox("Visible", &node.visible);
    const bool has_transform =
        node.kind == SceneNodeKind::folder ||
        node.kind == SceneNodeKind::cube ||
        node.kind == SceneNodeKind::ground;
    if (has_transform) {
        changed |= ImGui::DragFloat3(
            "Position",
            &node.transform.translation.x,
            0.05F);
        changed |= ImGui::DragFloat3(
            "Rotation",
            &node.transform.rotation_degrees.x,
            0.25F);
        changed |= ImGui::DragFloat3(
            "Scale",
            &node.transform.scale.x,
            0.02F,
            0.0001F,
            1000.0F);
    }
    if (node.kind == SceneNodeKind::cube ||
        node.kind == SceneNodeKind::ground) {
        ImGui::SeparatorText("PBR material");
        changed |= ImGui::ColorEdit3(
            "Base color", &node.material.base_color.x);
        changed |= ImGui::SliderFloat(
            "Metallic", &node.material.metallic, 0.0F, 1.0F);
        changed |= ImGui::SliderFloat(
            "Roughness",
            &node.material.roughness,
            minimum_pbr_roughness,
            1.0F);
        changed |= ImGui::SliderFloat(
            "Ambient occlusion",
            &node.material.ambient_occlusion,
            0.0F,
            1.0F);
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
        scene.mark_content_changed();
    }
    if (!has_transform) {
        ImGui::TextDisabled(
            node.kind == SceneNodeKind::camera
                ? "Scene View currently uses the editor fly camera."
                : "This component exposes visibility in this editor stage.");
    }
    ImGui::EndDisabled();
    if (!editable) {
        ImGui::TextDisabled(
            "Authoring changes are locked during Play.");
    }
    ImGui::End();
}

void EditorUi::build_viewport(
    Renderer& renderer,
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
    ImGui::TextDisabled(
        "RMB + WASD to fly | %ux%u",
        renderer.editor_viewport_extent().width,
        renderer.editor_viewport_extent().height);
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
    viewport_hovered_ = ImGui::IsItemHovered();
    viewport_focused_ = ImGui::IsWindowFocused(
        ImGuiFocusedFlags_RootAndChildWindows);
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
        external_viewport_hovered_ = ImGui::IsItemHovered();
        external_viewport_focused_ = ImGui::IsWindowFocused(
            ImGuiFocusedFlags_RootAndChildWindows);
    }
    ImGui::End();
}

void EditorUi::build_assets() {
    ImGui::SetNextWindowSize(
        ImVec2(720.0F, 270.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Assets")) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Up")) {
        if (asset_browser_.navigate_up(asset_error_)) {
            selected_asset_.reset();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        if (asset_browser_.refresh(asset_error_)) {
            selected_asset_.reset();
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(
        asset_browser_.current_directory_display().data());
    if (!asset_error_.empty()) {
        ImGui::TextColored(
            ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
            "%s",
            asset_error_.c_str());
    }
    if (asset_browser_.truncated()) {
        ImGui::TextColored(
            ImVec4(1.0F, 0.72F, 0.25F, 1.0F),
            "Directory contains more than %zu entries; "
            "the bounded view is truncated.",
            AssetBrowser::maximum_entries);
    }
    const auto entries = asset_browser_.entries();
    const std::size_t entry_count = entries.size();
    ImGui::BeginChild(
        "AssetList",
        ImVec2(0.0F, -58.0F),
        ImGuiChildFlags_Borders);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(entry_count));
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart;
             index < clipper.DisplayEnd;
             ++index) {
            const AssetEntry& entry =
                entries[static_cast<std::size_t>(index)];
            ImGui::PushID(index);
            ImGui::TextDisabled(
                "[%s]", asset_kind_name(entry.kind));
            ImGui::SameLine(
                0.0F,
                ImGui::GetStyle().ItemInnerSpacing.x);
            const bool activated = ImGui::Selectable(
                    entry.display_name.c_str(),
                    selected_asset_ &&
                        selected_asset_->relative_path ==
                            entry.relative_path,
                    ImGuiSelectableFlags_AllowDoubleClick);
            if (activated) {
                selected_asset_ = entry;
                if (ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left) &&
                    entry.kind == AssetEntryKind::directory) {
                    pending_asset_directory_ =
                        entry.relative_path;
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
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (pending_asset_directory_) {
        if (asset_browser_.navigate_to(
                *pending_asset_directory_, asset_error_)) {
            selected_asset_.reset();
        }
        pending_asset_directory_.reset();
    }
    if (selected_asset_) {
        ImGui::Text(
            "%s | %llu bytes",
            asset_kind_name(selected_asset_->kind),
            static_cast<unsigned long long>(
                selected_asset_->size));
        ImGui::SameLine();
        ImGui::TextWrapped(
            "%s",
            selected_asset_->relative_display.c_str());
    } else {
        ImGui::TextDisabled("Select an asset to inspect its path and size.");
    }
    ImGui::TextDisabled(
        "%zu entries | cache high-water %zu",
        entry_count,
        asset_browser_.high_water_mark());
    ImGui::End();
}

void EditorUi::build_performance(
    Renderer& renderer,
    const PerformanceSnapshot& performance) {
    ImGui::SetNextWindowSize(
        ImVec2(360.0F, 230.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Renderer")) {
        ImGui::End();
        return;
    }
    ImGui::Text(
        "FPS %.1f | 1%% low %.1f",
        performance.average_fps,
        performance.one_percent_low_fps);
    ImGui::Text(
        "CPU %.3f ms | GPU %.3f ms",
        performance.average.cpu_frame_ms,
        performance.average.gpu_frame_ms);
    ImGui::Text(
        "Present: %s",
        Swapchain::present_mode_name(
            renderer.swapchain().present_mode()));
    ImGui::Text(
        "AA: %s / %s",
        to_string(renderer.anti_aliasing().method).data(),
        renderer.anti_aliasing_quality_name().data());
    if (ImGui::Button("Cycle AA")) {
        renderer.cycle_anti_aliasing();
    }
    ImGui::SameLine();
    if (ImGui::Button("Quality +")) {
        renderer.adjust_anti_aliasing_quality(1);
    }
    ImGui::TextDisabled(
        "Editor UI is isolated from vulkan_cube benchmark target.");
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
        if (scene_is_dirty(scene)) {
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
            "The scene has unsaved changes.");
        if (ImGui::Button("Save and exit")) {
            save_scene(scene);
            if (!scene_is_dirty(scene)) {
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
    if (scene_path_.empty()) {
        scene_file_status_ =
            "Save failed: scene path is unavailable.";
        return;
    }
    std::string error;
    if (!save_scene_file(scene, scene_path_, error)) {
        scene_file_status_ = "Save failed: " + error;
        return;
    }
    saved_content_revision_ = scene.content_revision();
    saved_structure_revision_ = scene.structure_revision();
    scene_file_status_ =
        "Saved " + scene_path_.string();
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
    saved_content_revision_ = scene.content_revision();
    saved_structure_revision_ = scene.structure_revision();
    scene_file_status_ =
        "Opened " + scene_path_.string();
    scene_replaced_ = true;
    selected_node_ = {};
    pending_delete_ = {};
    delete_confirmation_ = {};
    pending_reparent_child_ = {};
    pending_reparent_parent_ = {};
    edit_scene_snapshot_.reset();
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

void EditorUi::apply_deferred_scene_commands(
    SceneDocument& scene) {
    if (play_state_ == EditorPlayState::edit &&
        pending_reparent_child_ &&
        pending_reparent_parent_) {
        static_cast<void>(scene.reparent(
            pending_reparent_child_,
            pending_reparent_parent_));
    }
    pending_reparent_child_ = {};
    pending_reparent_parent_ = {};
    if (play_state_ == EditorPlayState::edit &&
        pending_delete_) {
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
