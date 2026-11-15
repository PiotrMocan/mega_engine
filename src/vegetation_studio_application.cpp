#include "vegetation_studio_application.hpp"

#include "camera_projection.hpp"
#include "png_encoder.hpp"
#include "runtime_assets.hpp"
#include "runtime_scene.hpp"
#include "scene_visibility.hpp"
#include "vulkan_common.hpp"
#include "window_icon_sdl.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view text) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

void copy_to_buffer(const std::span<char> dst, const std::string_view src) noexcept {
    if (dst.empty()) {
        return;
    }
    const std::size_t n = std::min(dst.size() - 1U, src.size());
    if (n > 0U) {
        std::memcpy(dst.data(), src.data(), n);
    }
    dst[n] = '\0';
}

void check_veg_imgui_vk_result(const VkResult result) {
    VK_CHECK(result);
}

[[nodiscard]] ImTextureID imgui_tex_id(const VkDescriptorSet descriptor_set) noexcept {
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(descriptor_set));
}

[[nodiscard]] glm::vec4 matrix_row(const glm::mat4& m, const int row) noexcept {
    return glm::vec4(m[0][row], m[1][row], m[2][row], m[3][row]);
}

[[nodiscard]] std::string_view arena_mode_token(const VegArenaMode mode) noexcept {
    switch (mode) {
    case VegArenaMode::hero_single:
        return "hero_single";
    case VegArenaMode::lod_lineup:
        return "lod_lineup";
    case VegArenaMode::grove_100:
        return "grove_100";
    case VegArenaMode::forest_2500:
        return "forest_2500";
    case VegArenaMode::meadow_10000:
        return "meadow_10000";
    }
    return "hero_single";
}

[[nodiscard]] std::string_view arena_mode_label(const VegArenaMode mode) noexcept {
    switch (mode) {
    case VegArenaMode::hero_single:
        return "Hero x1";
    case VegArenaMode::lod_lineup:
        return "LOD x4";
    case VegArenaMode::grove_100:
        return "Grove 100";
    case VegArenaMode::forest_2500:
        return "Forest 2.5K";
    case VegArenaMode::meadow_10000:
        return "Meadow 10K";
    }
    return "Hero x1";
}

[[nodiscard]] std::optional<VegArenaMode> parse_arena_mode(
    const std::string_view token) noexcept {
    if (token == "hero_single") {
        return VegArenaMode::hero_single;
    }
    if (token == "lod_lineup") {
        return VegArenaMode::lod_lineup;
    }
    if (token == "grove_100") {
        return VegArenaMode::grove_100;
    }
    if (token == "forest_2500") {
        return VegArenaMode::forest_2500;
    }
    if (token == "meadow_10000") {
        return VegArenaMode::meadow_10000;
    }
    return std::nullopt;
}

[[nodiscard]] std::uint32_t arena_target_instance_count(const VegArenaMode mode) noexcept {
    switch (mode) {
    case VegArenaMode::hero_single:
        return 1U;
    case VegArenaMode::lod_lineup:
        return 4U;
    case VegArenaMode::grove_100:
        return 100U;
    case VegArenaMode::forest_2500:
        return 2'500U;
    case VegArenaMode::meadow_10000:
        return 10'000U;
    }
    return 1U;
}

[[nodiscard]] float hash_unit_01(const std::uint32_t x, const std::uint32_t seed) noexcept {
    std::uint32_t h = x * 747796405U + seed * 2891336453U + 277803737U;
    h = ((h >> ((h >> 28U) + 4U)) ^ h) * 277803737U;
    h = (h >> 22U) ^ h;
    return static_cast<float>(h & 0x00FFFFFFU) / static_cast<float>(0x01000000U);
}

[[nodiscard]] bool write_binary_file_local(
    const std::filesystem::path& target,
    const std::span<const std::uint8_t> bytes,
    std::string& error) {
    error.clear();
    std::error_code ec;
    if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            error = "Unable to create directory '" +
                path_to_utf8(target.parent_path()) + "': " + ec.message();
            return false;
        }
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Unable to open output file '" + path_to_utf8(target) + "'.";
        return false;
    }
    if (!bytes.empty()) {
        out.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) {
        error = "Write failed for '" + path_to_utf8(target) + "'.";
        return false;
    }
    return true;
}

[[nodiscard]] std::filesystem::path resolve_project_assets_root(
    const std::filesystem::path& project_or_assets_dir) {
    if (project_or_assets_dir.empty()) {
        return {};
    }
    if (project_or_assets_dir.filename() == "assets") {
        return project_or_assets_dir;
    }
    return project_or_assets_dir / "assets";
}

[[nodiscard]] std::filesystem::path default_workspace_root() {
    if (const auto runtime_root = find_runtime_asset_root()) {
        if (!runtime_root->project_root.empty()) {
            return runtime_root->project_root;
        }
        return runtime_root->path.parent_path();
    }
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

[[nodiscard]] bool ray_sphere_hit(
    const glm::vec3 origin,
    const glm::vec3 direction,
    const glm::vec3 center,
    const float radius,
    float& out_distance) noexcept {
    if (radius <= 0.001F) {
        return false;
    }
    const glm::vec3 oc = origin - center;
    const float b = glm::dot(oc, direction);
    const float c = glm::dot(oc, oc) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0F) {
        return false;
    }
    const float sqrt_d = std::sqrt(disc);
    float t = -b - sqrt_d;
    if (t < 0.05F) {
        t = -b + sqrt_d;
    }
    if (t < 0.05F) {
        return false;
    }
    out_distance = t;
    return true;
}

[[nodiscard]] std::optional<glm::vec3> raycast_plant_crown(
    const glm::vec3 origin,
    const glm::vec3 direction,
    const GeneratedVegetationAsset& asset) noexcept {
    float best_t = (std::numeric_limits<float>::max)();
    bool hit = false;

    const std::size_t count = std::min(
        asset.foliage_cluster_centers.size(),
        asset.foliage_cluster_radii.size());
    for (std::size_t i = 0U; i < count; ++i) {
        float t = 0.0F;
        const float r = std::max(asset.foliage_cluster_radii[i], 0.18F);
        if (ray_sphere_hit(origin, direction, asset.foliage_cluster_centers[i], r, t) &&
            t < best_t) {
            best_t = t;
            hit = true;
        }
    }

    if (!hit) {
        if (const auto box_t = ray_bounds_intersection(origin, direction, asset.bounds)) {
            if (*box_t >= 0.05F) {
                best_t = *box_t;
                hit = true;
            }
        }
    }

    if (!hit) {
        return std::nullopt;
    }
    return origin + direction * best_t;
}

[[nodiscard]] glm::vec3 compute_viewport_ray(
    const FlyCamera& camera,
    const float fov_degrees,
    const ImVec2 mouse_pos,
    const ImVec2 img_min,
    const ImVec2 img_max) noexcept {
    const float width = (std::max)(1.0F, img_max.x - img_min.x);
    const float height = (std::max)(1.0F, img_max.y - img_min.y);
    const float u = std::clamp((mouse_pos.x - img_min.x) / width, 0.0F, 1.0F);
    const float v = std::clamp((mouse_pos.y - img_min.y) / height, 0.0F, 1.0F);
    const float ndc_x = u * 2.0F - 1.0F;
    const float ndc_y_up = 1.0F - v * 2.0F;
    const float aspect = width / height;
    const float half_tan = std::tan(glm::radians(fov_degrees) * 0.5F);
    const glm::vec3 dir =
        camera.forward() +
        camera.right() * (ndc_x * half_tan * aspect) +
        camera.up() * (ndc_y_up * half_tan);
    const float len = glm::length(dir);
    return len > 1.0e-5F ? (dir / len) : camera.forward();
}

[[nodiscard]] std::optional<ImVec2> project_world_to_screen(
    const FlyCamera& camera,
    const float fov_degrees,
    const glm::vec3 world_pos,
    const ImVec2 img_min,
    const ImVec2 img_max) noexcept {
    const float width = (std::max)(1.0F, img_max.x - img_min.x);
    const float height = (std::max)(1.0F, img_max.y - img_min.y);
    const float aspect = width / height;
    const float half_tan = std::tan(glm::radians(fov_degrees) * 0.5F);
    if (half_tan <= 1.0e-5F || aspect <= 1.0e-5F) {
        return std::nullopt;
    }

    const glm::vec3 rel = world_pos - camera.position();
    const float z_cam = glm::dot(rel, camera.forward());
    if (z_cam <= camera_near + 0.02F) {
        return std::nullopt;
    }
    const float x_cam = glm::dot(rel, camera.right());
    const float y_cam = glm::dot(rel, camera.up());
    const float u = 0.5F + 0.5F * (x_cam / (z_cam * half_tan * aspect));
    const float v = 0.5F - 0.5F * (y_cam / (z_cam * half_tan));
    if (u < -0.15F || u > 1.15F || v < -0.15F || v > 1.15F) {
        return std::nullopt;
    }
    return ImVec2(img_min.x + u * width, img_min.y + v * height);
}

[[nodiscard]] FlyCamera make_look_at_camera(
    const glm::vec3 eye,
    const glm::vec3 target) noexcept {
    glm::vec3 dir = target - eye;
    const float len = glm::length(dir);
    if (len < 1.0e-4F) {
        return FlyCamera(eye);
    }
    dir /= len;
    const float yaw = std::atan2(dir.z, dir.x);
    const float pitch = std::asin(std::clamp(dir.y, -0.985F, 0.985F));
    return FlyCamera(eye, yaw, pitch);
}

[[nodiscard]] EditorTone tone_for_budget_grade(const std::string_view grade) noexcept {
    if (grade == "A") {
        return EditorTone::success;
    }
    if (grade == "B") {
        return EditorTone::info;
    }
    if (grade == "C") {
        return EditorTone::warning;
    }
    return EditorTone::danger;
}

[[nodiscard]] bool parse_uint32_arg(const std::string_view text, std::uint32_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t acc = 0U;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        acc = acc * 10U + static_cast<std::uint64_t>(ch - '0');
        if (acc > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
    }
    out = static_cast<std::uint32_t>(acc);
    return true;
}

[[nodiscard]] bool parse_vec3_json(const Json& arr, glm::vec3& out) noexcept {
    if (!arr.is_array() || arr.size() != 3U) {
        return false;
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        if (!arr[i].is_number()) {
            return false;
        }
    }
    const float x = arr[0U].get<float>();
    const float y = arr[1U].get<float>();
    const float z = arr[2U].get<float>();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
    }
    out = glm::vec3(x, y, z);
    return true;
}

void print_usage() {
    std::cout <<
        "Vulkan Vegetation Studio — Procedural Vegetation Creation & Optimization Suite\n"
        "Usage: vulkan_vegetation_studio [options]\n\n"
        "Options:\n"
        "  --help, -h                Show this help and exit\n"
        "  --cli-generate            Generate and cook vegetation headlessly (no window)\n"
        "  --preset <id>             Built-in preset ID (default: silver_birch)\n"
        "  --seed <uint32>           Override deterministic seed\n"
        "  --foliage <full|sparse|bare> Override foliage density state\n"
        "  --slug <slug>             Override exported asset slug\n"
        "  --open <path>             Open existing *.vcrveg.json or *.vcrasset.json\n"
        "  --project <dir>           Target project root directory (<dir>/assets)\n"
        "  --out-asset-root <dir>    Explicit cooked asset root directory for CLI export\n"
        "  --category-subdir <rel>   Category subdir (default: models/vegetation)\n"
        "  --mcp-stdio               Enable JSON-RPC 2.0 MCP control on stdin/stdout\n"
        "  --validation              Enable Vulkan validation layers\n"
        "  --no-validation           Disable Vulkan validation layers\n"
        "  --width <px>              Initial window width (default: 1540)\n"
        "  --height <px>             Initial window height (default: 920)\n"
        "  --smoke-frames <n>        Render N frames and exit cleanly\n";
}

} // namespace

void VegStudioWindowDeleter::operator()(SDL_Window* const window) const noexcept {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
    }
}

ScopedTempPreviewDir::ScopedTempPreviewDir(ScopedTempPreviewDir&& other) noexcept
    : root_(std::move(other.root_)),
      assets_dir_(std::move(other.assets_dir_)) {
    other.root_.clear();
    other.assets_dir_.clear();
}

ScopedTempPreviewDir& ScopedTempPreviewDir::operator=(
    ScopedTempPreviewDir&& other) noexcept {
    if (this != &other) {
        reset();
        root_ = std::move(other.root_);
        assets_dir_ = std::move(other.assets_dir_);
        other.root_.clear();
        other.assets_dir_.clear();
    }
    return *this;
}

bool ScopedTempPreviewDir::create(std::string& error) {
    reset();
    static std::atomic<std::uint64_t> counter{1U};
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec || base.empty()) {
        error = "Unable to query temporary directory.";
        return false;
    }
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t seq = counter.fetch_add(1U, std::memory_order_relaxed);
    root_ = base / ("vcr_veg_studio_" + std::to_string(ticks) + "_" + std::to_string(seq));
    assets_dir_ = root_ / "assets";
    std::filesystem::create_directories(assets_dir_, ec);
    if (ec) {
        error = "Unable to create temporary preview directory '" +
            path_to_utf8(assets_dir_) + "': " + ec.message();
        root_.clear();
        assets_dir_.clear();
        return false;
    }
    return true;
}

void ScopedTempPreviewDir::reset() noexcept {
    if (!root_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        root_.clear();
    }
    assets_dir_.clear();
}

VegetationStudioApplication::~VegetationStudioApplication() noexcept {
    shutdown();
}

