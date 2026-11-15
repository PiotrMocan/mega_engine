#include "character_package.hpp"
#include "cooked_audio_asset.hpp"
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "game_ui_runtime.hpp"
#include "localization.hpp"
#include "prefab_io.hpp"
#include "scene_document.hpp"
#include "skeletal_animation.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;

void print_usage() {
    std::cout
        << "vcr_char_cooker - VCR Headless Character Package Compiler & Validator\n"
        << "Usage:\n"
        << "  vcr_char_cooker --input <file.vcrchar.json> --project <dir> [options]\n\n"
        << "Options:\n"
        << "  --input <path>     Source .vcrchar.json package path\n"
        << "  --project <dir>    Target project directory (publishes into <dir>/assets/)\n"
        << "  --tex-res <N>      Baked texture resolution: 256|512|1024 (default 512)\n"
        << "  --no-voice         Skip formant WAV voice synthesis\n"
        << "  --no-prefabs       Skip NPC/Player .vcrprefab.json emission\n"
        << "  --no-loc           Skip EN/RU .vcrloc.json generation\n"
        << "  --verify           Load back every cooked artifact via engine loaders to prove integrity\n"
        << "  --dry-run          Validate .vcrchar.json + procedural mesh build without writing assets\n"
        << "  --json             Emit structured JSON report on stdout\n"
        << "  --help             Show this help\n";
}

