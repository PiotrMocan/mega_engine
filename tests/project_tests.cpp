#include "asset_browser.hpp"
#include "asset_pak.hpp"
#include "audio_engine.hpp"
#include "cli_options.hpp"
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "game_module_api.hpp"
#include "game_module_loader.hpp"
#include "gameplay_session.hpp"
#include "icon_system.hpp"
#include "png_encoder.hpp"
#include "prefab_io.hpp"
#include "project_system.hpp"
#include "runtime_assets.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"
#include "skeletal_animation.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

class ScopedTempDir final {
public:
    ScopedTempDir() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("vcr_project_tests_" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDir() noexcept {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

void require_true(
    const bool condition,
    const char* const expression,
    const int line,
    const std::string& detail = {}) {
    if (!condition) {
        std::cerr << "Assertion failed at line " << line << ": " << expression;
        if (!detail.empty()) {
            std::cerr << " (" << detail << ")";
        }
        std::cerr << '\n';
        std::exit(1);
    }
}

#define REQUIRE(cond) require_true((cond), #cond, __LINE__)
#define REQUIRE_MSG(cond, msg) require_true((cond), #cond, __LINE__, (msg))

void test_slug_and_scene_path_validation() {
    REQUIRE(vcr::make_project_folder_slug("My Cool Game!") == "my_cool_game");
    REQUIRE(vcr::make_project_folder_slug("  Sample -- Game 2077  ") == "sample_game_2077");
    REQUIRE(vcr::make_project_folder_slug("!!!") == "new_project");

    REQUIRE(vcr::valid_project_scene_relative_path("scenes/editor_scene.json"));
    REQUIRE(vcr::valid_project_scene_relative_path("scenes/sub/level_01.json"));
    REQUIRE(!vcr::valid_project_scene_relative_path(""));
    REQUIRE(!vcr::valid_project_scene_relative_path("editor_scene.json"));
    REQUIRE(!vcr::valid_project_scene_relative_path("scenes/level.txt"));
    REQUIRE(!vcr::valid_project_scene_relative_path("scenes/../secret.json"));
    REQUIRE(!vcr::valid_project_scene_relative_path("scenes\\editor_scene.json"));
    REQUIRE(!vcr::valid_project_scene_relative_path("/scenes/editor_scene.json"));

    REQUIRE(vcr::default_project_executable_filename("Sample Game") == "sample_game.exe");
    REQUIRE(vcr::valid_project_executable_filename("sample_game.exe"));
    REQUIRE(!vcr::valid_project_executable_filename("dir/sample_game.exe"));
    REQUIRE(!vcr::valid_project_executable_filename("sample_game.txt"));
    REQUIRE(vcr::valid_project_icon_relative_path("icon.ico"));
    REQUIRE(!vcr::valid_project_icon_relative_path("../icon.ico"));
    REQUIRE(!vcr::valid_project_icon_relative_path("icon.png"));
}

void test_standard_template_creation_and_assets() {
    const ScopedTempDir temp;
    vcr::CreateProjectResult created{};
    std::string error;
    REQUIRE_MSG(
        vcr::create_project_on_disk(
            vcr::CreateProjectOptions{
                .name = "Standard Template Check",
                .description = "Starter project verification",
                .parent_directory = temp.path(),
                .folder_name = {},
                .template_kind = vcr::ProjectTemplateKind::standard,
                .bundle_runtime = false,
            },
            created,
            error),
        error);

    REQUIRE(created.manifest.name == "Standard Template Check");
    REQUIRE(created.manifest.template_id == "standard");
    REQUIRE(created.manifest.startup_scene == "scenes/editor_scene.json");
    REQUIRE(created.manifest.executable == "standard_template_check.exe");
    REQUIRE(created.manifest.icon_path == "icon.ico");

    const vcr::ProjectInspection inspection =
        vcr::inspect_project_directory(created.project_directory);
    REQUIRE_MSG(
        inspection.state == vcr::ProjectDiskState::ready,
        inspection.diagnostic);
    REQUIRE(inspection.startup_scene_exists);
    REQUIRE(inspection.icon_exists);
    REQUIRE(inspection.scene_relative_paths.size() == 1U);
    REQUIRE(inspection.scene_relative_paths[0] == "scenes/editor_scene.json");

    // Verify all cooked starter models load cleanly.
    const std::filesystem::path assets_dir = inspection.assets_directory;
    const std::vector<std::string> starter_models{
        "starter/unit_cube/unit_cube.vcrasset.json",
        "starter/accent_cube/accent_cube.vcrasset.json",
        "starter/flat_plane/flat_plane.vcrasset.json",
    };
    for (const std::string& rel_model : starter_models) {
        const vcr::CookedModelLoadResult model =
            vcr::load_cooked_model_asset(assets_dir, rel_model);
        REQUIRE_MSG(
            static_cast<bool>(model),
            model ? std::string{} : model.error().message);
        REQUIRE(!model->vertices.empty());
        REQUIRE(!model->indices.empty());
        REQUIRE(!model->submeshes.empty());
    }

    // Verify cooked starter grid floor texture loads cleanly.
    const vcr::CookedTextureLoadResult texture =
        vcr::load_cooked_texture_asset(
            assets_dir,
            "starter/flat_plane/textures/grid_floor.vcrtexture.json");
    REQUIRE_MSG(
        static_cast<bool>(texture),
        texture ? std::string{} : texture.error().message);
    REQUIRE(texture->width == 128U);
    REQUIRE(texture->height == 128U);
    REQUIRE(texture->pixels.size() == 128U * 128U * 4U);

    // Verify starter prefab loads and instantiates into a SceneDocument.
    vcr::SceneDocument prefab_target;
    vcr::PrefabInstantiateResult prefab_result{};
    REQUIRE_MSG(
        vcr::instantiate_prefab_file(
            prefab_target,
            assets_dir,
            "starter/prefabs/cube_trio.vcrprefab.json",
            vcr::PrefabInstantiateOptions{},
            prefab_result,
            error),
        error);
    REQUIRE(static_cast<bool>(prefab_result.root_id));
    REQUIRE(prefab_result.instantiated_nodes >= 4U);

    // Verify starter scene loads cleanly and has camera, ground, lights, and models.
    vcr::SceneDocument scene;
    REQUIRE_MSG(
        vcr::load_scene_file(
            assets_dir / "scenes/editor_scene.json", scene, error),
        error);
    REQUIRE(scene.valid(scene.startup_camera));
    REQUIRE(scene.size() >= 8U);

    // Verify AssetBrowser indexes the starter project without errors.
    vcr::AssetBrowser browser;
    REQUIRE_MSG(browser.initialize(assets_dir, error), error);

    // Re-creating in the same directory must fail without clobbering existing files.
    vcr::CreateProjectResult duplicate{};
    REQUIRE(!vcr::create_project_on_disk(
        vcr::CreateProjectOptions{
            .name = "Standard Template Check",
            .parent_directory = temp.path(),
            .template_kind = vcr::ProjectTemplateKind::standard,
        },
        duplicate,
        error));
}

void test_empty_template_and_registry_lifecycle() {
    const ScopedTempDir temp;
    const std::filesystem::path projects_dir = temp.path() / "projects";
    const std::filesystem::path registry_file =
        projects_dir / vcr::project_registry_filename;

    vcr::CreateProjectResult minimal{};
    std::string error;
    REQUIRE_MSG(
        vcr::create_project_on_disk(
            vcr::CreateProjectOptions{
                .name = "Bare Plane Sandbox",
                .description = "Minimal plane only",
                .parent_directory = projects_dir,
                .template_kind = vcr::ProjectTemplateKind::empty,
                .bundle_runtime = false,
            },
            minimal,
            error),
        error);
    REQUIRE(!std::filesystem::exists(
        minimal.project_directory / "assets/starter"));

    vcr::SceneDocument minimal_scene;
    REQUIRE_MSG(
        vcr::load_scene_file(
            minimal.project_directory / "assets/scenes/editor_scene.json",
            minimal_scene,
            error),
        error);
    REQUIRE(minimal_scene.valid(minimal_scene.startup_camera));

    // Create an existing legacy folder with assets/scenes/custom.json (no manifest)
    // and initialize its manifest.
    const std::filesystem::path legacy_dir = projects_dir / "legacy_scene_proj";
    std::filesystem::create_directories(legacy_dir / "assets/scenes");
    REQUIRE_MSG(
        vcr::save_scene_file(
            minimal_scene,
            legacy_dir / "assets/scenes/custom_arena.json",
            error),
        error);
    REQUIRE_MSG(
        vcr::initialize_manifest_for_existing_directory(
            legacy_dir, "Legacy Arena", error),
        error);
    const vcr::ProjectInspection legacy_insp =
        vcr::inspect_project_directory(legacy_dir);
    REQUIRE(legacy_insp.state == vcr::ProjectDiskState::ready);
    REQUIRE(legacy_insp.manifest.startup_scene == "scenes/custom_arena.json");

    // Scan directory into ProjectRegistry and round-trip to disk.
    vcr::ProjectRegistry registry;
    REQUIRE_MSG(registry.load_or_initialize(registry_file, error), error);
    const std::size_t scanned = registry.scan_directory(projects_dir, error);
    REQUIRE_MSG(error.empty(), error);
    REQUIRE(scanned == 2U);
    REQUIRE(registry.entries().size() == 2U);

    REQUIRE(registry.set_pinned(minimal.project_directory, true));
    REQUIRE(registry.touch_last_opened(minimal.project_directory));
    REQUIRE_MSG(registry.save(registry_file, error), error);

    vcr::ProjectRegistry reloaded;
    REQUIRE_MSG(reloaded.load_or_initialize(registry_file, error), error);
    REQUIRE(reloaded.entries().size() == 2U);

    // Explicit runtime asset root resolution with custom startup scene.
    const std::string legacy_utf8 = legacy_dir.string();
    const auto resolved = vcr::find_runtime_asset_root(legacy_utf8);
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->project_owned);
    REQUIRE(resolved->project_name == "Legacy Arena");
    REQUIRE(resolved->startup_scene_relative_path == "scenes/custom_arena.json");

    // Invalid explicit project path must fail without falling back.
    const auto invalid_resolved = vcr::find_runtime_asset_root(
        (temp.path() / "does_not_exist").string());
    REQUIRE(!invalid_resolved.has_value());

    // Removing from registry preserves files on disk.
    REQUIRE(reloaded.remove_by_directory(minimal.project_directory));
    REQUIRE(reloaded.entries().size() == 1U);
    REQUIRE(std::filesystem::exists(
        minimal.project_directory / vcr::project_manifest_filename));
}

void test_cli_project_option() {
    {
        const char* argv[] = {"vulkan_cube", "--project", "projects/sample_game"};
        const vcr::CliParseResult parsed = vcr::parse_cli_options(3, argv);
        REQUIRE_MSG(static_cast<bool>(parsed), parsed.error);
        REQUIRE(parsed.options.project_path == "projects/sample_game");
    }
    {
        const char* argv[] = {"vulkan_cube", "--project"};
        const vcr::CliParseResult parsed = vcr::parse_cli_options(2, argv);
        REQUIRE(!static_cast<bool>(parsed));
    }
}

void write_bytes_file(
    const std::filesystem::path& path,
    const std::span<const std::uint8_t> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    if (!bytes.empty()) {
        out.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    REQUIRE(static_cast<bool>(out));
}

void write_text_file(
    const std::filesystem::path& path,
    const std::string_view text) {
    write_bytes_file(
        path,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()),
            text.size()));
}

[[nodiscard]] std::vector<std::uint8_t> read_binary_file(
    const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    REQUIRE(static_cast<bool>(in));
    const std::streamoff size = in.tellg();
    REQUIRE(size >= 0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        in.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    REQUIRE(static_cast<bool>(in));
    return bytes;
}

void test_icon_generation_conversion_and_pe_patching() {
    std::string error;
    constexpr std::array<vcr::BuiltinIconKind, 5U> kinds{{
        vcr::BuiltinIconKind::engine,
        vcr::BuiltinIconKind::editor,
        vcr::BuiltinIconKind::project_default,
        vcr::BuiltinIconKind::project_manager,
        vcr::BuiltinIconKind::icon_tool,
    }};
    for (const vcr::BuiltinIconKind kind : kinds) {
        const vcr::IconRgbaImage img =
            vcr::generate_builtin_icon_rgba(kind, 256U);
        REQUIRE(img.valid());
        REQUIRE(img.width == 256U);
        REQUIRE(img.height == 256U);

        std::uint64_t opaque_count = 0U;
        for (std::size_t i = 3U; i < img.pixels.size(); i += 4U) {
            if (img.pixels[i] > 200U) {
                ++opaque_count;
            }
        }
        REQUIRE(opaque_count > 10000U);

        std::vector<std::uint8_t> ico_bytes;
        REQUIRE_MSG(vcr::encode_builtin_ico(kind, ico_bytes, error), error);
        REQUIRE(ico_bytes.size() > 1000U);
        const std::uint16_t entry_count = static_cast<std::uint16_t>(
            ico_bytes[4] | (static_cast<std::uint16_t>(ico_bytes[5]) << 8U));
        REQUIRE(entry_count == 6U);
    }

    // Create a custom non-square PNG and verify conversion to multi-size .ico.
    const std::uint32_t w = 64U;
    const std::uint32_t h = 48U;
    std::vector<std::uint8_t> custom_rgba(static_cast<std::size_t>(w) * h * 4U);
    for (std::uint32_t y = 0U; y < h; ++y) {
        for (std::uint32_t x = 0U; x < w; ++x) {
            const std::size_t idx =
                (static_cast<std::size_t>(y) * w + x) * 4U;
            custom_rgba[idx + 0U] = static_cast<std::uint8_t>((x * 255U) / w);
            custom_rgba[idx + 1U] = static_cast<std::uint8_t>((y * 255U) / h);
            custom_rgba[idx + 2U] = 220U;
            custom_rgba[idx + 3U] = 255U;
        }
    }
    const vcr::PngEncodeResult png = vcr::encode_png_rgba(custom_rgba, w, h);
    REQUIRE_MSG(static_cast<bool>(png), png.error);

    const ScopedTempDir temp;
    const std::filesystem::path custom_png_path = temp.path() / "custom.png";
    write_bytes_file(custom_png_path, png.bytes);

    std::vector<std::uint8_t> custom_ico_bytes;
    vcr::IconRgbaImage preview{};
    REQUIRE_MSG(
        vcr::load_or_convert_to_ico_bytes(
            custom_png_path, custom_ico_bytes, preview, error),
        error);
    REQUIRE(preview.valid());
    const std::uint16_t custom_entries = static_cast<std::uint16_t>(
        custom_ico_bytes[4] |
        (static_cast<std::uint16_t>(custom_ico_bytes[5]) << 8U));
    REQUIRE(custom_entries == 6U);

#if defined(_WIN32)
    std::array<wchar_t, 4096U> self_buf{};
    const DWORD len = GetModuleFileNameW(
        nullptr, self_buf.data(), static_cast<DWORD>(self_buf.size()));
    REQUIRE(len > 0U && len < self_buf.size());
    const std::filesystem::path self_exe(self_buf.data());
    const std::filesystem::path dummy_exe = temp.path() / "dummy_game.exe";
    std::filesystem::copy_file(
        self_exe, dummy_exe, std::filesystem::copy_options::overwrite_existing);

    REQUIRE_MSG(
        vcr::apply_ico_to_pe_executable(dummy_exe, custom_ico_bytes, error),
        error);
    std::uint32_t frame_count = 0U;
    REQUIRE_MSG(
        vcr::read_pe_executable_icon_frame_count(dummy_exe, frame_count, error),
        error);
    REQUIRE(frame_count == 6U);
#endif
}

void test_shipping_runtime_content_and_rollback() {
    const ScopedTempDir temp;
    vcr::CreateProjectResult created;
    std::string error;
    REQUIRE_MSG(vcr::create_project_on_disk(vcr::CreateProjectOptions{
        .name = "Shipping Test", .parent_directory = temp.path(),
        .bundle_runtime = false}, created, error), error);
    const auto project = created.project_directory;
    write_text_file(project / "game/private.cpp", "private source code");
    write_text_file(project / "build/windows-release/game.pdb", "debug symbols");
    write_text_file(project / "assets/source/original.fbx", "source model");
    write_text_file(project / "assets/unused.png", "unreferenced image");
    write_text_file(project / "assets/saves/private.vcrsave.json", "save data");
    write_text_file(project / "assets/config/input_bindings.json", "{}");
    write_text_file(project / "assets/config/cast.json", R"([{"slug":"hero","height":1.78}])");
    // Game modules construct paths at runtime; no JSON references this clip.
    const auto audio_path = project / "assets/audio/from_code.wav";
    std::filesystem::create_directories(audio_path.parent_path());
    {
        std::ofstream wav(audio_path, std::ios::binary);
        const auto u32 = [&](const std::uint32_t value) {
            const auto bytes = std::bit_cast<std::array<char, 4>>(value);
            wav.write(bytes.data(), bytes.size());
        };
        const auto u16 = [&](const std::uint16_t value) {
            const auto bytes = std::bit_cast<std::array<char, 2>>(value);
            wav.write(bytes.data(), bytes.size());
        };
        wav.write("RIFF", 4); u32(36U + 9600U);
        wav.write("WAVEfmt ", 8); u32(16U); u16(1U); u16(1U);
        u32(48000U); u32(96000U); u16(2U); u16(16U);
        wav.write("data", 4); u32(9600U);
        for (std::uint32_t frame = 0U; frame < 4800U; ++frame) {
            const std::int16_t sample = (frame / 24U) % 2U == 0U ? 8192 : -8192;
            u16(std::bit_cast<std::uint16_t>(sample));
        }
        wav.close();
        REQUIRE(wav.good());
    }
    const auto destination = temp.path() / std::filesystem::path(u8"готовая игра") / "game";
    vcr::ShipProjectResult shipped;
    REQUIRE_MSG(vcr::ship_project(project, destination, shipped, error), error);
    REQUIRE(shipped.output_directory == std::filesystem::canonical(destination));
    REQUIRE(std::filesystem::is_regular_file(shipped.executable_path));
    REQUIRE(std::filesystem::is_regular_file(destination / "vcr_engine.dll"));
    REQUIRE(shipped.cooked_textures == 1U);
    vcr::AudioEngine audio;
    REQUIRE_MSG(audio.initialize({.asset_root = destination / "assets",
        .enable_hardware_output = false}, error), error);
    REQUIRE_MSG(static_cast<bool>(audio.play_asset("audio/from_code.wav", {}, error)), error);
    std::array<float, 2048U> mixed{};
    audio.render_frames_for_test(mixed);
    REQUIRE(std::ranges::any_of(mixed, [](float sample) { return sample > 0.01F || sample < -0.01F; }));
    audio.shutdown();
    REQUIRE(!std::filesystem::exists(destination / "game"));
    REQUIRE(!std::filesystem::exists(destination / "build"));
    REQUIRE(!std::filesystem::exists(destination / "engine/include"));
    REQUIRE(!std::filesystem::exists(destination / "engine/lib"));
    REQUIRE(!std::filesystem::exists(destination / "engine/cmake"));
    REQUIRE(!std::filesystem::exists(destination / "assets/source"));
    REQUIRE(!std::filesystem::exists(destination / "assets/unused.png"));
    REQUIRE(!std::filesystem::exists(destination / "assets/saves"));
    REQUIRE(std::filesystem::is_regular_file(destination / "assets/config/input_bindings.json"));
    REQUIRE(std::filesystem::is_regular_file(destination / "assets/config/cast.json"));
    REQUIRE(std::filesystem::is_regular_file(destination / "assets/content.vcrpak"));
    REQUIRE(!std::filesystem::exists(
        destination / "assets/starter/flat_plane/flat_plane.vcrasset.json"));
    REQUIRE(!std::filesystem::exists(
        destination / "assets/starter/flat_plane/mesh/flat_plane.vcrmesh"));
    REQUIRE(!std::filesystem::exists(
        destination / "assets/starter/flat_plane/materials/grid_floor.vcrmaterial.json"));
    REQUIRE(!std::filesystem::exists(
        destination / "assets/starter/flat_plane/textures/grid_floor.vcrtexture.json"));
    REQUIRE(!std::filesystem::exists(
        destination / "assets/starter/flat_plane/textures/grid_floor.vcrtexture.json.vcrtex"));
    const auto texture_rel = "starter/flat_plane/textures/grid_floor.vcrtexture.json";
    const auto texture = vcr::load_cooked_texture_asset(destination / "assets", texture_rel);
    REQUIRE_MSG(static_cast<bool>(texture), texture ? "" : texture.error().message);
    REQUIRE(texture->source_relative_path.extension() == ".vcrtex");
    REQUIRE(texture->pixel_format == vcr::CookedTexturePixelFormat::bc7_unorm);
    REQUIRE(texture->mip_levels == 8U);
    REQUIRE(texture->width == 128U && texture->height == 128U);
    const auto flat_plane = vcr::load_cooked_model_asset(
        destination / "assets", "starter/flat_plane/flat_plane.vcrasset.json");
    REQUIRE_MSG(static_cast<bool>(flat_plane), flat_plane ? "" : flat_plane.error().message);
    REQUIRE(flat_plane->prebaked_lods);
    REQUIRE(!flat_plane->generate_lods);
    REQUIRE(flat_plane->lods.empty());
    REQUIRE(!flat_plane->vertices.empty() && !flat_plane->indices.empty());
    const auto unit_cube = vcr::load_cooked_model_asset(
        destination / "assets", "starter/unit_cube/unit_cube.vcrasset.json");
    REQUIRE_MSG(static_cast<bool>(unit_cube), unit_cube ? "" : unit_cube.error().message);
    REQUIRE(unit_cube->prebaked_lods);
    vcr::invalidate_asset_pak_cache(destination / "assets");
    REQUIRE(!std::filesystem::exists(destination / "assets/starter/flat_plane/textures/grid_floor.png"));
    REQUIRE(std::filesystem::is_regular_file(project / "assets/starter/flat_plane/textures/grid_floor.png"));
    for (const auto& file : std::filesystem::recursive_directory_iterator(destination)) {
        const auto ext = file.path().extension();
        REQUIRE(ext != ".cpp" && ext != ".hpp" && ext != ".pdb" &&
                ext != ".obj" && ext != ".lib" && ext != ".fbx");
    }
    REQUIRE(!vcr::ship_project(project, destination, shipped, error));
    REQUIRE(error.find("already exists") != std::string::npos);
    REQUIRE(std::filesystem::is_regular_file(destination / "shipping_test.exe"));
    REQUIRE(!vcr::ship_project(project, project / "export", shipped, error));
    REQUIRE(!std::filesystem::exists(project / "export"));
    write_text_file(project / "assets/starter/flat_plane/textures/grid_floor.png", "invalid image");
    const auto failed = temp.path() / "failed_export";
    REQUIRE(!vcr::ship_project(project, failed, shipped, error));
    REQUIRE(error.find("Texture cook failed") != std::string::npos);
    REQUIRE(!std::filesystem::exists(failed));
    write_text_file(project / "assets/config/cast.json", "\"scalar\"");
    const auto invalid_config = temp.path() / "invalid_config_export";
    REQUIRE(!vcr::ship_project(project, invalid_config, shipped, error));
    REQUIRE(error.find("Invalid shipping metadata") != std::string::npos);
    REQUIRE(!std::filesystem::exists(invalid_config));
    for (const auto& entry : std::filesystem::directory_iterator(temp.path())) {
        REQUIRE(entry.path().filename().string().find(".shipping-") == std::string::npos);
    }
}

[[nodiscard]] std::vector<std::uint8_t> build_v1_vcrmesh_bytes(
    const std::span<const vcr::CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const std::span<const std::pair<std::uint64_t, std::uint64_t>> submesh_ranges,
    const std::span<const std::uint32_t> submesh_materials) {
    std::vector<std::uint8_t> out;
    const auto append_u32 = [&out](const std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
    };
    const auto append_u64 = [&append_u32](const std::uint64_t v) {
        append_u32(static_cast<std::uint32_t>(v));
        append_u32(static_cast<std::uint32_t>(v >> 32U));
    };
    const auto append_f32 = [&append_u32](const float v) {
        append_u32(std::bit_cast<std::uint32_t>(v));
    };
    constexpr std::array<char, 8U> magic{'V', 'C', 'R', 'M', 'E', 'S', 'H', '\0'};
    for (const char ch : magic) {
        out.push_back(static_cast<std::uint8_t>(ch));
    }
    append_u32(vcr::cooked_mesh_binary_version);
    append_u32(vcr::cooked_mesh_vertex_stride);
    append_u64(vertices.size());
    append_u64(indices.size());
    append_u32(static_cast<std::uint32_t>(submesh_ranges.size()));
    append_u32(0U);
    for (std::size_t i = 0U; i < submesh_ranges.size(); ++i) {
        append_u64(submesh_ranges[i].first);
        append_u64(submesh_ranges[i].second);
        append_u32(submesh_materials[i]);
        append_u32(0U);
    }
    for (const vcr::CookedModelVertex& v : vertices) {
        for (const float c : v.position) append_f32(c);
        for (const float c : v.normal) append_f32(c);
        for (const float c : v.texcoord) append_f32(c);
        for (const float c : v.tangent) append_f32(c);
    }
    for (const std::uint32_t idx : indices) {
        append_u32(idx);
    }
    return out;
}

void write_test_triangle_model(
    const std::filesystem::path& assets_root,
    const std::string_view rel_dir,
    const std::string_view model_name,
    const bool two_materials,
    const nlohmann::json& extra_lods = nlohmann::json(),
    const bool skinned = false) {
    const std::filesystem::path pkg = assets_root / std::string(rel_dir);
    std::filesystem::create_directories(pkg / "materials");
    std::filesystem::create_directories(pkg / "mesh");

    const nlohmann::json mat0{
        {"schema", "vcr.asset"},
        {"version", 1U},
        {"kind", "material"},
        {"name", "MatA"},
        {"pbr", {
            {"base_color", {0.7F, 0.2F, 0.2F, 1.0F}},
            {"metallic", 0.0F},
            {"roughness", 0.5F},
            {"ambient_occlusion", 1.0F},
        }},
        {"textures", nlohmann::json::object()},
    };
    write_text_file(pkg / "materials/mat_a.vcrmaterial.json", mat0.dump(2));

    nlohmann::json materials_arr = nlohmann::json::array({"materials/mat_a.vcrmaterial.json"});
    if (two_materials) {
        nlohmann::json mat1 = mat0;
        mat1["name"] = "MatB";
        write_text_file(pkg / "materials/mat_b.vcrmaterial.json", mat1.dump(2));
        materials_arr.push_back("materials/mat_b.vcrmaterial.json");
    }

    std::vector<vcr::CookedModelVertex> verts{
        {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
    };
    std::vector<std::uint32_t> inds{0U, 1U, 2U};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges{{0U, 3U}};
    std::vector<std::uint32_t> sub_mats{0U};
    nlohmann::json submeshes_json = nlohmann::json::array({
        nlohmann::json{
            {"name", "MatA"},
            {"first_index", 0U},
            {"index_count", 3U},
            {"material_index", 0U},
            {"material", "materials/mat_a.vcrmaterial.json"},
        },
    });
    if (two_materials) {
        verts.push_back({{1.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}});
        inds.insert(inds.end(), {1U, 3U, 2U});
        ranges.emplace_back(3U, 3U);
        sub_mats.push_back(1U);
        submeshes_json.push_back(nlohmann::json{
            {"name", "MatB"},
            {"first_index", 3U},
            {"index_count", 3U},
            {"material_index", 1U},
            {"material", "materials/mat_b.vcrmaterial.json"},
        });
    }

    const std::string mesh_rel = "mesh/" + std::string(model_name) + ".vcrmesh";
    write_bytes_file(pkg / mesh_rel, build_v1_vcrmesh_bytes(verts, inds, ranges, sub_mats));

    nlohmann::json model_json{
        {"schema", "vcr.asset"},
        {"version", 1U},
        {"kind", "model"},
        {"name", std::string(model_name)},
        {"mesh", mesh_rel},
        {"geometry", {
            {"primitive", "triangles"},
            {"vertex_count", verts.size()},
            {"vertex_stride", vcr::cooked_mesh_vertex_stride},
            {"index_count", inds.size()},
            {"index_type", "uint32"},
        }},
        {"materials", materials_arr},
        {"submeshes", submeshes_json},
    };
    if (extra_lods.is_array() && !extra_lods.empty()) {
        model_json["lods"] = extra_lods;
    }
    if (skinned) {
        vcr::CookedSkeletonAsset skel;
        skel.bones.push_back(vcr::SkeletonBone{.name = "root", .parent_index = -1});
        std::vector<vcr::CookedSkinVertex> skin_verts(verts.size());
        std::string io_err;
        REQUIRE_MSG(vcr::write_cooked_skin_file(pkg / "rig.vcrskin", skel, skin_verts, io_err), io_err);
        vcr::AnimationClip clip;
        clip.name = "idle";
        clip.duration_seconds = 0.5F;
        REQUIRE_MSG(vcr::write_cooked_anim_file(pkg / "idle.vcranim", clip, io_err), io_err);
        model_json["skeleton"] = "rig.vcrskin";
        model_json["animations"] = nlohmann::json::array({
            nlohmann::json{{"name", "idle"}, {"path", "idle.vcranim"}, {"duration_seconds", 0.5F}},
        });
    }
    write_text_file(pkg / (std::string(model_name) + ".vcrasset.json"), model_json.dump(2));
}

void test_shipping_prebaked_lod_pruning_and_skinned_v1() {
    const ScopedTempDir temp;
    vcr::CreateProjectResult created;
    std::string error;
    REQUIRE_MSG(vcr::create_project_on_disk(vcr::CreateProjectOptions{
        .name = "LOD Pruning Ship",
        .parent_directory = temp.path(),
        .bundle_runtime = false,
    }, created, error), error);

    const std::filesystem::path assets_dir = created.project_directory / "assets";
    write_test_triangle_model(assets_dir, "props/hero_tree_lod1", "hero_tree_lod1", true);
    write_test_triangle_model(assets_dir, "props/hero_tree_lod2", "hero_tree_lod2", true);
    const nlohmann::json authored_lods = nlohmann::json::array({
        {{"model", "props/hero_tree_lod1/hero_tree_lod1.vcrasset.json"}, {"geometric_error", 0.05}},
        {{"model", "props/hero_tree_lod2/hero_tree_lod2.vcrasset.json"}, {"geometric_error", 0.20}},
    });
    write_test_triangle_model(assets_dir, "props/hero_tree", "hero_tree", true, authored_lods);
    write_test_triangle_model(assets_dir, "chars/hero_rig", "hero_rig", false, nlohmann::json(), true);

    vcr::SceneDocument scene;
    REQUIRE_MSG(vcr::load_scene_file(assets_dir / "scenes/editor_scene.json", scene, error), error);
    const vcr::SceneNodeId tree_node = scene.create_node("Hero Tree", vcr::SceneNodeKind::model);
    REQUIRE(static_cast<bool>(tree_node));
    scene.node(tree_node)->get().asset_path = "props/hero_tree/hero_tree.vcrasset.json";
    const vcr::SceneNodeId char_node = scene.create_node("Hero Rig", vcr::SceneNodeKind::model);
    REQUIRE(static_cast<bool>(char_node));
    scene.node(char_node)->get().asset_path = "chars/hero_rig/hero_rig.vcrasset.json";
    REQUIRE_MSG(vcr::save_scene_file(scene, assets_dir / "scenes/editor_scene.json", error), error);

    // Reference LOD2 in a prefab so it stays directly usable even though LOD1 is derivative-only.
    vcr::SceneDocument prefab_doc;
    const vcr::SceneNodeId lod2_node = prefab_doc.create_node("LOD2 Prop", vcr::SceneNodeKind::model);
    REQUIRE(static_cast<bool>(lod2_node));
    prefab_doc.node(lod2_node)->get().asset_path = "props/hero_tree_lod2/hero_tree_lod2.vcrasset.json";
    REQUIRE_MSG(vcr::save_prefab_file(
        prefab_doc, lod2_node, assets_dir, "prefabs/lod2_prop.vcrprefab.json", error), error);

    const std::filesystem::path destination = temp.path() / "shipped_lod_game";
    vcr::ShipProjectResult shipped;
    REQUIRE_MSG(vcr::ship_project(created.project_directory, destination, shipped, error), error);

    const std::filesystem::path shipped_assets = destination / "assets";
    const std::filesystem::path canon_assets = std::filesystem::canonical(shipped_assets);
    REQUIRE(std::filesystem::is_regular_file(shipped_assets / "content.vcrpak"));

    REQUIRE(vcr::asset_pak_contains(canon_assets, "props/hero_tree/hero_tree.vcrasset.json"));
    REQUIRE(vcr::asset_pak_contains(canon_assets, "props/hero_tree/mesh/hero_tree.vcrmesh"));
    REQUIRE(!vcr::asset_pak_contains(canon_assets, "props/hero_tree_lod1/hero_tree_lod1.vcrasset.json"));
    REQUIRE(!vcr::asset_pak_contains(canon_assets, "props/hero_tree_lod1/mesh/hero_tree_lod1.vcrmesh"));
    REQUIRE(vcr::asset_pak_contains(canon_assets, "props/hero_tree_lod2/hero_tree_lod2.vcrasset.json"));
    REQUIRE(vcr::asset_pak_contains(canon_assets, "props/hero_tree_lod2/mesh/hero_tree_lod2.vcrmesh"));

    const auto loaded_tree = vcr::load_cooked_model_asset(
        shipped_assets, "props/hero_tree/hero_tree.vcrasset.json");
    REQUIRE_MSG(static_cast<bool>(loaded_tree), loaded_tree ? "" : loaded_tree.error().message);
    REQUIRE(loaded_tree->prebaked_lods);
    REQUIRE(!loaded_tree->generate_lods);
    REQUIRE(loaded_tree->lods.empty());
    REQUIRE(loaded_tree->submeshes.size() == 2U);
    REQUIRE(loaded_tree->submeshes[0].lod_count[0] == 3U);
    REQUIRE(loaded_tree->submeshes[0].lod_count[1] == 3U);
    REQUIRE(loaded_tree->submeshes[1].lod_count[0] == 3U);

    const auto loaded_lod2 = vcr::load_cooked_model_asset(
        shipped_assets, "props/hero_tree_lod2/hero_tree_lod2.vcrasset.json");
    REQUIRE_MSG(static_cast<bool>(loaded_lod2), loaded_lod2 ? "" : loaded_lod2.error().message);
    REQUIRE(loaded_lod2->prebaked_lods);

    const auto missing_lod1 = vcr::load_cooked_model_asset(
        shipped_assets, "props/hero_tree_lod1/hero_tree_lod1.vcrasset.json");
    REQUIRE(!missing_lod1);
    REQUIRE(missing_lod1.error().code == vcr::CookedModelLoadErrorCode::file_not_found);

    // Skinned models stay v1 with .vcrskin and .vcranim on disk.
    REQUIRE(std::filesystem::is_regular_file(shipped_assets / "chars/hero_rig/rig.vcrskin"));
    REQUIRE(std::filesystem::is_regular_file(shipped_assets / "chars/hero_rig/idle.vcranim"));
    REQUIRE(!std::filesystem::exists(shipped_assets / "chars/hero_rig/hero_rig.vcrasset.json"));
    REQUIRE(!std::filesystem::exists(shipped_assets / "chars/hero_rig/mesh/hero_rig.vcrmesh"));
    const auto loaded_rig = vcr::load_cooked_model_asset(
        shipped_assets, "chars/hero_rig/hero_rig.vcrasset.json");
    REQUIRE_MSG(static_cast<bool>(loaded_rig), loaded_rig ? "" : loaded_rig.error().message);
    REQUIRE(!loaded_rig->prebaked_lods);
    REQUIRE(loaded_rig->is_skinned());
    REQUIRE(loaded_rig->animations.size() == 1U);

    vcr::invalidate_asset_pak_cache(shipped_assets);
}

void test_project_bundle_and_icon_workflow() {
    const ScopedTempDir temp;
    const std::filesystem::path projects_dir = temp.path() / "projects";
    vcr::CreateProjectResult created{};
    std::string error;
    REQUIRE_MSG(
        vcr::create_project_on_disk(
            vcr::CreateProjectOptions{
                .name = "Portable Test Game",
                .description = "Standalone bundle check",
                .parent_directory = projects_dir,
                .template_kind = vcr::ProjectTemplateKind::standard,
                .bundle_runtime = false,
            },
            created,
            error),
        error);

#if defined(_WIN32)
    std::array<wchar_t, 4096U> self_buf{};
    const DWORD len = GetModuleFileNameW(
        nullptr, self_buf.data(), static_cast<DWORD>(self_buf.size()));
    REQUIRE(len > 0U && len < self_buf.size());
    const std::filesystem::path fake_bin = temp.path() / "fake_bin";
    std::filesystem::create_directories(fake_bin);
    const std::filesystem::path fake_engine_exe = fake_bin / "vulkan_cube.exe";
    std::filesystem::copy_file(
        std::filesystem::path(self_buf.data()),
        fake_engine_exe,
        std::filesystem::copy_options::overwrite_existing);
    write_text_file(
        fake_bin / vcr::engine_shared_library_filename, "FAKE_ENGINE_DLL");
    write_text_file(fake_bin / "amd_fidelityfx_vk.dll", "FAKE_FFX_DLL");
    write_text_file(fake_bin / "shaders/cube.vert.spv", "FAKE_SPV");
    write_text_file(fake_bin / "assets/sky_clouds.png", "FAKE_SKY");
    write_text_file(fake_bin / "assets/smaa/AreaTexRG8.dds", "FAKE_AREA");
    write_text_file(fake_bin / "assets/smaa/SearchTexR8.dds", "FAKE_SEARCH");
    write_text_file(fake_bin / "LICENSES/LICENSE.txt", "MIT");

    vcr::BundleProjectRuntimeResult bundle_res{};
    REQUIRE_MSG(
        vcr::bundle_project_runtime(
            created.project_directory, bundle_res, error, fake_engine_exe),
        error);
    REQUIRE(bundle_res.copied_engine_dll);
    REQUIRE(bundle_res.copied_fsr3_dll);
    REQUIRE(bundle_res.copied_shaders >= 1U);
    REQUIRE(bundle_res.icon_embedded);
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / "portable_test_game.exe"));
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / vcr::engine_shared_library_filename));
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / "amd_fidelityfx_vk.dll"));
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / "icon.ico"));
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / "engine/shaders/cube.vert.spv"));
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / "engine/assets/sky_clouds.png"));
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / "engine/assets/smaa/AreaTexRG8.dds"));
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / "engine/LICENSES/LICENSE.txt"));

    const vcr::ProjectInspection after_bundle =
        vcr::inspect_project_directory(created.project_directory);
    REQUIRE(after_bundle.runtime_bundled);
    REQUIRE(after_bundle.engine_dll_exists);
    REQUIRE(after_bundle.icon_exists);

    // Deleting vcr_engine.dll must invalidate runtime_bundled until updated.
    std::filesystem::remove(after_bundle.engine_dll_path);
    const vcr::ProjectInspection missing_dll =
        vcr::inspect_project_directory(created.project_directory);
    REQUIRE(!missing_dll.engine_dll_exists);
    REQUIRE(!missing_dll.runtime_bundled);

    REQUIRE_MSG(
        vcr::bundle_project_runtime(
            created.project_directory, bundle_res, error, fake_engine_exe),
        error);
    const vcr::ProjectInspection restored_bundle =
        vcr::inspect_project_directory(created.project_directory);
    REQUIRE(restored_bundle.engine_dll_exists);
    REQUIRE(restored_bundle.runtime_bundled);

    vcr::AssetBrowser browser;
    REQUIRE_MSG(browser.initialize(after_bundle.assets_directory, error), error);

    std::uint32_t frame_count = 0U;
    REQUIRE_MSG(
        vcr::read_pe_executable_icon_frame_count(
            after_bundle.executable_path, frame_count, error),
        error);
    REQUIRE(frame_count == 6U);

    // Apply a custom PNG icon and confirm project icon.ico + PE icon update cleanly.
    const std::vector<std::uint8_t> default_ico_bytes =
        read_binary_file(after_bundle.icon_file_path);
    std::vector<std::uint8_t> red_rgba(32U * 32U * 4U, 255U);
    for (std::size_t i = 0U; i < red_rgba.size(); i += 4U) {
        red_rgba[i + 0U] = 240U;
        red_rgba[i + 1U] = 40U;
        red_rgba[i + 2U] = 50U;
        red_rgba[i + 3U] = 255U;
    }
    const vcr::PngEncodeResult red_png = vcr::encode_png_rgba(red_rgba, 32U, 32U);
    REQUIRE_MSG(static_cast<bool>(red_png), red_png.error);
    const std::filesystem::path red_png_path = temp.path() / "red_icon.png";
    write_bytes_file(red_png_path, red_png.bytes);

    REQUIRE_MSG(
        vcr::set_project_icon(created.project_directory, red_png_path, error),
        error);
    const std::vector<std::uint8_t> custom_ico_bytes =
        read_binary_file(after_bundle.icon_file_path);
    REQUIRE(custom_ico_bytes != default_ico_bytes);
    REQUIRE_MSG(
        vcr::read_pe_executable_icon_frame_count(
            after_bundle.executable_path, frame_count, error),
        error);
    REQUIRE(frame_count == 6U);

    REQUIRE_MSG(
        vcr::reset_project_icon_to_default(created.project_directory, error),
        error);
    const std::vector<std::uint8_t> restored_ico_bytes =
        read_binary_file(after_bundle.icon_file_path);
    REQUIRE(restored_ico_bytes == default_ico_bytes);