int VegetationStudioApplication::run(const int argc, char** const argv) {
    std::string error;
    if (!parse_cli(argc, argv, options_, error)) {
        std::cerr << "Error: " << error << "\n\n";
        print_usage();
        return 1;
    }
    if (options_.show_help) {
        print_usage();
        return 0;
    }
    if (options_.cli_generate) {
        return run_cli_generate(options_);
    }

    if (options_.mcp_stdio) {
        original_stdout_buffer_ = std::ref(*std::cout.rdbuf(std::cerr.rdbuf()));
    }

    initialize_gui(options_);
    if (options_.mcp_stdio) {
        mcp_server_.start();
    }
    main_loop();
    shutdown();
    return 0;
}

bool VegetationStudioApplication::parse_cli(
    const int argc,
    char** const argv,
    VegetationStudioCliOptions& out,
    std::string& error) const {
    error.clear();
    out = VegetationStudioCliOptions{};

    auto require_value = [&](int& index, const std::string_view flag, std::string& dst) -> bool {
        if (index + 1 >= argc || argv[index + 1] == nullptr) {
            error = std::string(flag) + " requires a value.";
            return false;
        }
        dst = argv[++index];
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            out.show_help = true;
            return true;
        }
        if (arg == "--cli-generate") {
            out.cli_generate = true;
            continue;
        }
        if (arg == "--mcp-stdio") {
            out.mcp_stdio = true;
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
        if (arg == "--preset") {
            if (!require_value(i, arg, out.preset_id)) {
                return false;
            }
            continue;
        }
        if (arg == "--seed") {
            std::string val;
            if (!require_value(i, arg, val)) {
                return false;
            }
            std::uint32_t seed_val = 0U;
            if (!parse_uint32_arg(val, seed_val)) {
                error = "--seed must be an unsigned 32-bit integer.";
                return false;
            }
            out.seed_override = seed_val;
            continue;
        }
        if (arg == "--foliage") {
            std::string val;
            if (!require_value(i, arg, val)) {
                return false;
            }
            const auto parsed = parse_vegetation_foliage_mode(val);
            if (!parsed) {
                error = "--foliage must be full, sparse, or bare.";
                return false;
            }
            out.foliage_override = *parsed;
            continue;
        }
        if (arg == "--slug") {
            if (!require_value(i, arg, out.slug_override)) {
                return false;
            }
            continue;
        }
        if (arg == "--open") {
            if (!require_value(i, arg, out.open_path)) {
                return false;
            }
            continue;
        }
        if (arg == "--project") {
            if (!require_value(i, arg, out.project_dir)) {
                return false;
            }
            continue;
        }
        if (arg == "--out-asset-root") {
            if (!require_value(i, arg, out.out_asset_root)) {
                return false;
            }
            continue;
        }
        if (arg == "--category-subdir") {
            if (!require_value(i, arg, out.category_subdir)) {
                return false;
            }
            continue;
        }
        if (arg == "--width") {
            std::string val;
            if (!require_value(i, arg, val)) {
                return false;
            }
            std::uint32_t w = 0U;
            if (!parse_uint32_arg(val, w) || w < 320U || w > 7680U) {
                error = "--width must be in [320, 7680].";
                return false;
            }
            out.width = w;
            continue;
        }
        if (arg == "--height") {
            std::string val;
            if (!require_value(i, arg, val)) {
                return false;
            }
            std::uint32_t h = 0U;
            if (!parse_uint32_arg(val, h) || h < 240U || h > 4320U) {
                error = "--height must be in [240, 4320].";
                return false;
            }
            out.height = h;
            continue;
        }
        if (arg == "--smoke-frames") {
            std::string val;
            if (!require_value(i, arg, val)) {
                return false;
            }
            std::uint32_t frames = 0U;
            if (!parse_uint32_arg(val, frames)) {
                error = "--smoke-frames must be a non-negative integer.";
                return false;
            }
            out.smoke_frames = frames;
            continue;
        }

        error = "Unknown argument: " + std::string(arg);
        return false;
    }

    if (out.cli_generate && out.mcp_stdio) {
        error = "--cli-generate and --mcp-stdio cannot be combined.";
        return false;
    }
    return true;
}

int VegetationStudioApplication::run_cli_generate(
    const VegetationStudioCliOptions& options) const {
    std::string error;
    VegetationRecipe recipe{};
    if (!options.open_path.empty()) {
        if (!open_vegetation_package(path_from_utf8(options.open_path), recipe, error)) {
            std::cerr << "Open failed: " << error << '\n';
            return 1;
        }
    } else {
        const auto preset = find_vegetation_preset(options.preset_id);
        if (!preset) {
            std::cerr << "Unknown vegetation preset '" << options.preset_id << "'.\n";
            return 1;
        }
        recipe = preset->recipe;
    }

    if (options.seed_override.has_value()) {
        recipe.seed = *options.seed_override;
    }
    if (options.foliage_override.has_value()) {
        recipe.foliage_mode = *options.foliage_override;
    }
    if (!options.slug_override.empty()) {
        recipe.asset_slug = sanitize_vegetation_slug(options.slug_override);
    }
    recipe = sanitize_vegetation_recipe(recipe);

    GeneratedVegetationAsset generated{};
    if (!generate_vegetation_asset(recipe, generated, error)) {
        std::cerr << "Generation failed: " << error << '\n';
        return 1;
    }

    std::filesystem::path asset_root;
    if (!options.out_asset_root.empty()) {
        asset_root = path_from_utf8(options.out_asset_root);
    } else if (!options.project_dir.empty()) {
        asset_root = resolve_project_assets_root(path_from_utf8(options.project_dir));
    } else {
        asset_root = default_workspace_root() / "assets";
    }

    VegetationExportSummary summary{};
    if (!export_vegetation_package(
            generated, asset_root, options.category_subdir, summary, error)) {
        std::cerr << "Export failed: " << error << '\n';
        return 1;
    }

    const Json result{
        {"ok", true},
        {"asset_slug", generated.recipe.asset_slug},
        {"display_name", generated.recipe.display_name},
        {"archetype", to_token(generated.recipe.archetype)},
        {"foliage_mode", to_token(generated.recipe.foliage_mode)},
        {"seed", generated.recipe.seed},
        {"asset_root", path_to_utf8(summary.asset_root)},
        {"package_directory", path_to_utf8(summary.package_directory)},
        {"recipe_file", path_to_utf8(summary.recipe_file)},
        {"lod0_model_file", path_to_utf8(summary.lod0_model_file)},
        {"lod0_relative_path", summary.lod0_relative_path},
        {"recipe_relative_path", summary.recipe_relative_path},
        {"report", Json{
            {"budget_grade", generated.report.budget_grade},
            {"budget_notes", generated.report.budget_notes},
            {"lod_triangles", generated.report.triangles},
            {"lod_vertices", generated.report.vertices},
            {"lod_submeshes", generated.report.submeshes},
            {"packed_vertex_bytes_total", generated.report.packed_vertex_bytes_total},
            {"texture_rgba_bytes_total", generated.report.texture_rgba_bytes_total},
            {"uses_gpu_grass", generated.report.uses_gpu_grass},
        }},
    };
    std::cout << result.dump(2) << '\n';
    return 0;
}

