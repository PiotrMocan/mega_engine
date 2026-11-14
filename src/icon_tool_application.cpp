#include "icon_tool_application.hpp"

#include "image_barrier.hpp"
#include "vulkan_common.hpp"
#include "window_icon_sdl.hpp"

#include <imgui_impl_sdl3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

void check_it_imgui_vk_result(const VkResult result) {
    if (result != VK_SUCCESS) {
        fatal(
            "Icon Tool ImGui Vulkan error: " +
            std::to_string(static_cast<int>(result)));
    }
}

[[nodiscard]] std::string path_to_utf8_str(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    std::string out(u8.size(), '\0');
    std::transform(
        u8.begin(),
        u8.end(),
        out.begin(),
        [](const char8_t ch) { return static_cast<char>(ch); });
    return out;
}

[[nodiscard]] std::filesystem::path path_from_utf8_str(
    const std::string_view text) {
    std::u8string u8(text.size(), u8'\0');
    std::transform(
        text.begin(),
        text.end(),
        u8.begin(),
        [](const char ch) {
            return static_cast<char8_t>(static_cast<unsigned char>(ch));
        });
    return std::filesystem::path(u8);
}

[[nodiscard]] std::string ascii_lower(const std::string_view input) {
    std::string out(input.size(), '\0');
    for (std::size_t i = 0U; i < input.size(); ++i) {
        const auto ch = static_cast<unsigned char>(input[i]);
        if (ch >= 'A' && ch <= 'Z') {
            out[i] = static_cast<char>(ch - 'A' + 'a');
        } else {
            out[i] = static_cast<char>(ch);
        }
    }
    return out;
}

[[nodiscard]] bool is_supported_icon_extension(
    const std::filesystem::path& path) {
    const std::string ext = ascii_lower(path_to_utf8_str(path.extension()));
    return ext == ".png" || ext == ".ico" || ext == ".jpg" ||
           ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

template <std::size_t Capacity>
void copy_to_cstr_buffer(
    std::array<char, Capacity>& dest,
    const std::string_view src) noexcept {
    static_assert(Capacity > 0U);
    dest.fill('\0');
    const std::size_t count = (std::min)(src.size(), Capacity - 1U);
    if (count > 0U) {
        std::memcpy(dest.data(), src.data(), count);
    }
}

[[nodiscard]] bool parse_u32_arg(
    const std::string_view text,
    std::uint32_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint32_t parsed = 0U;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    out = parsed;
    return true;
}

[[nodiscard]] bool write_binary_file_atomic(
    const std::filesystem::path& target_path,
    const std::span<const std::uint8_t> bytes,
    std::string& error) {
    error.clear();
    if (target_path.empty()) {
        error = "Output path cannot be empty.";
        return false;
    }
    std::error_code ec;
    if (target_path.has_parent_path()) {
        std::filesystem::create_directories(target_path.parent_path(), ec);
        if (ec) {
            error = "Failed to create output parent directory: " + ec.message();
            return false;
        }
    }
    std::filesystem::path temp_path = target_path;
    temp_path += ".tmp";
    {
        std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Unable to open output file for writing: " +
                    path_to_utf8_str(temp_path);
            return false;
        }
        if (!bytes.empty()) {
            stream.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        if (!stream) {
            std::filesystem::remove(temp_path, ec);
            error = "Failed while writing file bytes.";
            return false;
        }
    }
    std::filesystem::rename(temp_path, target_path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(target_path, remove_ec);
        ec.clear();
        std::filesystem::rename(temp_path, target_path, ec);
        if (ec) {
            std::filesystem::remove(temp_path, remove_ec);
            error = "Failed to finalize output file: " + ec.message();
            return false;
        }
    }
    return true;
}

[[nodiscard]] IconRgbaImage clamp_preview_size(const IconRgbaImage& src) {
    if (!src.valid()) {
        return src;
    }
    if (src.width <= 256U && src.height <= 256U && src.width == src.height) {
        return src;
    }
    return resize_rgba_square_icon(src, 256U);
}

void draw_preview_with_border(
    const UiTexturePreview& preview,
    const float pixel_size,
    const ImVec4 border_color) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 box_min = cursor;
    const ImVec2 box_max(cursor.x + pixel_size, cursor.y + pixel_size);
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(
        box_min,
        box_max,
        IM_COL32(20, 24, 32, 255),
        6.0F);
    if (preview.valid()) {
        ImGui::Image(
            preview.imgui_id(),
            ImVec2(pixel_size, pixel_size),
            ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F));
    } else {
        ImGui::Dummy(ImVec2(pixel_size, pixel_size));
    }
    dl->AddRect(
        box_min,
        box_max,
        ImGui::ColorConvertFloat4ToU32(border_color),
        6.0F,
        0,
        1.0F);
}

void print_icon_tool_usage() {
    std::cout
        << "Vulkan Cube Icon Utility\n"
        << "Usage:\n"
        << "  vulkan_icon_tool [GUI options]\n"
        << "  vulkan_icon_tool --project <DIR> --icon <IMAGE_OR_ICO> [--bundle]\n"
        << "  vulkan_icon_tool --project <DIR> --reset-default [--bundle]\n"
        << "  vulkan_icon_tool --project <DIR> --bundle\n"
        << "  vulkan_icon_tool --exe <EXE_PATH> --icon <IMAGE_OR_ICO>\n"
        << "  vulkan_icon_tool --convert <IMAGE> --output <OUT.ico>\n"
        << "  vulkan_icon_tool --export-builtins <OUT_DIR>\n\n"
        << "GUI Options:\n"
        << "  --project <DIR>              Pre-select project directory in GUI\n"
        << "  --registry <FILE>            Override project_registry.json path\n"
        << "  --width <640..8192>          Initial window width\n"
        << "  --height <480..8192>         Initial window height\n"
        << "  --validation                 Enable Vulkan validation layers\n"
        << "  --no-validation              Disable Vulkan validation layers\n"
        << "  --present <auto|immediate|mailbox|fifo>\n"
        << "  --smoke-frames <N>           Exit cleanly after N GUI frames\n"
        << "  -h, --help                   Show this help text\n";
}

} // namespace

void IconToolWindowDeleter::operator()(SDL_Window* const window) const noexcept {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
    }
}

IconToolApplication::~IconToolApplication() noexcept {
    shutdown();
}

