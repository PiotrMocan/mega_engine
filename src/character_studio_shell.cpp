#include "character_studio_shell.hpp"

#include "icon_system.hpp"
#include "png_encoder.hpp"
#include "runtime_assets.hpp"
#include "vulkan_common.hpp"
#include "window_icon_sdl.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <utility>

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

void copy_cstr(char* dst, const std::size_t dst_size, const std::string_view src) noexcept {
    if (dst == nullptr || dst_size == 0U) {
        return;
    }
    const std::size_t n = std::min(src.size(), dst_size - 1U);
    if (n > 0U) {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
}

[[nodiscard]] std::optional<float> parse_finite_float(const std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const std::string copy(text);
    char* end_ptr = nullptr;
    const float value = std::strtof(copy.c_str(), &end_ptr);
    if (end_ptr == nullptr || end_ptr == copy.c_str()) {
        return std::nullopt;
    }
    while (*end_ptr != '\0' && std::isspace(static_cast<unsigned char>(*end_ptr)) != 0) {
        ++end_ptr;
    }
    if (*end_ptr != '\0' || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] CharacterPackage make_studio_character_template(
    const std::string_view slug,
    const std::string_view display_name,
    const CharacterGender gender,
    const CharacterRole role) {
    const std::string clean_slug = sanitize_character_slug(slug);
    CharacterPackage pkg = make_default_character_package(
        clean_slug.empty() ? "hero" : clean_slug,
        display_name.empty() ? "New Character" : display_name);
    pkg.identity.role = role;
    pkg.body.gender = gender;
    if (gender == CharacterGender::female) {
        pkg.body.height_meters = 1.70F;
        pkg.body.shoulder_width = 0.44F;
        pkg.body.waist_width = 0.42F;
        pkg.body.hip_width = 0.54F;
        pkg.body.musculature = 0.34F;
        pkg.body.lip_tint = 0.52F;
        pkg.wardrobe.hair_style = HairStyle::bob;
        pkg.wardrobe.has_beard = false;
        pkg.voice.pitch_hz = 195.0F;
    } else {
        pkg.wardrobe.hair_style = HairStyle::short_crop;
    }
    sanitize_character_package(pkg);
    return pkg;
}

[[nodiscard]] ImTextureID studio_imgui_texture_id(
    const VkDescriptorSet descriptor_set) noexcept {
    return static_cast<ImTextureID>(
        reinterpret_cast<std::uintptr_t>(descriptor_set));
}

void studio_check_imgui_vk_result(const VkResult result) {
    if (result != VK_SUCCESS) {
        check_vk(result, "Character Studio Dear ImGui Vulkan backend", __FILE__, __LINE__);
    }
}

struct ViewportRay final {
    glm::vec3 origin{0.0F};
    glm::vec3 direction{0.0F, 0.0F, -1.0F};
};

[[nodiscard]] std::optional<ViewportRay> compute_viewport_ray(
    const ImVec2 mouse,
    const ImVec2 image_min,
    const ImVec2 image_max,
    const FlyCamera& camera,
    const float vertical_fov_degrees) noexcept {
    const float width = image_max.x - image_min.x;
    const float height = image_max.y - image_min.y;
    if (width <= 1.0F || height <= 1.0F ||
        mouse.x < image_min.x || mouse.x > image_max.x ||
        mouse.y < image_min.y || mouse.y > image_max.y) {
        return std::nullopt;
    }
    const float ndc_x = ((mouse.x - image_min.x) / width) * 2.0F - 1.0F;
    // ImGui Y is top-down while camera up is +Y.
    const float ndc_y = 1.0F - ((mouse.y - image_min.y) / height) * 2.0F;
    const float tan_half_fov = std::tan(glm::radians(vertical_fov_degrees) * 0.5F);
    const float aspect = width / height;
    const glm::vec3 dir = glm::normalize(
        camera.forward() +
        camera.right() * (ndc_x * aspect * tan_half_fov) +
        camera.up() * (ndc_y * tan_half_fov));
    return ViewportRay{camera.position(), dir};
}

[[nodiscard]] std::optional<ImVec2> project_world_to_screen(
    const glm::vec3& world_pos,
    const FlyCamera& camera,
    const float vertical_fov_degrees,
    const ImVec2 image_min,
    const ImVec2 image_max) noexcept {
    const float width = image_max.x - image_min.x;
    const float height = image_max.y - image_min.y;
    if (width <= 1.0F || height <= 1.0F) {
        return std::nullopt;
    }
    const glm::vec3 rel = world_pos - camera.position();
    const float depth = glm::dot(rel, camera.forward());
    if (depth <= 0.05F) {
        return std::nullopt;
    }
    const float tan_half_fov = std::tan(glm::radians(vertical_fov_degrees) * 0.5F);
    const float aspect = width / height;
    const float ndc_x = glm::dot(rel, camera.right()) / (depth * aspect * tan_half_fov);
    const float ndc_y = glm::dot(rel, camera.up()) / (depth * tan_half_fov);
    if (std::abs(ndc_x) > 1.25F || std::abs(ndc_y) > 1.25F) {
        return std::nullopt;
    }
    const float sx = image_min.x + (ndc_x * 0.5F + 0.5F) * width;
    const float sy = image_min.y + (0.5F - ndc_y * 0.5F) * height;
    return ImVec2{sx, sy};
}

[[nodiscard]] std::filesystem::path resolve_default_project_directory(
    const std::filesystem::path& explicit_dir) {
    if (!explicit_dir.empty()) {
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::weakly_canonical(explicit_dir, ec);
        return ec ? explicit_dir : abs;
    }
    if (const auto asset_root = find_runtime_asset_root()) {
        if (!asset_root->project_root.empty()) {
            return asset_root->project_root;
        }
        return asset_root->path.parent_path();
    }
    return std::filesystem::current_path();
}

void write_mcp_response(const Json& response) {
    const std::string line = response.dump();
    std::fwrite(line.data(), 1U, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

[[nodiscard]] Json character_summary_json(
    const CharacterPackage& pkg,
    const std::filesystem::path& char_path,
    const std::filesystem::path& project_dir,
    const CharacterMeshBuildResult& mesh,
    const CharacterStudioWorkspace workspace,
    const bool last_cook_verified) {
    Json clip_names = Json::array();
    clip_names.push_back("idle");
    clip_names.push_back("walk");
    clip_names.push_back("run");
    clip_names.push_back("talk");
    return Json{
        {"slug", pkg.identity.slug},
        {"display_name", pkg.identity.display_name},
        {"role", std::string(to_token(pkg.identity.role))},
        {"gender", std::string(to_token(pkg.body.gender))},
        {"archetype", std::string(to_token(pkg.body.archetype))},
        {"height_meters", pkg.body.height_meters},
        {"sculpt_strokes", pkg.sculpt_strokes.size()},
        {"paint_strokes", pkg.paint_strokes.size()},
        {"dialogue_nodes", pkg.dialogue.nodes.size()},
        {"top_style", std::string(to_token(pkg.wardrobe.top_style))},
        {"bottom_style", std::string(to_token(pkg.wardrobe.bottom_style))},
        {"footwear_style", std::string(to_token(pkg.wardrobe.footwear_style))},
        {"hair_style", std::string(to_token(pkg.wardrobe.hair_style))},
        {"vertex_count", mesh.vertices.size()},
        {"triangle_count", mesh.indices.size() / 3U},
        {"bone_count", mesh.skeleton.bones.size()},
        {"submesh_count", mesh.submeshes.size()},
        {"workspace", std::string(to_token(workspace))},
        {"last_cook_verified", last_cook_verified},
        {"character_path", char_path.generic_string()},
        {"project_directory", project_dir.generic_string()},
        {"default_clips", clip_names},
    };
}

} // namespace

std::string_view to_token(const CharacterStudioWorkspace workspace) noexcept {
    switch (workspace) {
    case CharacterStudioWorkspace::anatomy_groom:
        return "gen";
    case CharacterStudioWorkspace::wardrobe_paint:
        return "outfit";
    case CharacterStudioWorkspace::acting_dialogue:
        return "act";
    case CharacterStudioWorkspace::cook_export:
        return "cook";
    }
    return "gen";
}

std::optional<CharacterStudioWorkspace> parse_character_studio_workspace(
    const std::string_view token) noexcept {
    const std::string lower = ascii_lower(token);
    if (lower == "gen" || lower == "anatomy" || lower == "1") {
        return CharacterStudioWorkspace::anatomy_groom;
    }
    if (lower == "outfit" || lower == "wardrobe" || lower == "paint" || lower == "2") {
        return CharacterStudioWorkspace::wardrobe_paint;
    }
    if (lower == "act" || lower == "acting" || lower == "dialogue" || lower == "3") {
        return CharacterStudioWorkspace::acting_dialogue;
    }
    if (lower == "cook" || lower == "export" || lower == "verify" || lower == "4") {
        return CharacterStudioWorkspace::cook_export;
    }
    return std::nullopt;
}

bool CharacterStudioShell::ScopedTempPreviewDir::create(
    std::string_view prefix,
    std::string& error) {
    reset();
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec || base.empty()) {
        error = "Failed to locate system temp directory for studio preview.";
        return false;
    }
    const std::uint64_t stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    dir = base / (std::string(prefix) + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir / "assets", ec);
    if (ec) {
        error = "Failed to create temporary preview workspace: " + ec.message();
        dir.clear();
        return false;
    }
    return true;
}

void CharacterStudioShell::ScopedTempPreviewDir::reset() noexcept {
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        dir.clear();
    }
}

CharacterStudioShell::~CharacterStudioShell() {
    shutdown_gui();
}

bool CharacterStudioShell::parse_cli_args(
    int argc,
    char** argv,
    CharacterStudioCliArgs& out_args,
    std::string& out_error) {
    out_args = CharacterStudioCliArgs{};
    out_error.clear();

    const auto require_value = [&](int& idx, std::string_view flag) -> std::optional<std::string> {
        if (idx + 1 >= argc) {
            out_error = "Missing value after option " + std::string(flag);
            return std::nullopt;
        }
        ++idx;
        return std::string(argv[idx]);
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i] != nullptr ? std::string_view(argv[i]) : std::string_view{};
        if (arg.empty()) {
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            out_args.help_requested = true;
            continue;
        }
        if (arg == "--cli") {
            out_args.cli_mode = true;
            continue;
        }
        if (arg == "--mcp-stdio") {
            out_args.mcp_stdio = true;
            continue;
        }
        if (arg == "--cook") {
            out_args.cook_after_cli = true;
            continue;
        }
        if (arg == "--validation") {
            out_args.validation = true;
            continue;
        }
        if (arg == "--no-validation") {
            out_args.validation = false;
            continue;
        }
        if (arg == "--offline") {
            out_args.ollama.enable_network = false;
            continue;
        }
        if (arg == "--focus-face") {
            out_args.focus_face = true;
            continue;
        }
        if (arg == "--verify" || arg == "--dry-run" || arg == "--json" ||
            arg == "--no-voice" || arg == "--no-prefabs" || arg == "--no-loc" ||
            arg == "--cape" || arg == "--no-cape" ||
            arg == "--pauldrons" || arg == "--no-pauldrons" ||
            arg == "--gloves" || arg == "--no-gloves" ||
            arg == "--belt" || arg == "--no-belt" ||
            arg == "--beard" || arg == "--no-beard" ||
            arg == "--studio-underwear" || arg == "--no-studio-underwear" ||
            arg == "--occlude-body" || arg == "--no-occlude-body" ||
            arg == "--rig-overlay") {
            out_args.options[std::string(arg.substr(2U))] = "true";
            continue;
        }
        if (arg == "--tab" || arg == "--workspace") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            const auto parsed = parse_character_studio_workspace(*val);
            if (!parsed) {
                out_error = "Unknown workspace tab for " + std::string(arg) + ": " + *val;
                return false;
            }
            out_args.initial_workspace = *parsed;
            continue;
        }
        if (arg == "--screenshot") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            out_args.screenshot_path = std::filesystem::path(*val);
            continue;
        }
        if (arg == "--project") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            out_args.project_directory = std::filesystem::path(*val);
            continue;
        }
        if (arg == "--character" || arg == "--input") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            const std::filesystem::path p(*val);
            if (p.has_parent_path() || p.extension() == ".json") {
                out_args.character_path = p;
            } else {
                out_args.character_slug = sanitize_character_slug(*val);
            }
            continue;
        }
        if (arg == "--slug") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            out_args.character_slug = sanitize_character_slug(*val);
            continue;
        }
        if (arg == "--smoke-frames") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            try {
                out_args.smoke_frames = static_cast<std::uint32_t>(std::stoul(*val));
            } catch (...) {
                out_error = "Invalid integer for --smoke-frames: " + *val;
                return false;
            }
            continue;
        }
        if (arg == "--width") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            try {
                out_args.width = std::clamp(
                    static_cast<std::uint32_t>(std::stoul(*val)), 320U, 3840U);
            } catch (...) {
                out_error = "Invalid integer for --width: " + *val;
                return false;
            }
            continue;
        }
        if (arg == "--height") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            try {
                out_args.height = std::clamp(
                    static_cast<std::uint32_t>(std::stoul(*val)), 240U, 2160U);
            } catch (...) {
                out_error = "Invalid integer for --height: " + *val;
                return false;
            }
            continue;
        }
        if (arg == "--ollama-host") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            out_args.ollama.host = *val;
            continue;
        }
        if (arg == "--ollama-port") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            try {
                const unsigned long parsed = std::stoul(*val);
                if (parsed == 0UL || parsed > 65535UL) {
                    out_error = "Ollama port out of range: " + *val;
                    return false;
                }
                out_args.ollama.port = static_cast<std::uint16_t>(parsed);
            } catch (...) {
                out_error = "Invalid integer for --ollama-port: " + *val;
                return false;
            }
            continue;
        }
        if (arg == "--ollama-model") {
            const auto val = require_value(i, arg);
            if (!val) {
                return false;
            }
            out_args.ollama.model = *val;
            continue;
        }
        if (arg.starts_with("--")) {
            const std::size_t eq = arg.find('=');
            if (eq != std::string_view::npos) {
                const std::string key(arg.substr(2U, eq - 2U));
                const std::string val(arg.substr(eq + 1U));
                out_args.options[key] = val;
            } else {
                const std::string key(arg.substr(2U));
                if (i + 1 < argc && argv[i + 1] != nullptr &&
                    std::string_view(argv[i + 1]).starts_with("--") == false) {
                    ++i;
                    out_args.options[key] = argv[i];
                } else {
                    out_args.options[key] = "true";
                }
            }
            continue;
        }
        out_args.positional.emplace_back(arg);
    }

    if (out_args.character_path.empty() && !out_args.character_slug.empty()) {
        out_args.project_directory =
            resolve_default_project_directory(out_args.project_directory);
        out_args.character_path =
            default_character_package_path(out_args.project_directory, out_args.character_slug);
    } else if (out_args.project_directory.empty() && !out_args.character_path.empty()) {
        std::filesystem::path parent = out_args.character_path.parent_path();
        if (parent.filename() == "characters" && parent.has_parent_path()) {
            parent = parent.parent_path();
        }
        out_args.project_directory =
            resolve_default_project_directory(parent);
    }
    if (out_args.project_directory.empty()) {
        out_args.project_directory = resolve_default_project_directory({});
    }
    return true;
}