void VegetationStudioApplication::initialize_gui(
    const VegetationStudioCliOptions& options) {
    std::string error;
    if (!options.open_path.empty()) {
        if (!open_vegetation_package(path_from_utf8(options.open_path), recipe_, error)) {
            throw std::runtime_error("Failed to open vegetation package: " + error);
        }
    } else if (const auto preset = find_vegetation_preset(options.preset_id)) {
        recipe_ = preset->recipe;
    } else if (!vegetation_presets().empty()) {
        recipe_ = vegetation_presets().front().recipe;
        set_status("Preset '" + options.preset_id + "' not found; loaded default.", EditorTone::warning);
    }

    if (options.seed_override.has_value()) {
        recipe_.seed = *options.seed_override;
    }
    if (options.foliage_override.has_value()) {
        recipe_.foliage_mode = *options.foliage_override;
    }
    if (!options.slug_override.empty()) {
        recipe_.asset_slug = sanitize_vegetation_slug(options.slug_override);
    }
    recipe_ = sanitize_vegetation_recipe(recipe_);

    if (!preview_temp_dir_.create(error)) {
        throw std::runtime_error(error);
    }

    copy_to_buffer(category_subdir_buf_, options.category_subdir);
    copy_to_buffer(slug_buf_, recipe_.asset_slug);
    copy_to_buffer(display_name_buf_, recipe_.display_name);
    reload_projects_and_scan();
    if (!options.project_dir.empty()) {
        select_project_directory(path_from_utf8(options.project_dir));
    } else if (target_project_dir_.empty()) {
        if (!registered_projects_.empty()) {
            select_project_directory(registered_projects_.front().project_directory);
        } else {
            select_project_directory(default_workspace_root());
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    sdl_initialized_ = true;

    const std::string title =
        "Vulkan Vegetation Studio - " + recipe_.display_name + " (" + recipe_.asset_slug + ")";
    window_.reset(SDL_CreateWindow(
        title.c_str(),
        static_cast<int>(options.width),
        static_cast<int>(options.height),
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!window_) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
    main_window_id_ = SDL_GetWindowID(window_.get());
    apply_window_builtin_icon(window_.get(), BuiltinIconKind::editor);

    context_.initialize(window_.get(), options.validation);

    LightingSettings lighting{};
    lighting.dynamic = true;
    lighting.voxel_gi = voxel_gi_enabled_;
    lighting.cycle = false;
    lighting.hour = static_cast<double>(sun_hour_);
    lighting.sky_intensity = 1.15F;

    renderer_.initialize(
        context_,
        window_.get(),
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        RunMode::scene,
        1U,
        0U,
        AntiAliasingSettings{},
        true /*enable_screenshot_capture*/,
        true /*enable_editor_output*/,
        true /*enable_authored_scene*/,
        lighting,
        RenderFixture::none,
        ModelVertexFormat::automatic);
    static_cast<void>(renderer_.set_subsurface_mode(SubsurfaceMode::full_resolution));

    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = context_.queue_families().graphics,
    };
    VK_CHECK(vkCreateCommandPool(
        context_.device(),
        &pool_info,
        nullptr,
        preview_upload_pool_.put(context_.device(), vkDestroyCommandPool)));

    static_cast<void>(renderer_.prepare_editor_viewport(
        VkExtent2D{desired_viewport_width_, desired_viewport_height_}));

    ImGui::CreateContext();
    imgui_initialized_ = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;

    ui_kit_.initialize();
    if (!ImGui_ImplSDL3_InitForVulkan(window_.get())) {
        throw std::runtime_error("ImGui_ImplSDL3_InitForVulkan failed.");
    }
    initialize_imgui_vulkan();
    register_viewport_texture();

    arena_models_.reserve(10'000U);
    arena_scene_revision_ = 1U;
    rebuild_vegetation_assets(true /*frame_camera*/);
    update_arena_scene_if_dirty();

    smoke_frames_remaining_ = options.smoke_frames;
    running_ = true;
    last_frame_tick_ = std::chrono::steady_clock::now();
    last_metrics_tick_ = last_frame_tick_;
    last_title_tick_ = last_frame_tick_;
}

void VegetationStudioApplication::initialize_imgui_vulkan() {
    imgui_color_format_ = renderer_.swapchain().color_format();
    imgui_rendering_info_ = {};
    imgui_rendering_info_.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    imgui_rendering_info_.colorAttachmentCount = 1U;
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
        static_cast<std::uint32_t>(renderer_.swapchain().images().size());
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo = imgui_rendering_info_;
    init_info.UseDynamicRendering = true;
    init_info.CheckVkResultFn = check_veg_imgui_vk_result;
    init_info.MinAllocationSize = 1024U * 1024U;
    if (!ImGui_ImplVulkan_Init(&init_info)) {
        throw std::runtime_error("ImGui_ImplVulkan_Init failed.");
    }
    known_swapchain_generation_ = renderer_.swapchain_generation();
    known_swapchain_image_count_ = init_info.ImageCount;
    imgui_vulkan_initialized_ = true;
}

void VegetationStudioApplication::shutdown_imgui_vulkan() noexcept {
    if (imgui_vulkan_initialized_ &&
        ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplVulkan_Shutdown();
    }
    imgui_vulkan_initialized_ = false;
}

void VegetationStudioApplication::register_viewport_texture() {
    const VkImageView view = renderer_.editor_viewport_image_view();
    if (view == VK_NULL_HANDLE) {
        viewport_texture_ = VK_NULL_HANDLE;
        return;
    }
    viewport_texture_ = ImGui_ImplVulkan_AddTexture(
        view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (viewport_texture_ == VK_NULL_HANDLE) {
        throw std::runtime_error("ImGui_ImplVulkan_AddTexture returned VK_NULL_HANDLE.");
    }
    known_viewport_generation_ = renderer_.editor_viewport_generation();
}

void VegetationStudioApplication::unregister_viewport_texture() noexcept {
    if (viewport_texture_ != VK_NULL_HANDLE && imgui_vulkan_initialized_) {
        ImGui_ImplVulkan_RemoveTexture(viewport_texture_);
    }
    viewport_texture_ = VK_NULL_HANDLE;
}

void VegetationStudioApplication::upload_preview_textures() {
    if (!imgui_vulkan_initialized_ || preview_upload_pool_.get() == VK_NULL_HANDLE) {
        return;
    }
    VK_CHECK(vkResetCommandPool(context_.device(), preview_upload_pool_.get(), 0U));
    std::string error;
    if (current_asset_.has_foliage_textures && current_asset_.foliage_atlas.valid()) {
        if (!foliage_preview_.upload(
                context_,
                preview_upload_pool_.get(),
                current_asset_.foliage_atlas,
                "VegStudio_FoliageAtlas",
                error,
                imgui_vulkan_initialized_)) {
            set_status("Foliage preview upload failed: " + error, EditorTone::warning);
        }
    } else if (current_asset_.has_grass_textures && current_asset_.grass_albedo.valid()) {
        if (!foliage_preview_.upload(
                context_,
                preview_upload_pool_.get(),
                current_asset_.grass_albedo,
                "VegStudio_GrassAlbedo",
                error,
                imgui_vulkan_initialized_)) {
            set_status("Grass preview upload failed: " + error, EditorTone::warning);
        }
    } else {
        foliage_preview_.destroy(imgui_vulkan_initialized_);
    }

    if (current_asset_.has_bark_textures && current_asset_.bark_albedo.valid()) {
        if (!bark_preview_.upload(
                context_,
                preview_upload_pool_.get(),
                current_asset_.bark_albedo,
                "VegStudio_BarkAlbedo",
                error,
                imgui_vulkan_initialized_)) {
            set_status("Bark preview upload failed: " + error, EditorTone::warning);
        }
    } else {
        bark_preview_.destroy(imgui_vulkan_initialized_);
    }
}

void VegetationStudioApplication::synchronize_renderer_textures() {
    if (renderer_.swapchain_generation() != known_swapchain_generation_) {
        const auto current_count =
            static_cast<std::uint32_t>(renderer_.swapchain().images().size());
        if (renderer_.swapchain().color_format() != imgui_color_format_ ||
            current_count != known_swapchain_image_count_) {
            VK_CHECK(vkDeviceWaitIdle(context_.device()));
            foliage_preview_.destroy(imgui_vulkan_initialized_);
            bark_preview_.destroy(imgui_vulkan_initialized_);
            unregister_viewport_texture();
            shutdown_imgui_vulkan();
            ImGui_ImplSDL3_Shutdown();
            if (!ImGui_ImplSDL3_InitForVulkan(window_.get())) {
                throw std::runtime_error("Failed to reinitialize ImGui SDL3 backend.");
            }
            initialize_imgui_vulkan();
            register_viewport_texture();
            upload_preview_textures();
            return;
        }
        known_swapchain_generation_ = renderer_.swapchain_generation();
    }
    if (renderer_.editor_viewport_generation() != known_viewport_generation_) {
        unregister_viewport_texture();
        register_viewport_texture();
    }
}

void VegetationStudioApplication::rebuild_vegetation_assets(const bool frame_camera) {
    recipe_ = sanitize_vegetation_recipe(recipe_);
    copy_to_buffer(slug_buf_, recipe_.asset_slug);
    copy_to_buffer(display_name_buf_, recipe_.display_name);

    std::string error;
    if (!generate_vegetation_asset(recipe_, current_asset_, error)) {
        set_status("Generation failed: " + error, EditorTone::danger);
        preview_dirty_ = false;
        return;
    }

    if (!export_vegetation_preview_package(
            current_asset_,
            preview_temp_dir_.assets_dir(),
            preview_lod_relative_paths_,
            error)) {
        set_status("Preview export failed: " + error, EditorTone::danger);
        preview_dirty_ = false;
        return;
    }

    // `reset_editor_asset_cache` calls `vkDeviceWaitIdle`, making command pool reset
    // and ImGui texture descriptor replacement completely free of in-flight hazards.
    renderer_.reset_editor_asset_cache();
    upload_preview_textures();

    preview_dirty_ = false;
    arena_scene_dirty_ = true;

    if (frame_camera) {
        frame_camera_to_arena();
    }
    if (window_) {
        const std::string title =
            "Vulkan Vegetation Studio - " + recipe_.display_name +
            " (" + recipe_.asset_slug + ")";
        SDL_SetWindowTitle(window_.get(), title.c_str());
    }
    set_status(
        "Generated " + recipe_.display_name + " [Grade " +
            current_asset_.report.budget_grade + ", LOD0 " +
            std::to_string(current_asset_.report.triangles[0U]) + " tris]",
        tone_for_budget_grade(current_asset_.report.budget_grade));
}

void VegetationStudioApplication::update_arena_scene_if_dirty() {
    VisibilitySettings vis = renderer_.visibility_settings();
    vis.lod = (arena_mode_ != VegArenaMode::lod_lineup);
    vis.frustum = true;
    vis.occlusion = true;
    renderer_.set_visibility_settings(vis);

    arena_models_.clear();
    const Bounds& b = current_asset_.bounds;
    const float footprint_dim =
        std::max({b.extent.x * 2.0F, b.extent.z * 2.0F, 1.0F});
    const float auto_step = std::max(arena_spacing_, footprint_dim * 1.15F + 0.8F);

    auto push_instance = [&](
        const glm::vec3 pos,
        const float yaw_rad,
        const float scale_mul,
        const std::string_view asset_rel,
        const std::uint32_t idx) {
        glm::mat4 m(1.0F);
        m = glm::translate(m, pos);
        m = glm::rotate(m, yaw_rad, glm::vec3(0.0F, 1.0F, 0.0F));
        m = glm::scale(m, glm::vec3(scale_mul));
        arena_models_.push_back(SceneModelInstance{
            .model_row_0 = matrix_row(m, 0),
            .model_row_1 = matrix_row(m, 1),
            .model_row_2 = matrix_row(m, 2),
            .asset_path = asset_rel,
            .id = SceneNodeId{idx + 1U, 1U},
        });
    };

    float half_span = 12.0F;
    switch (arena_mode_) {
    case VegArenaMode::hero_single: {
        push_instance(glm::vec3(0.0F), 0.0F, 1.0F, preview_lod_relative_paths_[0U], 0U);
        half_span = std::max(8.0F, b.extent.y * 2.2F);
        break;
    }
    case VegArenaMode::lod_lineup: {
        for (std::uint32_t i = 0U; i < 4U; ++i) {
            const float x = (static_cast<float>(i) - 1.5F) * auto_step;
            push_instance(
                glm::vec3(x, 0.0F, 0.0F),
                0.0F,
                1.0F,
                preview_lod_relative_paths_[i],
                i);
        }
        half_span = auto_step * 2.5F + 6.0F;
        break;
    }
    case VegArenaMode::grove_100:
    case VegArenaMode::forest_2500:
    case VegArenaMode::meadow_10000: {
        const std::uint32_t grid =
            arena_mode_ == VegArenaMode::grove_100 ? 10U :
            arena_mode_ == VegArenaMode::forest_2500 ? 50U : 100U;
        const float spacing =
            arena_mode_ == VegArenaMode::meadow_10000
                ? std::max(0.75F, footprint_dim * 0.85F)
                : auto_step;
        const float half = 0.5F * static_cast<float>(grid - 1U) * spacing;
        half_span = half + spacing + 8.0F;
        std::uint32_t idx = 0U;
        for (std::uint32_t gz = 0U; gz < grid; ++gz) {
            for (std::uint32_t gx = 0U; gx < grid; ++gx) {
                const float rx = hash_unit_01(idx * 3U + 1U, recipe_.seed) - 0.5F;
                const float rz = hash_unit_01(idx * 3U + 2U, recipe_.seed) - 0.5F;
                const float r_yaw = hash_unit_01(idx * 5U + 3U, recipe_.seed);
                const float r_scl = hash_unit_01(idx * 7U + 5U, recipe_.seed);
                const float x = static_cast<float>(gx) * spacing - half + rx * spacing * 0.42F;
                const float z = static_cast<float>(gz) * spacing - half + rz * spacing * 0.42F;
                const float yaw = r_yaw * 6.2831853F;
                const float scl = 0.84F + r_scl * 0.34F;
                push_instance(
                    glm::vec3(x, 0.0F, z),
                    yaw,
                    scl,
                    preview_lod_relative_paths_[0U],
                    idx);
                ++idx;
            }
        }
        break;
    }
    }

    const float ground_extent = std::clamp(half_span * 1.4F, 12.0F, 240.0F);
    glm::mat4 gm(1.0F);
    gm = glm::translate(gm, glm::vec3(0.0F, -0.05F, 0.0F));
    gm = glm::scale(gm, glm::vec3(ground_extent, 0.05F, ground_extent));

    SceneGpuInstance ground{};
    ground.model_row_0 = matrix_row(gm, 0);
    ground.model_row_1 = matrix_row(gm, 1);
    ground.model_row_2 = matrix_row(gm, 2);
    ground.material.base_color = glm::vec4(0.16F, 0.17F, 0.14F, 1.0F);
    ground.material.roughness = 0.90F;
    ground.material.metallic = 0.0F;
    ground.material.ambient_occlusion = 1.0F;

    ++arena_scene_revision_;
    SceneRenderView view{};
    view.ground = ground;
    view.models = arena_models_;
    view.revision = arena_scene_revision_;
    view.has_ground = true;
    view.sky_visible = true;
    view.sun_visible = true;

    std::string error;
    if (!renderer_.update_editor_scene(view, preview_temp_dir_.assets_dir(), error)) {
        set_status("Scene update failed: " + error, EditorTone::danger);
    }
    arena_scene_dirty_ = false;
}

void VegetationStudioApplication::frame_camera_to_arena() noexcept {
    const Bounds& b = current_asset_.bounds;
    const float h = std::max(b.extent.y * 2.0F, 0.8F);
    const float w = std::max({b.extent.x * 2.0F, b.extent.z * 2.0F, 0.8F});
    const float step = std::max(arena_spacing_, w * 1.15F + 0.8F);

    switch (arena_mode_) {
    case VegArenaMode::hero_single: {
        const glm::vec3 target = b.center;
        const float dist = std::max(h * 1.45F, w * 1.85F) + 1.4F;
        camera_ = make_look_at_camera(
            target + glm::vec3(dist * 0.45F, h * 0.22F, dist * 0.92F),
            target);
        break;
    }
    case VegArenaMode::lod_lineup: {
        const glm::vec3 target(0.0F, b.center.y, 0.0F);
        const float span = step * 3.0F + w;
        const float dist = std::max(span * 0.95F, h * 1.65F) + 2.0F;
        camera_ = make_look_at_camera(
            glm::vec3(0.0F, b.center.y + h * 0.35F, dist),
            target);
        break;
    }
    case VegArenaMode::grove_100: {
        const float dist = step * 7.5F + h;
        camera_ = make_look_at_camera(
            glm::vec3(dist * 0.65F, std::max(h * 1.8F, dist * 0.42F), dist * 0.82F),
            glm::vec3(0.0F, b.center.y * 0.7F, 0.0F));
        break;
    }
    case VegArenaMode::forest_2500: {
        const float dist = step * 18.0F + h * 2.0F;
        camera_ = make_look_at_camera(
            glm::vec3(dist * 0.55F, std::max(h * 2.4F, dist * 0.38F), dist * 0.85F),
            glm::vec3(0.0F, b.center.y * 0.5F, 0.0F));
        break;
    }
    case VegArenaMode::meadow_10000: {
        const float eff_step = std::max(0.75F, w * 0.85F);
        const float dist = eff_step * 26.0F + h * 2.0F;
        camera_ = make_look_at_camera(
            glm::vec3(dist * 0.5F, std::max(h * 3.0F, dist * 0.42F), dist * 0.86F),
            glm::vec3(0.0F, b.center.y * 0.4F, 0.0F));
        break;
    }
    }
}

void VegetationStudioApplication::reload_projects_and_scan() {
    std::string error;
    if (project_registry_.load_or_initialize(default_project_registry_path(), error)) {
        const auto entries = project_registry_.entries();
        registered_projects_.assign(entries.begin(), entries.end());
    } else {
        registered_projects_.clear();
    }

    project_vegetation_.clear();
    if (!target_project_dir_.empty()) {
        const std::filesystem::path assets_root =
            resolve_project_assets_root(target_project_dir_);
        static_cast<void>(
            list_project_vegetation_assets(assets_root, project_vegetation_, error));
    }
}

void VegetationStudioApplication::select_project_directory(
    const std::filesystem::path& dir) {
    target_project_dir_ = dir;
    copy_to_buffer(target_project_buf_, path_to_utf8(target_project_dir_));
    project_vegetation_.clear();
    if (!target_project_dir_.empty()) {
        std::string error;
        const std::filesystem::path assets_root =
            resolve_project_assets_root(target_project_dir_);
        static_cast<void>(
            list_project_vegetation_assets(assets_root, project_vegetation_, error));
    }
}

void VegetationStudioApplication::main_loop() {
    while (running_) {
        auto frame_begin = std::chrono::steady_clock::now();

        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            process_sdl_event(event);
        }
        process_mcp_commands();

        if (!running_) {
            if (quit_after_mcp_flush_ || pending_mcp_screenshot_id_ == 0U) {
                break;
            }
        }

        if (mcp_control_.quit_requested.load(std::memory_order_acquire) &&
            pending_mcp_screenshot_id_ == 0U) {
            running_ = false;
            break;
        }

        int pixel_w = 0;
        int pixel_h = 0;
        if (window_) {
            SDL_GetWindowSizeInPixels(window_.get(), &pixel_w, &pixel_h);
        }
        if (pixel_w <= 0 || pixel_h <= 0) {
            SDL_WaitEventTimeout(nullptr, 16);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(
            std::chrono::duration<float>(now - last_frame_tick_).count(),
            0.0F,
            0.1F);
        last_frame_tick_ = now;

        bool rebuilt_this_frame = false;
        if (viewport_resize_pending_ &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_viewport_resize_time_).count() >= 50) {
            static_cast<void>(renderer_.prepare_editor_viewport(
                VkExtent2D{desired_viewport_width_, desired_viewport_height_}));
            viewport_resize_pending_ = false;
            rebuilt_this_frame = true;
        }

        if (sun_gi_dirty_) {
            LightingSettings lighting = renderer_.lighting_settings();
            lighting.dynamic = true;
            lighting.cycle = false;
            lighting.hour = static_cast<double>(sun_hour_);
            lighting.voxel_gi = voxel_gi_enabled_;
            static_cast<void>(renderer_.set_lighting_settings(lighting));
            sun_gi_dirty_ = false;
        }

        if (preview_dirty_) {
            rebuild_vegetation_assets(false);
            rebuilt_this_frame = true;
        }
        if (arena_scene_dirty_) {
            update_arena_scene_if_dirty();
            rebuilt_this_frame = true;
        }
        if (rebuilt_this_frame) {
            renderer_.reset_metrics();
            frame_begin = std::chrono::steady_clock::now();
        }
        static_cast<void>(renderer_.poll_assets());
        synchronize_renderer_textures();

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        build_ui();
        ImGui::Render();

        static_cast<void>(dt);
        FrameTimings timings{};
        const bool rendered = renderer_.draw_frame(
            timings, camera_.view_matrix(), ImGui::GetDrawData());
        synchronize_renderer_textures();
        complete_mcp_screenshot();

        if (rendered) {
            const auto frame_end = std::chrono::steady_clock::now();
            timings.cpu_frame_ms = std::max(
                0.001,
                std::chrono::duration<double, std::milli>(frame_end - frame_begin).count());
            last_timings_ = timings;
            renderer_.record_metrics(timings);

            if (ui_metrics_.timed_frames == 0U ||
                rebuilt_this_frame ||
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame_end - last_metrics_tick_).count() >= 120) {
                ui_metrics_ = renderer_.metrics_snapshot(true);
                last_metrics_tick_ = frame_end;
            }
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame_end - last_title_tick_).count() >= 250) {
                update_window_title();
                last_title_tick_ = frame_end;
            }
        }

        if (smoke_frames_remaining_ > 0U) {
            --smoke_frames_remaining_;
            if (smoke_frames_remaining_ == 0U && pending_mcp_screenshot_id_ == 0U) {
                running_ = false;
            }
        }
    }
}