bool IconToolApplication::parse_cli(
    const int argc,
    char** const argv,
    IconToolCliOptions& out,
    std::string& error) const {
    out = IconToolCliOptions{};
    error.clear();

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i] != nullptr ? argv[i] : "";
        auto require_value = [&](std::string_view& val) -> bool {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                error = "Missing value for " + std::string(arg);
                return false;
            }
            val = argv[++i];
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            out.show_help = true;
            continue;
        }
        if (arg == "--validation") {
            out.validation = true;
            continue;
        }
        if (arg == "--no-validation") {
            out.validation = false;
            continue;
        }
        if (arg == "--reset-default") {
            out.reset_default = true;
            continue;
        }
        if (arg == "--bundle") {
            out.bundle_runtime = true;
            continue;
        }
        if (arg == "--export-builtins") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.cli_export_builtins = true;
            out.output_path = std::string(val);
            continue;
        }
        if (arg == "--convert") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.cli_convert = true;
            out.icon_path = std::string(val);
            continue;
        }
        if (arg == "--output") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.output_path = std::string(val);
            continue;
        }
        if (arg == "--exe") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.cli_apply_exe = true;
            out.exe_path = std::string(val);
            continue;
        }
        if (arg == "--project") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.project_dir = std::string(val);
            continue;
        }
        if (arg == "--icon") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.icon_path = std::string(val);
            continue;
        }
        if (arg == "--registry") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.registry_path = std::string(val);
            continue;
        }
        if (arg == "--width") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            std::uint32_t parsed = 0U;
            if (!parse_u32_arg(val, parsed) || parsed < 640U || parsed > 8192U) {
                error = "--width must be between 640 and 8192.";
                return false;
            }
            out.width = parsed;
            continue;
        }
        if (arg == "--height") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            std::uint32_t parsed = 0U;
            if (!parse_u32_arg(val, parsed) || parsed < 480U || parsed > 8192U) {
                error = "--height must be between 480 and 8192.";
                return false;
            }
            out.height = parsed;
            continue;
        }
        if (arg == "--smoke-frames") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            std::uint32_t parsed = 0U;
            if (!parse_u32_arg(val, parsed) || parsed == 0U || parsed > 100000U) {
                error = "--smoke-frames must be between 1 and 100000.";
                return false;
            }
            out.smoke_frames = parsed;
            continue;
        }
        if (arg == "--present") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            if (val == "auto" || val == "immediate") {
                out.preferred_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            } else if (val == "mailbox") {
                out.preferred_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            } else if (val == "fifo") {
                out.preferred_present_mode = VK_PRESENT_MODE_FIFO_KHR;
            } else {
                error = "Unknown --present mode: " + std::string(val);
                return false;
            }
            continue;
        }

        error = "Unknown option: " + std::string(arg);
        return false;
    }

    if (out.show_help) {
        return true;
    }

    const bool has_project_mutation =
        !out.project_dir.empty() &&
        (!out.icon_path.empty() || out.reset_default || out.bundle_runtime);
    const int action_count =
        (out.cli_export_builtins ? 1 : 0) +
        (out.cli_convert ? 1 : 0) +
        (out.cli_apply_exe ? 1 : 0) +
        (has_project_mutation ? 1 : 0);
    if (action_count > 1) {
        error = "Specify only one CLI target (--export-builtins, --convert, --exe, or --project).";
        return false;
    }
    if (out.cli_convert && out.output_path.empty()) {
        error = "--convert requires --output <file.ico>.";
        return false;
    }
    if (out.cli_apply_exe && out.icon_path.empty()) {
        error = "--exe requires --icon <image_or_ico>.";
        return false;
    }
    if (out.reset_default && !out.icon_path.empty()) {
        error = "Cannot combine --reset-default with --icon.";
        return false;
    }
    if ((!out.icon_path.empty() || out.reset_default || out.bundle_runtime) &&
        out.project_dir.empty() && !out.cli_apply_exe && !out.cli_convert) {
        error = "Specify --project <dir> or --exe <file.exe>.";
        return false;
    }
    return true;
}

int IconToolApplication::run(const int argc, char** const argv) {
    std::string cli_error;
    if (!parse_cli(argc, argv, options_, cli_error)) {
        std::cerr << "Error: " << cli_error << "\n\n";
        print_icon_tool_usage();
        return 1;
    }
    if (options_.show_help) {
        print_icon_tool_usage();
        return 0;
    }

    registry_file_path_ = default_project_registry_path(options_.registry_path);

    const bool has_cli_action =
        options_.cli_export_builtins ||
        options_.cli_convert ||
        options_.cli_apply_exe ||
        options_.reset_default ||
        options_.bundle_runtime ||
        !options_.icon_path.empty();
    if (has_cli_action) {
        return run_cli_action(options_);
    }

    initialize_gui(options_);
    main_loop();
    shutdown();
    return 0;
}

int IconToolApplication::run_cli_action(
    const IconToolCliOptions& options) const {
    std::string error;

    if (options.cli_export_builtins) {
        const std::filesystem::path out_dir =
            path_from_utf8_str(options.output_path);
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        if (ec) {
            std::cerr << "Failed to create directory '" << options.output_path
                      << "': " << ec.message() << '\n';
            return 1;
        }
        struct BuiltinOut {
            BuiltinIconKind kind;
            const char* filename;
        };
        constexpr std::array<BuiltinOut, 5U> entries{{
            {BuiltinIconKind::engine, "engine.ico"},
            {BuiltinIconKind::editor, "editor.ico"},
            {BuiltinIconKind::project_default, "project_default.ico"},
            {BuiltinIconKind::project_manager, "project_manager.ico"},
            {BuiltinIconKind::icon_tool, "icon_tool.ico"},
        }};
        Json files = Json::array();
        for (const BuiltinOut& entry : entries) {
            const std::filesystem::path dst = out_dir / entry.filename;
            if (!write_builtin_ico_file(entry.kind, dst, error)) {
                std::cerr << "Export failed for " << entry.filename << ": "
                          << error << '\n';
                return 1;
            }
            files.push_back(path_to_utf8_str(dst));
        }
        Json out = Json::object();
        out["exported"] = files;
        std::cout << out.dump(2) << '\n';
        return 0;
    }

    if (options.cli_convert) {
        std::vector<std::uint8_t> ico_bytes;
        IconRgbaImage preview{};
        if (!load_or_convert_to_ico_bytes(
                path_from_utf8_str(options.icon_path),
                ico_bytes,
                preview,
                error)) {
            std::cerr << "Icon conversion failed: " << error << '\n';
            return 1;
        }
        const std::filesystem::path out_file =
            path_from_utf8_str(options.output_path);
        if (!write_binary_file_atomic(out_file, ico_bytes, error)) {
            std::cerr << "Write .ico failed: " << error << '\n';
            return 1;
        }
        Json out = Json::object();
        out["input"] = options.icon_path;
        out["output"] = path_to_utf8_str(out_file);
        out["bytes"] = ico_bytes.size();
        std::cout << out.dump(2) << '\n';
        return 0;
    }

    if (options.cli_apply_exe) {
        std::vector<std::uint8_t> ico_bytes;
        IconRgbaImage preview{};
        if (!load_or_convert_to_ico_bytes(
                path_from_utf8_str(options.icon_path),
                ico_bytes,
                preview,
                error)) {
            std::cerr << "Icon load failed: " << error << '\n';
            return 1;
        }
        const std::filesystem::path exe_file =
            path_from_utf8_str(options.exe_path);
        if (!apply_ico_to_pe_executable(exe_file, ico_bytes, error)) {
            std::cerr << "Failed to stamp executable icon: " << error << '\n';
            return 1;
        }
        std::uint32_t frame_count = 0U;
        std::string read_err;
        static_cast<void>(
            read_pe_executable_icon_frame_count(exe_file, frame_count, read_err));
        Json out = Json::object();
        out["executable"] = path_to_utf8_str(exe_file);
        out["pe_icon_frames"] = frame_count;
        std::cout << out.dump(2) << '\n';
        return 0;
    }

    const std::filesystem::path project_dir =
        path_from_utf8_str(options.project_dir);
    if (!options.icon_path.empty()) {
        if (!set_project_icon(
                project_dir, path_from_utf8_str(options.icon_path), error)) {
            std::cerr << "Set project icon failed: " << error << '\n';
            return 1;
        }
    } else if (options.reset_default) {
        if (!reset_project_icon_to_default(project_dir, error)) {
            std::cerr << "Reset default icon failed: " << error << '\n';
            return 1;
        }
    }
    if (options.bundle_runtime) {
        BundleProjectRuntimeResult bundle_res{};
        if (!bundle_project_runtime(project_dir, bundle_res, error)) {
            std::cerr << "Bundle game runtime failed: " << error << '\n';
            return 1;
        }
    }

    const ProjectInspection insp = inspect_project_directory(project_dir);
    std::uint32_t frame_count = 0U;
    if (insp.runtime_bundled) {
        std::string pe_err;
        static_cast<void>(read_pe_executable_icon_frame_count(
            insp.executable_path, frame_count, pe_err));
    }
    Json out = Json::object();
    out["project_directory"] = path_to_utf8_str(insp.project_directory);
    out["icon_path"] = path_to_utf8_str(insp.icon_file_path);
    out["icon_exists"] = insp.icon_exists;
    out["executable"] = path_to_utf8_str(insp.executable_path);
    out["runtime_bundled"] = insp.runtime_bundled;
    out["pe_icon_frames"] = frame_count;
    std::cout << out.dump(2) << '\n';
    return 0;
}

