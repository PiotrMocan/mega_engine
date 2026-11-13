#include "project_manager_application.hpp"

#include "image_barrier.hpp"
#include "vulkan_common.hpp"

#include <imgui_impl_sdl3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
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

void check_pm_imgui_vk_result(const VkResult result) {
    if (result != VK_SUCCESS) {
        fatal(
            "Project Manager ImGui Vulkan error: " +
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

[[nodiscard]] std::string ascii_lowercase(const std::string_view input) {
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

void print_pm_usage() {
    std::cout
        << "Vulkan Cube Project Manager\n\n"
        << "Usage:\n"
        << "  vulkan_project_manager [options]\n\n"
        << "GUI Options:\n"
        << "  --width N               Initial window width (default: 1380)\n"
        << "  --height N              Initial window height (default: 840)\n"
        << "  --present MODE          auto | immediate | mailbox | fifo\n"
        << "  --validation            Enable VK_LAYER_KHRONOS_validation\n"
        << "  --no-validation         Disable validation layer\n"
        << "  --smoke-frames N        Render N UI frames and exit cleanly\n\n"
        << "Registry & Headless CLI Commands:\n"
        << "  --registry PATH         Override project_registry.json path\n"
        << "  --list                  Print JSON list of registered projects\n"
        << "  --create NAME           Create a new project and register it\n"
        << "    --location DIR        Parent folder (default: <engine>/projects)\n"
        << "    --folder SLUG         Override project directory name\n"
        << "    --template KIND       standard | empty (default: standard)\n"
        << "    --description TEXT    Optional project description\n"
        << "  --register DIR          Register an existing project directory\n"
        << "    --init-manifest       Create project.vcrproject.json if missing\n"
        << "  --remove DIR            Remove a project from registry (keeps files)\n"
        << "  --scan DIR              Scan immediate subfolders for projects\n"
        << "  --build-game DIR        Bundle standalone game .exe and runtime in DIR\n"
        << "  --build-all-games       Bundle standalone game .exe for all ready projects\n"
        << "  --set-icon DIR          Apply --icon image/.ico to project DIR\n"
        << "    --icon FILE           Source .png/.ico/.jpg/.bmp/.tga for --set-icon\n"
        << "  -h, --help              Show this help text\n";
}

[[nodiscard]] bool is_supported_pm_icon_extension(
    const std::filesystem::path& path) {
    const std::string ext = ascii_lowercase(path_to_utf8_str(path.extension()));
    return ext == ".png" || ext == ".ico" || ext == ".jpg" ||
           ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

#if defined(_WIN32)
[[nodiscard]] std::string executable_name_editor() noexcept {
    return "vulkan_editor.exe";
}
[[nodiscard]] std::string executable_name_runtime() noexcept {
    return "vulkan_cube.exe";
}
[[nodiscard]] std::string executable_name_icon_tool() noexcept {
    return "vulkan_icon_tool.exe";
}
#else
[[nodiscard]] std::string executable_name_editor() noexcept {
    return "vulkan_editor";
}
[[nodiscard]] std::string executable_name_runtime() noexcept {
    return "vulkan_cube";
}
[[nodiscard]] std::string executable_name_icon_tool() noexcept {
    return "vulkan_icon_tool";
}
#endif

} // namespace

void ProjectManagerWindowDeleter::operator()(SDL_Window* const window) const noexcept {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
    }
}

ProjectManagerApplication::~ProjectManagerApplication() noexcept {
    shutdown();
}

bool ProjectManagerApplication::parse_cli(
    const int argc,
    char** const argv,
    ProjectManagerCliOptions& out,
    std::string& error) const {
    error.clear();
    out = ProjectManagerCliOptions{};

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        const auto require_value = [&](std::string_view& value_out) -> bool {
            if (i + 1 >= argc) {
                error = "Missing value after " + std::string(arg);
                return false;
            }
            value_out = argv[++i];
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            out.show_help = true;
            return true;
        }
        if (arg == "--list") {
            out.cli_list = true;
            continue;
        }
        if (arg == "--init-manifest") {
            out.cli_init_manifest = true;
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
        if (arg == "--registry") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.registry_path = std::string(val);
            continue;
        }
        if (arg == "--create") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.cli_create = true;
            out.create_name = std::string(val);
            continue;
        }
        if (arg == "--location") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.create_location = std::string(val);
            continue;
        }
        if (arg == "--folder") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.create_folder = std::string(val);
            continue;
        }
        if (arg == "--description") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.create_description = std::string(val);
            continue;
        }
        if (arg == "--template") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            const auto parsed_tpl = parse_project_template_kind(val);
            if (!parsed_tpl) {
                error = "Unknown --template '" + std::string(val) +
                        "' (expected standard or empty).";
                return false;
            }
            out.create_template = *parsed_tpl;
            continue;
        }
        if (arg == "--register") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.cli_register = true;
            out.target_directory = std::string(val);
            continue;
        }
        if (arg == "--remove") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.cli_remove = true;
            out.target_directory = std::string(val);
            continue;
        }
        if (arg == "--scan") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.cli_scan = true;
            out.target_directory = std::string(val);
            continue;
        }
        if (arg == "--build-game") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.cli_build_game = true;
            out.target_directory = std::string(val);
            continue;
        }
        if (arg == "--build-all-games") {
            out.cli_build_all_games = true;
            continue;
        }
        if (arg == "--set-icon") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.cli_set_icon = true;
            out.target_directory = std::string(val);
            continue;
        }
        if (arg == "--icon") {
            std::string_view val;
            if (!require_value(val)) {
                return false;
            }
            out.icon_source_path = std::string(val);
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

    const int action_count =
        (out.cli_list ? 1 : 0) +
        (out.cli_create ? 1 : 0) +
        (out.cli_register ? 1 : 0) +
        (out.cli_remove ? 1 : 0) +
        (out.cli_scan ? 1 : 0) +
        (out.cli_build_game ? 1 : 0) +
        (out.cli_build_all_games ? 1 : 0) +
        (out.cli_set_icon ? 1 : 0);
    if (action_count > 1) {
        error = "Specify only one CLI action (--list, --create, --register, --remove, --scan, --build-game, --build-all-games, or --set-icon).";
        return false;
    }
    if (out.cli_set_icon && out.icon_source_path.empty()) {
        error = "--set-icon requires --icon <image_or_ico>.";
        return false;
    }
    if (!out.icon_source_path.empty() && !out.cli_set_icon) {
        error = "--icon can only be used together with --set-icon <DIR>.";
        return false;
    }
    return true;
}

int ProjectManagerApplication::run(const int argc, char** const argv) {
    std::string cli_error;
    if (!parse_cli(argc, argv, options_, cli_error)) {
        std::cerr << "Error: " << cli_error << "\n\n";
        print_pm_usage();
        return 1;
    }
    if (options_.show_help) {
        print_pm_usage();
        return 0;
    }

    registry_file_path_ = default_project_registry_path(options_.registry_path);

    if (options_.cli_list || options_.cli_create ||
        options_.cli_register || options_.cli_remove || options_.cli_scan ||
        options_.cli_build_game || options_.cli_build_all_games ||
        options_.cli_set_icon) {
        return run_cli_action(options_);
    }

    initialize_gui(options_);
    main_loop();
    shutdown();
    return 0;
}