void VegetationStudioApplication::process_sdl_event(const SDL_Event& event) {
    if (imgui_initialized_) {
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
    switch (event.type) {
    case SDL_EVENT_QUIT:
        running_ = false;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == main_window_id_) {
            running_ = false;
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event.key.key == SDLK_ESCAPE && !ImGui::GetIO().WantTextInput) {
            paint_brush_enabled_ = false;
        } else if (event.key.key == SDLK_F && !ImGui::GetIO().WantTextInput) {
            frame_camera_to_arena();
        }
        break;
    default:
        break;
    }
}

void VegetationStudioApplication::update_camera_controls(
    const float dt_seconds,
    const bool viewport_hovered,
    const bool viewport_focused) {
    const ImGuiIO& io = ImGui::GetIO();

    if (viewport_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        rmb_looking_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        rmb_looking_ = false;
    }

    if (viewport_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        mmb_panning_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        mmb_panning_ = false;
    }

    FlyCameraInput input{};
    if (rmb_looking_) {
        input.mouse_delta_x = io.MouseDelta.x;
        input.mouse_delta_y = io.MouseDelta.y;
    }
    if (mmb_panning_) {
        const float pan_scale = std::max(0.015F, glm::length(camera_.position()) * 0.002F);
        const glm::vec3 delta =
            (-camera_.right() * io.MouseDelta.x + camera_.up() * io.MouseDelta.y) *
            pan_scale;
        camera_ = FlyCamera(
            camera_.position() + delta,
            camera_.yaw_radians(),
            camera_.pitch_radians());
    }
    if (viewport_hovered && std::abs(io.MouseWheel) > 0.001F) {
        const float zoom_step =
            std::max(0.6F, glm::length(camera_.position()) * 0.14F) * io.MouseWheel;
        camera_ = FlyCamera(
            camera_.position() + camera_.forward() * zoom_step,
            camera_.yaw_radians(),
            camera_.pitch_radians());
    }

    if ((viewport_hovered || viewport_focused || rmb_looking_) && !io.WantTextInput) {
        input.forward = ImGui::IsKeyDown(ImGuiKey_W);
        input.backward = ImGui::IsKeyDown(ImGuiKey_S);
        input.right = ImGui::IsKeyDown(ImGuiKey_D);
        input.left = ImGui::IsKeyDown(ImGuiKey_A);
        input.up = ImGui::IsKeyDown(ImGuiKey_E);
        input.down = ImGui::IsKeyDown(ImGuiKey_Q);
        input.boost = ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
                      ImGui::IsKeyDown(ImGuiKey_RightShift);
    }

    if (input.forward || input.backward || input.left || input.right ||
        input.up || input.down ||
        input.mouse_delta_x != 0.0F || input.mouse_delta_y != 0.0F) {
        camera_.update(input, std::max(dt_seconds, 1.0F / 240.0F));
    }
}

void VegetationStudioApplication::handle_viewport_painting(
    const ImVec2 img_min,
    const ImVec2 img_max,
    const bool viewport_hovered) {
    last_brush_hit_.reset();
    if (!paint_brush_enabled_ || !viewport_hovered ||
        arena_mode_ != VegArenaMode::hero_single) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec3 ray_dir = compute_viewport_ray(
        camera_,
        renderer_.vertical_fov_degrees(),
        io.MousePos,
        img_min,
        img_max);
    last_brush_hit_ = raycast_plant_crown(camera_.position(), ray_dir, current_asset_);
    if (!last_brush_hit_.has_value()) {
        return;
    }

    if (const auto screen_pt = project_world_to_screen(
            camera_,
            renderer_.vertical_fov_degrees(),
            *last_brush_hit_,
            img_min,
            img_max)) {
        const float dist = std::max(glm::length(*last_brush_hit_ - camera_.position()), 0.5F);
        const float height_px = std::max(img_max.y - img_min.y, 1.0F);
        const float px_per_meter =
            height_px / (2.0F * dist * std::tan(glm::radians(renderer_.vertical_fov_degrees()) * 0.5F));
        const float screen_radius = std::clamp(brush_radius_ * px_per_meter, 8.0F, 140.0F);
        const ImU32 ring_col =
            brush_mode_ == VegetationPaintMode::prune_bare
                ? IM_COL32(255, 110, 90, 230)
                : IM_COL32(
                      static_cast<int>(brush_color_srgb_.r * 255.0F),
                      static_cast<int>(brush_color_srgb_.g * 255.0F),
                      static_cast<int>(brush_color_srgb_.b * 255.0F),
                      230);
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        dl->AddCircle(*screen_pt, screen_radius, ring_col, 32, 2.0F);
        dl->AddCircleFilled(*screen_pt, 3.5F, ring_col);
    }

    if (!rmb_looking_ && !mmb_panning_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const glm::vec3 hit = *last_brush_hit_;
        const bool just_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const float min_step = std::max(brush_radius_ * 0.30F, 0.12F);
        const bool moved_far_enough =
            !has_last_stroke_center_ ||
            glm::length(hit - last_stroke_center_) >= min_step;
        if (just_clicked || moved_far_enough) {
            if (recipe_.paint_strokes.size() >= maximum_vegetation_paint_strokes) {
                recipe_.paint_strokes.erase(recipe_.paint_strokes.begin());
            }
            recipe_.paint_strokes.push_back(VegetationPaintStroke{
                .local_center = hit,
                .radius = brush_radius_,
                .strength = brush_strength_,
                .falloff = brush_falloff_,
                .mode = brush_mode_,
                .tint_srgb = brush_color_srgb_,
            });
            last_stroke_center_ = hit;
            has_last_stroke_center_ = true;
            preview_dirty_ = true;
        }
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        has_last_stroke_center_ = false;
    }
}

void VegetationStudioApplication::process_mcp_commands() {
    VegMcpCommand cmd;
    while (mcp_control_.requests.try_pop(cmd)) {
        execute_mcp_command(cmd);
    }
}