void IconToolApplication::initialize_gui(const IconToolCliOptions& options) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fatal(SDL_GetError());
    }
    sdl_initialized_ = true;

    window_.reset(SDL_CreateWindow(
        "Vulkan Cube — Project Icon & Packaging Utility",
        static_cast<int>(options.width),
        static_cast<int>(options.height),
        SDL_WINDOW_VULKAN |
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!window_) {
        fatal(SDL_GetError());
    }
    SDL_SetWindowMinimumSize(window_.get(), 880, 540);
    apply_window_builtin_icon(window_.get(), BuiltinIconKind::icon_tool);

    context_.initialize(window_.get(), options.validation);
    swapchain_.initialize(
        context_,
        window_.get(),
        options.preferred_present_mode,
        false);
    initialize_frame_resources();
    rebuild_present_semaphores();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDpiScaleFonts = true;
    io.IniFilename = nullptr;

    ui_kit_.initialize();
    if (!ImGui_ImplSDL3_InitForVulkan(window_.get())) {
        fatal("Unable to initialize Dear ImGui SDL3 backend.");
    }
    initialize_imgui_backend();

    reload_registry();
    if (!options.project_dir.empty()) {
        select_project_directory(path_from_utf8_str(options.project_dir));
    } else if (!projects_.empty()) {
        selected_project_index_ = 0U;
        select_project_directory(projects_[0].entry.project_directory);
    }

    queue_candidate_from_builtin(
        BuiltinIconKind::project_default, "Built-in: Default Project Icon");
    running_ = true;
}

void IconToolApplication::initialize_frame_resources() {
    const VkDevice device = context_.device();
    const VkCommandPoolCreateInfo upload_pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = context_.queue_families().graphics,
    };
    VK_CHECK(vkCreateCommandPool(
        device,
        &upload_pool_info,
        nullptr,
        upload_command_pool_.put(device, vkDestroyCommandPool)));

    for (std::size_t i = 0U; i < frames_in_flight; ++i) {
        FrameSlot& slot = frames_[i];
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = context_.queue_families().graphics,
        };
        VK_CHECK(vkCreateCommandPool(
            device,
            &pool_info,
            nullptr,
            slot.command_pool.put(device, vkDestroyCommandPool)));

        const VkCommandBufferAllocateInfo alloc_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = slot.command_pool.get(),
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1U,
        };
        VK_CHECK(vkAllocateCommandBuffers(
            device, &alloc_info, &slot.command_buffer));

        const VkSemaphoreCreateInfo sem_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VK_CHECK(vkCreateSemaphore(
            device,
            &sem_info,
            nullptr,
            slot.image_available.put(device, vkDestroySemaphore)));

        const VkFenceCreateInfo fence_info{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        VK_CHECK(vkCreateFence(
            device,
            &fence_info,
            nullptr,
            slot.in_flight.put(device, vkDestroyFence)));
    }
}

void IconToolApplication::rebuild_present_semaphores() {
    present_semaphores_.clear();
    const std::size_t image_count = swapchain_.images().size();
    present_semaphores_.resize(image_count);
    const VkDevice device = context_.device();
    const VkSemaphoreCreateInfo sem_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    for (std::size_t i = 0U; i < image_count; ++i) {
        VK_CHECK(vkCreateSemaphore(
            device,
            &sem_info,
            nullptr,
            present_semaphores_[i].put(device, vkDestroySemaphore)));
    }
}

void IconToolApplication::initialize_imgui_backend() {
    imgui_color_format_ = swapchain_.color_format();
    imgui_rendering_info_.pColorAttachmentFormats = &imgui_color_format_;

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = context_.instance();
    init_info.PhysicalDevice = context_.physical_device();
    init_info.Device = context_.device();
    init_info.QueueFamily = context_.queue_families().graphics;
    init_info.Queue = context_.graphics_queue();
    init_info.DescriptorPoolSize = 256U;
    init_info.MinImageCount = 2U;
    init_info.ImageCount =
        static_cast<std::uint32_t>(swapchain_.images().size());
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo =
        imgui_rendering_info_;
    init_info.UseDynamicRendering = true;
    init_info.CheckVkResultFn = check_it_imgui_vk_result;
    init_info.MinAllocationSize = 1024U * 1024U;
    if (!ImGui_ImplVulkan_Init(&init_info)) {
        fatal("Unable to initialize Dear ImGui Vulkan backend.");
    }
    imgui_vulkan_initialized_ = true;
}

void IconToolApplication::shutdown_imgui_backend() noexcept {
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplVulkan_Shutdown();
    }
    imgui_vulkan_initialized_ = false;
}