int CharacterStudioShell::run(
    int argc,
    char** argv,
    ICharacterStudioTool& tool) {
    CharacterStudioCliArgs args;
    std::string parse_error;
    if (!parse_cli_args(argc, argv, args, parse_error)) {
        std::cerr << tool.meta().cli_program_name << ": " << parse_error << "\n";
        return 2;
    }
    if (args.help_requested) {
        tool.print_cli_help();
        return 0;
    }
    if (args.cli_mode || args.has_flag("dry-run")) {
        return tool.run_cli(args);
    }

    initialize_gui(args, tool);

    auto prev_clock = std::chrono::steady_clock::now();
    last_metrics_update_ = prev_clock;

    while (running_) {
        const auto frame_begin = std::chrono::steady_clock::now();
        const float dt_seconds = std::clamp(
            std::chrono::duration<float>(frame_begin - prev_clock).count(),
            0.0001F,
            0.1F);
        prev_clock = frame_begin;

        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            handle_sdl_event(event, tool);
        }
        if (!running_) {
            break;
        }

        const SDL_WindowFlags win_flags = SDL_GetWindowFlags(window_.get());
        int win_w = 0;
        int win_h = 0;
        SDL_GetWindowSizeInPixels(window_.get(), &win_w, &win_h);
        if ((win_flags & SDL_WINDOW_MINIMIZED) != 0U || win_w <= 0 || win_h <= 0) {
            // Only permitted sleep in AGENTS.md when 0x0 or minimized.
            SDL_Delay(16U);
            continue;
        }

        audio_.update(dt_seconds);
        if (active_speech_) {
            speech_elapsed_ += dt_seconds;
            if (speech_elapsed_ > active_speech_->duration_seconds + 0.08F) {
                active_speech_.reset();
                speech_elapsed_ = 0.0F;
                speech_voice_ = AudioVoiceHandle{};
            }
        }

        if (!preview_paused_) {
            preview_time_ += dt_seconds;
        }

        poll_ai_completion(tool);
        tool.on_frame_tick(*this, dt_seconds);

        if (renderer_.poll_assets()) {
            submitted_scene_revision_ = 0U;
        }
        if (preview_rebuild_requested_) {
            rebuild_preview_assets(true);
            preview_rebuild_requested_ = false;
        }

        const auto view = preview_scene_.build_render_view(
            scene_instances_, scene_models_, scene_lights_);
        if (view.revision != submitted_scene_revision_) {
            std::string scene_err;
            const std::filesystem::path asset_root = preview_temp_dir_.dir / "assets";
            if (!renderer_.update_editor_scene(view, asset_root, scene_err) &&
                !scene_err.empty()) {
                set_status("Preview scene load error: " + scene_err, EditorTone::danger);
            }
            submitted_scene_revision_ = view.revision;
        }

        if (preview_node_id_ && !preview_mesh_.skeleton.bones.empty()) {
            FacialPreviewParams effective_facial = preview_facial_;
            if (active_speech_) {
                const VisemeKeyframe vk = sample_viseme_track(
                    active_speech_->visemes, speech_elapsed_);
                effective_facial.jaw_open = std::max(effective_facial.jaw_open, vk.jaw_open);
                effective_facial.lip_round = std::max(effective_facial.lip_round, vk.lip_round);
                effective_facial.lip_wide = std::max(effective_facial.lip_wide, vk.lip_wide);
                effective_facial.upper_lip_raise =
                    std::max(effective_facial.upper_lip_raise, vk.upper_lip_raise);
            }
            if (bind_pose_override_) {
                set_bind_pose(preview_mesh_.skeleton, preview_pose_);
                apply_facial_preview_pose(
                    preview_mesh_.skeleton, effective_facial, preview_pose_);
                recompute_skeleton_pose_matrices(
                    preview_mesh_.skeleton, preview_pose_);
            } else {
                evaluate_character_preview_pose(
                    preview_mesh_.skeleton,
                    preview_clips_,
                    preview_clip_name_,
                    preview_time_,
                    effective_facial,
                    preview_pose_);
            }
            renderer_.set_skinned_model_pose(
                preview_node_id_,
                preview_model_rel_,
                preview_pose_.active_skin_matrices());
        }

        if (frame_begin - last_metrics_update_ >= std::chrono::milliseconds(250)) {
            ui_metrics_ = renderer_.metrics_snapshot();
            last_metrics_update_ = frame_begin;
        }

        synchronize_renderer_textures();
        render_gui_frame(tool, dt_seconds);
        ++rendered_frames_;

        if (!screenshot_output_path_.empty()) {
            const std::size_t expected_textures = preview_mesh_.submeshes.size() * 3U;
            const bool assets_ready =
                rendered_frames_ >= 12U &&
                renderer_.resident_editor_model_count() > 0U &&
                renderer_.resident_editor_texture_count() >= expected_textures;
            if (!screenshot_requested_once_ && (assets_ready || rendered_frames_ >= 45U)) {
                screenshot_requested_once_ = renderer_.request_screenshot();
            }
            if (auto cap = renderer_.take_completed_screenshot()) {
                if (!*cap) {
                    std::cerr << "Screenshot capture failed: " << cap->error << "\n";
                    screenshot_failed_ = true;
                } else {
                    const PngEncodeResult png = encode_png_rgba(
                        cap->rgba, cap->width, cap->height);
                    if (!png) {
                        std::cerr << "Screenshot PNG encode failed: " << png.error << "\n";
                        screenshot_failed_ = true;
                    } else {
                        std::error_code ec;
                        const auto parent = screenshot_output_path_.parent_path();
                        if (!parent.empty()) {
                            std::filesystem::create_directories(parent, ec);
                        }
                        std::ofstream ofs(
                            screenshot_output_path_, std::ios::binary | std::ios::trunc);
                        if (!ofs.write(
                                reinterpret_cast<const char*>(png.bytes.data()),
                                static_cast<std::streamsize>(png.bytes.size()))) {
                            std::cerr << "Failed writing screenshot: "
                                      << screenshot_output_path_.string() << "\n";
                            screenshot_failed_ = true;
                        }
                    }
                }
                running_ = false;
            } else if (rendered_frames_ >= 90U) {
                std::cerr << "Timed out waiting for screenshot capture.\n";
                screenshot_failed_ = true;
                running_ = false;
            }
        }

        if (smoke_frames_remaining_ > 0U) {
            --smoke_frames_remaining_;
            if (smoke_frames_remaining_ == 0U) {
                running_ = false;
            }
        }
    }

    shutdown_gui();
    return screenshot_failed_ ? 1 : 0;
}