int ProjectManagerApplication::run_cli_action(
    const ProjectManagerCliOptions& options) {
    std::error_code ec;
    const bool registry_existed =
        std::filesystem::is_regular_file(registry_file_path_, ec) && !ec;

    std::string error;
    if (!registry_.load_or_initialize(registry_file_path_, error)) {
        std::cerr << "Registry load error: " << error << '\n';
        return 1;
    }
    if (!registry_existed && options.registry_path.empty()) {
        const std::filesystem::path default_root =
            default_projects_root_directory();
        if (std::filesystem::is_directory(default_root, ec) && !ec) {
            std::string scan_err;
            if (registry_.scan_directory(default_root, scan_err) > 0U) {
                static_cast<void>(registry_.save(registry_file_path_, scan_err));
            }
        }
    }

    if (options.cli_list) {
        Json root = Json::object();
        root["registry_path"] = path_to_utf8_str(registry_file_path_);
        Json list = Json::array();
        for (const ProjectRegistryEntry& entry : registry_.entries()) {
            const ProjectInspection insp =
                inspect_project_directory(entry.project_directory);
            Json item = Json::object();
            item["name"] =
                insp.state == ProjectDiskState::ready ? insp.manifest.name : entry.name;
            item["description"] =
                insp.state == ProjectDiskState::ready
                    ? insp.manifest.description
                    : entry.description;
            item["project_directory"] = path_to_utf8_str(entry.project_directory);
            item["template"] =
                insp.state == ProjectDiskState::ready
                    ? insp.manifest.template_id
                    : entry.template_id;
            item["startup_scene"] =
                insp.state == ProjectDiskState::ready
                    ? insp.manifest.startup_scene
                    : entry.startup_scene;
            item["scenes"] = insp.scene_relative_paths;
            item["status"] = project_disk_state_label(insp.state);
            item["ready"] = insp.state == ProjectDiskState::ready;
            item["executable"] =
                insp.state == ProjectDiskState::ready
                    ? insp.manifest.executable
                    : std::string{};
            item["runtime_bundled"] = insp.runtime_bundled;
            item["icon"] =
                insp.state == ProjectDiskState::ready
                    ? insp.manifest.icon_path
                    : std::string{};
            item["icon_exists"] = insp.icon_exists;
            item["pinned"] = entry.pinned;
            item["last_opened_utc"] = entry.last_opened_utc;
            list.push_back(std::move(item));
        }
        root["projects"] = std::move(list);
        std::cout << root.dump(2) << '\n';
        return 0;
    }

    if (options.cli_create) {
        const std::filesystem::path parent_dir =
            options.create_location.empty()
                ? default_projects_root_directory()
                : path_from_utf8_str(options.create_location);
        CreateProjectResult created{};
        if (!create_project_on_disk(
                CreateProjectOptions{
                    .name = options.create_name,
                    .description = options.create_description,
                    .parent_directory = parent_dir,
                    .folder_name = options.create_folder,
                    .template_kind = options.create_template,
                },
                created,
                error)) {
            std::cerr << "Create project failed: " << error << '\n';
            return 1;
        }
        if (!registry_.register_project_directory(
                created.project_directory, error, true) ||
            !registry_.save(registry_file_path_, error)) {
            std::cerr << "Project created, but registry update failed: "
                      << error << '\n';
            return 1;
        }
        Json out = Json::object();
        out["name"] = created.manifest.name;
        out["project_directory"] = path_to_utf8_str(created.project_directory);
        out["template"] = created.manifest.template_id;
        out["startup_scene"] = created.manifest.startup_scene;
        out["executable"] = created.manifest.executable;
        out["runtime_bundled"] = created.runtime_bundled;
        if (!created.bundle_warning.empty()) {
            out["bundle_warning"] = created.bundle_warning;
        }
        std::cout << out.dump(2) << '\n';
        return 0;
    }

    if (options.cli_register) {
        const std::filesystem::path dir =
            path_from_utf8_str(options.target_directory);
        if (options.cli_init_manifest) {
            const ProjectInspection check = inspect_project_directory(dir);
            if (check.state == ProjectDiskState::missing_manifest) {
                if (!initialize_manifest_for_existing_directory(dir, {}, error)) {
                    std::cerr << "Failed to initialize manifest: " << error << '\n';
                    return 1;
                }
            }
        }
        if (!registry_.register_project_directory(dir, error, false) ||
            !registry_.save(registry_file_path_, error)) {
            std::cerr << "Register project failed: " << error << '\n';
            return 1;
        }
        std::cout << "Registered project: " << options.target_directory << '\n';
        return 0;
    }

    if (options.cli_remove) {
        const std::filesystem::path dir =
            path_from_utf8_str(options.target_directory);
        if (!registry_.remove_by_directory(dir)) {
            std::cerr << "Project was not registered: "
                      << options.target_directory << '\n';
            return 1;
        }
        if (!registry_.save(registry_file_path_, error)) {
            std::cerr << "Failed to save registry: " << error << '\n';
            return 1;
        }
        std::cout << "Removed from registry: " << options.target_directory << '\n';
        return 0;
    }

    if (options.cli_scan) {
        const std::filesystem::path dir =
            path_from_utf8_str(options.target_directory);
        const std::size_t count = registry_.scan_directory(dir, error);
        if (!error.empty()) {
            std::cerr << "Scan failed: " << error << '\n';
            return 1;
        }
        if (!registry_.save(registry_file_path_, error)) {
            std::cerr << "Failed to save registry: " << error << '\n';
            return 1;
        }
        std::cout << "Scanned " << options.target_directory
                  << ": registered " << count << " project(s).\n";
        return 0;
    }

    if (options.cli_build_game) {
        const std::filesystem::path dir =
            path_from_utf8_str(options.target_directory);
        BundleProjectRuntimeResult res{};
        if (!bundle_project_runtime(dir, res, error)) {
            std::cerr << "Build game failed: " << error << '\n';
            return 1;
        }
        Json out = Json::object();
        out["project_directory"] = path_to_utf8_str(res.project_directory);
        out["executable_path"] = path_to_utf8_str(res.executable_path);
        out["icon_path"] = path_to_utf8_str(res.icon_path);
        out["copied_shaders"] = res.copied_shaders;
        out["copied_fsr3_dll"] = res.copied_fsr3_dll;
        out["icon_embedded"] = res.icon_embedded;
        std::cout << out.dump(2) << '\n';
        return 0;
    }

    if (options.cli_build_all_games) {
        Json built = Json::array();
        Json failed = Json::array();
        std::size_t ready_count = 0U;
        for (const ProjectRegistryEntry& entry : registry_.entries()) {
            const ProjectInspection insp =
                inspect_project_directory(entry.project_directory);
            if (insp.state != ProjectDiskState::ready) {
                continue;
            }
            ++ready_count;
            BundleProjectRuntimeResult res{};
            std::string build_err;
            if (bundle_project_runtime(entry.project_directory, res, build_err)) {
                Json row = Json::object();
                row["name"] = insp.manifest.name;
                row["executable_path"] = path_to_utf8_str(res.executable_path);
                row["icon_embedded"] = res.icon_embedded;
                row["copied_shaders"] = res.copied_shaders;
                built.push_back(std::move(row));
            } else {
                Json row = Json::object();
                row["name"] = insp.manifest.name;
                row["project_directory"] =
                    path_to_utf8_str(entry.project_directory);
                row["error"] = build_err;
                failed.push_back(std::move(row));
            }
        }
        Json out = Json::object();
        out["ready_projects"] = ready_count;
        out["built"] = built;
        out["failed"] = failed;
        std::cout << out.dump(2) << '\n';
        return (ready_count > 0U && failed.empty()) ? 0 : 1;
    }

    if (options.cli_set_icon) {
        const std::filesystem::path dir =
            path_from_utf8_str(options.target_directory);
        const std::filesystem::path src =
            path_from_utf8_str(options.icon_source_path);
        if (!set_project_icon(dir, src, error)) {
            std::cerr << "Set project icon failed: " << error << '\n';
            return 1;
        }
        const ProjectInspection insp = inspect_project_directory(dir);
        Json out = Json::object();
        out["project_directory"] = path_to_utf8_str(insp.project_directory);
        out["icon_path"] = path_to_utf8_str(insp.icon_file_path);
        out["runtime_bundled"] = insp.runtime_bundled;
        out["executable_path"] = path_to_utf8_str(insp.executable_path);
        std::cout << out.dump(2) << '\n';
        return 0;
    }

    return 0;
}

void ProjectManagerApplication::initialize_gui(
    const ProjectManagerCliOptions& options) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fatal(SDL_GetError());
    }
    sdl_initialized_ = true;

    window_.reset(SDL_CreateWindow(
        "Vulkan Cube Project Manager",
        static_cast<int>(options.width),
        static_cast<int>(options.height),
        SDL_WINDOW_VULKAN |
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!window_) {
        fatal(SDL_GetError());
    }
    SDL_SetWindowMinimumSize(window_.get(), 920, 560);
    apply_window_builtin_icon(
        window_.get(), BuiltinIconKind::project_manager);

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

    copy_to_cstr_buffer(
        create_location_buffer_,
        path_to_utf8_str(default_projects_root_directory()));
    copy_to_cstr_buffer(create_name_buffer_, "My Starter Project");
    copy_to_cstr_buffer(
        create_folder_buffer_,
        make_project_folder_slug("My Starter Project"));

    reload_registry_and_cache(true);
    running_ = true;
}

void ProjectManagerApplication::initialize_frame_resources() {
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

void ProjectManagerApplication::rebuild_present_semaphores() {
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

void ProjectManagerApplication::initialize_imgui_backend() {
    imgui_color_format_ = swapchain_.color_format();
    imgui_rendering_info_.pColorAttachmentFormats = &imgui_color_format_;

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = context_.instance();
    init_info.PhysicalDevice = context_.physical_device();
    init_info.Device = context_.device();
    init_info.QueueFamily = context_.queue_families().graphics;
    init_info.Queue = context_.graphics_queue();
    init_info.DescriptorPoolSize = 512U;
    init_info.MinImageCount = 2U;
    init_info.ImageCount =
        static_cast<std::uint32_t>(swapchain_.images().size());
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo =
        imgui_rendering_info_;
    init_info.UseDynamicRendering = true;
    init_info.CheckVkResultFn = check_pm_imgui_vk_result;
    init_info.MinAllocationSize = 1024U * 1024U;
    if (!ImGui_ImplVulkan_Init(&init_info)) {
        fatal("Unable to initialize Dear ImGui Vulkan backend.");
    }
    imgui_vulkan_initialized_ = true;
}

void ProjectManagerApplication::shutdown_imgui_backend() noexcept {
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplVulkan_Shutdown();
    }
    imgui_vulkan_initialized_ = false;
}

void ProjectManagerApplication::handle_swapchain_resize() {
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
    resize_requested_ = false;
    rebuild_present_semaphores();

    if (swapchain_.color_format() != prev_format ||
        swapchain_.images().size() != prev_count) {
        for (UiTexturePreview& old : retired_icon_previews_) {
            old.destroy(imgui_vulkan_initialized_);
        }
        retired_icon_previews_.clear();
        for (CachedProjectItem& item : items_) {
            item.icon_preview.destroy(imgui_vulkan_initialized_);
        }
        shutdown_imgui_backend();
        initialize_imgui_backend();
        icon_previews_dirty_ = true;
    }
}

void ProjectManagerApplication::main_loop() {
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
        }
        draw_frame();
    }
}

void ProjectManagerApplication::process_event(const SDL_Event& event) {
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
                const ProjectInspection insp =
                    inspect_project_directory(dropped);
                if (insp.state == ProjectDiskState::ready) {
                    std::string err;
                    if (registry_.register_project_directory(
                            dropped, err, false)) {
                        save_registry();
                        refresh_cached_inspections();
                        for (std::size_t i = 0U; i < items_.size(); ++i) {
                            std::error_code eq_ec;
                            if (items_[i].entry.project_directory == dropped ||
                                std::filesystem::equivalent(
                                    items_[i].entry.project_directory,
                                    dropped,
                                    eq_ec)) {
                                selected_index_ = i;
                                sync_inspector_from_selection();
                                break;
                            }
                        }
                        status_message_ =
                            "Registered and selected dropped project.";
                        status_tone_ = EditorTone::success;
                    } else {
                        status_message_ = "Drop register failed: " + err;
                        status_tone_ = EditorTone::danger;
                    }
                } else {
                    status_message_ =
                        "Dropped directory is not a ready project.";
                    status_tone_ = EditorTone::warning;
                }
            } else if (is_supported_pm_icon_extension(dropped)) {
                apply_icon_to_selected_project(dropped);
            } else {
                status_message_ =
                    "Drop a .png/.ico/.jpg/.bmp/.tga icon onto a selected project, or a project folder.";
                status_tone_ = EditorTone::warning;
            }
        }
        break;
    default:
        break;
    }
}