void VegetationStudioApplication::execute_mcp_command(const VegMcpCommand& cmd) {
    VegMcpResponse resp{};
    resp.request_id = cmd.request_id;
    resp.ok = false;

    const std::string_view tool = cmd.tool_name;
    const Json& args = cmd.arguments;

    if (tool == "veg.list_presets") {
        Json presets = Json::array();
        for (const VegetationPresetInfo& info : vegetation_presets()) {
            presets.push_back(Json{
                {"id", info.id},
                {"label", info.label},
                {"category", info.category},
                {"archetype", to_token(info.recipe.archetype)},
                {"foliage_mode", to_token(info.recipe.foliage_mode)},
                {"seed", info.recipe.seed},
            });
        }
        resp.ok = true;
        resp.payload = Json{{"presets", std::move(presets)}};
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.get_state") {
        resp.ok = true;
        resp.payload = build_state_json();
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.apply_preset") {
        if (!args.contains("preset_id") || !args["preset_id"].is_string()) {
            resp.error = "veg.apply_preset requires string 'preset_id'.";
            push_mcp_response(std::move(resp));
            return;
        }
        const std::string preset_id = args["preset_id"].get<std::string>();
        const auto preset = find_vegetation_preset(preset_id);
        if (!preset) {
            resp.error = "Unknown vegetation preset '" + preset_id + "'.";
            push_mcp_response(std::move(resp));
            return;
        }
        recipe_ = preset->recipe;
        if (args.contains("seed") && args["seed"].is_number_unsigned()) {
            recipe_.seed = args["seed"].get<std::uint32_t>();
        }
        rebuild_vegetation_assets(true /*frame_camera*/);
        update_arena_scene_if_dirty();
        resp.ok = true;
        resp.payload = Json{{"state", build_state_json()}};
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.set_recipe") {
        const Json& patch =
            (args.contains("recipe") && args["recipe"].is_object())
                ? args["recipe"]
                : args;
        if (!patch.is_object()) {
            resp.error = "veg.set_recipe requires an object patch.";
            push_mcp_response(std::move(resp));
            return;
        }
        Json merged = serialize_vegetation_recipe_json(recipe_);
        for (const auto& [k, v] : patch.items()) {
            merged[k] = v;
        }
        VegetationRecipe next{};
        std::string parse_err;
        if (!deserialize_vegetation_recipe_json(merged, next, parse_err)) {
            resp.error = parse_err;
            push_mcp_response(std::move(resp));
            return;
        }
        recipe_ = sanitize_vegetation_recipe(std::move(next));
        rebuild_vegetation_assets(false);
        update_arena_scene_if_dirty();
        resp.ok = true;
        resp.payload = Json{{"state", build_state_json()}};
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.randomize_seed") {
        if (args.contains("seed") && args["seed"].is_number_unsigned()) {
            recipe_.seed = args["seed"].get<std::uint32_t>();
        } else {
            recipe_.seed = recipe_.seed * 1664525U + 1013904223U;
            if (recipe_.seed == 0U) {
                recipe_.seed = 1337U;
            }
        }
        rebuild_vegetation_assets(false);
        update_arena_scene_if_dirty();
        resp.ok = true;
        resp.payload = Json{{"seed", recipe_.seed}, {"state", build_state_json()}};
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.paint_foliage") {
        if (!args.contains("mode") || !args["mode"].is_string()) {
            resp.error = "veg.paint_foliage requires string 'mode'.";
            push_mcp_response(std::move(resp));
            return;
        }
        const auto mode = parse_vegetation_paint_mode(args["mode"].get<std::string>());
        if (!mode) {
            resp.error = "Invalid paint mode.";
            push_mcp_response(std::move(resp));
            return;
        }
        glm::vec3 center{0.0F};
        if (!args.contains("center") || !parse_vec3_json(args["center"], center)) {
            resp.error = "veg.paint_foliage requires numeric [x,y,z] 'center'.";
            push_mcp_response(std::move(resp));
            return;
        }
        VegetationPaintStroke stroke{};
        stroke.mode = *mode;
        stroke.local_center = center;
        if (args.contains("radius") && args["radius"].is_number()) {
            stroke.radius = std::clamp(args["radius"].get<float>(), 0.05F, 12.0F);
        }
        if (args.contains("strength") && args["strength"].is_number()) {
            stroke.strength = std::clamp(args["strength"].get<float>(), 0.05F, 1.0F);
        }
        if (args.contains("falloff") && args["falloff"].is_number()) {
            stroke.falloff = std::clamp(args["falloff"].get<float>(), 0.0F, 1.0F);
        }
        if (args.contains("color")) {
            glm::vec3 col{stroke.tint_srgb};
            if (!parse_vec3_json(args["color"], col)) {
                resp.error = "veg.paint_foliage 'color' must be numeric [r,g,b].";
                push_mcp_response(std::move(resp));
                return;
            }
            stroke.tint_srgb = glm::clamp(col, glm::vec3(0.0F), glm::vec3(1.0F));
        }
        if (recipe_.paint_strokes.size() >= maximum_vegetation_paint_strokes) {
            recipe_.paint_strokes.erase(recipe_.paint_strokes.begin());
        }
        recipe_.paint_strokes.push_back(stroke);
        rebuild_vegetation_assets(false);
        update_arena_scene_if_dirty();
        resp.ok = true;
        resp.payload = Json{
            {"stroke_count", recipe_.paint_strokes.size()},
            {"state", build_state_json()},
        };
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.clear_paint") {
        recipe_.paint_strokes.clear();
        rebuild_vegetation_assets(false);
        update_arena_scene_if_dirty();
        resp.ok = true;
        resp.payload = Json{{"state", build_state_json()}};
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.set_arena") {
        if (!args.contains("mode") || !args["mode"].is_string()) {
            resp.error = "veg.set_arena requires string 'mode'.";
            push_mcp_response(std::move(resp));
            return;
        }
        const auto parsed = parse_arena_mode(args["mode"].get<std::string>());
        if (!parsed) {
            resp.error = "Invalid arena mode.";
            push_mcp_response(std::move(resp));
            return;
        }
        arena_mode_ = *parsed;
        if (args.contains("spacing") && args["spacing"].is_number()) {
            arena_spacing_ = std::clamp(args["spacing"].get<float>(), 0.5F, 60.0F);
        }
        arena_scene_dirty_ = true;
        update_arena_scene_if_dirty();
        frame_camera_to_arena();
        resp.ok = true;
        resp.payload = Json{{"state", build_state_json()}};
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.set_camera") {
        if (args.contains("fov_degrees") && args["fov_degrees"].is_number()) {
            renderer_.set_vertical_fov_degrees(args["fov_degrees"].get<float>());
        }
        if (args.value("frame_bounds", false)) {
            frame_camera_to_arena();
        } else if (args.contains("position")) {
            glm::vec3 pos{0.0F};
            if (!parse_vec3_json(args["position"], pos)) {
                resp.error = "veg.set_camera 'position' must be numeric [x,y,z].";
                push_mcp_response(std::move(resp));
                return;
            }
            if (args.contains("target")) {
                glm::vec3 target{0.0F};
                if (!parse_vec3_json(args["target"], target)) {
                    resp.error = "veg.set_camera 'target' must be numeric [x,y,z].";
                    push_mcp_response(std::move(resp));
                    return;
                }
                camera_ = make_look_at_camera(pos, target);
            } else {
                camera_ = FlyCamera(pos, camera_.yaw_radians(), camera_.pitch_radians());
            }
        }
        resp.ok = true;
        resp.payload = Json{{"state", build_state_json()}};
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.export_to_project") {
        std::filesystem::path project_dir = target_project_dir_;
        if (args.contains("project_dir") && args["project_dir"].is_string() &&
            !args["project_dir"].get<std::string>().empty()) {
            project_dir = path_from_utf8(args["project_dir"].get<std::string>());
        }
        if (project_dir.empty()) {
            resp.error = "No target project_dir specified or selected.";
            push_mcp_response(std::move(resp));
            return;
        }
        std::string subdir(category_subdir_buf_.data());
        if (args.contains("category_subdir") && args["category_subdir"].is_string() &&
            !args["category_subdir"].get<std::string>().empty()) {
            subdir = args["category_subdir"].get<std::string>();
            copy_to_buffer(category_subdir_buf_, subdir);
        }
        if (args.contains("asset_slug") && args["asset_slug"].is_string() &&
            !args["asset_slug"].get<std::string>().empty()) {
            const std::string clean_slug =
                sanitize_vegetation_slug(args["asset_slug"].get<std::string>());
            if (clean_slug != recipe_.asset_slug) {
                recipe_.asset_slug = clean_slug;
                rebuild_vegetation_assets(false);
                update_arena_scene_if_dirty();
            }
        }

        const std::filesystem::path asset_root =
            resolve_project_assets_root(project_dir);
        VegetationExportSummary summary{};
        std::string export_err;
        if (!export_vegetation_package(
                current_asset_, asset_root, subdir, summary, export_err)) {
            resp.error = export_err;
            push_mcp_response(std::move(resp));
            return;
        }
        last_export_ = summary;
        select_project_directory(project_dir);
        set_status("Exported to " + path_to_utf8(summary.package_directory), EditorTone::success);

        resp.ok = true;
        resp.payload = Json{
            {"asset_root", path_to_utf8(summary.asset_root)},
            {"package_directory", path_to_utf8(summary.package_directory)},
            {"recipe_file", path_to_utf8(summary.recipe_file)},
            {"lod0_model_file", path_to_utf8(summary.lod0_model_file)},
            {"lod0_relative_path", summary.lod0_relative_path},
            {"recipe_relative_path", summary.recipe_relative_path},
            {"material_count", summary.material_files.size()},
            {"texture_count", summary.texture_files.size()},
        };
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.open_from_project") {
        if (!args.contains("path") || !args["path"].is_string()) {
            resp.error = "veg.open_from_project requires string 'path'.";
            push_mcp_response(std::move(resp));
            return;
        }
        std::filesystem::path target = path_from_utf8(args["path"].get<std::string>());
        if (target.is_relative() && !target_project_dir_.empty()) {
            const std::filesystem::path candidate =
                resolve_project_assets_root(target_project_dir_) / target;
            if (std::filesystem::exists(candidate)) {
                target = candidate;
            }
        }
        VegetationRecipe loaded{};
        std::string open_err;
        if (!open_vegetation_package(target, loaded, open_err)) {
            resp.error = open_err;
            push_mcp_response(std::move(resp));
            return;
        }
        recipe_ = sanitize_vegetation_recipe(std::move(loaded));
        rebuild_vegetation_assets(true);
        update_arena_scene_if_dirty();
        resp.ok = true;
        resp.payload = Json{{"state", build_state_json()}};
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "veg.list_project_vegetation") {
        std::filesystem::path project_dir = target_project_dir_;
        if (args.contains("project_dir") && args["project_dir"].is_string() &&
            !args["project_dir"].get<std::string>().empty()) {
            project_dir = path_from_utf8(args["project_dir"].get<std::string>());
        }
        if (project_dir.empty()) {
            resp.error = "No project_dir specified.";
            push_mcp_response(std::move(resp));
            return;
        }
        std::vector<ProjectVegetationEntry> entries{};
        std::string list_err;
        const std::filesystem::path asset_root = resolve_project_assets_root(project_dir);
        if (!list_project_vegetation_assets(asset_root, entries, list_err)) {
            resp.error = list_err;
            push_mcp_response(std::move(resp));
            return;
        }
        Json list = Json::array();
        for (const ProjectVegetationEntry& e : entries) {
            list.push_back(Json{
                {"asset_slug", e.asset_slug},
                {"display_name", e.display_name},
                {"archetype", to_token(e.archetype)},
                {"recipe_relative_path", e.recipe_relative_path},
                {"lod0_relative_path", e.lod0_relative_path},
                {"recipe_absolute_path", path_to_utf8(e.recipe_absolute_path)},
            });
        }
        resp.ok = true;
        resp.payload = Json{
            {"asset_root", path_to_utf8(asset_root)},
            {"assets", std::move(list)},
        };
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "render.get_metrics") {
        resp.ok = true;
        resp.payload = build_metrics_json();
        push_mcp_response(std::move(resp));
        return;
    }

    if (tool == "capture.screenshot") {
        if (pending_mcp_screenshot_id_ != 0U) {
            resp.error = "Another MCP screenshot capture is already pending.";
            push_mcp_response(std::move(resp));
            return;
        }
        if (!renderer_.request_screenshot()) {
            resp.error = "Screenshot capture is unavailable on this swapchain.";
            push_mcp_response(std::move(resp));
            return;
        }
        pending_mcp_screenshot_id_ = cmd.request_id;
        pending_mcp_screenshot_path_.clear();
        if (args.contains("output_path") && args["output_path"].is_string()) {
            pending_mcp_screenshot_path_ = args["output_path"].get<std::string>();
        }
        return;
    }

    if (tool == "engine.quit") {
        quit_after_mcp_flush_ = true;
        running_ = false;
        resp.ok = true;
        resp.payload = Json{{"quitting", true}};
        push_mcp_response(std::move(resp));
        return;
    }

    resp.error = "Unhandled tool: " + cmd.tool_name;
    push_mcp_response(std::move(resp));
}

void VegetationStudioApplication::complete_mcp_screenshot() {
    if (pending_mcp_screenshot_id_ == 0U && !save_manual_screenshot_) {
        return;
    }
    const auto capture = renderer_.take_completed_screenshot();
    if (!capture.has_value()) {
        return;
    }

    if (pending_mcp_screenshot_id_ != 0U) {
        VegMcpResponse resp{};
        resp.request_id = pending_mcp_screenshot_id_;
        if (!*capture) {
            resp.ok = false;
            resp.error = capture->error.empty()
                ? "Screenshot capture failed."
                : capture->error;
        } else {
            PngEncodeResult png = encode_png_rgba(
                capture->rgba, capture->width, capture->height);
            if (!png) {
                resp.ok = false;
                resp.error = png.error;
            } else {
                std::string write_err;
                bool disk_ok = true;
                if (!pending_mcp_screenshot_path_.empty()) {
                    disk_ok = write_binary_file_local(
                        path_from_utf8(pending_mcp_screenshot_path_),
                        png.bytes,
                        write_err);
                }
                if (!disk_ok) {
                    resp.ok = false;
                    resp.error = write_err;
                } else {
                    resp.ok = true;
                    resp.screenshot_width = capture->width;
                    resp.screenshot_height = capture->height;
                    resp.payload = Json{
                        {"width", capture->width},
                        {"height", capture->height},
                        {"png_bytes", png.bytes.size()},
                        {"saved_path", pending_mcp_screenshot_path_},
                    };
                    resp.screenshot_png = std::move(png.bytes);
                }
            }
        }
        pending_mcp_screenshot_id_ = 0U;
        pending_mcp_screenshot_path_.clear();
        push_mcp_response(std::move(resp));
    }

    if (save_manual_screenshot_) {
        save_manual_screenshot_ = false;
        if (*capture) {
            const PngEncodeResult png = encode_png_rgba(
                capture->rgba, capture->width, capture->height);
            if (png) {
                const auto ticks = static_cast<std::uint64_t>(
                    std::chrono::system_clock::now().time_since_epoch().count() / 1'000'000ULL);
                const std::filesystem::path out_file =
                    default_workspace_root() / "screenshots" /
                    ("veg_" + recipe_.asset_slug + "_" + std::to_string(ticks) + ".png");
                std::string err;
                if (write_binary_file_local(out_file, png.bytes, err)) {
                    set_status("Saved screenshot: " + path_to_utf8(out_file), EditorTone::success);
                } else {
                    set_status("Screenshot write failed: " + err, EditorTone::danger);
                }
            }
        }
    }
}

void VegetationStudioApplication::push_mcp_response(VegMcpResponse response) {
    if (!mcp_control_.responses.try_push(std::move(response))) {
        std::cerr << "Vegetation Studio MCP response queue full; dropping response.\n";
    }
    mcp_control_.notify_response_available();
}

nlohmann::json VegetationStudioApplication::build_state_json() const {
    const VegetationOptimizationReport& rep = current_asset_.report;
    Json export_json = nullptr;
    if (last_export_.has_value()) {
        export_json = Json{
            {"asset_root", path_to_utf8(last_export_->asset_root)},
            {"package_directory", path_to_utf8(last_export_->package_directory)},
            {"recipe_relative_path", last_export_->recipe_relative_path},
            {"lod0_relative_path", last_export_->lod0_relative_path},
        };
    }
    return Json{
        {"recipe", serialize_vegetation_recipe_json(recipe_)},
        {"bounds", Json{
            {"center", Json::array({current_asset_.bounds.center.x, current_asset_.bounds.center.y, current_asset_.bounds.center.z})},
            {"extent", Json::array({current_asset_.bounds.extent.x, current_asset_.bounds.extent.y, current_asset_.bounds.extent.z})},
        }},
        {"report", Json{
            {"vertices", rep.vertices},
            {"triangles", rep.triangles},
            {"submeshes", rep.submeshes},
            {"geometric_errors", rep.geometric_errors},
            {"float_vertex_bytes_total", rep.float_vertex_bytes_total},
            {"packed_vertex_bytes_total", rep.packed_vertex_bytes_total},
            {"texture_rgba_bytes_total", rep.texture_rgba_bytes_total},
            {"alpha_coverage_ratio", rep.alpha_coverage_ratio},
            {"estimated_overdraw_factor", rep.estimated_overdraw_factor},
            {"has_foliage_submesh", rep.has_foliage_submesh},
            {"uses_gpu_grass", rep.uses_gpu_grass},
            {"budget_grade", rep.budget_grade},
            {"budget_notes", rep.budget_notes},
        }},
        {"arena", Json{
            {"mode", arena_mode_token(arena_mode_)},
            {"spacing", arena_spacing_},
            {"submitted_instances", arena_models_.size()},
            {"target_instances", arena_target_instance_count(arena_mode_)},
        }},
        {"camera", Json{
            {"position", Json::array({camera_.position().x, camera_.position().y, camera_.position().z})},
            {"forward", Json::array({camera_.forward().x, camera_.forward().y, camera_.forward().z})},
            {"fov_degrees", renderer_.vertical_fov_degrees()},
        }},
        {"lighting", Json{
            {"sun_hour", sun_hour_},
            {"voxel_gi", voxel_gi_enabled_},
        }},
        {"paint_brush", Json{
            {"enabled", paint_brush_enabled_},
            {"mode", to_token(brush_mode_)},
            {"radius", brush_radius_},
            {"strength", brush_strength_},
            {"falloff", brush_falloff_},
            {"stroke_count", recipe_.paint_strokes.size()},
        }},
        {"target_project_dir", path_to_utf8(target_project_dir_)},
        {"last_export", std::move(export_json)},
        {"metrics", build_metrics_json()},
    };
}