void CharacterStudioShell::initialize_gui(
    const CharacterStudioCliArgs& args,
    ICharacterStudioTool& tool) {
    project_directory_ = resolve_default_project_directory(args.project_directory);
    character_path_ = args.character_path.empty()
        ? default_character_package_path(project_directory_, args.character_slug)
        : args.character_path;
    ollama_ = args.ollama;
    active_workspace_ = args.initial_workspace;
    window_title_base_ = std::string(tool.meta().window_title);
    smoke_frames_remaining_ = args.smoke_frames;
    screenshot_output_path_ = args.screenshot_path;
    screenshot_requested_once_ = false;
    screenshot_failed_ = false;
    rendered_frames_ = 0U;
    mcp_stdio_enabled_ = args.mcp_stdio;

    std::string err;
    if (!preview_temp_dir_.create(tool.meta().tool_id, err)) {
        throw std::runtime_error(err);
    }

    if (!load_or_create_character_package(
            character_path_, args.character_slug, package_, err)) {
        package_ = make_default_character_package(args.character_slug, args.character_slug);
        set_status("Created fresh character template: " + err, EditorTone::warning);
    } else {
        set_status("Loaded " + character_path_.filename().string(), EditorTone::success);
    }
    package_dirty_ = false;
    last_cook_verified_ = false;
    refresh_project_characters();
    tool.on_package_loaded(package_);
    tool.on_workspace_changed(*this, active_workspace_);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    sdl_initialized_ = true;

    window_.handle = SDL_CreateWindow(
        window_title_base_.c_str(),
        static_cast<int>(args.width),
        static_cast<int>(args.height),
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_.handle == nullptr) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
    update_window_title();
    main_window_id_ = SDL_GetWindowID(window_.handle);
    apply_window_builtin_icon(window_.handle, BuiltinIconKind::editor);

    context_.initialize(window_.handle, args.validation);

    LightingSettings studio_lighting{};
    studio_lighting.dynamic = true;
    studio_lighting.voxel_gi = false;
    studio_lighting.cycle = false;
    studio_lighting.hour = 13.0;
    studio_lighting.sky_intensity = 0.48F;

    AntiAliasingSettings studio_aa{};
    studio_aa.method = AntiAliasingMethod::smaa;

    renderer_.initialize(
        context_,
        window_.handle,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        RunMode::scene,
        1U,
        0U,
        studio_aa,
        !args.screenshot_path.empty(),
        true,
        true,
        studio_lighting,
        RenderFixture::none,
        ModelVertexFormat::automatic);
    static_cast<void>(renderer_.set_subsurface_mode(SubsurfaceMode::full_resolution));
    static_cast<void>(renderer_.set_tone_mapper(ToneMapper::gt7));
    ExposureSettings studio_exposure{};
    studio_exposure.mode = ExposureMode::fixed;
    studio_exposure.compensation_ev = 0.15F;
    static_cast<void>(renderer_.set_exposure_settings(studio_exposure));

    // Audio failure in headless/no-speaker test environments stays non-fatal.
    std::string audio_err;
    if (!audio_.initialize(AudioEngineOptions{}, audio_err)) {
        set_status("Audio initialized offline: " + audio_err, EditorTone::warning);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imgui_initialized_ = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;

    ui_kit_.initialize();

    if (!ImGui_ImplSDL3_InitForVulkan(window_.handle)) {
        throw std::runtime_error("ImGui_ImplSDL3_InitForVulkan failed.");
    }
    initialize_vulkan_imgui();
    static_cast<void>(renderer_.prepare_editor_viewport(VkExtent2D{800U, 720U}));
    register_viewport_texture();

    // Pre-reserve steady-state scene build buffers (AGENTS.md Law 5).
    scene_instances_.reserve(16U);
    scene_models_.reserve(8U);
    scene_lights_.reserve(8U);

    // Build clean 3-point front portrait studio lighting scene on a slate turntable plinth.
    preview_scene_ = SceneDocument{};
    const SceneNodeId env_id = preview_scene_.create_node("StudioSky", SceneNodeKind::environment);
    static_cast<void>(env_id);
    const SceneNodeId floor_id = preview_scene_.create_node("StudioPlinth", SceneNodeKind::ground);
    if (auto n = preview_scene_.node(floor_id)) {
        n->get().transform.translation = glm::vec3(0.0F, -0.025F, 0.0F);
        n->get().transform.scale = glm::vec3(4.2F, 0.05F, 4.2F);
        n->get().material.base_color = glm::vec4(0.11F, 0.12F, 0.14F, 1.0F);
        n->get().material.roughness = 0.72F;
        n->get().material.metallic = 0.08F;
    }
    const SceneNodeId key_id = preview_scene_.create_node("StudioKey", SceneNodeKind::point_light);
    if (auto n = preview_scene_.node(key_id)) {
        n->get().transform.translation = glm::vec3(1.55F, 1.92F, 2.35F);
        n->get().light.color = glm::vec3(1.00F, 0.96F, 0.92F);
        n->get().light.intensity_candela = 18.0F;
        n->get().light.range = 10.0F;
    }
    const SceneNodeId fill_id = preview_scene_.create_node("StudioFill", SceneNodeKind::point_light);
    if (auto n = preview_scene_.node(fill_id)) {
        n->get().transform.translation = glm::vec3(-1.95F, 1.48F, 2.30F);
        n->get().light.color = glm::vec3(0.90F, 0.95F, 1.00F);
        n->get().light.intensity_candela = 7.5F;
        n->get().light.range = 10.0F;
    }
    const SceneNodeId rim_id = preview_scene_.create_node("StudioRim", SceneNodeKind::point_light);
    if (auto n = preview_scene_.node(rim_id)) {
        n->get().transform.translation = glm::vec3(-1.15F, 2.25F, -1.45F);
        n->get().light.color = glm::vec3(0.88F, 0.93F, 1.00F);
        n->get().light.intensity_candela = 4.2F;
        n->get().light.range = 7.5F;
    }
    preview_node_id_ = preview_scene_.create_node("CharacterPreview", SceneNodeKind::model);

    rebuild_preview_assets(true);
    preview_rebuild_requested_ = false;
    if (args.focus_face) {
        focus_camera_face();
    } else {
        focus_camera_full_body();
    }

    // Flush initial package-loaded defaults before applying deterministic CLI preview overrides.
    tool.on_frame_tick(*this, 0.0F);

    if (const auto clip_opt = args.find_option("clip")) {
        if (!clip_opt->empty()) {
            set_preview_clip(*clip_opt);
        }
    }
    if (const auto time_opt = args.find_option("anim-time")) {
        if (const auto parsed = parse_finite_float(*time_opt)) {
            set_preview_time(std::max(0.0F, *parsed));
            set_preview_paused(true);
        }
    }
    if (const auto yaw_opt = args.find_option("yaw-deg")) {
        if (const auto yaw_deg = parse_finite_float(*yaw_opt)) {
            orbit_azimuth_ = glm::radians(*yaw_deg);
            update_orbit_camera();
        }
    }
    if (args.has_flag("rig-overlay")) {
        set_show_skeleton_overlay(true);
    }

    if (args.ollama.enable_network) {
        trigger_ollama_probe_async();
    }
    if (mcp_stdio_enabled_) {
        start_mcp_reader();
    }
    running_ = true;
}

void CharacterStudioShell::shutdown_gui() noexcept {
    running_ = false;
    stop_mcp_reader();
    ai_thread_.request_stop();
    if (ai_thread_.joinable()) {
        ai_thread_.join();
    }

    if (context_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context_.device());
    }

    unregister_viewport_texture();
    shutdown_vulkan_imgui();
    ui_kit_.shutdown();

    if (imgui_initialized_ && ImGui::GetCurrentContext() != nullptr) {
        if (ImGui::GetIO().BackendPlatformUserData != nullptr) {
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext();
        imgui_initialized_ = false;
    }

    audio_.shutdown();
    renderer_.shutdown();
    context_.shutdown();

    if (window_.handle != nullptr) {
        SDL_DestroyWindow(window_.handle);
        window_.handle = nullptr;
    }
    if (sdl_initialized_) {
        SDL_Quit();
        sdl_initialized_ = false;
    }
    preview_temp_dir_.reset();
}

void CharacterStudioShell::initialize_vulkan_imgui() {
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
    init_info.ImageCount = static_cast<std::uint32_t>(renderer_.swapchain().images().size());
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo = imgui_rendering_info_;
    init_info.UseDynamicRendering = true;
    init_info.CheckVkResultFn = studio_check_imgui_vk_result;
    init_info.MinAllocationSize = 1024U * 1024U;
    if (!ImGui_ImplVulkan_Init(&init_info)) {
        throw std::runtime_error("ImGui_ImplVulkan_Init failed.");
    }
    vulkan_imgui_initialized_ = true;
    known_swapchain_generation_ = renderer_.swapchain_generation();
    known_swapchain_image_count_ =
        static_cast<std::uint32_t>(renderer_.swapchain().images().size());
}

void CharacterStudioShell::shutdown_vulkan_imgui() noexcept {
    if (vulkan_imgui_initialized_) {
        ImGui_ImplVulkan_Shutdown();
        vulkan_imgui_initialized_ = false;
    }
}

void CharacterStudioShell::register_viewport_texture() {
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

void CharacterStudioShell::unregister_viewport_texture() noexcept {
    if (viewport_texture_ != VK_NULL_HANDLE && vulkan_imgui_initialized_) {
        ImGui_ImplVulkan_RemoveTexture(viewport_texture_);
    }
    viewport_texture_ = VK_NULL_HANDLE;
}

void CharacterStudioShell::synchronize_renderer_textures() {
    if (renderer_.swapchain_generation() != known_swapchain_generation_) {
        const auto current_count =
            static_cast<std::uint32_t>(renderer_.swapchain().images().size());
        if (renderer_.swapchain().color_format() != imgui_color_format_ ||
            current_count != known_swapchain_image_count_) {
            vkDeviceWaitIdle(context_.device());
            unregister_viewport_texture();
            shutdown_vulkan_imgui();
            ImGui_ImplSDL3_Shutdown();
            if (!ImGui_ImplSDL3_InitForVulkan(window_.handle)) {
                throw std::runtime_error("Unable to restart ImGui SDL3 backend after resize.");
            }
            initialize_vulkan_imgui();
            register_viewport_texture();
            return;
        }
        known_swapchain_generation_ = renderer_.swapchain_generation();
    }
    if (renderer_.editor_viewport_generation() != known_viewport_generation_) {
        unregister_viewport_texture();
        register_viewport_texture();
    }
}

void CharacterStudioShell::rebuild_preview_assets(bool full_gpu_reload) {
    sanitize_character_package(package_);
    preview_mesh_ = build_character_mesh(package_);
    preview_clips_ = generate_character_animation_clips(package_, preview_mesh_.skeleton);
    set_bind_pose(preview_mesh_.skeleton, preview_pose_);

    if (!full_gpu_reload || preview_temp_dir_.dir.empty()) {
        return;
    }

    renderer_.reset_editor_asset_cache();

    CharacterCookOptions opts{};
    opts.texture_resolution = 1024U;
    opts.bake_voice_lines = false;
    opts.generate_prefabs = false;
    opts.generate_localization = false;
    opts.overwrite_existing = true;

    CharacterCookReport preview_report{};
    std::string cook_err;
    if (!cook_character_package_to_project(
            package_, preview_temp_dir_.dir, opts, preview_report, cook_err)) {
        set_status("Preview bake error: " + cook_err, EditorTone::danger);
        return;
    }

    preview_model_rel_ = preview_report.model_asset_relative.generic_string();
    if (auto node = preview_scene_.node(preview_node_id_)) {
        node->get().asset_path = preview_model_rel_;
        node->get().transform = SceneTransform{};
    }
    preview_scene_.mark_content_changed();
    submitted_scene_revision_ = 0U;
}

void CharacterStudioShell::update_orbit_camera() noexcept {
    orbit_distance_ = std::clamp(orbit_distance_, 0.35F, 6.5F);
    orbit_elevation_ = std::clamp(orbit_elevation_, -1.25F, 1.35F);

    const float cos_el = std::cos(orbit_elevation_);
    const glm::vec3 offset(
        cos_el * std::sin(orbit_azimuth_),
        std::sin(orbit_elevation_),
        cos_el * std::cos(orbit_azimuth_));
    const glm::vec3 eye = orbit_target_ + offset * orbit_distance_;
    const glm::vec3 forward = glm::normalize(orbit_target_ - eye);
    const float pitch = std::asin(std::clamp(forward.y, -0.99F, 0.99F));
    const float yaw = std::atan2(forward.z, forward.x);
    camera_ = FlyCamera(eye, yaw, pitch);
}

void CharacterStudioShell::focus_camera_full_body() noexcept {
    static_cast<void>(renderer_.set_vertical_fov_degrees(40.0F));
    const float h = std::clamp(package_.body.height_meters, 1.2F, 2.5F);
    orbit_target_ = glm::vec3(0.0F, h * 0.50F, 0.0F);
    orbit_distance_ = std::max(2.15F, h * 1.48F);
    orbit_azimuth_ = 0.20F;
    orbit_elevation_ = 0.08F;
    update_orbit_camera();
}

void CharacterStudioShell::focus_camera_face() noexcept {
    static_cast<void>(renderer_.set_vertical_fov_degrees(34.0F));
    const float h = std::clamp(package_.body.height_meters, 1.2F, 2.5F);
    glm::vec3 target(0.0F, h * 0.905F, 0.03F);
    if (preview_mesh_.skeleton.bones.size() > 44U) {
        const glm::vec3 eye_l = glm::vec3(
            glm::inverse(preview_mesh_.skeleton.bones[43].inverse_bind_matrix)[3]);
        const glm::vec3 eye_r = glm::vec3(
            glm::inverse(preview_mesh_.skeleton.bones[44].inverse_bind_matrix)[3]);
        const glm::vec3 eye_mid = 0.5F * (eye_l + eye_r);
        if (std::isfinite(eye_mid.y) && eye_mid.y > 0.5F) {
            target = eye_mid + glm::vec3(0.0F, -0.015F, 0.010F);
        }
    }
    orbit_target_ = target;
    orbit_distance_ = 0.56F;
    orbit_azimuth_ = 0.16F;
    orbit_elevation_ = 0.04F;
    update_orbit_camera();
}

void CharacterStudioShell::start_speech_preview(SynthesizedSpeechClip clip) {
    stop_speech_preview();
    if (clip.mono_pcm.empty()) {
        return;
    }
    AudioPlayParams params{};
    params.volume = 0.85F;
    std::string err;
    speech_voice_ = audio_.play_pcm_clip(
        clip.line_id, clip.mono_pcm, 1U, params, err);
    active_speech_ = std::move(clip);
    speech_elapsed_ = 0.0F;
}

void CharacterStudioShell::stop_speech_preview() noexcept {
    if (audio_.is_voice_active(speech_voice_)) {
        audio_.stop_voice(speech_voice_, 0.02F);
    }
    speech_voice_ = AudioVoiceHandle{};
    active_speech_.reset();
    speech_elapsed_ = 0.0F;
}

void CharacterStudioShell::set_status(std::string message, EditorTone tone) {
    status_message_ = std::move(message);
    status_tone_ = tone;
}

void CharacterStudioShell::trigger_ollama_probe_async() {
    if (ai_in_flight_.exchange(true)) {
        return;
    }
    const OllamaBridgeConfig cfg = ollama_;
    ai_thread_ = std::jthread([this, cfg](std::stop_token /*st*/) {
        const OllamaStatus probe = probe_ollama_status(cfg);
        std::lock_guard<std::mutex> lock(ai_mutex_);
        ai_completed_probe_ = probe;
        ai_completed_kind_ = PendingAiKind::probe;
    });
}

void CharacterStudioShell::enqueue_ai_generation(
    std::string description,
    AiTaskCallback worker_fn) {
    if (ai_in_flight_.exchange(true)) {
        set_status("AI worker busy; wait for current generation to finish.", EditorTone::warning);
        return;
    }
    set_status("AI generating: " + description + "...", EditorTone::info);
    CharacterPackage snapshot = package_;
    const OllamaBridgeConfig cfg = ollama_;
    ai_thread_ = std::jthread([this, cfg, snapshot = std::move(snapshot), fn = std::move(worker_fn)](
                                  std::stop_token /*st*/) mutable {
        CharacterAiGenerationResult result{};
        const bool ok = fn(cfg, snapshot, result);
        std::lock_guard<std::mutex> lock(ai_mutex_);
        ai_completed_package_ = std::move(snapshot);
        ai_completed_result_ = std::move(result);
        ai_completed_success_ = ok;
        ai_completed_kind_ = PendingAiKind::generate;
    });
}

void CharacterStudioShell::poll_ai_completion(ICharacterStudioTool& tool) {
    PendingAiKind kind = PendingAiKind::none;
    OllamaStatus probe{};
    CharacterPackage gen_pkg{};
    CharacterAiGenerationResult gen_res{};
    bool gen_ok = false;

    {
        std::lock_guard<std::mutex> lock(ai_mutex_);
        if (ai_completed_kind_ == PendingAiKind::none) {
            return;
        }
        kind = ai_completed_kind_;
        ai_completed_kind_ = PendingAiKind::none;
        if (kind == PendingAiKind::probe) {
            probe = std::move(ai_completed_probe_);
        } else {
            gen_pkg = std::move(ai_completed_package_);
            gen_res = std::move(ai_completed_result_);
            gen_ok = ai_completed_success_;
        }
    }

    if (ai_thread_.joinable()) {
        ai_thread_.join();
    }
    ai_in_flight_.store(false);

    if (kind == PendingAiKind::probe) {
        ollama_status_ = std::move(probe);
        if (ollama_status_.reachable && ollama_.model.empty() &&
            !ollama_status_.active_model.empty()) {
            ollama_.model = ollama_status_.active_model;
        }
        return;
    }

    if (!gen_ok) {
        set_status("AI error: " + gen_res.diagnostic, EditorTone::danger);
        return;
    }
    package_ = std::move(gen_pkg);
    package_dirty_ = true;
    preview_rebuild_requested_ = true;
    tool.on_package_loaded(package_);
    set_status(
        std::string(gen_res.used_ollama ? "[Ollama] " : "[Offline AI] ") + gen_res.summary,
        gen_res.used_ollama ? EditorTone::success : EditorTone::info);
}

void CharacterStudioShell::update_window_title() noexcept {
    title_dirty_state_ = package_dirty_;
    title_slug_state_ = package_.identity.slug;
    if (window_.handle == nullptr) {
        return;
    }
    const std::string title = window_title_base_ + " - " +
        package_.identity.display_name + " (" + package_.identity.slug + ")" +
        (package_dirty_ ? " *" : "");
    SDL_SetWindowTitle(window_.handle, title.c_str());
}

void CharacterStudioShell::refresh_project_characters() {
    project_characters_.clear();
    const auto discovered = discover_project_character_packages(project_directory_);
    auto make_label = [](const std::filesystem::path& p) {
        std::string name = p.filename().string();
        static constexpr std::string_view suffix = ".vcrchar.json";
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
        }
        return name;
    };
    for (const auto& p : discovered) {
        project_characters_.push_back(DiscoveredCharacterEntry{p, make_label(p)});
    }
    if (!character_path_.empty()) {
        std::error_code ec;
        const std::filesystem::path cur_norm =
            std::filesystem::weakly_canonical(character_path_, ec);
        const std::filesystem::path cur_cmp =
            ec ? character_path_.lexically_normal() : cur_norm;
        bool found = false;
        for (const auto& e : project_characters_) {
            std::error_code e_ec;
            const std::filesystem::path e_norm =
                std::filesystem::weakly_canonical(e.path, e_ec);
            const std::filesystem::path e_cmp = e_ec ? e.path.lexically_normal() : e_norm;
            if (e_cmp == cur_cmp) {
                found = true;
                break;
            }
        }
        if (!found) {
            project_characters_.push_back(
                DiscoveredCharacterEntry{character_path_, make_label(character_path_)});
        }
    }
}

bool CharacterStudioShell::load_character_file(
    const std::filesystem::path& path,
    ICharacterStudioTool& tool,
    std::string& error) {
    CharacterPackage loaded{};
    if (!load_character_package_file(path, loaded, error)) {
        set_status("Load failed: " + error, EditorTone::danger);
        return false;
    }
    stop_speech_preview();
    package_ = std::move(loaded);
    character_path_ = path;
    package_dirty_ = false;
    last_cook_report_.reset();
    last_cook_verified_ = false;
    preview_rebuild_requested_ = true;
    tool.on_package_loaded(package_);
    refresh_project_characters();
    update_window_title();
    set_status("Loaded " + character_path_.filename().string(), EditorTone::success);
    return true;
}

void CharacterStudioShell::apply_new_character(
    CharacterPackage pkg,
    const std::filesystem::path& path,
    ICharacterStudioTool& tool) {
    stop_speech_preview();
    sanitize_character_package(pkg);
    package_ = std::move(pkg);
    character_path_ = path;
    package_dirty_ = true;
    last_cook_report_.reset();
    last_cook_verified_ = false;
    preview_rebuild_requested_ = true;
    tool.on_package_loaded(package_);
    refresh_project_characters();
    update_window_title();
    set_status("Created new character '" + package_.identity.display_name + "'.", EditorTone::info);
}

bool CharacterStudioShell::save_character_as_slug(
    const std::string_view slug,
    std::string& error) {
    const std::string clean_slug = sanitize_character_slug(slug);
    package_.identity.slug = clean_slug.empty() ? package_.identity.slug : clean_slug;
    character_path_ = default_character_package_path(project_directory_, package_.identity.slug);
    return save_character(error);
}

bool CharacterStudioShell::save_character(std::string& error) {
    if (character_path_.empty()) {
        character_path_ = default_character_package_path(
            project_directory_, package_.identity.slug);
    }
    if (!save_character_package_file(character_path_, package_, error)) {
        set_status("Save failed: " + error, EditorTone::danger);
        return false;
    }
    package_dirty_ = false;
    refresh_project_characters();
    update_window_title();
    set_status("Saved " + character_path_.filename().string(), EditorTone::success);
    return true;
}

bool CharacterStudioShell::cook_to_project_with_options(
    const CharacterCookOptions& opts,
    const bool verify_after_cook,
    std::string& error) {
    if (!save_character(error)) {
        return false;
    }

    CharacterCookReport report{};
    if (!cook_character_package_to_project(
            package_, project_directory_, opts, report, error)) {
        last_cook_verified_ = false;
        set_status("Cook failed: " + error, EditorTone::danger);
        return false;
    }

    if (verify_after_cook) {
        if (!verify_cooked_character_outputs(project_directory_, report, error)) {
            last_cook_report_ = report;
            last_cook_verified_ = false;
            set_status("Cook verify failed: " + error, EditorTone::danger);
            return false;
        }
    }

    last_cook_report_ = report;
    last_cook_verified_ = verify_after_cook;
    set_status(
        std::string(verify_after_cook ? "Cooked & verified " : "Cooked ") +
            std::to_string(report.cooked_file_count) +
            " files into assets/characters/" + report.character_slug,
        EditorTone::success);
    return true;
}

bool CharacterStudioShell::cook_to_project(std::string& error) {
    CharacterCookOptions opts{};
    opts.texture_resolution = character_default_texture_size;
    opts.bake_voice_lines = true;
    opts.generate_prefabs = true;
    opts.generate_localization = true;
    opts.overwrite_existing = true;
    return cook_to_project_with_options(opts, true, error);
}

void CharacterStudioShell::handle_sdl_event(
    const SDL_Event& event,
    ICharacterStudioTool& tool) {
    if (imgui_initialized_) {
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
    if (event.type == SDL_EVENT_QUIT) {
        running_ = false;
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        event.window.windowID == main_window_id_) {
        running_ = false;
        return;
    }
    if ((event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
         event.type == SDL_EVENT_WINDOW_RESIZED) &&
        event.window.windowID == main_window_id_) {
        renderer_.request_resize();
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        imgui_initialized_ && !ImGui::GetIO().WantTextInput) {
        const SDL_Keymod mods = SDL_GetModState();
        const bool ctrl = (mods & SDL_KMOD_CTRL) != 0;
        const bool alt = (mods & SDL_KMOD_ALT) != 0;
        if (ctrl && event.key.key == SDLK_S) {
            std::string err;
            static_cast<void>(save_character(err));
        } else if (ctrl && event.key.key == SDLK_N) {
            if (package_dirty_) {
                pending_unsaved_action_ = PendingUnsavedAction::create_new;
                pending_switch_path_.clear();
                open_unsaved_modal_ = true;
            } else {
                copy_cstr(new_char_name_buf_.data(), new_char_name_buf_.size(), "New Character");
                copy_cstr(new_char_slug_buf_.data(), new_char_slug_buf_.size(), "new_character");
                open_new_char_modal_ = true;
            }
        } else if (!ctrl && !alt && event.key.key == SDLK_1) {
            set_workspace(CharacterStudioWorkspace::anatomy_groom, &tool);
        } else if (!ctrl && !alt && event.key.key == SDLK_2) {
            set_workspace(CharacterStudioWorkspace::wardrobe_paint, &tool);
        } else if (!ctrl && !alt && event.key.key == SDLK_3) {
            set_workspace(CharacterStudioWorkspace::acting_dialogue, &tool);
        } else if (!ctrl && !alt && event.key.key == SDLK_4) {
            set_workspace(CharacterStudioWorkspace::cook_export, &tool);
        } else if (event.key.key == SDLK_F) {
            focus_camera_full_body();
        } else if (event.key.key == SDLK_G) {
            focus_camera_face();
        } else if (event.key.key == SDLK_SPACE) {
            preview_paused_ = !preview_paused_;
        }
    }
}

void CharacterStudioShell::render_gui_frame(
    ICharacterStudioTool& tool,
    float /*dt_seconds*/) {
    CharacterToolFrameActions actions{};
    if (mcp_stdio_enabled_) {
        process_mcp_commands(tool, actions);
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const ImGuiDockNodeFlags dock_flags = ImGuiDockNodeFlags_PassthruCentralNode;
    const ImGuiViewport* main_vp = ImGui::GetMainViewport();
    const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0U, main_vp, dock_flags);
    if (!dock_layout_initialized_) {
        ImGuiDockNode* const node = ImGui::DockBuilderGetNode(dockspace_id);
        if (node != nullptr) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, main_vp->WorkSize);

            ImGuiID center = dockspace_id;
            ImGuiID top = 0U;
            ImGuiID left = 0U;
            ImGuiID right = 0U;
            ImGuiID bottom = 0U;
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.085F, &top, &center);
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24F, &left, &center);
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30F, &right, &center);
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22F, &bottom, &center);

            if (ImGuiDockNode* top_node = ImGui::DockBuilderGetNode(top); top_node != nullptr) {
                top_node->LocalFlags |=
                    ImGuiDockNodeFlags_NoTabBar |
                    ImGuiDockNodeFlags_NoWindowMenuButton |
                    ImGuiDockNodeFlags_NoCloseButton;
            }
            ImGui::DockBuilderDockWindow("Studio Bar", top);
            ImGui::DockBuilderDockWindow("Tool Controls", left);
            ImGui::DockBuilderDockWindow("Character Viewport", center);
            ImGui::DockBuilderDockWindow("Inspector & AI", right);
            ImGui::DockBuilderDockWindow("Timeline & Output", bottom);
            ImGui::DockBuilderFinish(dockspace_id);
            dock_layout_initialized_ = true;
        }
    }

    draw_top_toolbar(tool, actions);

    ImGui::Begin("Tool Controls");
    tool.draw_left_panel(*this, actions);
    ImGui::End();

    draw_viewport_panel(tool, actions);

    ImGui::Begin("Inspector & AI");
    draw_ai_status_widget(actions);
    ImGui::Separator();
    tool.draw_right_panel(*this, actions);
    ImGui::End();

    ImGui::Begin("Timeline & Output");
    if (ImGui::SmallButton(preview_paused_ ? "Play" : "Pause")) {
        preview_paused_ = !preview_paused_;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Bind Pose", &bind_pose_override_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125.0F);
    if (ImGui::BeginCombo("##SharedClipCombo", preview_clip_name_.c_str())) {
        for (const AnimationClip& clip : preview_clips_) {
            const bool sel = (clip.name == preview_clip_name_);
            if (ImGui::Selectable(clip.name.c_str(), sel)) {
                set_preview_clip(clip.name);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        preview_time_ = 0.0F;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "| %zu verts / %zu tris / %zu submeshes",
        preview_mesh_.vertices.size(),
        preview_mesh_.indices.size() / 3U,
        preview_mesh_.submeshes.size());
    ImGui::SameLine();
    tool.draw_bottom_panel(*this, actions);
    ImGui::Separator();
    ui_kit_.status_text(status_message_.c_str(), status_tone_);
    ImGui::End();

    draw_modals(tool);

    ImGui::Render();
    ImDrawData* const draw_data = ImGui::GetDrawData();

    if (actions.mark_dirty) {
        package_dirty_ = true;
    }
    if (!actions.status_message.empty()) {
        set_status(actions.status_message, actions.status_tone);
    }
    if (actions.rebuild_preview) {
        if (left_mouse_dragging_tool_) {
            // Fast interactive CPU mesh rebuild while dragging; GPU reloads cleanly on release.
            rebuild_preview_assets(false);
            preview_rebuild_requested_ = true;
        } else {
            rebuild_preview_assets(true);
            preview_rebuild_requested_ = false;
        }
    }
    if (actions.save_package) {
        std::string err;
        static_cast<void>(save_character(err));
    }
    if (actions.cook_to_project) {
        std::string err;
        static_cast<void>(cook_to_project(err));
    }
    if (title_dirty_state_ != package_dirty_ || title_slug_state_ != package_.identity.slug) {
        update_window_title();
    }

    if (renderer_.draw_frame(last_timings_, camera_.view_matrix(), draw_data)) {
        renderer_.record_metrics(last_timings_);
    }
}

void CharacterStudioShell::draw_top_toolbar(
    ICharacterStudioTool& tool,
    CharacterToolFrameActions& actions) {
    const ImGuiWindowFlags bar_flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("Studio Bar", nullptr, bar_flags);

    const CharacterStudioToolMeta meta = tool.meta();
    ui_kit_.badge(meta.short_badge.data(), EditorTone::primary);
    ImGui::SameLine();

    std::string active_label = package_.identity.slug;
    if (package_dirty_) {
        active_label += " *";
    }
    ImGui::SetNextItemWidth(165.0F);
    if (ImGui::BeginCombo("##ProjectCharCombo", active_label.c_str())) {
        std::error_code cur_ec;
        const std::filesystem::path cur_canon =
            std::filesystem::weakly_canonical(character_path_, cur_ec);
        const std::filesystem::path cur_cmp =
            cur_ec ? character_path_.lexically_normal() : cur_canon;
        for (const DiscoveredCharacterEntry& entry : project_characters_) {
            std::error_code e_ec;
            const std::filesystem::path e_canon =
                std::filesystem::weakly_canonical(entry.path, e_ec);
            const std::filesystem::path e_cmp =
                e_ec ? entry.path.lexically_normal() : e_canon;
            const bool is_cur = (e_cmp == cur_cmp);
            if (ImGui::Selectable(entry.label.c_str(), is_cur) && !is_cur) {
                if (package_dirty_) {
                    pending_unsaved_action_ = PendingUnsavedAction::switch_character;
                    pending_switch_path_ = entry.path;
                    open_unsaved_modal_ = true;
                } else {
                    std::string err;
                    static_cast<void>(load_character_file(entry.path, tool, err));
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ui_kit_.icon_button(
            "RefreshChars", EditorIcon::refresh, "Refresh Characters",
            EditorButtonStyle::quiet)) {
        refresh_project_characters();
        set_status(
            "Found " + std::to_string(project_characters_.size()) + " character(s).",
            EditorTone::info);
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "BarNewChar", EditorIcon::add, "New",
            EditorButtonStyle::secondary)) {
        if (package_dirty_) {
            pending_unsaved_action_ = PendingUnsavedAction::create_new;
            pending_switch_path_.clear();
            open_unsaved_modal_ = true;
        } else {
            copy_cstr(new_char_name_buf_.data(), new_char_name_buf_.size(), "New Character");
            copy_cstr(new_char_slug_buf_.data(), new_char_slug_buf_.size(), "new_character");
            open_new_char_modal_ = true;
        }
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "BarSave", EditorIcon::save, package_dirty_ ? "Save *" : "Save",
            EditorButtonStyle::secondary)) {
        actions.save_package = true;
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "BarCook", EditorIcon::import_asset, "Cook & Verify",
            EditorButtonStyle::primary)) {
        actions.cook_to_project = true;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("| Workspace:");
    ImGui::SameLine();

    const bool is_gen = (active_workspace_ == CharacterStudioWorkspace::anatomy_groom);
    const bool is_outfit = (active_workspace_ == CharacterStudioWorkspace::wardrobe_paint);
    const bool is_act = (active_workspace_ == CharacterStudioWorkspace::acting_dialogue);
    const bool is_cook = (active_workspace_ == CharacterStudioWorkspace::cook_export);

    if (ui_kit_.action_button(
            "TabGen", EditorIcon::cube, "1. Anatomy & Groom",
            EditorButtonStyle::quiet, true, is_gen)) {
        set_workspace(CharacterStudioWorkspace::anatomy_groom, &tool);
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "TabOutfit", EditorIcon::material, "2. Wardrobe & Paint",
            EditorButtonStyle::quiet, true, is_outfit)) {
        set_workspace(CharacterStudioWorkspace::wardrobe_paint, &tool);
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "TabAct", EditorIcon::play, "3. Acting & Dialogue",
            EditorButtonStyle::quiet, true, is_act)) {
        set_workspace(CharacterStudioWorkspace::acting_dialogue, &tool);
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "TabCook", EditorIcon::import_asset, "4. Cook & Verify",
            EditorButtonStyle::quiet, true, is_cook)) {
        set_workspace(CharacterStudioWorkspace::cook_export, &tool);
    }

    ImGui::SameLine();
    if (ui_kit_.action_button(
            "FocusFull", EditorIcon::camera, "Body",
            EditorButtonStyle::quiet)) {
        focus_camera_full_body();
    }
    ImGui::SameLine();
    if (ui_kit_.action_button(
            "FocusFace", EditorIcon::camera, "Face",
            EditorButtonStyle::quiet)) {
        focus_camera_face();
    }
    ImGui::SameLine();
    ImGui::Checkbox("50-Bone Rig", &show_skeleton_overlay_);

    ImGui::End();
}