void IconToolApplication::handle_swapchain_resize() {
    int pixel_w = 0;
    int pixel_h = 0;
    SDL_GetWindowSizeInPixels(window_.get(), &pixel_w, &pixel_h);
    if (pixel_w <= 0 || pixel_h <= 0) {
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(context_.device()));
    const VkFormat prev_format = swapchain_.color_format();
    const std::size_t prev_count = swapchain_.images().size();
    if (!swapchain_.recreate(options_.preferred_present_mode)) {
        return;
    }
    if (swapchain_.images().size() != prev_count) {
        rebuild_present_semaphores();
    }
    if (swapchain_.color_format() != prev_format ||
        swapchain_.images().size() != prev_count) {
        for (UiTexturePreview& old : retired_previews_) {
            old.destroy(imgui_vulkan_initialized_);
        }
        retired_previews_.clear();
        current_preview_.destroy(imgui_vulkan_initialized_);
        candidate_preview_.destroy(imgui_vulkan_initialized_);
        shutdown_imgui_backend();
        initialize_imgui_backend();
        current_preview_dirty_ = pending_current_rgba_.valid();
        candidate_preview_dirty_ = pending_candidate_rgba_.valid();
    }
    resize_requested_ = false;
}

void IconToolApplication::main_loop() {
    while (running_) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            process_event(event);
        }
        if (!running_) {
            break;
        }

        int pixel_width = 0;
        int pixel_height = 0;
        SDL_GetWindowSizeInPixels(window_.get(), &pixel_width, &pixel_height);
        if (pixel_width <= 0 || pixel_height <= 0) {
            SDL_WaitEventTimeout(nullptr, 50);
            continue;
        }
        if (resize_requested_) {
            handle_swapchain_resize();
            resize_requested_ = false;
        }
        draw_frame();
    }
}

void IconToolApplication::process_event(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        running_ = false;
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        resize_requested_ = true;
        break;
    case SDL_EVENT_DROP_FILE:
        if (event.drop.data != nullptr) {
            const std::filesystem::path dropped =
                path_from_utf8_str(event.drop.data);
            std::error_code ec;
            if (std::filesystem::is_directory(dropped, ec) && !ec) {
                select_project_directory(dropped);
            } else if (is_supported_icon_extension(dropped)) {
                queue_candidate_from_file(dropped);
            } else {
                status_message_ =
                    "Unsupported dropped item. Drop a project folder or .png/.ico/.jpg/.bmp/.tga image.";
                status_tone_ = EditorTone::warning;
            }
        }
        break;
    default:
        break;
    }
}

void IconToolApplication::sync_gpu_previews() {
    if (retired_previews_.empty() &&
        !current_preview_dirty_ &&
        !candidate_preview_dirty_) {
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(context_.device()));
    for (UiTexturePreview& old : retired_previews_) {
        old.destroy(imgui_vulkan_initialized_);
    }
    retired_previews_.clear();

    if (current_preview_dirty_) {
        current_preview_.destroy(imgui_vulkan_initialized_);
        if (pending_current_rgba_.valid()) {
            std::string err;
            if (!current_preview_.upload(
                    context_,
                    upload_command_pool_.get(),
                    pending_current_rgba_,
                    "IconToolCurrentPreview",
                    err,
                    imgui_vulkan_initialized_)) {
                status_message_ = "Preview upload error: " + err;
                status_tone_ = EditorTone::warning;
            }
        }
        current_preview_dirty_ = false;
    }

    if (candidate_preview_dirty_) {
        candidate_preview_.destroy(imgui_vulkan_initialized_);
        if (pending_candidate_rgba_.valid()) {
            std::string err;
            if (!candidate_preview_.upload(
                    context_,
                    upload_command_pool_.get(),
                    pending_candidate_rgba_,
                    "IconToolCandidatePreview",
                    err,
                    imgui_vulkan_initialized_)) {
                status_message_ = "Candidate preview upload error: " + err;
                status_tone_ = EditorTone::warning;
            }
        }
        candidate_preview_dirty_ = false;
    }
}

void IconToolApplication::draw_frame() {
    FrameSlot& frame = frames_[frame_index_];
    const VkDevice device = context_.device();

    const VkFence fence = frame.in_flight.get();
    VK_CHECK(vkWaitForFences(device, 1U, &fence, VK_TRUE, UINT64_MAX));

    sync_gpu_previews();

    std::uint32_t image_index = 0U;
    const VkResult acquire = vkAcquireNextImageKHR(
        device,
        swapchain_.handle(),
        UINT64_MAX,
        frame.image_available.get(),
        VK_NULL_HANDLE,
        &image_index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        handle_swapchain_resize();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(acquire);
    }
    if (acquire == VK_SUBOPTIMAL_KHR) {
        resize_requested_ = true;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    build_ui();
    ImGui::Render();
    ImDrawData* const draw_data = ImGui::GetDrawData();

    VK_CHECK(vkResetFences(device, 1U, &fence));
    VK_CHECK(vkResetCommandPool(device, frame.command_pool.get(), 0U));

    const VkCommandBuffer cmd = frame.command_buffer;
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

    const VkImage color_image = swapchain_.images()[image_index];
    transition_image(
        cmd,
        color_image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const VkExtent2D extent = swapchain_.extent();
    const VkRenderingAttachmentInfo color_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = swapchain_.image_view(image_index),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = VkClearValue{
            .color = VkClearColorValue{{0.045F, 0.052F, 0.065F, 1.0F}},
        },
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = VkRect2D{
            .offset = {0, 0},
            .extent = extent,
        },
        .layerCount = 1U,
        .colorAttachmentCount = 1U,
        .pColorAttachments = &color_attachment,
    };
    vkCmdBeginRendering(cmd, &rendering_info);
    if (draw_data != nullptr) {
        ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
    }
    vkCmdEndRendering(cmd);

    transition_image(
        cmd,
        color_image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE);

    VK_CHECK(vkEndCommandBuffer(cmd));

    if (image_index >= present_semaphores_.size()) {
        fatal("Swapchain image index exceeded present semaphore pool.");
    }
    const VkSemaphore render_finished = present_semaphores_[image_index].get();
    const VkSemaphoreSubmitInfo wait_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = frame.image_available.get(),
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkCommandBufferSubmitInfo cmd_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    const VkSemaphoreSubmitInfo signal_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = render_finished,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1U,
        .pWaitSemaphoreInfos = &wait_info,
        .commandBufferInfoCount = 1U,
        .pCommandBufferInfos = &cmd_info,
        .signalSemaphoreInfoCount = 1U,
        .pSignalSemaphoreInfos = &signal_info,
    };
    VK_CHECK(vkQueueSubmit2(
        context_.graphics_queue(), 1U, &submit_info, fence));

    const VkSwapchainKHR sc_handle = swapchain_.handle();
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &render_finished,
        .swapchainCount = 1U,
        .pSwapchains = &sc_handle,
        .pImageIndices = &image_index,
    };
    const VkResult present =
        vkQueuePresentKHR(context_.present_queue(), &present_info);
    if (present == VK_ERROR_OUT_OF_DATE_KHR ||
        present == VK_SUBOPTIMAL_KHR ||
        resize_requested_) {
        handle_swapchain_resize();
        resize_requested_ = false;
    } else if (present != VK_SUCCESS) {
        VK_CHECK(present);
    }

    frame_index_ = (frame_index_ + 1U) % frames_in_flight;
    ++rendered_frames_;
    if (options_.smoke_frames > 0U &&
        rendered_frames_ >= options_.smoke_frames) {
        running_ = false;
    }
}