void ProjectManagerApplication::draw_frame() {
    FrameSlot& frame = frames_[frame_index_];
    const VkDevice device = context_.device();

    const VkFence fence = frame.in_flight.get();
    VK_CHECK(vkWaitForFences(device, 1U, &fence, VK_TRUE, UINT64_MAX));

    sync_gpu_icon_previews();

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
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
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

void ProjectManagerApplication::shutdown() noexcept {
    running_ = false;
    if (context_.device() != VK_NULL_HANDLE) {
        const VkResult idle = vkDeviceWaitIdle(context_.device());
        if (idle != VK_SUCCESS) {
            std::cerr << "Project Manager vkDeviceWaitIdle failed: "
                      << static_cast<int>(idle) << '\n';
        }
    }
    for (UiTexturePreview& old : retired_icon_previews_) {
        old.destroy(imgui_vulkan_initialized_);
    }
    retired_icon_previews_.clear();
    for (CachedProjectItem& item : items_) {
        item.icon_preview.destroy(imgui_vulkan_initialized_);
    }
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

void ProjectManagerApplication::reload_registry_and_cache(
    const bool auto_scan_first_time) {
    std::error_code ec;
    const bool existed =
        std::filesystem::is_regular_file(registry_file_path_, ec) && !ec;

    std::string error;
    if (!registry_.load_or_initialize(registry_file_path_, error)) {
        status_message_ = "Registry load failed: " + error;
        status_tone_ = EditorTone::danger;
    } else if (auto_scan_first_time && !existed) {
        const std::filesystem::path default_dir =
            default_projects_root_directory();
        if (std::filesystem::is_directory(default_dir, ec) && !ec) {
            std::string scan_err;
            const std::size_t found =
                registry_.scan_directory(default_dir, scan_err);
            if (found > 0U) {
                save_registry();
                status_message_ =
                    "Discovered " + std::to_string(found) +
                    " project(s) in projects/.";
                status_tone_ = EditorTone::success;
            }
        }
    }
    refresh_cached_inspections();
}

void ProjectManagerApplication::refresh_cached_inspections() {
    std::optional<std::filesystem::path> prev_selected_dir;
    if (selected_index_ && *selected_index_ < items_.size()) {
        prev_selected_dir = items_[*selected_index_].entry.project_directory;
    }

    for (CachedProjectItem& old_item : items_) {
        if (old_item.icon_preview.valid()) {
            retired_icon_previews_.push_back(std::move(old_item.icon_preview));
        }
    }
    items_.clear();
    const auto entries = registry_.entries();
    items_.reserve(entries.size());
    for (const ProjectRegistryEntry& entry : entries) {
        CachedProjectItem item{};
        item.entry = entry;
        item.inspection = inspect_project_directory(entry.project_directory);
        if (item.inspection.state == ProjectDiskState::ready) {
            item.entry.name = item.inspection.manifest.name;
            item.entry.description = item.inspection.manifest.description;
            item.entry.template_id = item.inspection.manifest.template_id;
            item.entry.startup_scene = item.inspection.manifest.startup_scene;
        }
        item.directory_utf8 = path_to_utf8_str(entry.project_directory);
        item.search_haystack_lower = ascii_lowercase(
            item.entry.name + " " +
            item.entry.description + " " +
            item.entry.template_id + " " +
            item.directory_utf8);
        items_.push_back(std::move(item));
    }

    std::sort(
        items_.begin(),
        items_.end(),
        [](const CachedProjectItem& lhs, const CachedProjectItem& rhs) {
            if (lhs.entry.pinned != rhs.entry.pinned) {
                return lhs.entry.pinned > rhs.entry.pinned;
            }
            if (lhs.entry.last_opened_utc != rhs.entry.last_opened_utc) {
                return lhs.entry.last_opened_utc > rhs.entry.last_opened_utc;
            }
            return lhs.entry.name < rhs.entry.name;
        });

    selected_index_.reset();
    if (prev_selected_dir) {
        for (std::size_t i = 0U; i < items_.size(); ++i) {
            if (items_[i].entry.project_directory == *prev_selected_dir) {
                selected_index_ = i;
                break;
            }
        }
    }
    if (!selected_index_ && !items_.empty()) {
        selected_index_ = 0U;
    }
    if (context_.device() != VK_NULL_HANDLE &&
        upload_command_pool_.get() != VK_NULL_HANDLE) {
        icon_previews_dirty_ = true;
    }
    sync_inspector_from_selection();
}

void ProjectManagerApplication::sync_gpu_icon_previews() {
    if (retired_icon_previews_.empty() && !icon_previews_dirty_) {
        return;
    }
    if (context_.device() == VK_NULL_HANDLE ||
        upload_command_pool_.get() == VK_NULL_HANDLE) {
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(context_.device()));
    for (UiTexturePreview& old : retired_icon_previews_) {
        old.destroy(imgui_vulkan_initialized_);
    }
    retired_icon_previews_.clear();

    if (icon_previews_dirty_) {
        const IconRgbaImage fallback_icon =
            generate_builtin_icon_rgba(BuiltinIconKind::project_default, 64U);
        for (CachedProjectItem& item : items_) {
            if (item.icon_preview.valid()) {
                continue;
            }
            IconRgbaImage preview_rgba{};
            std::string load_err;
            if (item.inspection.icon_exists &&
                load_icon_source_image(
                    item.inspection.icon_file_path, preview_rgba, load_err)) {
                if (preview_rgba.width > 128U ||
                    preview_rgba.height > 128U ||
                    preview_rgba.width != preview_rgba.height) {
                    preview_rgba =
                        resize_rgba_square_icon(preview_rgba, 64U);
                }
            } else {
                preview_rgba = fallback_icon;
            }
            std::string upload_err;
            static_cast<void>(item.icon_preview.upload(
                context_,
                upload_command_pool_.get(),
                preview_rgba,
                "ProjectManagerCardIcon",
                upload_err,
                imgui_vulkan_initialized_));
        }
        icon_previews_dirty_ = false;
    }
}

void ProjectManagerApplication::save_registry() {
    std::string error;
    if (!registry_.save(registry_file_path_, error)) {
        status_message_ = "Registry save error: " + error;
        status_tone_ = EditorTone::danger;
    }
}

void ProjectManagerApplication::sync_inspector_from_selection() {
    if (!selected_index_ || *selected_index_ >= items_.size()) {
        edit_name_buffer_.fill('\0');
        edit_description_buffer_.fill('\0');
        edit_startup_scene_.clear();
        return;
    }
    const CachedProjectItem& selected = items_[*selected_index_];
    copy_to_cstr_buffer(edit_name_buffer_, selected.entry.name);
    copy_to_cstr_buffer(edit_description_buffer_, selected.entry.description);
    edit_startup_scene_ = selected.entry.startup_scene;
}

void ProjectManagerApplication::build_ui() {
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
    const bool host_open =
        ImGui::Begin("##ProjectManagerRoot", nullptr, host_flags);
    ImGui::PopStyleVar(2);

    if (host_open) {
        build_header_bar();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float available_width = ImGui::GetContentRegionAvail().x;
        const float available_height = ImGui::GetContentRegionAvail().y - 26.0F;
        const float inspector_width =
            (std::clamp)(available_width * 0.34F, 340.0F, 480.0F);
        const float list_width =
            (std::max)(280.0F, available_width - inspector_width - 12.0F);

        if (ImGui::BeginChild(
                "##ProjectListPane",
                ImVec2(list_width, (std::max)(120.0F, available_height)),
                ImGuiChildFlags_Borders)) {
            build_project_list_panel();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0F, 12.0F);

        if (ImGui::BeginChild(
                "##ProjectInspectorPane",
                ImVec2(inspector_width, (std::max)(120.0F, available_height)),
                ImGuiChildFlags_Borders)) {
            build_inspector_panel();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (!status_message_.empty()) {
            ui_kit_.status_text(status_message_.c_str(), status_tone_);
        } else {
            ImGui::TextDisabled(
                "Registry: %s",
                registry_file_path_.empty()
                    ? "(unset)"
                    : registry_file_path_.string().c_str());
        }
    }
    ImGui::End();

    build_create_project_modal();
    build_add_existing_modal();
    build_remove_confirm_modal();
    build_folder_picker_modal();
    build_icon_file_picker_modal();
}

void ProjectManagerApplication::build_header_bar() {
    ui_kit_.brand_lockup();
    ImGui::SameLine(0.0F, 10.0F);
    ui_kit_.badge("PROJECT MANAGER", EditorTone::primary);

    char count_label[48U]{};
    std::snprintf(
        count_label,
        sizeof(count_label),
        "%zu PROJECTS",
        items_.size());
    ImGui::SameLine(0.0F, 8.0F);
    ui_kit_.badge(count_label, EditorTone::neutral);

    ImGui::SameLine(0.0F, 18.0F);
    ImGui::SetNextItemWidth(260.0F);
    if (ImGui::InputTextWithHint(
            "##SearchProjects",
            "Search projects...",
            search_buffer_.data(),
            search_buffer_.size())) {
        search_lower_ = ascii_lowercase(search_buffer_.data());
    }

    ImGui::SameLine(0.0F, 14.0F);
    if (ui_kit_.action_button(
            "NewProjectBtn",
            EditorIcon::add,
            "New Project",
            EditorButtonStyle::primary)) {
        create_modal_error_.clear();
        open_create_modal_requested_ = true;
    }

    ImGui::SameLine(0.0F, 8.0F);
    if (ui_kit_.action_button(
            "AddExistingBtn",
            EditorIcon::folder,
            "Add Existing...")) {
        existing_modal_error_.clear();
        existing_init_manifest_ = false;
        existing_preview_ = ProjectInspection{};
        open_add_existing_requested_ = true;
    }

    ImGui::SameLine(0.0F, 8.0F);
    if (ui_kit_.action_button(
            "ScanFolderBtn",
            EditorIcon::search,
            "Scan Folder...")) {
        open_folder_picker(
            FolderPickerTarget::scan_root,
            default_projects_root_directory());
    }

    ImGui::SameLine(0.0F, 8.0F);
    if (ui_kit_.icon_button(
            "RefreshProjectsBtn",
            EditorIcon::refresh,
            "Reload project registry and inspect disk state",
            EditorButtonStyle::secondary)) {
        reload_registry_and_cache(false);
        status_message_ = "Refreshed project list.";
        status_tone_ = EditorTone::info;
    }
}

void ProjectManagerApplication::build_project_list_panel() {
    ui_kit_.panel_header(
        EditorIcon::folder,
        "Registered Projects",
        "Double-click any ready project to open it in Vulkan Cube Editor");

    if (ui_kit_.action_button(
            "TabAll",
            EditorIcon::folder,
            "All",
            EditorButtonStyle::quiet,
            true,
            filter_tab_ == FilterTab::all)) {
        filter_tab_ = FilterTab::all;
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ui_kit_.action_button(
            "TabPinned",
            EditorIcon::eye,
            "Pinned",
            EditorButtonStyle::quiet,
            true,
            filter_tab_ == FilterTab::pinned)) {
        filter_tab_ = FilterTab::pinned;
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ui_kit_.action_button(
            "TabReady",
            EditorIcon::play,
            "Ready",
            EditorButtonStyle::quiet,
            true,
            filter_tab_ == FilterTab::ready)) {
        filter_tab_ = FilterTab::ready;
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ui_kit_.action_button(
            "TabMissing",
            EditorIcon::revert,
            "Missing",
            EditorButtonStyle::quiet,
            true,
            filter_tab_ == FilterTab::missing)) {
        filter_tab_ = FilterTab::missing;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (items_.empty()) {
        ui_kit_.empty_state(
            EditorIcon::cube,
            "No projects registered yet",
            "Click 'New Project' to create a starter project with ground plane "
            "and cubes, or 'Scan Folder...' to discover existing projects.");
        return;
    }

    if (!ImGui::BeginChild("##ProjectCardsScroll")) {
        ImGui::EndChild();
        return;
    }

    std::size_t visible_count = 0U;
    std::optional<std::size_t> deferred_editor_launch;
    std::optional<std::size_t> deferred_runtime_launch;
    std::optional<std::pair<std::size_t, bool>> deferred_pin_toggle;

    for (std::size_t i = 0U; i < items_.size(); ++i) {
        const CachedProjectItem& item = items_[i];
        const bool is_ready = item.inspection.state == ProjectDiskState::ready;

        if (filter_tab_ == FilterTab::pinned && !item.entry.pinned) {
            continue;
        }
        if (filter_tab_ == FilterTab::ready && !is_ready) {
            continue;
        }
        if (filter_tab_ == FilterTab::missing && is_ready) {
            continue;
        }
        if (!search_lower_.empty() &&
            item.search_haystack_lower.find(search_lower_) == std::string::npos) {
            continue;
        }
        ++visible_count;

        ImGui::PushID(static_cast<int>(i));
        const bool is_selected = selected_index_ && *selected_index_ == i;

        const ImVec4 card_bg = is_selected
            ? ImVec4(0.14F, 0.19F, 0.27F, 0.95F)
            : ImVec4(0.085F, 0.098F, 0.12F, 0.92F);
        const ImVec4 border_col = is_selected
            ? EditorUiKit::color(EditorTone::primary)
            : EditorUiKit::border_color();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, card_bg);
        ImGui::PushStyleColor(ImGuiCol_Border, border_col);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0F);

        if (ImGui::BeginChild(
                "##ProjectCard",
                ImVec2(0.0F, 98.0F),
                ImGuiChildFlags_Borders)) {
            const ImVec2 card_min = ImGui::GetWindowPos();
            const ImVec2 card_size = ImGui::GetWindowSize();
            const ImVec2 card_max(
                card_min.x + card_size.x,
                card_min.y + card_size.y);

            if (item.icon_preview.valid()) {
                ImGui::Image(
                    item.icon_preview.imgui_id(),
                    ImVec2(36.0F, 36.0F),
                    ImVec2(0.0F, 0.0F),
                    ImVec2(1.0F, 1.0F));
            } else {
                ImGui::Dummy(ImVec2(36.0F, 36.0F));
            }
            ImGui::SameLine(0.0F, 10.0F);
            ImGui::Indent(46.0F);

            ImGui::TextUnformatted(item.entry.name.c_str());
            ImGui::SameLine(0.0F, 8.0F);
            ui_kit_.badge(
                project_disk_state_label(item.inspection.state),
                is_ready ? EditorTone::success : EditorTone::danger);

            ImGui::SameLine(0.0F, 6.0F);
            ui_kit_.badge(item.entry.template_id.c_str(), EditorTone::info);

            if (is_ready) {
                ImGui::SameLine(0.0F, 6.0F);
                ui_kit_.badge(
                    item.inspection.runtime_bundled ? "GAME .EXE" : "NO .EXE",
                    item.inspection.runtime_bundled
                        ? EditorTone::success
                        : EditorTone::warning);
            }

            if (item.entry.pinned) {
                ImGui::SameLine(0.0F, 6.0F);
                ui_kit_.badge("PINNED", EditorTone::warning);
            }

            if (is_ready) {
                char scenes_badge[48U]{};
                std::snprintf(
                    scenes_badge,
                    sizeof(scenes_badge),
                    "%zu SCENE%s",
                    item.inspection.scene_relative_paths.size(),
                    item.inspection.scene_relative_paths.size() == 1U ? "" : "S");
                ImGui::SameLine(0.0F, 6.0F);
                ui_kit_.badge(scenes_badge, EditorTone::neutral);
            }

            const float actions_width = 250.0F;
            const float right_x =
                (std::max)(220.0F, ImGui::GetWindowWidth() - actions_width);
            ImGui::SameLine(right_x);
            if (ui_kit_.action_button(
                    "OpenEditor",
                    EditorIcon::renderer,
                    "Editor",
                    EditorButtonStyle::primary,
                    is_ready)) {
                selected_index_ = i;
                sync_inspector_from_selection();
                deferred_editor_launch = i;
            }
            ImGui::SameLine(0.0F, 6.0F);
            if (ui_kit_.action_button(
                    "RunGame",
                    EditorIcon::play,
                    "Play",
                    EditorButtonStyle::secondary,
                    is_ready)) {
                selected_index_ = i;
                sync_inspector_from_selection();
                deferred_runtime_launch = i;
            }
            ImGui::SameLine(0.0F, 6.0F);
            if (ui_kit_.icon_button(
                    "PinBtn",
                    EditorIcon::eye,
                    item.entry.pinned ? "Unpin project" : "Pin project to top",
                    EditorButtonStyle::quiet,
                    true,
                    item.entry.pinned)) {
                deferred_pin_toggle = std::pair{i, !item.entry.pinned};
            }
            ImGui::SameLine(0.0F, 4.0F);
            if (ui_kit_.icon_button(
                    "ExplorerBtn",
                    EditorIcon::folder,
                    "Open folder in Windows Explorer",
                    EditorButtonStyle::quiet,
                    item.inspection.state !=
                        ProjectDiskState::missing_directory)) {
                std::string explorer_err;
                if (!open_directory_in_file_explorer(
                        item.entry.project_directory, explorer_err)) {
                    status_message_ = explorer_err;
                    status_tone_ = EditorTone::danger;
                }
            }

            if (!item.entry.description.empty()) {
                ImGui::TextDisabled("%s", item.entry.description.c_str());
            } else if (!is_ready && !item.inspection.diagnostic.empty()) {
                ui_kit_.status_text(
                    item.inspection.diagnostic.c_str(),
                    EditorTone::warning);
            } else {
                ImGui::TextDisabled(
                    "Startup scene: assets/%s",
                    item.entry.startup_scene.c_str());
            }

            ImGui::TextDisabled("%s", item.directory_utf8.c_str());
            ImGui::Unindent(46.0F);

            const bool hovered_card =
                ImGui::IsMouseHoveringRect(card_min, card_max) &&
                !ImGui::IsAnyItemHovered();
            if (hovered_card && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selected_index_ = i;
                sync_inspector_from_selection();
            }
            if (hovered_card &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                is_ready) {
                selected_index_ = i;
                sync_inspector_from_selection();
                deferred_editor_launch = i;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
        ImGui::PopID();
    }

    if (visible_count == 0U) {
        ui_kit_.empty_state(
            EditorIcon::search,
            "No matching projects",
            "Clear the search filter or switch filter tabs.");
    }

    ImGui::EndChild();

    if (deferred_pin_toggle) {
        const auto [idx, new_pin] = *deferred_pin_toggle;
        if (idx < items_.size()) {
            const std::filesystem::path dir =
                items_[idx].entry.project_directory;
            if (registry_.set_pinned(dir, new_pin)) {
                save_registry();
                refresh_cached_inspections();
            }
        }
    }
    if (deferred_editor_launch) {
        launch_project_in_editor(*deferred_editor_launch);
    }
    if (deferred_runtime_launch) {
        launch_project_in_runtime(*deferred_runtime_launch);
    }
}

void ProjectManagerApplication::build_inspector_panel() {
    ui_kit_.panel_header(
        EditorIcon::renderer,
        "Project Details",
        "Inspect manifest settings, scenes, and launch options");

    if (!selected_index_ || *selected_index_ >= items_.size()) {
        ui_kit_.empty_state(
            EditorIcon::folder,
            "Select a project",
            "Choose a project from the list on the left to inspect or configure it.");
        return;
    }

    const std::size_t idx = *selected_index_;
    const CachedProjectItem& selected = items_[idx];
    const bool is_ready = selected.inspection.state == ProjectDiskState::ready;

    ui_kit_.section_header(EditorIcon::brand, "Overview");
    if (ui_kit_.begin_property_grid("##OverviewGrid")) {
        ui_kit_.property_row("Status");
        ui_kit_.badge(
            project_disk_state_label(selected.inspection.state),
            is_ready ? EditorTone::success : EditorTone::danger);

        ui_kit_.property_row("Template");
        ImGui::TextUnformatted(selected.entry.template_id.c_str());

        ui_kit_.property_row("Location");
        ImGui::TextWrapped("%s", selected.directory_utf8.c_str());

        if (!selected.entry.last_opened_utc.empty()) {
            ui_kit_.property_row("Last Opened");
            ImGui::TextUnformatted(selected.entry.last_opened_utc.c_str());
        }
        ui_kit_.end_property_grid();
    }

    if (!is_ready && !selected.inspection.diagnostic.empty()) {
        ImGui::Spacing();
        ui_kit_.status_text(
            selected.inspection.diagnostic.c_str(),
            EditorTone::danger);
        if (selected.inspection.state == ProjectDiskState::missing_manifest) {
            ImGui::Spacing();
            if (ui_kit_.action_button(
                    "InitMissingManifest",
                    EditorIcon::save,
                    "Initialize project.vcrproject.json",
                    EditorButtonStyle::primary)) {
                std::string err;
                if (initialize_manifest_for_existing_directory(
                        selected.entry.project_directory,
                        selected.entry.name,
                        err) &&
                    registry_.register_project_directory(
                        selected.entry.project_directory, err, false)) {
                    save_registry();
                    refresh_cached_inspections();
                    status_message_ = "Initialized project manifest.";
                    status_tone_ = EditorTone::success;
                    return;
                }
                status_message_ = "Manifest init failed: " + err;
                status_tone_ = EditorTone::danger;
            }
        }
    }

    if (is_ready) {
        ImGui::Spacing();
        ui_kit_.section_header(EditorIcon::save, "Manifest Settings");
        if (ui_kit_.begin_property_grid("##ManifestEditGrid")) {
            ui_kit_.property_row("Name");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputText(
                "##EditProjName",
                edit_name_buffer_.data(),
                edit_name_buffer_.size());

            ui_kit_.property_row("Description");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputTextMultiline(
                "##EditProjDesc",
                edit_description_buffer_.data(),
                edit_description_buffer_.size(),
                ImVec2(0.0F, 56.0F));

            ui_kit_.property_row("Startup Scene");
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::BeginCombo(
                    "##StartupSceneCombo",
                    edit_startup_scene_.c_str())) {
                for (const std::string& scene_rel :
                     selected.inspection.scene_relative_paths) {
                    const bool chosen = scene_rel == edit_startup_scene_;
                    if (ImGui::Selectable(scene_rel.c_str(), chosen)) {
                        edit_startup_scene_ = scene_rel;
                    }
                    if (chosen) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ui_kit_.end_property_grid();
        }

        if (ui_kit_.action_button(
                "SaveManifestBtn",
                EditorIcon::save,
                "Save Project Settings",
                EditorButtonStyle::secondary)) {
            ProjectManifest updated = selected.inspection.manifest;
            updated.name = edit_name_buffer_.data();
            updated.description = edit_description_buffer_.data();
            if (!edit_startup_scene_.empty()) {
                updated.startup_scene = edit_startup_scene_;
            }
            std::string err;
            const std::filesystem::path manifest_file =
                selected.entry.project_directory / project_manifest_filename;
            if (save_project_manifest(updated, manifest_file, err) &&
                registry_.register_project_directory(
                    selected.entry.project_directory, err, false)) {
                save_registry();
                refresh_cached_inspections();
                status_message_ = "Saved project.vcrproject.json.";
                status_tone_ = EditorTone::success;
                return;
            }
            status_message_ = "Failed to save manifest: " + err;
            status_tone_ = EditorTone::danger;
        }

        ImGui::Spacing();
        ui_kit_.section_header(EditorIcon::ground, "Project Scenes");
        if (selected.inspection.scene_relative_paths.empty()) {
            ImGui::TextDisabled("No JSON scenes in assets/scenes/");
        } else {
            for (std::size_t s = 0U;
                 s < selected.inspection.scene_relative_paths.size();
                 ++s) {
                const std::string& rel =
                    selected.inspection.scene_relative_paths[s];
                ImGui::PushID(static_cast<int>(s));
                const bool is_startup =
                    rel == selected.inspection.manifest.startup_scene;
                ImGui::BulletText("%s", rel.c_str());
                if (is_startup) {
                    ImGui::SameLine(0.0F, 6.0F);
                    ui_kit_.badge("STARTUP", EditorTone::primary);
                } else {
                    ImGui::SameLine(0.0F, 8.0F);
                    if (ImGui::SmallButton("Set Startup")) {
                        ProjectManifest updated = selected.inspection.manifest;
                        updated.startup_scene = rel;
                        std::string err;
                        if (save_project_manifest(
                                updated,
                                selected.entry.project_directory /
                                    project_manifest_filename,
                                err) &&
                            registry_.register_project_directory(
                                selected.entry.project_directory, err, false)) {
                            save_registry();
                            refresh_cached_inspections();
                            status_message_ =
                                "Startup scene set to " + rel + ".";
                            status_tone_ = EditorTone::success;
                            ImGui::PopID();
                            return;
                        }
                        status_message_ = err;
                        status_tone_ = EditorTone::danger;
                    }
                }
                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        ui_kit_.section_header(EditorIcon::cube, "Standalone Game & Icon");
        if (selected.icon_preview.valid()) {
            ImGui::Image(
                selected.icon_preview.imgui_id(),
                ImVec2(64.0F, 64.0F),
                ImVec2(0.0F, 0.0F),
                ImVec2(1.0F, 1.0F));
            ImGui::SameLine(0.0F, 12.0F);
        }
        ImGui::BeginGroup();
        ImGui::Text(
            "Executable: %s",
            selected.inspection.manifest.executable.c_str());
        ui_kit_.badge(
            selected.inspection.runtime_bundled ? "GAME .EXE READY" : "NO .EXE",
            selected.inspection.runtime_bundled
                ? EditorTone::success
                : EditorTone::warning);
        ImGui::SameLine(0.0F, 6.0F);
        ImGui::TextDisabled(
            "Icon: %s (%s)",
            selected.inspection.manifest.icon_path.c_str(),
            selected.inspection.icon_exists ? "ok" : "missing");
        ImGui::EndGroup();

        ImGui::Spacing();
        if (ui_kit_.action_button(
                "InspectorBundleGame",
                EditorIcon::cube,
                selected.inspection.runtime_bundled
                    ? "Update Game (.exe)"
                    : "Build Game (.exe)",
                EditorButtonStyle::secondary,
                true,
                false,
                ImVec2(-1.0F, 0.0F))) {
            bundle_selected_project_runtime();
            return;
        }
        ImGui::Spacing();
        if (ui_kit_.action_button(
                "InspectorChangeIcon",
                EditorIcon::save,
                "Change Icon...",
                EditorButtonStyle::secondary)) {
            open_icon_file_picker(selected.entry.project_directory);
        }
        ImGui::SameLine(0.0F, 6.0F);
        if (ui_kit_.action_button(
                "InspectorResetIcon",
                EditorIcon::revert,
                "Reset Default",
                EditorButtonStyle::quiet)) {
            reset_selected_project_icon();
            return;
        }
        ImGui::SameLine(0.0F, 6.0F);
        if (ui_kit_.action_button(
                "InspectorIconUtility",
                EditorIcon::external_window,
                "Icon Utility...",
                EditorButtonStyle::quiet)) {
            launch_icon_tool_for_selected_project();
        }
    }

    ImGui::Spacing();
    ui_kit_.section_header(EditorIcon::play, "Launch Options");
    if (ui_kit_.begin_property_grid("##LaunchOptsGrid")) {
        ui_kit_.property_row("Resolution");
        ImGui::SetNextItemWidth(-1.0F);
        static constexpr const char* resolution_items[] = {
            "Default (1280x720)",
            "HD+ (1600x900)",
            "Full HD (1920x1080)",
            "QHD (2560x1440)",
        };
        ImGui::Combo(
            "##LaunchRes",
            &launch_resolution_preset_,
            resolution_items,
            4);

        ui_kit_.property_row("Validation");
        ImGui::SetNextItemWidth(-1.0F);
        static constexpr const char* validation_items[] = {
            "Build Default",
            "Force Validation On",
            "Force Validation Off",
        };
        ImGui::Combo(
            "##LaunchVal",
            &launch_validation_mode_,
            validation_items,
            3);
        ui_kit_.end_property_grid();
    }

    ImGui::Spacing();
    if (ui_kit_.action_button(
            "InspectorOpenEditor",
            EditorIcon::renderer,
            "Open in Editor",
            EditorButtonStyle::primary,
            is_ready,
            false,
            ImVec2(-1.0F, 34.0F))) {
        launch_project_in_editor(idx);
    }
    ImGui::Spacing();
    if (ui_kit_.action_button(
            "InspectorLaunchRuntime",
            EditorIcon::play,
            "Launch Runtime",
            EditorButtonStyle::secondary,
            is_ready,
            false,
            ImVec2(-1.0F, 32.0F))) {
        launch_project_in_runtime(idx);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ui_kit_.action_button(
            "InspectorOpenFolder",
            EditorIcon::folder,
            "Open Folder in Explorer",
            EditorButtonStyle::secondary,
            selected.inspection.state != ProjectDiskState::missing_directory,
            false,
            ImVec2(-1.0F, 0.0F))) {
        std::string err;
        if (!open_directory_in_file_explorer(
                selected.entry.project_directory, err)) {
            status_message_ = err;
            status_tone_ = EditorTone::danger;
        }
    }

    ImGui::Spacing();
    if (ui_kit_.action_button(
            "InspectorRemoveBtn",
            EditorIcon::revert,
            "Remove from Registry",
            EditorButtonStyle::danger,
            true,
            false,
            ImVec2(-1.0F, 0.0F))) {
        pending_remove_index_ = idx;
        open_remove_confirm_requested_ = true;
    }
}

void ProjectManagerApplication::build_create_project_modal() {
    if (open_create_modal_requested_) {
        ImGui::OpenPopup("Create New Project");
        open_create_modal_requested_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(620.0F, 0.0F), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(
            "Create New Project",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ui_kit_.section_header(EditorIcon::cube, "Project Template");
    const std::array<ProjectTemplateKind, 2U> templates{
        ProjectTemplateKind::standard,
        ProjectTemplateKind::empty,
    };
    for (std::size_t t = 0U; t < templates.size(); ++t) {
        const ProjectTemplateKind kind = templates[t];
        const bool chosen = create_template_ == kind;
        ImGui::PushID(static_cast<int>(t));
        const ImVec4 bg = chosen
            ? ImVec4(0.15F, 0.22F, 0.31F, 1.0F)
            : ImVec4(0.08F, 0.09F, 0.11F, 1.0F);
        const ImVec4 border = chosen
            ? EditorUiKit::color(EditorTone::primary)
            : EditorUiKit::border_color();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
        ImGui::PushStyleColor(ImGuiCol_Border, border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0F);
        if (ImGui::BeginChild(
                "##TplCard",
                ImVec2(580.0F, 64.0F),
                ImGuiChildFlags_Borders)) {
            const ImVec2 rect_min = ImGui::GetWindowPos();
            const ImVec2 rect_sz = ImGui::GetWindowSize();
            if (ImGui::RadioButton(
                    project_template_display_name(kind), chosen)) {
                create_template_ = kind;
            }
            ImGui::SameLine(0.0F, 8.0F);
            ui_kit_.badge(
                std::string(to_token(kind)).c_str(),
                chosen ? EditorTone::primary : EditorTone::neutral);
            ImGui::TextWrapped("%s", project_template_description(kind));
            if (ImGui::IsMouseHoveringRect(
                    rect_min,
                    ImVec2(rect_min.x + rect_sz.x, rect_min.y + rect_sz.y)) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                create_template_ = kind;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    ImGui::Spacing();
    ui_kit_.section_header(EditorIcon::folder, "Identity & Location");
    if (ui_kit_.begin_property_grid("##CreateProjGrid")) {
        ui_kit_.property_row("Project Name");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputText(
                "##CreateName",
                create_name_buffer_.data(),
                create_name_buffer_.size())) {
            if (!create_folder_manually_edited_) {
                copy_to_cstr_buffer(
                    create_folder_buffer_,
                    make_project_folder_slug(create_name_buffer_.data()));
            }
        }

        ui_kit_.property_row("Folder Name");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputText(
                "##CreateFolder",
                create_folder_buffer_.data(),
                create_folder_buffer_.size())) {
            create_folder_manually_edited_ =
                create_folder_buffer_[0] != '\0';
        }

        ui_kit_.property_row("Parent Folder");
        ImGui::SetNextItemWidth(-96.0F);
        ImGui::InputText(
            "##CreateLocation",
            create_location_buffer_.data(),
            create_location_buffer_.size());
        ImGui::SameLine(0.0F, 6.0F);
        if (ImGui::Button("Browse...", ImVec2(88.0F, 0.0F))) {
            open_folder_picker(
                FolderPickerTarget::create_parent,
                path_from_utf8_str(create_location_buffer_.data()));
        }

        ui_kit_.property_row("Description");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText(
            "##CreateDesc",
            create_description_buffer_.data(),
            create_description_buffer_.size());
        ui_kit_.end_property_grid();
    }

    const std::filesystem::path preview_target =
        path_from_utf8_str(create_location_buffer_.data()) /
        path_from_utf8_str(create_folder_buffer_.data());
    ImGui::TextDisabled(
        "Target path: %s",
        path_to_utf8_str(preview_target).c_str());

    ImGui::Checkbox(
        "Bundle standalone game executable (.exe)",
        &create_bundle_runtime_);

    ImGui::Checkbox(
        "Open in Editor immediately after creation",
        &create_open_in_editor_);

    if (!create_modal_error_.empty()) {
        ImGui::Spacing();
        ui_kit_.status_text(create_modal_error_.c_str(), EditorTone::danger);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ui_kit_.action_button(
            "ConfirmCreateBtn",
            EditorIcon::add,
            "Create Project",
            EditorButtonStyle::primary)) {
        CreateProjectResult created{};
        std::string err;
        if (!create_project_on_disk(
                CreateProjectOptions{
                    .name = create_name_buffer_.data(),
                    .description = create_description_buffer_.data(),
                    .parent_directory =
                        path_from_utf8_str(create_location_buffer_.data()),
                    .folder_name = create_folder_buffer_.data(),
                    .template_kind = create_template_,
                    .bundle_runtime = create_bundle_runtime_,
                },
                created,
                err)) {
            create_modal_error_ = err;
        } else if (!registry_.register_project_directory(
                       created.project_directory, err, true)) {
            create_modal_error_ = err;
        } else {
            save_registry();
            refresh_cached_inspections();
            for (std::size_t i = 0U; i < items_.size(); ++i) {
                if (items_[i].entry.project_directory ==
                    created.project_directory) {
                    selected_index_ = i;
                    sync_inspector_from_selection();
                    if (create_open_in_editor_) {
                        launch_project_in_editor(i);
                    }
                    break;
                }
            }
            if (!created.bundle_warning.empty()) {
                status_message_ =
                    "Created '" + created.manifest.name +
                    "' (bundle warning: " + created.bundle_warning + ")";
                status_tone_ = EditorTone::warning;
            } else {
                status_message_ =
                    "Created project '" + created.manifest.name + "'.";
                status_tone_ = EditorTone::success;
            }
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine(0.0F, 8.0F);
    if (ImGui::Button("Cancel", ImVec2(100.0F, 0.0F))) {
        ImGui::CloseCurrentPopup();
    }

    build_folder_picker_modal();
    ImGui::EndPopup();
}

void ProjectManagerApplication::build_add_existing_modal() {
    if (open_add_existing_requested_) {
        ImGui::OpenPopup("Add Existing Project");
        open_add_existing_requested_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(580.0F, 0.0F), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(
            "Add Existing Project",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped(
        "Select a directory containing project.vcrproject.json (or an "
        "existing folder with an assets/ directory to initialize a manifest).");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(450.0F);
    if (ImGui::InputTextWithHint(
            "##ExistingDirInput",
            "Path to project folder...",
            existing_dir_buffer_.data(),
            existing_dir_buffer_.size())) {
        existing_preview_ = inspect_project_directory(
            path_from_utf8_str(existing_dir_buffer_.data()));
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::Button("Browse...", ImVec2(92.0F, 0.0F))) {
        const std::filesystem::path start =
            existing_dir_buffer_[0] != '\0'
                ? path_from_utf8_str(existing_dir_buffer_.data())
                : default_projects_root_directory();
        open_folder_picker(FolderPickerTarget::add_existing, start);
    }

    if (existing_dir_buffer_[0] != '\0') {
        ImGui::Spacing();
        ui_kit_.badge(
            project_disk_state_label(existing_preview_.state),
            existing_preview_.state == ProjectDiskState::ready
                ? EditorTone::success
                : EditorTone::warning);
        if (existing_preview_.state == ProjectDiskState::ready) {
            ImGui::SameLine(0.0F, 8.0F);
            ImGui::Text(
                "%s (%zu scene(s))",
                existing_preview_.manifest.name.c_str(),
                existing_preview_.scene_relative_paths.size());
        } else if (
            existing_preview_.state == ProjectDiskState::missing_manifest) {
            ImGui::Spacing();
            ImGui::Checkbox(
                "Initialize project.vcrproject.json in this folder",
                &existing_init_manifest_);
            if (existing_init_manifest_) {
                ImGui::SetNextItemWidth(-1.0F);
                ImGui::InputTextWithHint(
                    "##ExistingInitName",
                    "Display name (optional)...",
                    existing_name_buffer_.data(),
                    existing_name_buffer_.size());
            }
        } else if (!existing_preview_.diagnostic.empty()) {
            ImGui::TextDisabled("%s", existing_preview_.diagnostic.c_str());
        }
    }

    if (!existing_modal_error_.empty()) {
        ImGui::Spacing();
        ui_kit_.status_text(existing_modal_error_.c_str(), EditorTone::danger);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ui_kit_.action_button(
            "ConfirmRegisterBtn",
            EditorIcon::add,
            "Register Project",
            EditorButtonStyle::primary)) {
        const std::filesystem::path candidate =
            path_from_utf8_str(existing_dir_buffer_.data());
        std::string err;
        bool ok = true;
        if (existing_init_manifest_ &&
            existing_preview_.state == ProjectDiskState::missing_manifest) {
            ok = initialize_manifest_for_existing_directory(
                candidate, existing_name_buffer_.data(), err);
        }
        if (ok) {
            ok = registry_.register_project_directory(candidate, err, true);
        }
        if (!ok) {
            existing_modal_error_ = err;
        } else {
            save_registry();
            refresh_cached_inspections();
            status_message_ = "Registered project folder.";
            status_tone_ = EditorTone::success;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine(0.0F, 8.0F);
    if (ImGui::Button("Cancel", ImVec2(100.0F, 0.0F))) {
        ImGui::CloseCurrentPopup();
    }

    build_folder_picker_modal();
    ImGui::EndPopup();
}

void ProjectManagerApplication::build_remove_confirm_modal() {
    if (open_remove_confirm_requested_) {
        ImGui::OpenPopup("Remove Project from Registry?");
        open_remove_confirm_requested_ = false;
    }

    if (!ImGui::BeginPopupModal(
            "Remove Project from Registry?",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    if (!pending_remove_index_ || *pending_remove_index_ >= items_.size()) {
        pending_remove_index_.reset();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const CachedProjectItem& target = items_[*pending_remove_index_];
    ImGui::Text("Remove \"%s\" from the Project Manager list?", target.entry.name.c_str());
    ImGui::TextDisabled("%s", target.directory_utf8.c_str());
    ImGui::Spacing();
    ui_kit_.status_text(
        "Project files and scenes on disk will NOT be deleted.",
        EditorTone::info);
    ImGui::Spacing();

    if (ui_kit_.action_button(
            "ConfirmRemoveRegistry",
            EditorIcon::revert,
            "Remove from List",
            EditorButtonStyle::danger)) {
        const std::filesystem::path dir = target.entry.project_directory;
        const std::string removed_name = target.entry.name;
        pending_remove_index_.reset();
        if (registry_.remove_by_directory(dir)) {
            save_registry();
            refresh_cached_inspections();
            status_message_ =
                "Removed '" + removed_name + "' from registry.";
            status_tone_ = EditorTone::neutral;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.0F, 8.0F);
    if (ImGui::Button("Cancel", ImVec2(100.0F, 0.0F))) {
        pending_remove_index_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProjectManagerApplication::open_folder_picker(
    const FolderPickerTarget target,
    const std::filesystem::path& initial_dir) {
    folder_picker_target_ = target;
    folder_picker_new_dir_buffer_.fill('\0');
    folder_picker_error_.clear();

    folder_picker_drives_.clear();
#if defined(_WIN32)
    const DWORD drives_mask = GetLogicalDrives();
    for (unsigned int bit = 0U; bit < 26U; ++bit) {
        if ((drives_mask & (1UL << bit)) != 0UL) {
            char root[4U]{
                static_cast<char>('A' + bit),
                ':',
                '\\',
                '\0',
            };
            folder_picker_drives_.emplace_back(root);
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
    navigate_folder_picker(start);
    open_folder_picker_requested_ = true;
}

void ProjectManagerApplication::navigate_folder_picker(
    const std::filesystem::path& directory) {
    folder_picker_error_.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) {
        folder_picker_error_ = "Directory is not accessible.";
        return;
    }
    const auto canonical_or_abs =
        std::filesystem::weakly_canonical(directory, ec);
    folder_picker_current_ =
        (!ec && !canonical_or_abs.empty()) ? canonical_or_abs : directory;
    folder_picker_current_utf8_ = path_to_utf8_str(folder_picker_current_);

    folder_picker_subdirs_.clear();
    ec.clear();
    for (const auto& entry :
         std::filesystem::directory_iterator(folder_picker_current_, ec)) {
        if (ec) {
            break;
        }
        std::error_code item_ec;
        if (entry.is_directory(item_ec) && !item_ec) {
            folder_picker_subdirs_.push_back(entry.path());
        }
    }
    std::sort(folder_picker_subdirs_.begin(), folder_picker_subdirs_.end());
}

void ProjectManagerApplication::build_folder_picker_modal() {
    if (open_folder_picker_requested_) {
        ImGui::OpenPopup("Select Directory");
        open_folder_picker_requested_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0F, 440.0F), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Select Directory", nullptr)) {
        return;
    }

    if (!folder_picker_drives_.empty()) {
        for (std::size_t d = 0U; d < folder_picker_drives_.size(); ++d) {
            if (d > 0U) {
                ImGui::SameLine(0.0F, 4.0F);
            }
            const std::string drive_label =
                path_to_utf8_str(folder_picker_drives_[d]);
            if (ImGui::SmallButton(drive_label.c_str())) {
                navigate_folder_picker(folder_picker_drives_[d]);
            }
        }
    }

    const std::filesystem::path parent = folder_picker_current_.parent_path();
    const bool can_go_up =
        !parent.empty() && parent != folder_picker_current_;
    if (!can_go_up) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Up")) {
        navigate_folder_picker(parent);
    }
    if (!can_go_up) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0.0F, 8.0F);
    ImGui::TextWrapped("%s", folder_picker_current_utf8_.c_str());

    ImGui::Separator();

    const float list_height = ImGui::GetContentRegionAvail().y - 76.0F;
    if (ImGui::BeginChild(
            "##DirEntries",
            ImVec2(0.0F, (std::max)(100.0F, list_height)),
            ImGuiChildFlags_Borders)) {
        std::optional<std::filesystem::path> next_dir;
        for (std::size_t i = 0U; i < folder_picker_subdirs_.size(); ++i) {
            const std::filesystem::path& subdir = folder_picker_subdirs_[i];
            const std::string name = path_to_utf8_str(subdir.filename());
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(name.c_str(), false)) {
                next_dir = subdir;
            }
            ImGui::PopID();
        }
        if (folder_picker_subdirs_.empty()) {
            ImGui::TextDisabled("(No subdirectories)");
        }
        if (next_dir) {
            navigate_folder_picker(*next_dir);
        }
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(240.0F);
    ImGui::InputTextWithHint(
        "##NewSubfolderName",
        "New folder name...",
        folder_picker_new_dir_buffer_.data(),
        folder_picker_new_dir_buffer_.size());
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::Button("Create Folder") &&
        folder_picker_new_dir_buffer_[0] != '\0') {
        std::error_code ec;
        const std::filesystem::path created =
            folder_picker_current_ /
            path_from_utf8_str(folder_picker_new_dir_buffer_.data());
        std::filesystem::create_directories(created, ec);
        if (ec) {
            folder_picker_error_ = "Failed to create folder.";
        } else {
            folder_picker_new_dir_buffer_.fill('\0');
            navigate_folder_picker(created);
        }
    }

    if (!folder_picker_error_.empty()) {
        ui_kit_.status_text(folder_picker_error_.c_str(), EditorTone::danger);
    }

    ImGui::Spacing();
    if (ui_kit_.action_button(
            "SelectThisFolderBtn",
            EditorIcon::folder,
            "Select Current Folder",
            EditorButtonStyle::primary)) {
        if (folder_picker_target_ == FolderPickerTarget::create_parent) {
            copy_to_cstr_buffer(
                create_location_buffer_, folder_picker_current_utf8_);
        } else if (folder_picker_target_ == FolderPickerTarget::add_existing) {
            copy_to_cstr_buffer(
                existing_dir_buffer_, folder_picker_current_utf8_);
            existing_preview_ =
                inspect_project_directory(folder_picker_current_);
        } else if (folder_picker_target_ == FolderPickerTarget::scan_root) {
            std::string scan_err;
            const std::size_t count =
                registry_.scan_directory(folder_picker_current_, scan_err);
            if (!scan_err.empty()) {
                status_message_ = "Scan failed: " + scan_err;
                status_tone_ = EditorTone::danger;
            } else {
                save_registry();
                refresh_cached_inspections();
                status_message_ =
                    "Scanned '" + folder_picker_current_utf8_ +
                    "': registered " + std::to_string(count) + " project(s).";
                status_tone_ = EditorTone::success;
            }
        }
        folder_picker_target_ = FolderPickerTarget::none;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.0F, 8.0F);
    if (ImGui::Button("Cancel", ImVec2(100.0F, 0.0F))) {
        folder_picker_target_ = FolderPickerTarget::none;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProjectManagerApplication::launch_project_in_editor(
    const std::size_t item_index) {
    if (item_index >= items_.size()) {
        return;
    }
    const CachedProjectItem& item = items_[item_index];
    if (item.inspection.state != ProjectDiskState::ready) {
        status_message_ = "Project is not ready on disk.";
        status_tone_ = EditorTone::danger;
        return;
    }
    const auto editor_exe = find_sibling_executable(executable_name_editor());
    if (!editor_exe) {
        status_message_ =
            "Could not find " + executable_name_editor() +
            " next to Project Manager.";
        status_tone_ = EditorTone::danger;
        return;
    }

    std::vector<std::string> args;
    args.emplace_back("--project");
    args.push_back(item.directory_utf8);
    if (launch_resolution_preset_ == 1) {
        args.emplace_back("--width");
        args.emplace_back("1600");
        args.emplace_back("--height");
        args.emplace_back("900");
    } else if (launch_resolution_preset_ == 2) {
        args.emplace_back("--width");
        args.emplace_back("1920");
        args.emplace_back("--height");
        args.emplace_back("1080");
    } else if (launch_resolution_preset_ == 3) {
        args.emplace_back("--width");
        args.emplace_back("2560");
        args.emplace_back("--height");
        args.emplace_back("1440");
    }
    if (launch_validation_mode_ == 1) {
        args.emplace_back("--validation");
    } else if (launch_validation_mode_ == 2) {
        args.emplace_back("--no-validation");
    }

    std::string launch_err;
    if (!launch_detached_executable(
            *editor_exe,
            args,
            item.entry.project_directory,
            launch_err)) {
        status_message_ = "Editor launch failed: " + launch_err;
        status_tone_ = EditorTone::danger;
        return;
    }
    if (registry_.touch_last_opened(item.entry.project_directory)) {
        save_registry();
        refresh_cached_inspections();
    }
    status_message_ =
        "Opened '" + item.entry.name + "' in Vulkan Cube Editor.";
    status_tone_ = EditorTone::success;
}

void ProjectManagerApplication::launch_project_in_runtime(
    const std::size_t item_index) {
    if (item_index >= items_.size()) {
        return;
    }
    const std::filesystem::path project_dir =
        items_[item_index].entry.project_directory;
    const std::string project_name = items_[item_index].entry.name;
    const std::string dir_utf8 = items_[item_index].directory_utf8;
    if (items_[item_index].inspection.state != ProjectDiskState::ready) {
        status_message_ = "Project is not ready on disk.";
        status_tone_ = EditorTone::danger;
        return;
    }

    ProjectInspection current_insp = items_[item_index].inspection;
    if (!current_insp.runtime_bundled) {
        BundleProjectRuntimeResult bundle_res{};
        std::string bundle_err;
        if (bundle_project_runtime(project_dir, bundle_res, bundle_err)) {
            refresh_cached_inspections();
            current_insp = inspect_project_directory(project_dir);
        }
    }

    std::filesystem::path launch_exe{};
    std::vector<std::string> args;
    if (current_insp.runtime_bundled) {
        launch_exe = current_insp.executable_path;
    } else {
        const auto runtime_exe =
            find_sibling_executable(executable_name_runtime());
        if (!runtime_exe) {
            status_message_ =
                "Could not find " + executable_name_runtime() +
                " next to Project Manager.";
            status_tone_ = EditorTone::danger;
            return;
        }
        launch_exe = *runtime_exe;
        args.emplace_back("--project");
        args.push_back(dir_utf8);
    }

    if (launch_resolution_preset_ == 1) {
        args.emplace_back("--width");
        args.emplace_back("1600");
        args.emplace_back("--height");
        args.emplace_back("900");
    } else if (launch_resolution_preset_ == 2) {
        args.emplace_back("--width");
        args.emplace_back("1920");
        args.emplace_back("--height");
        args.emplace_back("1080");
    } else if (launch_resolution_preset_ == 3) {
        args.emplace_back("--width");
        args.emplace_back("2560");
        args.emplace_back("--height");
        args.emplace_back("1440");
    }
    if (launch_validation_mode_ == 1) {
        args.emplace_back("--validation");
    } else if (launch_validation_mode_ == 2) {
        args.emplace_back("--no-validation");
    }

    std::string launch_err;
    if (!launch_detached_executable(
            launch_exe,
            args,
            project_dir,
            launch_err)) {
        status_message_ = "Runtime launch failed: " + launch_err;
        status_tone_ = EditorTone::danger;
        return;
    }
    if (registry_.touch_last_opened(project_dir)) {
        save_registry();
        refresh_cached_inspections();
    }
    status_message_ =
        "Launched '" + project_name + "' (" +
        path_to_utf8_str(launch_exe.filename()) + ").";
    status_tone_ = EditorTone::success;
}

void ProjectManagerApplication::apply_icon_to_selected_project(
    const std::filesystem::path& source_file) {
    if (!selected_index_ || *selected_index_ >= items_.size()) {
        status_message_ = "Select a project first to change its icon.";
        status_tone_ = EditorTone::warning;
        return;
    }
    const CachedProjectItem& selected = items_[*selected_index_];
    if (selected.inspection.state != ProjectDiskState::ready) {
        status_message_ = "Selected project is not ready on disk.";
        status_tone_ = EditorTone::danger;
        return;
    }
    std::string err;
    if (!set_project_icon(selected.entry.project_directory, source_file, err)) {
        status_message_ = "Change icon failed: " + err;
        status_tone_ = EditorTone::danger;
        return;
    }
    refresh_cached_inspections();
    status_message_ =
        "Updated icon for '" + selected.entry.name + "'.";
    status_tone_ = EditorTone::success;
}

void ProjectManagerApplication::bundle_selected_project_runtime() {
    if (!selected_index_ || *selected_index_ >= items_.size()) {
        return;
    }
    const CachedProjectItem& selected = items_[*selected_index_];
    if (selected.inspection.state != ProjectDiskState::ready) {
        status_message_ = "Selected project is not ready on disk.";
        status_tone_ = EditorTone::danger;
        return;
    }
    BundleProjectRuntimeResult res{};
    std::string err;
    if (!bundle_project_runtime(selected.entry.project_directory, res, err)) {
        status_message_ = "Game build failed: " + err;
        status_tone_ = EditorTone::danger;
        return;
    }
    refresh_cached_inspections();
    status_message_ =
        "Bundled standalone game '" +
        path_to_utf8_str(res.executable_path.filename()) + "'.";
    status_tone_ = EditorTone::success;
}

void ProjectManagerApplication::reset_selected_project_icon() {
    if (!selected_index_ || *selected_index_ >= items_.size()) {
        return;
    }
    const CachedProjectItem& selected = items_[*selected_index_];
    if (selected.inspection.state != ProjectDiskState::ready) {
        return;
    }
    std::string err;
    if (!reset_project_icon_to_default(selected.entry.project_directory, err)) {
        status_message_ = "Reset default icon failed: " + err;
        status_tone_ = EditorTone::danger;
        return;
    }
    refresh_cached_inspections();
    status_message_ =
        "Restored default icon for '" + selected.entry.name + "'.";
    status_tone_ = EditorTone::success;
}

void ProjectManagerApplication::launch_icon_tool_for_selected_project() {
    if (!selected_index_ || *selected_index_ >= items_.size()) {
        return;
    }
    const CachedProjectItem& selected = items_[*selected_index_];
    const auto tool_exe = find_sibling_executable(executable_name_icon_tool());
    if (!tool_exe) {
        status_message_ =
            "Could not find " + executable_name_icon_tool() +
            " next to Project Manager.";
        status_tone_ = EditorTone::danger;
        return;
    }
    const std::vector<std::string> args{
        "--project",
        selected.directory_utf8,
    };
    std::string launch_err;
    if (!launch_detached_executable(
            *tool_exe, args, selected.entry.project_directory, launch_err)) {
        status_message_ = "Icon Utility launch failed: " + launch_err;
        status_tone_ = EditorTone::danger;
        return;
    }
    status_message_ =
        "Opened Icon Utility for '" + selected.entry.name + "'.";
    status_tone_ = EditorTone::info;
}

void ProjectManagerApplication::open_icon_file_picker(
    const std::filesystem::path& initial_dir) {
    icon_picker_error_.clear();
    icon_picker_drives_.clear();

#if defined(_WIN32)
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1U << i)) != 0U) {
            const char drive_letter = static_cast<char>('A' + i);
            const std::string root = std::string(1U, drive_letter) + ":\\";
            icon_picker_drives_.push_back(path_from_utf8_str(root));
        }
    }
#endif

    std::error_code ec;
    std::filesystem::path start = initial_dir;
    if (start.empty() || !std::filesystem::is_directory(start, ec) || ec) {
        ec.clear();
        start = std::filesystem::current_path(ec);
    }
    navigate_icon_file_picker(start);
    open_icon_file_picker_requested_ = true;
}

void ProjectManagerApplication::navigate_icon_file_picker(
    const std::filesystem::path& directory) {
    icon_picker_error_.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) {
        icon_picker_error_ = "Directory is not accessible.";
        return;
    }
    const auto canonical_or_abs =
        std::filesystem::weakly_canonical(directory, ec);
    icon_picker_current_ =
        (!ec && !canonical_or_abs.empty()) ? canonical_or_abs : directory;
    icon_picker_current_utf8_ = path_to_utf8_str(icon_picker_current_);

    icon_picker_subdirs_.clear();
    icon_picker_files_.clear();
    ec.clear();
    for (const auto& entry :
         std::filesystem::directory_iterator(icon_picker_current_, ec)) {
        if (ec) {
            break;
        }
        std::error_code item_ec;
        if (entry.is_directory(item_ec) && !item_ec) {
            icon_picker_subdirs_.push_back(entry.path());
        } else {
            item_ec.clear();
            if (entry.is_regular_file(item_ec) && !item_ec &&
                is_supported_pm_icon_extension(entry.path())) {
                icon_picker_files_.push_back(entry.path());
            }
        }
    }
    std::sort(icon_picker_subdirs_.begin(), icon_picker_subdirs_.end());
    std::sort(icon_picker_files_.begin(), icon_picker_files_.end());
}

void ProjectManagerApplication::build_icon_file_picker_modal() {
    if (open_icon_file_picker_requested_) {
        ImGui::OpenPopup("Choose Project Icon");
        open_icon_file_picker_requested_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(580.0F, 450.0F), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Choose Project Icon", nullptr)) {
        return;
    }

    if (!icon_picker_drives_.empty()) {
        for (std::size_t d = 0U; d < icon_picker_drives_.size(); ++d) {
            if (d > 0U) {
                ImGui::SameLine(0.0F, 4.0F);
            }
            const std::string drive_label =
                path_to_utf8_str(icon_picker_drives_[d]);
            if (ImGui::SmallButton(drive_label.c_str())) {
                navigate_icon_file_picker(icon_picker_drives_[d]);
            }
        }
    }

    const std::filesystem::path parent = icon_picker_current_.parent_path();
    const bool can_go_up =
        !parent.empty() && parent != icon_picker_current_;
    if (!can_go_up) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Up")) {
        navigate_icon_file_picker(parent);
    }
    if (!can_go_up) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0.0F, 8.0F);
    ImGui::TextWrapped("%s", icon_picker_current_utf8_.c_str());

    ImGui::Separator();

    const float list_height = ImGui::GetContentRegionAvail().y - 54.0F;
    if (ImGui::BeginChild(
            "##IconPickerEntries",
            ImVec2(0.0F, (std::max)(100.0F, list_height)),
            ImGuiChildFlags_Borders)) {
        std::optional<std::filesystem::path> next_dir;
        for (std::size_t i = 0U; i < icon_picker_subdirs_.size(); ++i) {
            const std::string label =
                "[DIR] " + path_to_utf8_str(icon_picker_subdirs_[i].filename());
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), false)) {
                next_dir = icon_picker_subdirs_[i];
            }
            ImGui::PopID();
        }
        std::optional<std::filesystem::path> picked_file;
        for (std::size_t i = 0U; i < icon_picker_files_.size(); ++i) {
            const std::string label =
                path_to_utf8_str(icon_picker_files_[i].filename());
            ImGui::PushID(static_cast<int>(100000U + i));
            if (ImGui::Selectable(label.c_str(), false)) {
                picked_file = icon_picker_files_[i];
            }
            ImGui::PopID();
        }
        if (icon_picker_subdirs_.empty() && icon_picker_files_.empty()) {
            ImGui::TextDisabled(
                "(No subdirectories or .png/.ico/.jpg/.bmp/.tga files)");
        }
        if (next_dir) {
            navigate_icon_file_picker(*next_dir);
        } else if (picked_file) {
            apply_icon_to_selected_project(*picked_file);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();

    if (!icon_picker_error_.empty()) {
        ui_kit_.status_text(icon_picker_error_.c_str(), EditorTone::danger);
    }
    if (ImGui::Button("Cancel", ImVec2(100.0F, 0.0F))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace vcr