nlohmann::json VegetationStudioApplication::build_metrics_json() const {
    const PerformanceSnapshot snap = renderer_.metrics_snapshot(true);
    const VisibilityStatistics& vis = renderer_.visibility_statistics();
    return Json{
        {"frames_rendered", snap.frames_rendered},
        {"instantaneous_fps", snap.instantaneous_fps},
        {"one_second_average_fps", snap.one_second_average_fps},
        {"one_percent_low_fps", snap.one_percent_low_fps},
        {"cpu_frame_ms", snap.latest.cpu_frame_ms},
        {"gpu_frame_ms", snap.latest.gpu_frame_ms},
        {"gpu_visibility_ms", snap.latest.gpu_visibility_ms},
        {"gpu_subsurface_ms", snap.latest.gpu_subsurface_ms},
        {"present_mode", Swapchain::present_mode_name(renderer_.swapchain().present_mode())},
        {"visibility_backend", renderer_.visibility_backend()},
        {"gpu_visibility_active", renderer_.gpu_visibility_active()},
        {"model_vertex_format", to_token(renderer_.model_vertex_format_actual())},
        {"visibility", Json{
            {"submitted", vis.submitted},
            {"visible", vis.visible},
            {"frustum_rejected", vis.frustum_rejected},
            {"occlusion_rejected", vis.occlusion_rejected},
            {"lod_instances", vis.lod_instances},
            {"triangles", vis.triangles},
        }},
        {"gpu_resources", Json{
            {"resident_models", renderer_.resident_editor_model_count()},
            {"resident_textures", renderer_.resident_editor_texture_count()},
            {"model_gpu_bytes", renderer_.editor_model_gpu_bytes()},
            {"texture_gpu_bytes", renderer_.editor_texture_gpu_bytes()},
            {"gpu_grass_instances", renderer_.gpu_grass_instances()},
            {"gpu_grass_wind_seconds", renderer_.gpu_grass_wind_seconds()},
        }},
    };
}

void VegetationStudioApplication::build_ui() {
    const ImGuiViewport* const main_vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(main_vp->WorkPos);
    ImGui::SetNextWindowSize(main_vp->WorkSize);
    ImGui::SetNextWindowViewport(main_vp->ID);

    constexpr ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::Begin("VegetationStudioHost", nullptr, host_flags);
    ImGui::PopStyleVar(2);

    const ImGuiID dockspace_id = ImGui::GetID("VegetationStudioDockspace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0F, 0.0F), ImGuiDockNodeFlags_PassthruCentralNode);

    if (!dock_layout_initialized_) {
        ImGuiDockNode* const root_node = ImGui::DockBuilderGetNode(dockspace_id);
        if (root_node != nullptr) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, main_vp->WorkSize);

            ImGuiID center = dockspace_id;
            ImGuiID top = 0U;
            ImGuiID left = 0U;
            ImGuiID right = 0U;
            ImGuiID bottom = 0U;
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.085F, &top, &center);
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.25F, &left, &center);
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30F, &right, &center);
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.26F, &bottom, &center);

            if (ImGuiDockNode* const top_node = ImGui::DockBuilderGetNode(top);
                top_node != nullptr) {
                top_node->LocalFlags |=
                    ImGuiDockNodeFlags_NoTabBar |
                    ImGuiDockNodeFlags_NoWindowMenuButton |
                    ImGuiDockNodeFlags_NoCloseButton;
            }
            ImGui::DockBuilderDockWindow("Studio Bar", top);
            ImGui::DockBuilderDockWindow("Archetype & Growth", left);
            ImGui::DockBuilderDockWindow("Live 3D Viewport", center);
            ImGui::DockBuilderDockWindow("Foliage & Engine Bridge", right);
            ImGui::DockBuilderDockWindow("LOD & Perf Lab", bottom);
            ImGui::DockBuilderFinish(dockspace_id);
            dock_layout_initialized_ = true;
        }
    }
    ImGui::End();

    draw_top_bar();
    draw_left_panel();
    draw_viewport_panel();
    draw_right_panel();
    draw_bottom_panel();
}