void IconToolApplication::shutdown() noexcept {
    running_ = false;
    if (context_.device() != VK_NULL_HANDLE) {
        const VkResult idle = vkDeviceWaitIdle(context_.device());
        if (idle != VK_SUCCESS) {
            std::cerr << "Icon Tool vkDeviceWaitIdle failed: "
                      << static_cast<int>(idle) << '\n';
        }
    }
    for (UiTexturePreview& old : retired_previews_) {
        old.destroy(imgui_vulkan_initialized_);
    }
    retired_previews_.clear();
    current_preview_.destroy(imgui_vulkan_initialized_);
    candidate_preview_.destroy(imgui_vulkan_initialized_);

    shutdown_imgui_backend();
    ui_kit_.shutdown();
    if (ImGui::GetCurrentContext() != nullptr) {
        if (ImGui::GetIO().BackendPlatformUserData != nullptr) {
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext();
    }
    upload_command_pool_.reset();
    for (FrameSlot& slot : frames_) {
        slot.in_flight.reset();
        slot.image_available.reset();
        slot.command_buffer = VK_NULL_HANDLE;
        slot.command_pool.reset();
    }
    present_semaphores_.clear();
    swapchain_.shutdown();
    context_.shutdown();
    window_.reset();
    if (sdl_initialized_) {
        SDL_Quit();
        sdl_initialized_ = false;
    }
}

void IconToolApplication::reload_registry() {
    std::error_code ec;
    const bool existed =
        std::filesystem::is_regular_file(registry_file_path_, ec) && !ec;
    std::string error;
    if (!registry_.load_or_initialize(registry_file_path_, error)) {
        status_message_ = "Registry load failed: " + error;
        status_tone_ = EditorTone::warning;
    } else if (!existed) {
        const std::filesystem::path default_dir =
            default_projects_root_directory();
        if (std::filesystem::is_directory(default_dir, ec) && !ec) {
            std::string scan_err;
            if (registry_.scan_directory(default_dir, scan_err) > 0U) {
                static_cast<void>(registry_.save(registry_file_path_, scan_err));
            }
        }
    }

    projects_.clear();
    for (const ProjectRegistryEntry& entry : registry_.entries()) {
        RegisteredItem item{};
        item.entry = entry;
        item.inspection = inspect_project_directory(entry.project_directory);
        item.directory_utf8 = path_to_utf8_str(entry.project_directory);
        projects_.push_back(std::move(item));
    }
}

void IconToolApplication::select_project_directory(
    const std::filesystem::path& project_dir) {
    target_project_dir_ = project_dir;
    copy_to_cstr_buffer(target_project_buffer_, path_to_utf8_str(project_dir));

    selected_project_index_.reset();
    for (std::size_t i = 0U; i < projects_.size(); ++i) {
        std::error_code ec;
        if (projects_[i].entry.project_directory == project_dir ||
            std::filesystem::equivalent(
                projects_[i].entry.project_directory, project_dir, ec)) {
            selected_project_index_ = i;
            break;
        }
    }
    refresh_target_inspection();
}

void IconToolApplication::refresh_target_inspection() {
    target_pe_icon_frames_ = 0U;
    target_pe_diagnostic_.clear();

    if (target_project_dir_.empty()) {
        target_inspection_ = ProjectInspection{};
        if (current_preview_.valid()) {
            retired_previews_.push_back(std::move(current_preview_));
        }
        pending_current_rgba_ = IconRgbaImage{};
        current_preview_dirty_ = true;
        return;
    }

    target_inspection_ = inspect_project_directory(target_project_dir_);
    if (target_inspection_.runtime_bundled) {
        std::string pe_err;
        if (!read_pe_executable_icon_frame_count(
                target_inspection_.executable_path,
                target_pe_icon_frames_,
                pe_err)) {
            target_pe_diagnostic_ = pe_err;
        }
    }

    if (current_preview_.valid()) {
        retired_previews_.push_back(std::move(current_preview_));
    }
    if (target_inspection_.icon_exists) {
        IconRgbaImage loaded{};
        std::string load_err;
        if (load_icon_source_image(
                target_inspection_.icon_file_path, loaded, load_err)) {
            pending_current_rgba_ = clamp_preview_size(loaded);
        } else {
            pending_current_rgba_ = generate_builtin_icon_rgba(
                BuiltinIconKind::project_default, 256U);
            status_message_ = "Current icon read warning: " + load_err;
            status_tone_ = EditorTone::warning;
        }
    } else {
        pending_current_rgba_ = generate_builtin_icon_rgba(
            BuiltinIconKind::project_default, 256U);
    }
    current_preview_dirty_ = true;

    if (selected_project_index_ &&
        *selected_project_index_ < projects_.size()) {
        projects_[*selected_project_index_].inspection = target_inspection_;
    }
}

void IconToolApplication::queue_candidate_from_file(
    const std::filesystem::path& file_path) {
    IconRgbaImage loaded{};
    std::string err;
    if (!load_icon_source_image(file_path, loaded, err)) {
        status_message_ = "Failed to load icon image: " + err;
        status_tone_ = EditorTone::danger;
        return;
    }
    const std::string utf8 = path_to_utf8_str(file_path);
    copy_to_cstr_buffer(candidate_icon_buffer_, utf8);
    candidate_source_label_ = path_to_utf8_str(file_path.filename());
    candidate_builtin_.reset();

    if (candidate_preview_.valid()) {
        retired_previews_.push_back(std::move(candidate_preview_));
    }
    pending_candidate_rgba_ = clamp_preview_size(loaded);
    candidate_preview_dirty_ = true;

    status_message_ = "Loaded candidate icon: " + candidate_source_label_;
    status_tone_ = EditorTone::info;
}

void IconToolApplication::queue_candidate_from_builtin(
    const BuiltinIconKind kind,
    const char* const label) {
    candidate_builtin_ = kind;
    candidate_icon_buffer_.fill('\0');
    candidate_source_label_ = label;

    if (candidate_preview_.valid()) {
        retired_previews_.push_back(std::move(candidate_preview_));
    }
    pending_candidate_rgba_ = generate_builtin_icon_rgba(kind, 256U);
    candidate_preview_dirty_ = true;
}

void IconToolApplication::apply_candidate_to_project() {
    if (target_inspection_.state != ProjectDiskState::ready) {
        status_message_ = "Select a valid ready project directory first.";
        status_tone_ = EditorTone::danger;
        return;
    }

    std::string err;
    if (candidate_builtin_) {
        std::vector<std::uint8_t> ico_bytes;
        if (!encode_builtin_ico(*candidate_builtin_, ico_bytes, err)) {
            status_message_ = "Built-in icon encode failed: " + err;
            status_tone_ = EditorTone::danger;
            return;
        }
        std::filesystem::path icon_target = target_inspection_.icon_file_path;
        if (icon_target.empty()) {
            icon_target = target_inspection_.project_directory / "icon.ico";
        }
        if (!write_binary_file_atomic(icon_target, ico_bytes, err)) {
            status_message_ = "Failed writing project icon.ico: " + err;
            status_tone_ = EditorTone::danger;
            return;
        }
        if (target_inspection_.runtime_bundled) {
            if (!apply_ico_to_pe_executable(
                    target_inspection_.executable_path, ico_bytes, err)) {
                status_message_ =
                    "Updated icon.ico, but PE executable icon update failed: " + err;
                status_tone_ = EditorTone::warning;
                refresh_target_inspection();
                return;
            }
        }
    } else if (candidate_icon_buffer_[0] != '\0') {
        const std::filesystem::path src =
            path_from_utf8_str(candidate_icon_buffer_.data());
        if (!set_project_icon(target_inspection_.project_directory, src, err)) {
            status_message_ = "Failed applying icon: " + err;
            status_tone_ = EditorTone::danger;
            return;
        }
    } else {
        status_message_ = "Choose an image file or built-in preset first.";
        status_tone_ = EditorTone::warning;
        return;
    }

    refresh_target_inspection();
    status_message_ = target_inspection_.runtime_bundled
        ? "Applied icon to project icon.ico and standalone .exe!"
        : "Applied icon.ico to project (bundle game .exe to embed into executable).";
    status_tone_ = EditorTone::success;
}

void IconToolApplication::reset_target_to_default_icon() {
    if (target_inspection_.state != ProjectDiskState::ready) {
        status_message_ = "Select a valid ready project directory first.";
        status_tone_ = EditorTone::danger;
        return;
    }
    std::string err;
    if (!reset_project_icon_to_default(
            target_inspection_.project_directory, err)) {
        status_message_ = "Reset icon failed: " + err;
        status_tone_ = EditorTone::danger;
        return;
    }
    queue_candidate_from_builtin(
        BuiltinIconKind::project_default, "Built-in: Default Project Icon");
    refresh_target_inspection();
    status_message_ = "Restored project icon to Default Project Icon.";
    status_tone_ = EditorTone::success;
}

void IconToolApplication::bundle_target_project_runtime() {
    if (target_inspection_.state != ProjectDiskState::ready) {
        status_message_ = "Select a valid ready project directory first.";
        status_tone_ = EditorTone::danger;
        return;
    }
    BundleProjectRuntimeResult res{};
    std::string err;
    if (!bundle_project_runtime(
            target_inspection_.project_directory, res, err)) {
        status_message_ = "Build/update game executable failed: " + err;
        status_tone_ = EditorTone::danger;
        return;
    }
    refresh_target_inspection();
    status_message_ =
        "Bundled standalone executable '" +
        path_to_utf8_str(res.executable_path.filename()) +
        "' (" + std::to_string(res.copied_shaders) + " SPIR-V shaders).";
    status_tone_ = EditorTone::success;
}

void IconToolApplication::launch_target_game() {
    if (target_inspection_.state != ProjectDiskState::ready) {
        status_message_ = "Select a valid ready project directory first.";
        status_tone_ = EditorTone::danger;
        return;
    }
    if (!target_inspection_.runtime_bundled) {
        bundle_target_project_runtime();
        if (!target_inspection_.runtime_bundled) {
            return;
        }
    }
    std::string err;
    if (!launch_detached_executable(
            target_inspection_.executable_path,
            {},
            target_inspection_.project_directory,
            err)) {
        status_message_ = "Failed launching game: " + err;
        status_tone_ = EditorTone::danger;
        return;
    }
    status_message_ =
        "Launched '" +
        path_to_utf8_str(target_inspection_.executable_path.filename()) + "'.";
    status_tone_ = EditorTone::success;
}

void IconToolApplication::open_browser_modal(
    const BrowserTarget target,
    const std::filesystem::path& initial_dir) {
    browser_target_ = target;
    browser_error_.clear();
    browser_drives_.clear();

#if defined(_WIN32)
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1U << i)) != 0U) {
            const char drive_letter = static_cast<char>('A' + i);
            const std::string root = std::string(1U, drive_letter) + ":\\";
            browser_drives_.push_back(path_from_utf8_str(root));
        }
    }
