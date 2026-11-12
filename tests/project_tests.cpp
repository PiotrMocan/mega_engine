#include "asset_browser.hpp"
#include "cli_options.hpp"
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "prefab_io.hpp"
#include "project_system.hpp"
#include "runtime_assets.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

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
    REQUIRE(vcr::make_project_folder_slug("  Neon -- Night 2077  ") == "neon_night_2077");
    REQUIRE(vcr::make_project_folder_slug("!!!") == "new_project");

    REQUIRE(vcr::valid_project_scene_relative_path("scenes/editor_scene.json"));
    REQUIRE(vcr::valid_project_scene_relative_path("scenes/sub/level_01.json"));
    REQUIRE(!vcr::valid_project_scene_relative_path(""));
    REQUIRE(!vcr::valid_project_scene_relative_path("editor_scene.json"));
    REQUIRE(!vcr::valid_project_scene_relative_path("scenes/level.txt"));
    REQUIRE(!vcr::valid_project_scene_relative_path("scenes/../secret.json"));
    REQUIRE(!vcr::valid_project_scene_relative_path("scenes\\editor_scene.json"));
    REQUIRE(!vcr::valid_project_scene_relative_path("/scenes/editor_scene.json"));
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
            },
            created,
            error),
        error);

    REQUIRE(created.manifest.name == "Standard Template Check");
    REQUIRE(created.manifest.template_id == "standard");
    REQUIRE(created.manifest.startup_scene == "scenes/editor_scene.json");

    const vcr::ProjectInspection inspection =
        vcr::inspect_project_directory(created.project_directory);
    REQUIRE_MSG(
        inspection.state == vcr::ProjectDiskState::ready,
        inspection.diagnostic);
    REQUIRE(inspection.startup_scene_exists);
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
        const char* argv[] = {"vulkan_cube", "--project", "projects/neon_night_city"};
        const vcr::CliParseResult parsed = vcr::parse_cli_options(3, argv);
        REQUIRE_MSG(static_cast<bool>(parsed), parsed.error);
        REQUIRE(parsed.options.project_path == "projects/neon_night_city");
    }
    {
        const char* argv[] = {"vulkan_cube", "--project"};
        const vcr::CliParseResult parsed = vcr::parse_cli_options(2, argv);
        REQUIRE(!static_cast<bool>(parsed));
    }
}

} // namespace

int main() {
    test_slug_and_scene_path_validation();
    test_standard_template_creation_and_assets();
    test_empty_template_and_registry_lifecycle();
    test_cli_project_option();
    std::cout << "All project system tests passed.\n";
    return 0;
}