#endif
}

void test_cpp_module_project_and_manifest_round_trip() {
    REQUIRE(vcr::valid_project_module_relative_path("bin/demo_game.dll"));
    REQUIRE(!vcr::valid_project_module_relative_path(""));
    REQUIRE(!vcr::valid_project_module_relative_path("../evil.dll"));
    REQUIRE(!vcr::valid_project_module_relative_path("C:/evil.dll"));
    REQUIRE(!vcr::valid_project_module_relative_path("bin/evil.exe"));

    const ScopedTempDir temp;
    vcr::CreateProjectResult created{};
    std::string error;
    REQUIRE_MSG(
        vcr::create_project_on_disk(
            vcr::CreateProjectOptions{
                .name = "My Cpp Game",
                .description = "Standalone C++ DLL module template",
                .parent_directory = temp.path() / "projects",
                .template_kind = vcr::ProjectTemplateKind::cpp_module,
                .bundle_runtime = false,
            },
            created,
            error),
        error);
    REQUIRE(created.manifest.template_id == "cpp_module");
    REQUIRE(created.manifest.game_module == "bin/my_cpp_game_game.dll");
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / "CMakeLists.txt"));
    REQUIRE(std::filesystem::is_regular_file(
        created.project_directory / "game/my_cpp_game_game.cpp"));

    const vcr::ProjectInspection insp =
        vcr::inspect_project_directory(created.project_directory);
    REQUIRE(insp.state == vcr::ProjectDiskState::ready);
    REQUIRE(insp.has_cpp_code);
    REQUIRE(!insp.game_module_built);
    REQUIRE(insp.manifest.game_module == "bin/my_cpp_game_game.dll");

    vcr::ProjectManifest loaded{};
    REQUIRE_MSG(
        vcr::load_project_manifest(
            created.project_directory / vcr::project_manifest_filename,
            loaded,
            error),
        error);
    REQUIRE(loaded.game_module == "bin/my_cpp_game_game.dll");