[[nodiscard]] bool verify_cooked_outputs(
    const std::filesystem::path& project_dir,
    const vcr::CharacterCookReport& report,
    std::string& error) {
    const std::filesystem::path asset_root = project_dir / "assets";

    const auto model_res = vcr::load_cooked_model_asset(
        asset_root, report.model_asset_relative);
    if (!model_res) {
        error = "Model verification failed: " + model_res.error().message;
        return false;
    }
    if (model_res.value().skeleton.bones.size() != vcr::character_total_bone_count) {
        error = "Expected 50-bone skeleton, got " +
            std::to_string(model_res.value().skeleton.bones.size());
        return false;
    }
    if (model_res.value().animations.size() < 12U) {
        error = "Expected at least 12 locomotion/gesture clips.";
        return false;
    }

    const std::filesystem::path tex_dir =
        asset_root / "characters" / report.character_slug / "textures";
    std::error_code tex_ec;
    std::size_t verified_textures = 0U;
    if (std::filesystem::exists(tex_dir, tex_ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(tex_dir, tex_ec)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::filesystem::path rel =
                std::filesystem::relative(entry.path(), asset_root, tex_ec);
            if (tex_ec) {
                error = "Failed to compute relative texture path.";
                return false;
            }
            const auto tex_res = vcr::load_cooked_texture_asset(asset_root, rel);
            if (!tex_res) {
                error = "Texture verification failed (" + rel.generic_string() + "): " +
                    tex_res.error().message;
                return false;
            }
            ++verified_textures;
        }
    }
    if (verified_textures < 3U) {
        error = "Expected at least 3 baked PBR textures for character '" +
            report.character_slug + "'.";
        return false;
    }

    if (!report.dialogue_relative.empty()) {
        vcr::GameUiRuntime ui_runtime;
        std::string dlg_err;
        if (!ui_runtime.load_dialogue_file(asset_root / report.dialogue_relative, &dlg_err)) {
            error = "Dialogue verification failed: " + dlg_err;
            return false;
        }
    }

    if (!report.loc_en_relative.empty()) {
        vcr::LocalizationSystem loc;
        std::string loc_err;
        if (!loc.load_file(asset_root / report.loc_en_relative, &loc_err) ||
            !loc.load_file(asset_root / report.loc_ru_relative, &loc_err)) {
            error = "Localization verification failed: " + loc_err;
            return false;
        }
    }

    for (const auto& audio_rel : report.audio_assets_relative) {
        vcr::CookedAudioAsset audio_asset{};
        if (!vcr::load_cooked_audio_asset(
                asset_root, audio_rel.generic_string(), audio_asset, error)) {
            error = "Audio verification failed (" + audio_rel.generic_string() + "): " + error;
            return false;
        }
    }

    if (!report.npc_prefab_relative.empty()) {
        vcr::SceneDocument scene;
        vcr::PrefabInstantiateOptions popts{};
        vcr::PrefabInstantiateResult pres{};
        if (!vcr::instantiate_prefab_file(
                scene, asset_root, report.npc_prefab_relative.generic_string(),
                popts, pres, error)) {
            error = "NPC prefab verification failed: " + error;
            return false;
        }
    }

    if (!report.player_prefab_relative.empty()) {
        vcr::SceneDocument scene;
        vcr::PrefabInstantiateOptions popts{};
        vcr::PrefabInstantiateResult pres{};
        if (!vcr::instantiate_prefab_file(
                scene, asset_root, report.player_prefab_relative.generic_string(),
                popts, pres, error)) {
            error = "Player prefab verification failed: " + error;
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path input_path;
    std::filesystem::path project_dir;
    vcr::CharacterCookOptions options{};
    bool verify = false;
    bool dry_run = false;
    bool json_out = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i] != nullptr ? std::string_view(argv[i]) : std::string_view{};
        auto next_val = [&](std::string_view flag) -> std::string_view {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                std::cerr << "Missing value for " << flag << "\n";
                std::exit(2);
            }
            ++i;
            return std::string_view(argv[i]);
        };
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--input" || arg == "--character") {
            input_path = std::filesystem::path(next_val(arg));
        } else if (arg == "--project") {
            project_dir = std::filesystem::path(next_val(arg));
        } else if (arg == "--tex-res") {
            try {
                options.texture_resolution = static_cast<std::uint32_t>(
                    std::stoul(std::string(next_val(arg))));
            } catch (...) {
                std::cerr << "Invalid --tex-res value.\n";
                return 2;
            }
        } else if (arg == "--no-voice") {
            options.bake_voice_lines = false;
        } else if (arg == "--no-prefabs") {
            options.generate_prefabs = false;
        } else if (arg == "--no-loc") {
            options.generate_localization = false;
        } else if (arg == "--verify") {
            verify = true;
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--json") {
            json_out = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 2;
        }
    }

    if (input_path.empty() || (!dry_run && project_dir.empty())) {
        std::cerr << "vcr_char_cooker requires --input <file.vcrchar.json> and --project <dir>.\n";
        return 2;
    }

    vcr::CharacterPackage pkg{};
    std::string err;
    if (!vcr::load_character_package_file(input_path, pkg, err)) {
        std::cerr << "Failed to load character package: " << err << "\n";
        return 1;
    }

    if (dry_run) {
        const auto mesh = vcr::build_character_mesh(pkg);
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            std::cerr << "Dry-run build failed: empty mesh.\n";
            return 1;
        }
        if (json_out) {
            std::cout << Json{
                {"status", "ok"},
                {"dry_run", true},
                {"slug", pkg.identity.slug},
                {"vertices", mesh.vertices.size()},
                {"triangles", mesh.indices.size() / 3U},
                {"bones", mesh.skeleton.bones.size()},
                {"submeshes", mesh.submeshes.size()},
            }.dump(2) << "\n";
        } else {
            std::cout << "Dry-run valid: " << pkg.identity.slug
                      << " (" << mesh.vertices.size() << " verts, "
                      << (mesh.indices.size() / 3U) << " tris, "
                      << mesh.skeleton.bones.size() << " bones)\n";
        }
        return 0;
    }

    vcr::CharacterCookReport report{};
    if (!vcr::cook_character_package_to_project(pkg, project_dir, options, report, err)) {
        std::cerr << "Cook failed: " << err << "\n";
        return 1;
    }

    if (verify && !verify_cooked_outputs(project_dir, report, err)) {
        std::cerr << "Verification failed: " << err << "\n";
        return 1;
    }

    if (json_out) {
        std::cout << Json{
            {"status", "ok"},
            {"verified", verify},
            {"slug", report.character_slug},
            {"model_asset", report.model_asset_relative.generic_string()},
            {"dialogue", report.dialogue_relative.generic_string()},
            {"npc_prefab", report.npc_prefab_relative.generic_string()},
            {"player_prefab", report.player_prefab_relative.generic_string()},
            {"loc_en", report.loc_en_relative.generic_string()},
            {"loc_ru", report.loc_ru_relative.generic_string()},
            {"vertices", report.vertex_count},
            {"triangles", report.triangle_count},
            {"bones", report.bone_count},
            {"submeshes", report.submesh_count},
            {"audio_clips", report.audio_assets_relative.size()},
            {"animation_clips", report.animation_clips_relative.size()},
            {"cooked_files", report.cooked_file_count},
        }.dump(2) << "\n";
    } else {
        std::cout << "Cooked '" << report.character_slug << "' ("
                  << report.cooked_file_count << " files, "
                  << report.vertex_count << " verts, "
                  << report.triangle_count << " tris, "
                  << report.bone_count << " bones"
                  << (verify ? ", VERIFIED" : "") << ")\n"
                  << "  Model:  " << report.model_asset_relative.generic_string() << "\n"
                  << "  NPC:    " << report.npc_prefab_relative.generic_string() << "\n"
                  << "  Player: " << report.player_prefab_relative.generic_string() << "\n"
                  << "  Dlg:    " << report.dialogue_relative.generic_string() << "\n";
    }
    return 0;
}