void CharacterStudioShell::draw_modals(ICharacterStudioTool& tool) {
    if (open_unsaved_modal_) {
        ImGui::OpenPopup("Unsaved Character Changes");
        open_unsaved_modal_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(440.0F, 0.0F), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(
            "Unsaved Character Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Character '%s' (%s) has unsaved edits. Save before continuing?",
            package_.identity.display_name.c_str(),
            package_.identity.slug.c_str());
        ImGui::Spacing();
        auto perform_pending = [&]() {
            if (pending_unsaved_action_ == PendingUnsavedAction::switch_character &&
                !pending_switch_path_.empty()) {
                std::string err;
                static_cast<void>(load_character_file(pending_switch_path_, tool, err));
            } else if (pending_unsaved_action_ == PendingUnsavedAction::create_new) {
                copy_cstr(new_char_name_buf_.data(), new_char_name_buf_.size(), "New Character");
                copy_cstr(new_char_slug_buf_.data(), new_char_slug_buf_.size(), "new_character");
                open_new_char_modal_ = true;
            }
            pending_unsaved_action_ = PendingUnsavedAction::none;
            pending_switch_path_.clear();
        };
        if (ui_kit_.action_button(
                "UnsavedSaveContinue", EditorIcon::save, "Save & Continue",
                EditorButtonStyle::primary)) {
            std::string err;
            if (save_character(err)) {
                ImGui::CloseCurrentPopup();
                perform_pending();
            }
        }
        ImGui::SameLine();
        if (ui_kit_.action_button(
                "UnsavedDiscard", EditorIcon::revert, "Discard Changes",
                EditorButtonStyle::danger)) {
            package_dirty_ = false;
            ImGui::CloseCurrentPopup();
            perform_pending();
        }
        ImGui::SameLine();
        if (ui_kit_.action_button(
                "UnsavedCancel", EditorIcon::stop, "Cancel",
                EditorButtonStyle::secondary)) {
            pending_unsaved_action_ = PendingUnsavedAction::none;
            pending_switch_path_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (open_new_char_modal_) {
        ImGui::OpenPopup("Create New Character");
        open_new_char_modal_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(460.0F, 0.0F), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(
            "Create New Character", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::InputText("Display Name", new_char_name_buf_.data(), new_char_name_buf_.size())) {
            const std::string auto_slug = sanitize_character_slug(new_char_name_buf_.data());
            if (!auto_slug.empty()) {
                copy_cstr(new_char_slug_buf_.data(), new_char_slug_buf_.size(), auto_slug);
            }
        }
        ImGui::InputText("Slug", new_char_slug_buf_.data(), new_char_slug_buf_.size());
        static constexpr std::array<const char*, 2U> gender_items{"female", "male"};
        ImGui::Combo("Gender", &new_char_gender_, gender_items.data(), 2);
        static constexpr std::array<CharacterRole, 5U> roles{
            CharacterRole::npc,
            CharacterRole::player,
            CharacterRole::companion,
            CharacterRole::vendor,
            CharacterRole::boss,
        };
        static constexpr std::array<const char*, 5U> role_items{
            "npc", "player", "companion", "vendor", "boss"};
        ImGui::Combo("Role", &new_char_role_, role_items.data(), 5);
        ImGui::Spacing();
        if (ui_kit_.action_button(
                "CreateCharConfirm", EditorIcon::add, "Create",
                EditorButtonStyle::primary)) {
            const std::string clean_slug = sanitize_character_slug(new_char_slug_buf_.data());
            const std::string final_slug = clean_slug.empty() ? "hero" : clean_slug;
            const CharacterGender g = (new_char_gender_ == 0)
                ? CharacterGender::female
                : CharacterGender::male;
            const CharacterRole r = roles[static_cast<std::size_t>(
                std::clamp(new_char_role_, 0, static_cast<int>(roles.size()) - 1))];
            CharacterPackage new_pkg = make_studio_character_template(
                final_slug, new_char_name_buf_.data(), g, r);
            const std::filesystem::path target_path =
                default_character_package_path(project_directory_, final_slug);
            apply_new_character(std::move(new_pkg), target_path, tool);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui_kit_.action_button(
                "CreateCharCancel", EditorIcon::stop, "Cancel",
                EditorButtonStyle::secondary)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void CharacterStudioShell::draw_viewport_panel(
    ICharacterStudioTool& tool,
    CharacterToolFrameActions& actions) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::Begin("Character Viewport");
    ImGui::PopStyleVar();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const std::uint32_t vp_w = std::max(static_cast<std::uint32_t>(avail.x), 64U);
    const std::uint32_t vp_h = std::max(static_cast<std::uint32_t>(avail.y), 64U);
    const VkExtent2D current_ext = renderer_.editor_viewport_extent();
    if (current_ext.width != vp_w || current_ext.height != vp_h) {
        if (renderer_.prepare_editor_viewport(VkExtent2D{vp_w, vp_h})) {
            synchronize_renderer_textures();
        }
    }

    if (viewport_texture_ != VK_NULL_HANDLE) {
        ImGui::Image(
            studio_imgui_texture_id(viewport_texture_),
            ImVec2(static_cast<float>(vp_w), static_cast<float>(vp_h)),
            ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F));
    }

    const ImVec2 img_min = ImGui::GetItemRectMin();
    const ImVec2 img_max = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();

    if (hovered) {
        if (std::abs(io.MouseWheel) > 0.001F) {
            orbit_distance_ *= std::pow(0.88F, io.MouseWheel);
            update_orbit_camera();
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0F)) {
            orbit_azimuth_ -= io.MouseDelta.x * 0.009F;
            orbit_elevation_ += io.MouseDelta.y * 0.008F;
            update_orbit_camera();
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0F)) {
            const float pan_scale = orbit_distance_ * 0.0018F;
            orbit_target_ -= camera_.right() * (io.MouseDelta.x * pan_scale);
            orbit_target_ += camera_.up() * (io.MouseDelta.y * pan_scale);
            update_orbit_camera();
        }
    }

    ViewportInteractionContext vp_ctx{};
    vp_ctx.hovered = hovered;
    vp_ctx.shift_down = io.KeyShift;
    vp_ctx.ctrl_down = io.KeyCtrl;

    if (hovered) {
        const auto ray = compute_viewport_ray(
            io.MousePos, img_min, img_max, camera_, renderer_.vertical_fov_degrees());
        if (ray) {
            vp_ctx.ray_origin = ray->origin;
            vp_ctx.ray_direction = ray->direction;
            const CharacterRaycastHit hit = raycast_character_mesh(
                preview_mesh_, ray->origin, ray->direction);
            if (hit.hit) {
                vp_ctx.hit = hit;
                last_hover_hit_ = hit;
            } else {
                last_hover_hit_.reset();
            }
        }
        vp_ctx.left_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        vp_ctx.left_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (vp_ctx.left_clicked) {
            left_mouse_dragging_tool_ = true;
        }
    } else {
        last_hover_hit_.reset();
    }

    if (left_mouse_dragging_tool_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        vp_ctx.left_released = true;
        left_mouse_dragging_tool_ = false;
        preview_rebuild_requested_ = true;
    }

    if (vp_ctx.left_clicked || vp_ctx.left_down || vp_ctx.left_released) {
        tool.handle_viewport_interaction(*this, vp_ctx, actions);
    }

    ImDrawList* const dl = ImGui::GetWindowDrawList();
    if (last_hover_hit_ && last_hover_hit_->hit &&
        preview_brush_visible_ && preview_brush_radius_ > 0.0F) {
        const glm::vec3 p = last_hover_hit_->position;
        const auto s_center = project_world_to_screen(
            p, camera_, renderer_.vertical_fov_degrees(), img_min, img_max);
        if (s_center) {
            const float px_radius = std::clamp(
                (preview_brush_radius_ / std::max(last_hover_hit_->distance, 0.25F)) *
                    static_cast<float>(vp_h) * 0.85F,
                4.0F,
                140.0F);
            dl->AddCircle(*s_center, px_radius, IM_COL32(255, 200, 70, 220), 24, 1.8F);
        }
    }

    if (show_skeleton_overlay_ &&
        preview_pose_.bone_count == preview_mesh_.skeleton.bones.size()) {
        for (std::size_t bi = 0U; bi < preview_mesh_.skeleton.bones.size(); ++bi) {
            const glm::vec3 joint(preview_pose_.model_space[bi][3]);
            const auto sp = project_world_to_screen(
                joint, camera_, renderer_.vertical_fov_degrees(), img_min, img_max);
            if (!sp) {
                continue;
            }
            const bool is_face = bi >= character_body_bone_count;
            const ImU32 col = is_face ? IM_COL32(255, 130, 210, 230) : IM_COL32(90, 230, 175, 220);
            dl->AddCircleFilled(*sp, is_face ? 3.2F : 4.2F, col);
            const std::int32_t parent = preview_mesh_.skeleton.bones[bi].parent_index;
            if (parent >= 0 && static_cast<std::uint32_t>(parent) < preview_pose_.bone_count) {
                const glm::vec3 p_joint(preview_pose_.model_space[static_cast<std::size_t>(parent)][3]);
                if (const auto sp_parent = project_world_to_screen(
                        p_joint, camera_, renderer_.vertical_fov_degrees(), img_min, img_max)) {
                    dl->AddLine(*sp_parent, *sp, col, 1.4F);
                }
            }
        }
    }

    char hud_buf[256]{};
    const double fps = last_timings_.cpu_frame_ms > 0.01
        ? (1000.0 / last_timings_.cpu_frame_ms)
        : 0.0;
    std::snprintf(
        hud_buf, sizeof(hud_buf),
        "%.0f FPS | %.2f ms | %zu verts / %zu tris / %zu bones%s%s",
        fps,
        last_timings_.cpu_frame_ms > 0.0 ? last_timings_.cpu_frame_ms : 0.0,
        preview_mesh_.vertices.size(),
        preview_mesh_.indices.size() / 3U,
        preview_mesh_.skeleton.bones.size(),
        last_hover_hit_ ? " | Hit: " : "",
        last_hover_hit_ ? to_token(last_hover_hit_->target).data() : "");
    ui_kit_.viewport_badge(ImVec2(img_min.x + 12.0F, img_min.y + 12.0F), hud_buf);

    ImGui::End();
}

void CharacterStudioShell::draw_ai_status_widget(CharacterToolFrameActions& /*actions*/) {
    ui_kit_.section_header(EditorIcon::environment, "Local AI (Ollama / Offline)");
    if (ollama_status_.reachable) {
        ui_kit_.badge(
            ("Ollama Online: " + ollama_.model).c_str(),
            EditorTone::success);
    } else {
        ui_kit_.badge("Offline Deterministic AI", EditorTone::info);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Probe Ollama")) {
        trigger_ollama_probe_async();
    }
    if (!ollama_status_.available_models.empty()) {
        if (ImGui::BeginCombo("Model", ollama_.model.c_str())) {
            for (const std::string& m : ollama_status_.available_models) {
                const bool selected = (m == ollama_.model);
                if (ImGui::Selectable(m.c_str(), selected)) {
                    ollama_.model = m;
                }
            }
            ImGui::EndCombo();
        }
    }
}

void CharacterStudioShell::start_mcp_reader() {
    mcp_reader_thread_ = std::jthread([this](std::stop_token st) {
        std::string line;
        while (!st.stop_requested() && std::getline(std::cin, line)) {
            if (line.empty()) {
                continue;
            }
            std::lock_guard<std::mutex> lock(mcp_mutex_);
            mcp_queue_.push_back(PendingMcpCommand{std::move(line)});
        }
    });
}

void CharacterStudioShell::stop_mcp_reader() noexcept {
    mcp_reader_thread_.request_stop();
    if (mcp_reader_thread_.joinable()) {
        // detach cleanly if blocked on synchronous stdin read on shutdown
        mcp_reader_thread_.detach();
    }
}

void CharacterStudioShell::process_mcp_commands(
    ICharacterStudioTool& tool,
    CharacterToolFrameActions& actions) {
    std::deque<PendingMcpCommand> batch;
    {
        std::lock_guard<std::mutex> lock(mcp_mutex_);
        batch.swap(mcp_queue_);
    }
    for (const PendingMcpCommand& cmd : batch) {
        Json req;
        try {
            req = Json::parse(cmd.raw_line);
        } catch (const std::exception& ex) {
            write_mcp_response(Json{
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32700}, {"message", ex.what()}}},
            });
            continue;
        }
        const Json id = req.contains("id") ? req["id"] : Json(nullptr);
        const std::string method = req.value("method", std::string{});
        const Json params = req.contains("params") ? req["params"] : Json::object();

        const auto send_ok = [&](const Json& result) {
            if (!id.is_null()) {
                write_mcp_response(Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", result},
                });
            }
        };
        const auto send_err = [&](int code, const std::string& msg) {
            if (!id.is_null()) {
                write_mcp_response(Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"error", {{"code", code}, {"message", msg}}},
                });
            }
        };

        const bool is_tools_call = (method == "tools/call");
        const std::string call_name = is_tools_call
            ? params.value("name", std::string{})
            : method;
        const Json call_args =
            (is_tools_call && params.contains("arguments") && params["arguments"].is_object())
            ? params["arguments"]
            : params;

        if (method == "initialize") {
            send_ok(Json{
                {"protocolVersion", "2024-11-05"},
                {"serverInfo", {
                    {"name", "vulkan_character_studio"},
                    {"version", "1.0.0"},
                }},
                {"capabilities", {{"tools", Json::object()}}},
            });
        } else if (method == "tools/list") {
            send_ok(Json{{"tools", Json::array({
                {{"name", "get_character_summary"}, {"description", "Inspect current character metadata & stats"}},
                {{"name", "list_project_characters"}, {"description", "List .vcrchar.json packages in the project"}},
                {{"name", "load_character"}, {"description", "Load a project character by path or slug"}},
                {{"name", "new_character"}, {"description", "Create a new character template in memory"}},
                {{"name", "set_workspace_tab"}, {"description", "Switch active workspace (gen|outfit|act|cook)"}},
                {{"name", "save_character"}, {"description", "Save .vcrchar.json package"}},
                {{"name", "cook_character"}, {"description", "Cook and verify package into project assets/"}},
                {{"name", "request_quit"}, {"description", "Close studio cleanly"}},
                {{"name", "ai_generate_body"}, {"description", "Generate body proportions, face & skin via AI"}},
                {{"name", "ai_generate_outfit"}, {"description", "Generate wardrobe & materials via AI"}},
                {{"name", "ai_generate_acting"}, {"description", "Generate locomotion temperament, voice & dialogue via AI"}},
                {{"name", "ai_generate_full_character"}, {"description", "Generate full character spec via AI"}},
            })}});
        } else if (call_name == "get_character_summary") {
            send_ok(character_summary_json(
                package_,
                character_path_,
                project_directory_,
                preview_mesh_,
                active_workspace_,
                last_cook_verified_));
        } else if (call_name == "list_project_characters") {
            refresh_project_characters();
            Json chars_arr = Json::array();
            for (const DiscoveredCharacterEntry& entry : project_characters_) {
                chars_arr.push_back(Json{
                    {"label", entry.label},
                    {"path", entry.path.generic_string()},
                });
            }
            send_ok(Json{
                {"project_directory", project_directory_.generic_string()},
                {"active_character", character_path_.generic_string()},
                {"characters", chars_arr},
            });
        } else if (call_name == "load_character") {
            const std::string path_str = call_args.value("path", std::string{});
            const std::string slug_str = call_args.value("slug", std::string{});
            std::filesystem::path target_path;
            if (!path_str.empty()) {
                target_path = std::filesystem::path(path_str);
            } else if (!slug_str.empty()) {
                target_path = default_character_package_path(project_directory_, slug_str);
            } else {
                send_err(-32602, "load_character requires 'path' or 'slug'.");
                continue;
            }
            std::string err;
            if (load_character_file(target_path, tool, err)) {
                send_ok(Json{
                    {"loaded", true},
                    {"slug", package_.identity.slug},
                    {"path", character_path_.generic_string()},
                });
            } else {
                send_err(-32000, err);
            }
        } else if (call_name == "new_character") {
            const std::string slug_str = call_args.value("slug", std::string{"new_character"});
            const std::string name_str = call_args.value("name", std::string{"New Character"});
            const std::string gender_str = call_args.value("gender", std::string{"female"});
            const std::string role_str = call_args.value("role", std::string{"npc"});
            const CharacterGender g = parse_character_gender(gender_str)
                .value_or(CharacterGender::female);
            const CharacterRole r = parse_character_role(role_str)
                .value_or(CharacterRole::npc);
            const std::string clean_slug = sanitize_character_slug(slug_str);
            const std::string final_slug = clean_slug.empty() ? "hero" : clean_slug;
            CharacterPackage new_pkg = make_studio_character_template(
                final_slug, name_str, g, r);
            const std::filesystem::path target_path =
                default_character_package_path(project_directory_, final_slug);
            apply_new_character(std::move(new_pkg), target_path, tool);
            send_ok(Json{
                {"created", true},
                {"slug", package_.identity.slug},
                {"path", character_path_.generic_string()},
            });
        } else if (call_name == "set_workspace_tab") {
            const std::string tab_str = call_args.contains("workspace")
                ? call_args.value("workspace", std::string{})
                : call_args.value("tab", std::string{});
            const auto parsed = parse_character_studio_workspace(tab_str);
            if (!parsed) {
                send_err(-32602, "Unknown workspace tab: " + tab_str);
                continue;
            }
            set_workspace(*parsed, &tool);
            send_ok(Json{
                {"workspace", std::string(to_token(active_workspace_))},
            });
        } else if (call_name == "save_character") {
            std::string err;
            if (save_character(err)) {
                send_ok(Json{{"saved", true}, {"path", character_path_.generic_string()}});
            } else {
                send_err(-32000, err);
            }
        } else if (call_name == "cook_character") {
            CharacterCookOptions opts{};
            opts.texture_resolution = call_args.value(
                "texture_resolution", character_default_texture_size);
            const bool verify = call_args.value("verify", true);
            std::string err;
            if (cook_to_project_with_options(opts, verify, err)) {
                send_ok(Json{
                    {"cooked", true},
                    {"verified", last_cook_verified_},
                    {"model_asset", last_cook_report_->model_asset_relative.generic_string()},
                    {"cooked_files", last_cook_report_->cooked_file_count},
                });
            } else {
                send_err(-32000, err);
            }
        } else if (call_name == "request_quit") {
            running_ = false;
            send_ok(Json{{"quitting", true}});
        } else {
            std::string custom_result_str;
            std::string custom_err;
            bool need_rebuild = false;
            if (tool.handle_mcp_custom_method(
                    call_name,
                    call_args.dump(),
                    package_,
                    ollama_,
                    custom_result_str,
                    need_rebuild,
                    custom_err)) {
                if (need_rebuild) {
                    package_dirty_ = true;
                    actions.rebuild_preview = true;
                    tool.on_package_loaded(package_);
                }
                Json parsed = Json::object();
                try {
                    if (!custom_result_str.empty()) {
                        parsed = Json::parse(custom_result_str);
                    }
                } catch (...) {
                    parsed = Json{{"text", custom_result_str}};
                }
                send_ok(parsed);
            } else {
                send_err(-32601, custom_err);
            }
        }
    }
}

} // namespace vcr