#endif

    std::error_code ec;
    std::filesystem::path start = initial_dir;
    if (start.empty() || !std::filesystem::is_directory(start, ec) || ec) {
        ec.clear();
        start = default_projects_root_directory();
        if (!std::filesystem::is_directory(start, ec) || ec) {
            ec.clear();
            start = std::filesystem::current_path(ec);
        }
    }
    navigate_browser(start);
    open_browser_requested_ = true;
}

void IconToolApplication::navigate_browser(
    const std::filesystem::path& directory) {
    browser_error_.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) {
        browser_error_ = "Directory is not accessible.";
        return;
    }
    const auto canonical_or_abs =
        std::filesystem::weakly_canonical(directory, ec);
    browser_current_dir_ =
        (!ec && !canonical_or_abs.empty()) ? canonical_or_abs : directory;
    browser_current_utf8_ = path_to_utf8_str(browser_current_dir_);

    browser_subdirs_.clear();
    browser_files_.clear();
    ec.clear();
    for (const auto& entry :
         std::filesystem::directory_iterator(browser_current_dir_, ec)) {
        if (ec) {
            break;
        }
        std::error_code item_ec;
        if (entry.is_directory(item_ec) && !item_ec) {
            browser_subdirs_.push_back(entry.path());
        } else if (browser_target_ == BrowserTarget::icon_file) {
            item_ec.clear();
            if (entry.is_regular_file(item_ec) && !item_ec &&
                is_supported_icon_extension(entry.path())) {
                browser_files_.push_back(entry.path());
            }
        }
    }
    std::sort(browser_subdirs_.begin(), browser_subdirs_.end());
    std::sort(browser_files_.begin(), browser_files_.end());
}

void IconToolApplication::build_ui() {
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0F, 14.0F));
    const bool open = ImGui::Begin("##IconToolRoot", nullptr, host_flags);
    ImGui::PopStyleVar(2);

    if (open) {
        build_header_bar();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float avail_h = ImGui::GetContentRegionAvail().y - 26.0F;
        const float left_w = (std::max)(260.0F, avail_w * 0.34F);

        if (ImGui::BeginChild(
                "##IconLeftPanel",
                ImVec2(left_w, (std::max)(160.0F, avail_h)),
                ImGuiChildFlags_Borders)) {
            build_left_panel();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0F, 12.0F);

        if (ImGui::BeginChild(
                "##IconRightPanel",
                ImVec2(0.0F, (std::max)(160.0F, avail_h)),
                ImGuiChildFlags_Borders)) {
            build_right_panel();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (!status_message_.empty()) {
            ui_kit_.status_text(status_message_.c_str(), status_tone_);
        } else {
            ImGui::TextDisabled(
                "Tip: Drag and drop a .png/.ico/.jpg/.bmp/.tga image or a project folder anywhere onto this window.");
        }
    }
    ImGui::End();

    build_browser_modal();
}