#if defined(_WIN32)
    const std::filesystem::path workspace_root =
        vcr::default_projects_root_directory().parent_path();
    const std::filesystem::path sdk_src =
        workspace_root / "cmake" / "VcrEngineSdk.cmake";
    std::error_code fs_ec;
    if (std::filesystem::is_regular_file(sdk_src, fs_ec) && !fs_ec) {
        std::filesystem::create_directories(
            created.project_directory / "engine" / "cmake", fs_ec);
        REQUIRE_MSG(!fs_ec, fs_ec.message());
        std::filesystem::copy_file(
            sdk_src,
            created.project_directory / "engine" / "cmake" / "VcrEngineSdk.cmake",
            std::filesystem::copy_options::overwrite_existing,
            fs_ec);
        REQUIRE_MSG(!fs_ec, fs_ec.message());

        const std::filesystem::path debug_build_dir =
            workspace_root / "build" / "windows-debug";
        if (std::filesystem::is_regular_file(
                debug_build_dir / "bin" / "vcr_engined.dll", fs_ec) &&
            !fs_ec &&
            std::filesystem::is_regular_file(
                debug_build_dir / "lib" / "vcr_engined.lib", fs_ec) &&
            !fs_ec) {
            vcr::CompileProjectModuleResult debug_res{};
            REQUIRE_MSG(
                vcr::compile_project_game_module(
                    created.project_directory, debug_res, error, "Debug"),
                error);
            REQUIRE(debug_res.built);
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "build" / "windows-debug" / "bin" /
                    "my_cpp_game_game.dll"));
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "build" / "windows-debug" / "bin" /
                    "vcr_engined.dll"));
            // Debug module builds must never clobber the Release <project>/bin/ DLL.
            REQUIRE(!std::filesystem::exists(
                created.project_directory / "bin" / "my_cpp_game_game.dll"));
        }

        const std::filesystem::path release_build_dir =
            workspace_root / "build" / "windows-release";
        const std::filesystem::path release_exe =
            release_build_dir / "bin" / "vulkan_cube.exe";
        const std::filesystem::path release_dll =
            release_build_dir / "bin" / "vcr_engine.dll";
        const std::filesystem::path release_lib =
            release_build_dir / "lib" / "vcr_engine.lib";
        fs_ec.clear();
        if (std::filesystem::is_regular_file(release_exe, fs_ec) && !fs_ec &&
            std::filesystem::is_regular_file(release_dll, fs_ec) && !fs_ec &&
            std::filesystem::is_regular_file(release_lib, fs_ec) && !fs_ec) {
            vcr::BundleProjectRuntimeResult cpp_bundle{};
            REQUIRE_MSG(
                vcr::bundle_project_runtime(
                    created.project_directory, cpp_bundle, error, release_exe),
                error);
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "my_cpp_game.exe"));
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "vcr_engine.dll"));
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "engine" / "lib" / "vcr_engine.lib"));
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "engine" / "cmake" / "VcrEngineSdk.cmake"));
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "engine" / "include" / "game_module_api.hpp"));
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "engine" / "include" / "gameplay_session.hpp"));

            vcr::CompileProjectModuleResult release_res{};
            REQUIRE_MSG(
                vcr::compile_project_game_module(
                    created.project_directory, release_res, error, "Release"),
                error);
            REQUIRE(release_res.built);
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "build" / "windows-release" /
                    "bin" / "my_cpp_game_game.dll"));
            REQUIRE(std::filesystem::is_regular_file(
                created.project_directory / "bin" / "my_cpp_game_game.dll"));

            // Re-running a Debug build after Release must preserve the Release DLL in bin/.
            if (std::filesystem::is_regular_file(
                    debug_build_dir / "bin" / "vcr_engined.dll", fs_ec) &&
                !fs_ec &&
                std::filesystem::is_regular_file(
                    debug_build_dir / "lib" / "vcr_engined.lib", fs_ec) &&
                !fs_ec) {
                const auto release_dll_bytes = read_binary_file(
                    created.project_directory / "bin" / "my_cpp_game_game.dll");
                vcr::CompileProjectModuleResult debug_after{};
                REQUIRE_MSG(
                    vcr::compile_project_game_module(
                        created.project_directory, debug_after, error, "Debug"),
                    error);
                REQUIRE(debug_after.built);
                REQUIRE(read_binary_file(
                            created.project_directory / "bin" /
                            "my_cpp_game_game.dll") == release_dll_bytes);
            }
        }
    }