void VegetationStudioApplication::draw_top_bar() {
    ImGui::Begin("Studio Bar", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ui_kit_.brand_lockup();
    ImGui::SameLine(0.0F, 10.0F);

    ImGui::SetNextItemWidth(156.0F);
    if (ImGui::BeginCombo("##PresetSelect", recipe_.display_name.c_str())) {
        for (const VegetationPresetInfo& preset : vegetation_presets()) {
            const bool selected = (preset.id == recipe_.asset_slug);
            std::array<char, 160U> item_buf{};
            std::snprintf(
                item_buf.data(),
                item_buf.size(),
                "[%s] %s",
                preset.category.c_str(),
                preset.label.c_str());
            if (ImGui::Selectable(item_buf.data(), selected)) {
                recipe_ = preset.recipe;
                preview_dirty_ = true;
                frame_camera_to_arena();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(0.0F, 6.0F);
    if (ui_kit_.action_button("SeedBtn", EditorIcon::refresh, "Seed", EditorButtonStyle::secondary)) {
        recipe_.seed = recipe_.seed * 1664525U + 1013904223U;
        if (recipe_.seed == 0U) {
            recipe_.seed = 1337U;
        }
        preview_dirty_ = true;
    }

    ImGui::SameLine(0.0F, 6.0F);
    constexpr std::array<VegetationFoliageMode, 3U> kFoliageModes{
        VegetationFoliageMode::full,
        VegetationFoliageMode::sparse,
        VegetationFoliageMode::bare,
    };
    constexpr std::array<const char*, 3U> kFoliageLabels{"Full", "Sparse", "Bare"};
    for (std::size_t i = 0U; i < kFoliageModes.size(); ++i) {
        if (i > 0U) {
            ImGui::SameLine(0.0F, 3.0F);
        }
        const bool active = (recipe_.foliage_mode == kFoliageModes[i]);
        std::array<char, 24U> id_buf{};
        std::snprintf(id_buf.data(), id_buf.size(), "##Fol%zu", i);
        if (ui_kit_.action_button(
                id_buf.data(),
                EditorIcon::environment,
                kFoliageLabels[i],
                EditorButtonStyle::quiet,
                true,
                active)) {
            if (!active) {
                recipe_.foliage_mode = kFoliageModes[i];
                preview_dirty_ = true;
            }
        }
    }

    ImGui::SameLine(0.0F, 6.0F);
    constexpr std::array<VegArenaMode, 5U> kArenas{
        VegArenaMode::hero_single,
        VegArenaMode::lod_lineup,
        VegArenaMode::grove_100,
        VegArenaMode::forest_2500,
        VegArenaMode::meadow_10000,
    };
    constexpr std::array<const char*, 5U> kArenaShortLabels{
        "Hero",
        "LOD x4",
        "Grove",
        "2.5K",
        "10K",
    };
    for (std::size_t i = 0U; i < kArenas.size(); ++i) {
        if (i > 0U) {
            ImGui::SameLine(0.0F, 3.0F);
        }
        const bool active = (arena_mode_ == kArenas[i]);
        std::array<char, 24U> id_buf{};
        std::snprintf(id_buf.data(), id_buf.size(), "##Ar%zu", i);
        if (ui_kit_.action_button(
                id_buf.data(),
                EditorIcon::cube,
                kArenaShortLabels[i],
                EditorButtonStyle::quiet,
                true,
                active)) {
            arena_mode_ = kArenas[i];
            arena_scene_dirty_ = true;
            frame_camera_to_arena();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", arena_mode_label(kArenas[i]).data());
        }
    }

    ImGui::SameLine(0.0F, 6.0F);
    if (ui_kit_.action_button("FrameCam", EditorIcon::camera, "Frame", EditorButtonStyle::secondary)) {
        frame_camera_to_arena();
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ui_kit_.action_button("ExportBtn", EditorIcon::save, "Export", EditorButtonStyle::primary)) {
        std::string export_err;
        if (target_project_dir_.empty()) {
            set_status("Select a target project before exporting.", EditorTone::warning);
        } else {
            const std::filesystem::path asset_root =
                resolve_project_assets_root(target_project_dir_);
            VegetationExportSummary summary{};
            if (export_vegetation_package(
                    current_asset_,
                    asset_root,
                    category_subdir_buf_.data(),
                    summary,
                    export_err)) {
                last_export_ = summary;
                select_project_directory(target_project_dir_);
                set_status(
                    "Exported cooked assets -> " + summary.lod0_relative_path,
                    EditorTone::success);
            } else {
                set_status("Export failed: " + export_err, EditorTone::danger);
            }
        }
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ui_kit_.action_button("SnapBtn", EditorIcon::eye, "PNG", EditorButtonStyle::secondary)) {
        if (renderer_.request_screenshot()) {
            save_manual_screenshot_ = true;
        }
    }

    std::array<char, 96U> grade_buf{};
    const double bar_fps = ui_metrics_.one_second_average_fps > 0.0
        ? ui_metrics_.one_second_average_fps
        : ui_metrics_.instantaneous_fps;
    std::snprintf(
        grade_buf.data(),
        grade_buf.size(),
        "Grade %s | %.0f FPS",
        current_asset_.report.budget_grade.c_str(),
        bar_fps);
    ImGui::SameLine(0.0F, 8.0F);
    const float badge_w = ImGui::CalcTextSize(grade_buf.data()).x + 15.0F;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    if (avail_w > badge_w) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_w - badge_w);
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0F);
    ui_kit_.badge(grade_buf.data(), tone_for_budget_grade(current_asset_.report.budget_grade));

    ImGui::End();
}

void VegetationStudioApplication::draw_left_panel() {
    ImGui::Begin("Archetype & Growth");
    ui_kit_.panel_header(
        EditorIcon::model,
        "Procedural DNA",
        "Morphology, branching, foliage seasonality & LOD ratios.");

    bool changed = false;

    ui_kit_.section_header(EditorIcon::folder, "Identity & Archetype");
    if (ImGui::InputText(
            "Slug",
            slug_buf_.data(),
            slug_buf_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue)) {
        recipe_.asset_slug = sanitize_vegetation_slug(slug_buf_.data());
        changed = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string cleaned = sanitize_vegetation_slug(slug_buf_.data());
        if (cleaned != recipe_.asset_slug) {
            recipe_.asset_slug = cleaned;
            changed = true;
        }
    }
    if (ImGui::InputText(
            "Name",
            display_name_buf_.data(),
            display_name_buf_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        if (recipe_.display_name != display_name_buf_.data()) {
            recipe_.display_name = display_name_buf_.data();
            changed = true;
        }
    }

    constexpr std::array<VegetationArchetype, 6U> kArchetypes{
        VegetationArchetype::grass_patch_gpu,
        VegetationArchetype::grass_tuft,
        VegetationArchetype::bush,
        VegetationArchetype::broadleaf_tree,
        VegetationArchetype::conifer_tree,
        VegetationArchetype::palm_tree,
    };
    if (ImGui::BeginCombo("Archetype", to_token(recipe_.archetype).data())) {
        for (const VegetationArchetype a : kArchetypes) {
            const bool selected = (a == recipe_.archetype);
            if (ImGui::Selectable(to_token(a).data(), selected) && !selected) {
                recipe_.archetype = a;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    int seed_int = static_cast<int>(recipe_.seed & 0x7FFFFFFFU);
    if (ImGui::InputInt("Seed", &seed_int)) {
        recipe_.seed = static_cast<std::uint32_t>((std::max)(1, seed_int));
        changed = true;
    }

    ui_kit_.section_header(EditorIcon::terrain, "Trunk & Structure");
    changed |= ImGui::SliderFloat("Height (m)", &recipe_.height_meters, 0.15F, 24.0F, "%.2f");
    changed |= ImGui::SliderFloat("Canopy Radius", &recipe_.canopy_radius_meters, 0.15F, 12.0F, "%.2f");

    const bool has_trunk =
        recipe_.archetype == VegetationArchetype::broadleaf_tree ||
        recipe_.archetype == VegetationArchetype::conifer_tree ||
        recipe_.archetype == VegetationArchetype::palm_tree ||
        recipe_.archetype == VegetationArchetype::bush;
    if (has_trunk) {
        changed |= ImGui::SliderFloat("Trunk Radius", &recipe_.trunk_radius_meters, 0.02F, 1.5F, "%.3f");
        changed |= ImGui::SliderFloat("Trunk Lean", &recipe_.trunk_lean, 0.0F, 1.0F, "%.2f");
        changed |= ImGui::SliderFloat("Trunk Curve", &recipe_.trunk_curve, 0.0F, 1.0F, "%.2f");
        changed |= ImGui::SliderFloat("Root Flare", &recipe_.trunk_flare, 0.0F, 1.0F, "%.2f");
        changed |= ImGui::SliderFloat("Bark Roughness", &recipe_.bark_roughness, 0.25F, 1.0F, "%.2f");
        changed |= ImGui::ColorEdit3("Bark Color", &recipe_.bark_color_srgb.x);
    }

    if (recipe_.archetype == VegetationArchetype::broadleaf_tree ||
        recipe_.archetype == VegetationArchetype::bush) {
        int prim = static_cast<int>(recipe_.primary_branches);
        int lvls = static_cast<int>(recipe_.branch_levels);
        if (ImGui::SliderInt("Primary Branches", &prim, 2, 16)) {
            recipe_.primary_branches = static_cast<std::uint32_t>(prim);
            changed = true;
        }
        if (ImGui::SliderInt("Branch Levels", &lvls, 1, 4)) {
            recipe_.branch_levels = static_cast<std::uint32_t>(lvls);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Branch Spread", &recipe_.branch_spread, 0.15F, 1.4F, "%.2f");
        changed |= ImGui::SliderFloat("Branch Droop", &recipe_.branch_droop, -0.4F, 0.9F, "%.2f");
    } else if (recipe_.archetype == VegetationArchetype::conifer_tree) {
        int whorls = static_cast<int>(recipe_.whorl_count);
        if (ImGui::SliderInt("Conifer Whorls", &whorls, 3, 18)) {
            recipe_.whorl_count = static_cast<std::uint32_t>(whorls);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Branch Droop", &recipe_.branch_droop, -0.2F, 0.9F, "%.2f");
    } else if (recipe_.archetype == VegetationArchetype::palm_tree) {
        int fronds = static_cast<int>(recipe_.frond_count);
        if (ImGui::SliderInt("Palm Fronds", &fronds, 5, 44)) {
            recipe_.frond_count = static_cast<std::uint32_t>(fronds);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Frond Arch", &recipe_.frond_arch, 0.05F, 1.2F, "%.2f");
        changed |= ImGui::SliderFloat("Dry Skirt", &recipe_.dry_skirt_ratio, 0.0F, 0.65F, "%.2f");
    } else {
        int blades = static_cast<int>(recipe_.grass_blade_count);
        if (ImGui::SliderInt("Blade / Clump Count", &blades, 16, 4096)) {
            recipe_.grass_blade_count = static_cast<std::uint32_t>(blades);
            changed = true;
        }
    }

    ui_kit_.section_header(EditorIcon::material, "Foliage & Seasonality");
    changed |= ImGui::SliderFloat("Foliage Density", &recipe_.foliage_density, 0.2F, 2.0F, "%.2f");
    changed |= ImGui::SliderFloat("Cluster Scale", &recipe_.cluster_scale, 0.40F, 2.0F, "%.2f");
    changed |= ImGui::SliderFloat("Alpha Cutoff", &recipe_.alpha_cutoff, 0.10F, 0.90F, "%.2f");
    changed |= ImGui::ColorEdit3("Leaf Base", &recipe_.leaf_base_srgb.x);
    changed |= ImGui::ColorEdit3("Leaf Tip", &recipe_.leaf_tip_srgb.x);
    changed |= ImGui::ColorEdit3("Leaf Autumn", &recipe_.leaf_autumn_srgb.x);
    changed |= ImGui::ColorEdit3("Leaf Dry", &recipe_.leaf_dry_srgb.x);
    changed |= ImGui::SliderFloat("Autumn Mix", &recipe_.autumn_amount, 0.0F, 1.0F, "%.2f");
    changed |= ImGui::SliderFloat("Dryness Mix", &recipe_.dryness_amount, 0.0F, 1.0F, "%.2f");
    changed |= ImGui::SliderFloat("Color Variation", &recipe_.color_variation, 0.0F, 1.0F, "%.2f");
    changed |= ImGui::SliderFloat("Crown AO", &recipe_.crown_ao, 0.0F, 1.0F, "%.2f");
    changed |= ImGui::SliderFloat("Subsurface SSS", &recipe_.subsurface_strength, 0.0F, 1.0F, "%.2f");

    constexpr std::array<std::uint32_t, 4U> kAtlasResOptions{256U, 512U, 1024U, 2048U};
    const std::string atlas_label = std::to_string(recipe_.atlas_resolution) + " px";
    if (ImGui::BeginCombo("Atlas Res", atlas_label.c_str())) {
        for (const std::uint32_t res_opt : kAtlasResOptions) {
            const bool selected = (recipe_.atlas_resolution == res_opt);
            const std::string opt_str = std::to_string(res_opt) + " px";
            if (ImGui::Selectable(opt_str.c_str(), selected) && !selected) {
                recipe_.atlas_resolution = res_opt;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    constexpr std::array<std::uint32_t, 4U> kBarkResOptions{128U, 256U, 512U, 1024U};
    const std::string bark_res_label = std::to_string(recipe_.bark_resolution) + " px";
    if (ImGui::BeginCombo("Bark Res", bark_res_label.c_str())) {
        for (const std::uint32_t res_opt : kBarkResOptions) {
            const bool selected = (recipe_.bark_resolution == res_opt);
            const std::string opt_str = std::to_string(res_opt) + " px";
            if (ImGui::Selectable(opt_str.c_str(), selected) && !selected) {
                recipe_.bark_resolution = res_opt;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ui_kit_.section_header(EditorIcon::renderer, "Cooked LOD & Collision");
    constexpr std::array<VegetationCollisionKind, 3U> kCollisions{
        VegetationCollisionKind::none,
        VegetationCollisionKind::trunk_cylinder,
        VegetationCollisionKind::bounds_box,
    };
    if (ImGui::BeginCombo("Collision", to_token(recipe_.collision_kind).data())) {
        for (const VegetationCollisionKind ck : kCollisions) {
            const bool selected = (ck == recipe_.collision_kind);
            if (ImGui::Selectable(to_token(ck).data(), selected) && !selected) {
                recipe_.collision_kind = ck;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed |= ImGui::SliderFloat("LOD1 Keep", &recipe_.lod_keep_ratios[0U], 0.15F, 0.85F, "%.2f");
    changed |= ImGui::SliderFloat("LOD2 Keep", &recipe_.lod_keep_ratios[1U], 0.08F, 0.60F, "%.2f");
    changed |= ImGui::SliderFloat("LOD3 Keep", &recipe_.lod_keep_ratios[2U], 0.03F, 0.35F, "%.2f");

    if (changed) {
        preview_dirty_ = true;
    }
    ImGui::End();
}

void VegetationStudioApplication::draw_viewport_panel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::Begin("Live 3D Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const std::uint32_t vp_w = std::max(64U, static_cast<std::uint32_t>(avail.x));
    const std::uint32_t vp_h = std::max(64U, static_cast<std::uint32_t>(avail.y));
    const VkExtent2D current_ext = renderer_.editor_viewport_extent();
    if (vp_w != current_ext.width || vp_h != current_ext.height) {
        desired_viewport_width_ = vp_w;
        desired_viewport_height_ = vp_h;
        last_viewport_resize_time_ = std::chrono::steady_clock::now();
        viewport_resize_pending_ = true;
    }

    const bool vp_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool vp_focused = ImGui::IsWindowFocused();

    const ImVec2 img_min = ImGui::GetCursorScreenPos();
    const ImVec2 img_max(img_min.x + static_cast<float>(vp_w), img_min.y + static_cast<float>(vp_h));

    if (viewport_texture_ != VK_NULL_HANDLE) {
        ImGui::Image(
            imgui_tex_id(viewport_texture_),
            ImVec2(static_cast<float>(vp_w), static_cast<float>(vp_h)),
            ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F));
    } else {
        ImGui::Dummy(ImVec2(static_cast<float>(vp_w), static_cast<float>(vp_h)));
    }

    const float frame_dt = ImGui::GetIO().DeltaTime;
    update_camera_controls(frame_dt, vp_hovered, vp_focused);
    handle_viewport_painting(img_min, img_max, vp_hovered);

    std::array<char, 160U> hud_buf{};
    std::snprintf(
        hud_buf.data(),
        hud_buf.size(),
        "%s | %s | %s",
        recipe_.display_name.c_str(),
        arena_mode_label(arena_mode_).data(),
        paint_brush_enabled_ ? "BRUSH ON (LMB, Esc)" : "RMB Look, WASD, [F] Frame");
    ui_kit_.viewport_badge(
        ImVec2(img_min.x + 12.0F, img_min.y + 10.0F),
        hud_buf.data(),
        paint_brush_enabled_ ? EditorTone::warning : EditorTone::info);

    const double vp_fps = ui_metrics_.one_second_average_fps > 0.0
        ? ui_metrics_.one_second_average_fps
        : ui_metrics_.instantaneous_fps;
    const double cpu_ms = ui_metrics_.latest.cpu_frame_ms >= 0.0
        ? ui_metrics_.latest.cpu_frame_ms
        : 0.0;
    const double gpu_ms = ui_metrics_.latest.gpu_frame_ms;
    const char* const present_name =
        Swapchain::present_mode_name(renderer_.swapchain().present_mode());
    std::array<char, 160U> perf_hud{};
    if (gpu_ms >= 0.0) {
        std::snprintf(
            perf_hud.data(),
            perf_hud.size(),
            "%.0f FPS (1%% %.0f) | CPU %.2f ms | GPU %.2f ms | %s",
            vp_fps,
            ui_metrics_.one_percent_low_fps,
            cpu_ms,
            gpu_ms,
            present_name);
    } else {
        std::snprintf(
            perf_hud.data(),
            perf_hud.size(),
            "%.0f FPS (1%% %.0f) | CPU %.2f ms | GPU -- | %s",
            vp_fps,
            ui_metrics_.one_percent_low_fps,
            cpu_ms,
            present_name);
    }
    const float left_w = ImGui::CalcTextSize(hud_buf.data()).x + 29.0F;
    const float right_w = ImGui::CalcTextSize(perf_hud.data()).x + 29.0F;
    const float right_x = img_max.x - right_w - 12.0F;
    const ImVec2 perf_pos = (right_x >= img_min.x + 12.0F + left_w + 10.0F)
        ? ImVec2(right_x, img_min.y + 10.0F)
        : ImVec2(img_min.x + 12.0F, img_min.y + 38.0F);
    const EditorTone perf_tone = (ui_metrics_.timed_frames == 0U)
        ? EditorTone::neutral
        : ((vp_fps >= 60.0) ? EditorTone::success : EditorTone::warning);
    ui_kit_.viewport_badge(perf_pos, perf_hud.data(), perf_tone);

    if (arena_mode_ == VegArenaMode::lod_lineup) {
        const Bounds& b = current_asset_.bounds;
        const float w = std::max({b.extent.x * 2.0F, b.extent.z * 2.0F, 1.0F});
        const float step = std::max(arena_spacing_, w * 1.15F + 0.8F);
        const float top_y = b.center.y + b.extent.y + 0.35F;
        for (std::uint32_t i = 0U; i < 4U; ++i) {
            const float x = (static_cast<float>(i) - 1.5F) * step;
            if (const auto sp = project_world_to_screen(
                    camera_,
                    renderer_.vertical_fov_degrees(),
                    glm::vec3(x, top_y, 0.0F),
                    img_min,
                    img_max)) {
                std::array<char, 48U> lod_buf{};
                std::snprintf(
                    lod_buf.data(),
                    lod_buf.size(),
                    "LOD%u: %u tris",
                    i,
                    current_asset_.report.triangles[i]);
                ui_kit_.viewport_badge(
                    ImVec2(sp->x - 44.0F, sp->y - 12.0F),
                    lod_buf.data(),
                    i == 0U ? EditorTone::success : EditorTone::neutral);
            }
        }
    }

    ImGui::End();
}

void VegetationStudioApplication::draw_right_panel() {
    ImGui::Begin("Foliage & Engine Bridge");
    ui_kit_.panel_header(
        EditorIcon::material,
        "Foliage Paint & Bridge",
        "Interactive 3D crown painting, procedural textures & cooked export.");

    ui_kit_.section_header(EditorIcon::cube, "3D Crown Foliage Brush");
    const bool brush_hero_only = (arena_mode_ == VegArenaMode::hero_single);
    if (!brush_hero_only) {
        ui_kit_.status_text("Switch arena to Hero x1 to paint directly in 3D.", EditorTone::warning);
    }
    if (ui_kit_.action_button(
            "ToggleBrushBtn",
            EditorIcon::cube,
            paint_brush_enabled_ ? "Brush Armed (LMB)" : "Enable 3D Crown Brush",
            paint_brush_enabled_ ? EditorButtonStyle::primary : EditorButtonStyle::secondary,
            brush_hero_only,
            paint_brush_enabled_)) {
        paint_brush_enabled_ = !paint_brush_enabled_;
    }

    constexpr std::array<VegetationPaintMode, 4U> kPaintModes{
        VegetationPaintMode::tint,
        VegetationPaintMode::autumn,
        VegetationPaintMode::dry,
        VegetationPaintMode::prune_bare,
    };
    if (ImGui::BeginCombo("Brush Mode", to_token(brush_mode_).data())) {
        for (const VegetationPaintMode pm : kPaintModes) {
            const bool selected = (pm == brush_mode_);
            if (ImGui::Selectable(to_token(pm).data(), selected)) {
                brush_mode_ = pm;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SliderFloat("Brush Radius (m)", &brush_radius_, 0.10F, 5.0F, "%.2f");
    ImGui::SliderFloat("Brush Strength", &brush_strength_, 0.10F, 1.0F, "%.2f");
    ImGui::SliderFloat("Brush Falloff", &brush_falloff_, 0.0F, 1.0F, "%.2f");
    if (brush_mode_ == VegetationPaintMode::tint) {
        ImGui::ColorEdit3("Brush Tint", &brush_color_srgb_.x);
    }

    std::array<char, 48U> stroke_buf{};
    std::snprintf(
        stroke_buf.data(),
        stroke_buf.size(),
        "Strokes: %zu / %zu",
        recipe_.paint_strokes.size(),
        maximum_vegetation_paint_strokes);
    ImGui::TextDisabled("%s", stroke_buf.data());
    ImGui::SameLine(0.0F, 8.0F);
    if (ImGui::SmallButton("Undo Stroke") && !recipe_.paint_strokes.empty()) {
        recipe_.paint_strokes.pop_back();
        preview_dirty_ = true;
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ImGui::SmallButton("Clear Paint") && !recipe_.paint_strokes.empty()) {
        recipe_.paint_strokes.clear();
        preview_dirty_ = true;
    }

    ui_kit_.section_header(EditorIcon::eye, "Procedural Atlases");
    if (foliage_preview_.valid()) {
        const char* foliage_kind =
            (recipe_.archetype == VegetationArchetype::palm_tree)
                ? "Palm Atlas 1x8"
                : ((recipe_.archetype == VegetationArchetype::grass_patch_gpu)
                    ? "Grass Blade PBR"
                    : "Sprig Atlas 4x4");
        const std::uint32_t tex_w = current_asset_.has_grass_textures
            ? current_asset_.grass_albedo.width
            : current_asset_.foliage_atlas.width;
        const std::uint32_t tex_h = current_asset_.has_grass_textures
            ? current_asset_.grass_albedo.height
            : current_asset_.foliage_atlas.height;
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s (%ux%u)", foliage_kind, tex_w, tex_h);
        ImGui::Image(foliage_preview_.imgui_id(), ImVec2(112.0F, 112.0F));
        ImGui::EndGroup();
        ImGui::SameLine(0.0F, 12.0F);
    }
    if (bark_preview_.valid()) {
        ImGui::BeginGroup();
        ImGui::TextDisabled(
            "Bark Albedo (%ux%u)",
            current_asset_.bark_albedo.width,
            current_asset_.bark_albedo.height);
        ImGui::Image(bark_preview_.imgui_id(), ImVec2(112.0F, 112.0F));
        ImGui::EndGroup();
    }
    if (!foliage_preview_.valid() && !bark_preview_.valid()) {
        ImGui::TextDisabled("Bare / single-shader preset has no texture preview.");
    }

    ui_kit_.section_header(EditorIcon::folder, "Engine Project Bridge");
    if (!registered_projects_.empty()) {
        const std::string_view full_proj_sv(target_project_buf_.data());
        std::string_view trimmed_proj = full_proj_sv;
        while (!trimmed_proj.empty() &&
               (trimmed_proj.back() == '/' || trimmed_proj.back() == '\\')) {
            trimmed_proj.remove_suffix(1U);
        }
        const std::size_t sep = trimmed_proj.find_last_of("/\\");
        const std::string_view leaf_sv =
            sep == std::string_view::npos ? trimmed_proj : trimmed_proj.substr(sep + 1U);
        std::array<char, 128U> proj_label_buf{};
        if (leaf_sv.empty()) {
            copy_to_buffer(proj_label_buf, "Select Registered Project...");
        } else {
            copy_to_buffer(proj_label_buf, leaf_sv);
        }
        if (ImGui::BeginCombo("Project", proj_label_buf.data())) {
            for (const ProjectRegistryEntry& entry : registered_projects_) {
                const bool sel = (entry.project_directory == target_project_dir_);
                std::array<char, 160U> pbuf{};
                std::snprintf(
                    pbuf.data(),
                    pbuf.size(),
                    "%s (%s)",
                    entry.name.c_str(),
                    entry.template_id.c_str());
                if (ImGui::Selectable(pbuf.data(), sel)) {
                    select_project_directory(entry.project_directory);
                }
            }
            ImGui::EndCombo();
        }
    }

    if (ImGui::InputText(
            "Target Root",
            target_project_buf_.data(),
            target_project_buf_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        select_project_directory(path_from_utf8(target_project_buf_.data()));
    }
    ImGui::InputText(
        "Category Subdir",
        category_subdir_buf_.data(),
        category_subdir_buf_.size());

    if (ui_kit_.action_button("ScanProjBtn", EditorIcon::refresh, "Rescan Project", EditorButtonStyle::secondary)) {
        reload_projects_and_scan();
    }
    ImGui::SameLine(0.0F, 6.0F);
    if (ui_kit_.action_button("ExportBridgeBtn", EditorIcon::import_asset, "Cook & Export", EditorButtonStyle::primary)) {
        std::string export_err;
        if (target_project_dir_.empty()) {
            set_status("Target project directory is empty.", EditorTone::warning);
        } else {
            VegetationExportSummary summary{};
            if (export_vegetation_package(
                    current_asset_,
                    resolve_project_assets_root(target_project_dir_),
                    category_subdir_buf_.data(),
                    summary,
                    export_err)) {
                last_export_ = summary;
                select_project_directory(target_project_dir_);
                set_status(
                    "Exported package -> " + summary.lod0_relative_path,
                    EditorTone::success);
            } else {
                set_status("Export failed: " + export_err, EditorTone::danger);
            }
        }
    }

    if (!project_vegetation_.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Project Vegetation Packages (%zu)", project_vegetation_.size());
        if (ImGui::BeginChild("ProjectVegList", ImVec2(0.0F, 130.0F), ImGuiChildFlags_Borders)) {
            for (std::size_t i = 0U; i < project_vegetation_.size(); ++i) {
                const ProjectVegetationEntry& e = project_vegetation_[i];
                const bool active = (e.asset_slug == recipe_.asset_slug);
                const EditorAssetRowResult row = ui_kit_.asset_row(
                    e.recipe_relative_path.c_str(),
                    EditorIcon::model,
                    e.display_name.c_str(),
                    to_token(e.archetype).data(),
                    active);
                if (row.activated || row.double_clicked) {
                    VegetationRecipe loaded{};
                    std::string load_err;
                    if (open_vegetation_package(e.recipe_absolute_path, loaded, load_err)) {
                        recipe_ = sanitize_vegetation_recipe(std::move(loaded));
                        rebuild_vegetation_assets(true);
                        update_arena_scene_if_dirty();
                    } else {
                        set_status("Load failed: " + load_err, EditorTone::danger);
                    }
                }
            }
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

void VegetationStudioApplication::draw_bottom_panel() {
    ImGui::Begin("LOD & Perf Lab");
    const VegetationOptimizationReport& rep = current_asset_.report;

    if (ImGui::BeginTable("VegMainPerfSplit", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("LODTableCol", ImGuiTableColumnFlags_WidthStretch, 0.52F);
        ImGui::TableSetupColumn("TelemetryCol", ImGuiTableColumnFlags_WidthStretch, 0.48F);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginTable(
                "LodBreakdownTable",
                5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("LOD");
            ImGui::TableSetupColumn("Vertices");
            ImGui::TableSetupColumn("Triangles");
            ImGui::TableSetupColumn("Submeshes");
            ImGui::TableSetupColumn("Geom Error");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0U; i < 4U; ++i) {
                ImGui::TableNextRow();
                std::array<char, 24U> buf{};
                ImGui::TableSetColumnIndex(0);
                std::snprintf(buf.data(), buf.size(), "LOD%zu", i);
                ImGui::TextUnformatted(buf.data());

                ImGui::TableSetColumnIndex(1);
                std::snprintf(buf.data(), buf.size(), "%u", rep.vertices[i]);
                ImGui::TextUnformatted(buf.data());

                ImGui::TableSetColumnIndex(2);
                std::snprintf(buf.data(), buf.size(), "%u", rep.triangles[i]);
                ImGui::TextUnformatted(buf.data());

                ImGui::TableSetColumnIndex(3);
                std::snprintf(buf.data(), buf.size(), "%u", rep.submeshes[i]);
                ImGui::TextUnformatted(buf.data());

                ImGui::TableSetColumnIndex(4);
                std::snprintf(buf.data(), buf.size(), "%.3f", rep.geometric_errors[i]);
                ImGui::TextUnformatted(buf.data());
            }
            ImGui::EndTable();
        }
        std::array<char, 192U> budget_buf{};
        std::snprintf(
            budget_buf.data(),
            budget_buf.size(),
            "Packed: %.1f KB (float %.1f KB) | Tex: %.1f MB | Overdraw: %.2fx",
            static_cast<double>(rep.packed_vertex_bytes_total) / 1024.0,
            static_cast<double>(rep.float_vertex_bytes_total) / 1024.0,
            static_cast<double>(rep.texture_rgba_bytes_total) / (1024.0 * 1024.0),
            rep.estimated_overdraw_factor);
        ImGui::PushStyleColor(ImGuiCol_Text, EditorUiKit::muted_text_color());
        ImGui::TextWrapped("%s", budget_buf.data());
        ImGui::PopStyleColor();
        if (!rep.budget_notes.empty()) {
            ImGui::TextWrapped("%s", rep.budget_notes.c_str());
        }

        ImGui::TableSetColumnIndex(1);
        const VisibilityStatistics& vis = renderer_.visibility_statistics();
        const double card_fps = ui_metrics_.one_second_average_fps > 0.0
            ? ui_metrics_.one_second_average_fps
            : ui_metrics_.instantaneous_fps;
        const double cpu_ms = ui_metrics_.latest.cpu_frame_ms >= 0.0
            ? ui_metrics_.latest.cpu_frame_ms
            : 0.0;
        const std::uint64_t scene_tris = vis.triangles > 0U
            ? vis.triangles
            : static_cast<std::uint64_t>(vis.visible) * rep.triangles[0U];

        std::array<char, 64U> m_fps{};
        std::array<char, 64U> m_frame{};
        std::array<char, 64U> m_vis{};
        std::array<char, 64U> m_mem{};
        std::snprintf(
            m_fps.data(),
            m_fps.size(),
            "%.0f FPS (1%% %.0f)",
            card_fps,
            ui_metrics_.one_percent_low_fps);
        if (ui_metrics_.latest.gpu_frame_ms >= 0.0) {
            std::snprintf(
                m_frame.data(),
                m_frame.size(),
                "%.2f / %.2f ms",
                cpu_ms,
                ui_metrics_.latest.gpu_frame_ms);
        } else {
            std::snprintf(
                m_frame.data(),
                m_frame.size(),
                "%.2f / -- ms",
                cpu_ms);
        }
        if (scene_tris >= 1'000'000ULL) {
            std::snprintf(
                m_vis.data(),
                m_vis.size(),
                "%u vis | %.2fM tris",
                vis.visible,
                static_cast<double>(scene_tris) / 1'000'000.0);
        } else if (scene_tris >= 10'000ULL) {
            std::snprintf(
                m_vis.data(),
                m_vis.size(),
                "%u vis | %.1fK tris",
                vis.visible,
                static_cast<double>(scene_tris) / 1'000.0);
        } else {
            std::snprintf(
                m_vis.data(),
                m_vis.size(),
                "%u vis | %llu tris",
                vis.visible,
                static_cast<unsigned long long>(scene_tris));
        }
        std::snprintf(
            m_mem.data(),
            m_mem.size(),
            "%.1f / %.1f MB",
            static_cast<double>(renderer_.editor_model_gpu_bytes()) / (1024.0 * 1024.0),
            static_cast<double>(renderer_.editor_texture_gpu_bytes()) / (1024.0 * 1024.0));

        if (ImGui::BeginTable("VegTelemetryGrid", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ui_kit_.metric_card(
                "M1",
                "Frame Rate",
                m_fps.data(),
                card_fps >= 60.0 ? EditorTone::success : EditorTone::warning);
            ImGui::TableSetColumnIndex(1);
            ui_kit_.metric_card("M2", "CPU / GPU Frame", m_frame.data(), EditorTone::info);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ui_kit_.metric_card("M3", "Visible / Scene Tris", m_vis.data(), EditorTone::neutral);
            ImGui::TableSetColumnIndex(1);
            ui_kit_.metric_card("M4", "VRAM Model / Tex", m_mem.data(), EditorTone::purple);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::SetNextItemWidth(96.0F);
        if (ImGui::SliderFloat("Sun", &sun_hour_, 5.0F, 20.5F, "%.1fh")) {
            sun_gi_dirty_ = true;
        }
        ImGui::SameLine(0.0F, 10.0F);
        if (ImGui::Checkbox("Voxel GI", &voxel_gi_enabled_)) {
            sun_gi_dirty_ = true;
        }
        ImGui::SameLine(0.0F, 10.0F);
        ImGui::TextColored(EditorUiKit::color(status_tone_), "%s", status_text_.c_str());
        if (ImGui::IsItemHovered() && !status_text_.empty()) {
            ImGui::SetTooltip("%s", status_text_.c_str());
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void VegetationStudioApplication::update_window_title() noexcept {
    if (!window_) {
        return;
    }
    const double fps = ui_metrics_.one_second_average_fps > 0.0
        ? ui_metrics_.one_second_average_fps
        : ui_metrics_.instantaneous_fps;
    std::array<char, 256U> title{};
    std::snprintf(
        title.data(),
        title.size(),
        "Vulkan Vegetation Studio - %s (%s) | %.0f FPS | CPU %.2f ms | GPU %.2f ms | %s",
        recipe_.display_name.c_str(),
        recipe_.asset_slug.c_str(),
        fps,
        ui_metrics_.latest.cpu_frame_ms >= 0.0 ? ui_metrics_.latest.cpu_frame_ms : 0.0,
        ui_metrics_.latest.gpu_frame_ms >= 0.0 ? ui_metrics_.latest.gpu_frame_ms : 0.0,
        Swapchain::present_mode_name(renderer_.swapchain().present_mode()));
    SDL_SetWindowTitle(window_.get(), title.data());
}

void VegetationStudioApplication::set_status(std::string text, const EditorTone tone) {
    status_text_ = std::move(text);
    status_tone_ = tone;
}

void VegetationStudioApplication::shutdown() noexcept {
    mcp_control_.quit_requested.store(true, std::memory_order_release);
    mcp_control_.notify_response_available();
    mcp_server_.stop();

    if (context_.device() != VK_NULL_HANDLE) {
        static_cast<void>(vkDeviceWaitIdle(context_.device()));
    }

    foliage_preview_.destroy(imgui_vulkan_initialized_);
    bark_preview_.destroy(imgui_vulkan_initialized_);
    unregister_viewport_texture();
    preview_upload_pool_.reset();

    shutdown_imgui_vulkan();
    ui_kit_.shutdown();

    if (imgui_initialized_ && ImGui::GetCurrentContext() != nullptr) {
        if (ImGui::GetIO().BackendPlatformUserData != nullptr) {
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext();
        imgui_initialized_ = false;
    }

    renderer_.shutdown();
    context_.shutdown();
    window_.reset();

    if (sdl_initialized_) {
        SDL_Quit();
        sdl_initialized_ = false;
    }

    preview_temp_dir_.reset();

    if (original_stdout_buffer_.has_value()) {
        std::cout.rdbuf(&original_stdout_buffer_->get());
        original_stdout_buffer_.reset();
    }
}

} // namespace vcr