void IconToolApplication::build_header_bar() {
    ui_kit_.brand_lockup();
    ImGui::SameLine(0.0F, 14.0F);
    ui_kit_.badge("ICON & GAME PACKAGER", EditorTone::primary);
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::TextDisabled(
        "Convert images to multi-size .ico and stamp portable project executables");

    const float right_buttons_w = 240.0F;
    const float button_x =
        (std::max)(420.0F, ImGui::GetWindowWidth() - right_buttons_w);
    ImGui::SameLine(button_x);
    if (ui_kit_.action_button(
            "RefreshIconTool",
            EditorIcon::refresh,
            "Refresh",
            EditorButtonStyle::secondary)) {
        reload_registry();
        refresh_target_inspection();
        status_message_ = "Refreshed project status.";
        status_tone_ = EditorTone::info;
    }
}

void IconToolApplication::build_left_panel() {
    ui_kit_.panel_header(
        EditorIcon::folder,
        "Projects",
        "Select a registered project or browse for any project folder");

    if (ui_kit_.action_button(
            "BrowseProjectDirBtn",
            EditorIcon::folder,
            "Browse Project Folder...",
            EditorButtonStyle::secondary,
            true,
            false,
            ImVec2(-1.0F, 0.0F))) {
        open_browser_modal(
            BrowserTarget::project_folder,
            target_project_dir_.empty()
                ? default_projects_root_directory()
                : target_project_dir_.parent_path());
    }

    ImGui::Spacing();
    const float list_h = (std::max)(120.0F, ImGui::GetContentRegionAvail().y - 68.0F);
    if (ImGui::BeginChild(
            "##IconToolProjectList",
            ImVec2(0.0F, list_h),
            ImGuiChildFlags_Borders)) {
        for (std::size_t i = 0U; i < projects_.size(); ++i) {
            const RegisteredItem& item = projects_[i];
            const bool selected =
                selected_project_index_ && *selected_project_index_ == i;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable("##ProjRow", selected, 0, ImVec2(0.0F, 42.0F))) {
                select_project_directory(item.entry.project_directory);
            }
            const ImVec2 row_min = ImGui::GetItemRectMin();
            const ImVec2 row_max = ImGui::GetItemRectMax();
            ImGui::SetCursorScreenPos(ImVec2(row_min.x + 8.0F, row_min.y + 4.0F));
            ImGui::TextUnformatted(item.entry.name.c_str());
            ImGui::SameLine(0.0F, 8.0F);
            ui_kit_.badge(
                item.inspection.runtime_bundled ? "EXE" : "NO EXE",
                item.inspection.runtime_bundled
                    ? EditorTone::success
                    : EditorTone::warning);
            ImGui::SetCursorScreenPos(ImVec2(row_min.x + 8.0F, row_min.y + 22.0F));
            ImGui::TextDisabled("%s", item.directory_utf8.c_str());
            ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_max.y));
            ImGui::Dummy(ImVec2(0.0F, 2.0F));
            ImGui::PopID();
        }
        if (projects_.empty()) {
            ui_kit_.empty_state(
                EditorIcon::folder,
                "No projects in registry",
                "Use Browse Project Folder... above to open a project.");
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextDisabled("Direct Project Path");
    ImGui::SetNextItemWidth(-76.0F);
    ImGui::InputText(
        "##DirectProjInput",
        target_project_buffer_.data(),
        target_project_buffer_.size());
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::Button("Load", ImVec2(66.0F, 0.0F)) &&
        target_project_buffer_[0] != '\0') {
        select_project_directory(
            path_from_utf8_str(target_project_buffer_.data()));
    }
}