#endif
}

void test_game_module_loader_hardening() {
    REQUIRE(vcr::vcr_game_module_api_version == 2U);
    REQUIRE(vcr::vcr_current_module_abi_stamp() != 0U);

    ScopedTempDir temp;
    vcr::SceneDocument scene;
    vcr::GameplaySession session;
    vcr::LoadedGameModule mod;
    const vcr::GameModuleHostContext host{
        .project_root = &temp.path(),
        .asset_root = &temp.path(),
        .scene = &scene,
        .session = &session,
    };

    std::string error;
    REQUIRE(!mod.load(temp.path() / "does_not_exist_game.dll", host, false, error));
    REQUIRE(!mod.is_loaded());
    REQUIRE(!error.empty());

    const std::filesystem::path dir_path = temp.path() / "not_a_regular_dll";
    std::filesystem::create_directories(dir_path);
    error.clear();
    REQUIRE(!mod.load(dir_path, host, true, error));
    REQUIRE(!mod.is_loaded());
    REQUIRE(!error.empty());
}

} // namespace

int main() {
    test_slug_and_scene_path_validation();
    test_standard_template_creation_and_assets();
    test_empty_template_and_registry_lifecycle();
    test_cli_project_option();
    test_icon_generation_conversion_and_pe_patching();
    test_project_bundle_and_icon_workflow();
    test_shipping_runtime_content_and_rollback();
    test_shipping_prebaked_lod_pruning_and_skinned_v1();
    test_cpp_module_project_and_manifest_round_trip();
    test_game_module_loader_hardening();
    std::cout << "All project system tests passed.\n";
    return 0;
}