void IconToolApplication::build_right_panel() {
    ui_kit_.panel_header(
        EditorIcon::cube,
        "Project Packaging & Icon Editor",
        "Inspect standalone game executable and apply custom multi-size icons");

    if (target_project_dir_.empty()) {
        ui_kit_.empty_state(
            EditorIcon::folder,
            "No project selected",
            "Choose a project from the list or drop a project folder into this window.");
        return;
    }

    const bool ready = target_inspection_.state == ProjectDiskState::ready;
    ui_kit_.section_header(EditorIcon::brand, "Current Project State");

    if (ui_kit_.begin_property_grid("##CurrentProjectGrid")) {
        ui_kit_.property_row("Project");
        ImGui::TextUnformatted(
            ready ? target_inspection_.manifest.name.c_str()
                  : "(Invalid or Missing Manifest)");

        ui_kit_.property_row("Status");
        ui_kit_.badge(
            project_disk_state_label(target_inspection_.state),
            ready ? EditorTone::success : EditorTone::danger);

        if (ready) {
            ui_kit_.property_row("Game Executable");
            ImGui::TextUnformatted(
                target_inspection_.manifest.executable.c_str());
            ImGui::SameLine(0.0F, 8.0F);
            ui_kit_.badge(
                target_inspection_.runtime_bundled ? "BUNDLED" : "NOT BUNDLED",
                target_inspection_.runtime_bundled
                    ? EditorTone::success
                    : EditorTone::warning);

            if (target_inspection_.runtime_bundled) {
                ui_kit_.property_row("Embedded PE Icons");
                ImGui::Text("%u frame(s)", target_pe_icon_frames_);
                if (!target_pe_diagnostic_.empty()) {
                    ImGui::SameLine(0.0F, 8.0F);
                    ui_kit_.status_text(
                        target_pe_diagnostic_.c_str(), EditorTone::warning);
                }
            }

            ui_kit_.property_row("Project icon.ico");
            ImGui::Text(
                "%s (%s)",
                target_inspection_.manifest.icon_path.c_str(),
                target_inspection_.icon_exists ? "present" : "missing");
        }
        ui_kit_.end_property_grid();
    }

    if (ready) {
        ImGui::Spacing();
        ImGui::TextDisabled("Current Project Icon Previews:");
        const ImVec4 border = EditorUiKit::border_color();
        draw_preview_with_border(current_preview_, 64.0F, border);
        ImGui::SameLine(0.0F, 10.0F);
        draw_preview_with_border(current_preview_, 32.0F, border);
        ImGui::SameLine(0.0F, 10.0F);
        draw_preview_with_border(current_preview_, 16.0F, border);

        ImGui::Spacing();
        if (ui_kit_.action_button(
                "BuildUpdateExeBtn",
                EditorIcon::cube,
                target_inspection_.runtime_bundled
                    ? "Update Standalone Game (.exe)"
                    : "Build Standalone Game (.exe)",
                EditorButtonStyle::secondary,
                true)) {
            bundle_target_project_runtime();
        }
        ImGui::SameLine(0.0F, 8.0F);
        if (ui_kit_.action_button(
                "LaunchGameBtn",
                EditorIcon::play,
                "Launch Game",
                EditorButtonStyle::primary,
                target_inspection_.runtime_bundled)) {
            launch_target_game();
        }
        ImGui::SameLine(0.0F, 8.0F);
        if (ui_kit_.action_button(
                "OpenFolderBtn",
                EditorIcon::folder,
                "Open Folder",
                EditorButtonStyle::quiet)) {
            std::string err;
            if (!open_directory_in_file_explorer(
                    target_inspection_.project_directory, err)) {
                status_message_ = err;
                status_tone_ = EditorTone::danger;
            }
        }
    } else if (!target_inspection_.diagnostic.empty()) {
        ui_kit_.status_text(
            target_inspection_.diagnostic.c_str(), EditorTone::danger);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ui_kit_.section_header(EditorIcon::save, "New Icon Source");
    ImGui::TextDisabled(
        "Drop a .png / .ico / .jpg / .bmp / .tga file here, choose a file, or pick a built-in preset.");

    ImGui::SetNextItemWidth(-180.0F);
    ImGui::InputTextWithHint(
        "##CandidateIconPath",
        "Path to .png, .ico, .jpg, .bmp, or .tga...",
        candidate_icon_buffer_.data(),
        candidate_icon_buffer_.size());
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::Button("Load Path", ImVec2(80.0F, 0.0F)) &&
        candidate_icon_buffer_[0] != '\0') {
        queue_candidate_from_file(
            path_from_utf8_str(candidate_icon_buffer_.data()));
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::Button("Browse...", ImVec2(88.0F, 0.0F))) {
        open_browser_modal(
            BrowserTarget::icon_file,
            target_project_dir_.empty()
                ? std::filesystem::current_path()
                : target_project_dir_);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Built-in Presets:");
    if (ImGui::SmallButton("Project Default")) {
        queue_candidate_from_builtin(
            BuiltinIconKind::project_default, "Built-in: Default Project Icon");
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::SmallButton("Engine")) {
        queue_candidate_from_builtin(
            BuiltinIconKind::engine, "Built-in: Engine Icon");
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::SmallButton("Editor")) {
        queue_candidate_from_builtin(
            BuiltinIconKind::editor, "Built-in: Editor Icon");
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::SmallButton("Project Manager")) {
        queue_candidate_from_builtin(
            BuiltinIconKind::project_manager, "Built-in: Project Manager Icon");
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::SmallButton("Icon Utility")) {
        queue_candidate_from_builtin(
            BuiltinIconKind::icon_tool, "Built-in: Icon Utility Icon");
    }

    ImGui::Spacing();
    ImGui::Text(
        "Candidate Preview (%s):",
        candidate_source_label_.empty() ? "None" : candidate_source_label_.c_str());
    const ImVec4 active_border = EditorUiKit::color(EditorTone::primary);
    draw_preview_with_border(candidate_preview_, 128.0F, active_border);
    ImGui::SameLine(0.0F, 12.0F);
    draw_preview_with_border(candidate_preview_, 64.0F, active_border);
    ImGui::SameLine(0.0F, 10.0F);
    draw_preview_with_border(candidate_preview_, 32.0F, active_border);
    ImGui::SameLine(0.0F, 10.0F);
    draw_preview_with_border(candidate_preview_, 16.0F, active_border);

    ImGui::Spacing();
    if (ui_kit_.action_button(
            "ApplyIconToProjectBtn",
            EditorIcon::save,
            "Apply Icon to Project",
            EditorButtonStyle::primary,
            ready && (candidate_builtin_.has_value() ||
                      candidate_icon_buffer_[0] != '\0'))) {
        apply_candidate_to_project();
    }
    ImGui::SameLine(0.0F, 8.0F);
    if (ui_kit_.action_button(
            "ResetProjectDefaultIconBtn",
            EditorIcon::revert,
            "Reset Default Icon",
            EditorButtonStyle::secondary,
            ready)) {
        reset_target_to_default_icon();
    }
}

void IconToolApplication::build_browser_modal() {
    if (open_browser_requested_) {
        ImGui::OpenPopup("Select File or Directory");
        open_browser_requested_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(600.0F, 460.0F), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Select File or Directory", nullptr)) {
        return;
    }

    if (!browser_drives_.empty()) {
        for (std::size_t d = 0U; d < browser_drives_.size(); ++d) {
            if (d > 0U) {
                ImGui::SameLine(0.0F, 4.0F);
            }
            const std::string label = path_to_utf8_str(browser_drives_[d]);
            if (ImGui::SmallButton(label.c_str())) {
                navigate_browser(browser_drives_[d]);
            }
        }
    }

    const std::filesystem::path parent = browser_current_dir_.parent_path();
    const bool can_go_up =
        !parent.empty() && parent != browser_current_dir_;
    if (!can_go_up) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Up")) {
        navigate_browser(parent);
    }
    if (!can_go_up) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0.0F, 8.0F);
    ImGui::TextWrapped("%s", browser_current_utf8_.c_str());

    ImGui::Separator();

    const float list_h = ImGui::GetContentRegionAvail().y - 54.0F;
    if (ImGui::BeginChild(
            "##IconToolBrowserEntries",
            ImVec2(0.0F, (std::max)(120.0F, list_h)),
            ImGuiChildFlags_Borders)) {
        std::optional<std::filesystem::path> next_dir;
        for (std::size_t i = 0U; i < browser_subdirs_.size(); ++i) {
            const std::string label =
                "[DIR] " + path_to_utf8_str(browser_subdirs_[i].filename());
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), false)) {
                next_dir = browser_subdirs_[i];
            }
            ImGui::PopID();
        }
        std::optional<std::filesystem::path> picked_file;
        for (std::size_t i = 0U; i < browser_files_.size(); ++i) {
            const std::string label =
                path_to_utf8_str(browser_files_[i].filename());
            ImGui::PushID(static_cast<int>(100000U + i));
            if (ImGui::Selectable(label.c_str(), false)) {
                picked_file = browser_files_[i];
            }
            ImGui::PopID();
        }
        if (browser_subdirs_.empty() && browser_files_.empty()) {
            ImGui::TextDisabled("(No subdirectories or matching icon files)");
        }
        if (next_dir) {
            navigate_browser(*next_dir);
        } else if (picked_file) {
            queue_candidate_from_file(*picked_file);
            browser_target_ = BrowserTarget::none;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();

    if (!browser_error_.empty()) {
        ui_kit_.status_text(browser_error_.c_str(), EditorTone::danger);
    }

    if (browser_target_ == BrowserTarget::project_folder) {
        if (ui_kit_.action_button(
                "SelectCurrentProjectFolderBtn",
                EditorIcon::folder,
                "Select Current Folder",
                EditorButtonStyle::primary)) {
            select_project_directory(browser_current_dir_);
            browser_target_ = BrowserTarget::none;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0.0F, 8.0F);
    }
    if (ImGui::Button("Cancel", ImVec2(100.0F, 0.0F))) {
        browser_target_ = BrowserTarget::none;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace vcr
