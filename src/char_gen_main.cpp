#include "character_ai_bridge.hpp"
#include "character_package.hpp"
#include "character_studio_shell.hpp"

#include <glm/geometric.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::optional<glm::vec3> parse_hex_rgb(std::string_view hex) noexcept {
    if (hex.starts_with('#')) {
        hex.remove_prefix(1U);
    }
    if (hex.size() != 6U) {
        return std::nullopt;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    std::array<float, 3> out{};
    for (std::size_t i = 0U; i < 3U; ++i) {
        const int hi = nibble(hex[i * 2U]);
        const int lo = nibble(hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out[i] = static_cast<float>((hi << 4) | lo) / 255.0F;
    }
    return glm::vec3(out[0], out[1], out[2]);
}

void copy_to_buf(char* dst, std::size_t dst_size, std::string_view src) noexcept {
    if (dst == nullptr || dst_size == 0U) {
        return;
    }
    const std::size_t n = std::min(src.size(), dst_size - 1U);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

class CharGenTool final : public ICharacterStudioTool {
public:
    [[nodiscard]] CharacterStudioToolMeta meta() const noexcept override {
        return CharacterStudioToolMeta{
            .tool_id = "char_gen",
            .window_title = "VCR Char Gen Studio (Anatomy, Face & Sculpt)",
            .short_badge = "01 // CHAR GEN",
            .cli_program_name = "vulkan_char_gen",
            .exe_base_name = "vulkan_char_gen",
        };
    }

    void on_package_loaded(const CharacterPackage& package) override {
        copy_to_buf(name_buf_.data(), name_buf_.size(), package.identity.display_name);
        copy_to_buf(slug_buf_.data(), slug_buf_.size(), package.identity.slug);
        if (!package.identity.prompt.empty()) {
            copy_to_buf(prompt_buf_.data(), prompt_buf_.size(), package.identity.prompt);
        }
    }

    void handle_viewport_interaction(
        CharacterStudioShell& shell,
        const ViewportInteractionContext& ctx,
        CharacterToolFrameActions& actions) override {
        shell.set_preview_brush_radius(brush_radius_);
        if (ctx.left_released) {
            has_last_stroke_pos_ = false;
            return;
        }
        if (!ctx.hit || !ctx.hit->hit) {
            return;
        }
        if (!ctx.left_clicked && !ctx.left_down) {
            return;
        }
        if (!ctx.left_clicked && has_last_stroke_pos_ &&
            glm::length(ctx.hit->position - last_stroke_pos_) < 0.022F) {
            return;
        }

        SculptBrushKind active_brush = brush_;
        if (ctx.shift_down) {
            active_brush = SculptBrushKind::smooth;
        } else if (ctx.ctrl_down && active_brush == SculptBrushKind::inflate) {
            active_brush = SculptBrushKind::deflate;
        }

        glm::vec3 disp{0.0F};
        const float mag = 0.018F * brush_strength_;
        switch (active_brush) {
        case SculptBrushKind::inflate:
            disp = ctx.hit->normal * mag;
            break;
        case SculptBrushKind::deflate:
            disp = -ctx.hit->normal * mag;
            break;
        case SculptBrushKind::grab:
            disp = has_last_stroke_pos_
                ? glm::normalize(ctx.hit->position - last_stroke_pos_ + glm::vec3(1e-5F)) * mag
                : ctx.hit->normal * (mag * 0.4F);
            break;
        case SculptBrushKind::smooth:
        case SculptBrushKind::flatten:
            disp = -ctx.hit->normal * (mag * 0.25F);
            break;
        }

        SculptStrokeRecord rec{};
        rec.brush = active_brush;
        rec.local_center = ctx.hit->position;
        rec.displacement = disp;
        rec.radius = brush_radius_;
        rec.strength = brush_strength_;
        rec.mirror_x = mirror_x_;

        CharacterPackage& pkg = shell.package();
        if (pkg.sculpt_strokes.size() < 256U) {
            pkg.sculpt_strokes.push_back(rec);
            last_stroke_pos_ = ctx.hit->position;
            has_last_stroke_pos_ = true;
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
            actions.status_message = "Sculpt stroke (" +
                std::string(to_token(active_brush)) + ") #" +
                std::to_string(pkg.sculpt_strokes.size());
            actions.status_tone = EditorTone::info;
        }
    }

    void draw_left_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override {
        CharacterPackage& pkg = shell.package();
        const EditorUiKit& kit = shell.ui_kit();
        shell.set_preview_brush_radius(brush_radius_);

        kit.section_header(EditorIcon::cube, "Identity & Archetype");
        if (ImGui::InputText("Name", name_buf_.data(), name_buf_.size())) {
            pkg.identity.display_name = name_buf_.data();
            actions.mark_dirty = true;
        }
        if (ImGui::InputText("Slug", slug_buf_.data(), slug_buf_.size())) {
            pkg.identity.slug = sanitize_character_slug(slug_buf_.data());
            actions.mark_dirty = true;
        }

        static constexpr std::array<CharacterRole, 5> roles{
            CharacterRole::npc, CharacterRole::player, CharacterRole::companion,
            CharacterRole::vendor, CharacterRole::boss};
        if (ImGui::BeginCombo("Role", to_token(pkg.identity.role).data())) {
            for (const CharacterRole r : roles) {
                const bool sel = (r == pkg.identity.role);
                if (ImGui::Selectable(to_token(r).data(), sel)) {
                    pkg.identity.role = r;
                    actions.mark_dirty = true;
                }
            }
            ImGui::EndCombo();
        }

        static constexpr std::array<CharacterGender, 2> genders{
            CharacterGender::female, CharacterGender::male};
        if (ImGui::BeginCombo("Gender", to_token(pkg.body.gender).data())) {
            for (const CharacterGender g : genders) {
                const bool sel = (g == pkg.body.gender);
                if (ImGui::Selectable(to_token(g).data(), sel)) {
                    pkg.body.gender = g;
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
            ImGui::EndCombo();
        }

        static constexpr std::array<CharacterArchetype, 6> archetypes{
            CharacterArchetype::human_balanced, CharacterArchetype::human_athletic,
            CharacterArchetype::human_heavy, CharacterArchetype::human_slender,
            CharacterArchetype::stylized_hero, CharacterArchetype::veteran_wanderer};
        if (ImGui::BeginCombo("Archetype", to_token(pkg.body.archetype).data())) {
            for (const CharacterArchetype a : archetypes) {
                const bool sel = (a == pkg.body.archetype);
                if (ImGui::Selectable(to_token(a).data(), sel)) {
                    pkg.body.archetype = a;
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();
        kit.section_header(EditorIcon::renderer, "Body Proportions");
        auto body_slider = [&](const char* label, float* v, float mn, float mx) {
            if (ImGui::SliderFloat(label, v, mn, mx, "%.2f")) {
                actions.mark_dirty = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                actions.rebuild_preview = true;
            }
        };
        body_slider("Height (m)", &pkg.body.height_meters, 1.45F, 2.25F);
        body_slider("Shoulders", &pkg.body.shoulder_width, 0.1F, 1.0F);
        body_slider("Chest Depth", &pkg.body.chest_depth, 0.1F, 1.0F);
        body_slider("Waist", &pkg.body.waist_width, 0.1F, 1.0F);
        body_slider("Hips", &pkg.body.hip_width, 0.1F, 1.0F);
        body_slider("Musculature", &pkg.body.musculature, 0.0F, 1.0F);
        body_slider("Body Fat", &pkg.body.body_fat, 0.0F, 1.0F);
        body_slider("Arm Length", &pkg.body.arm_length, 0.1F, 1.0F);
        body_slider("Leg Length", &pkg.body.leg_length, 0.1F, 1.0F);

        ImGui::Separator();
        kit.section_header(EditorIcon::cube, "3D Sculpt Brush (LMB on Mesh)");
        static constexpr std::array<SculptBrushKind, 5> brushes{
            SculptBrushKind::inflate, SculptBrushKind::deflate,
            SculptBrushKind::smooth, SculptBrushKind::grab,
            SculptBrushKind::flatten};
        if (ImGui::BeginCombo("Brush", to_token(brush_).data())) {
            for (const SculptBrushKind b : brushes) {
                if (ImGui::Selectable(to_token(b).data(), b == brush_)) {
                    brush_ = b;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SliderFloat("Brush Radius", &brush_radius_, 0.02F, 0.30F, "%.3f");
        ImGui::SliderFloat("Brush Strength", &brush_strength_, 0.1F, 1.5F, "%.2f");
        ImGui::Checkbox("Mirror X", &mirror_x_);
        ImGui::TextDisabled("Hold Shift = Smooth, Ctrl = Deflate");

        if (ImGui::Button("Undo Stroke") && !pkg.sculpt_strokes.empty()) {
            pkg.sculpt_strokes.pop_back();
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Sculpt") && !pkg.sculpt_strokes.empty()) {
            pkg.sculpt_strokes.clear();
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
        }
    }

    void draw_right_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override {
        CharacterPackage& pkg = shell.package();
        const EditorUiKit& kit = shell.ui_kit();

        kit.section_header(EditorIcon::add, "AI Anatomy & Face Generator");
        ImGui::InputTextMultiline(
            "##CharPrompt", prompt_buf_.data(), prompt_buf_.size(),
            ImVec2(-1.0F, 64.0F));
        const bool can_ai = !shell.ai_busy();
        if (kit.action_button(
                "GenBodyAi", EditorIcon::play,
                shell.ai_busy() ? "Generating..." : "Generate Anatomy (AI)",
                EditorButtonStyle::primary, can_ai)) {
            const std::string prompt_str = prompt_buf_.data();
            const std::uint64_t seed = pkg.identity.seed;
            shell.enqueue_ai_generation("Body & Face",
                [prompt_str, seed](const OllamaBridgeConfig& cfg,
                                   CharacterPackage& target,
                                   CharacterAiGenerationResult& res) {
                    return ai_generate_body_spec(prompt_str, seed, cfg, target, res);
                });
        }

        ImGui::Separator();
        kit.section_header(EditorIcon::model, "Facial Structure & Complexion");
        auto face_slider = [&](const char* label, float* v, float mn = 0.0F, float mx = 1.0F) {
            if (ImGui::SliderFloat(label, v, mn, mx, "%.2f")) {
                actions.mark_dirty = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                actions.rebuild_preview = true;
            }
        };
        face_slider("Head Scale", &pkg.body.face.head_scale, 0.75F, 1.30F);
        face_slider("Jaw Width", &pkg.body.face.jaw_width);
        face_slider("Chin Length", &pkg.body.face.chin_length);
        face_slider("Cheekbones", &pkg.body.face.cheekbone_prominence);
        face_slider("Brow Ridge", &pkg.body.face.brow_ridge);
        face_slider("Nose Length", &pkg.body.face.nose_length);
        face_slider("Nose Width", &pkg.body.face.nose_width);
        face_slider("Eye Size", &pkg.body.face.eye_size);
        face_slider("Eye Spacing", &pkg.body.face.eye_spacing);
        face_slider("Mouth Width", &pkg.body.face.mouth_width);
        face_slider("Lip Fullness", &pkg.body.face.lip_fullness);
        if (ImGui::ColorEdit3("Eye Color", &pkg.body.face.eye_color.x)) {
            actions.mark_dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            actions.rebuild_preview = true;
        }

        ImGui::Separator();
        if (ImGui::ColorEdit3("Skin Tone", &pkg.body.skin_color.x)) {
            actions.mark_dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            actions.rebuild_preview = true;
        }
        face_slider("Skin Roughness", &pkg.body.skin_roughness, 0.15F, 0.95F);
        face_slider("SSS Strength", &pkg.body.skin_subsurface_strength, 0.0F, 1.0F);
        face_slider("Freckles", &pkg.body.freckles, 0.0F, 1.0F);
        face_slider("Age / Weather", &pkg.body.age_wear, 0.0F, 1.0F);
        face_slider("Lip Tint", &pkg.body.lip_tint, 0.0F, 1.0F);

        ImGui::SeparatorText("3D Strand Groom & Studio");
        static constexpr std::array<const char*, 7U> kHairLabels = {
            "Bald (Brows/Lashes Only)",
            "Short Crop",
            "Spiky",
            "Bob",
            "Ponytail",
            "Mohawk",
            "Hooded Crop",
        };
        int hair_idx = std::clamp(
            static_cast<int>(pkg.wardrobe.hair_style),
            0,
            static_cast<int>(kHairLabels.size() - 1U));
        if (ImGui::Combo("Hair Groom", &hair_idx, kHairLabels.data(), static_cast<int>(kHairLabels.size()))) {
            pkg.wardrobe.hair_style = static_cast<HairStyle>(hair_idx);
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
        }
        if (ImGui::ColorEdit3("Hair Melanin", &pkg.wardrobe.hair_color.x)) {
            actions.mark_dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            actions.rebuild_preview = true;
        }
        if (ImGui::Checkbox("3D Beard Groom", &pkg.wardrobe.has_beard)) {
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
        }
        if (ImGui::Checkbox("Studio Underwear", &pkg.wardrobe.studio_underwear)) {
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
        }
    }

    void draw_bottom_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override {
        const CharacterPackage& pkg = shell.package();
        const CharacterMeshBuildResult& mesh = shell.preview_mesh();
        ImGui::Text(
            "Anatomy Mesh: %zu vertices | %zu triangles | %zu bones | Sculpt Strokes: %zu",
            mesh.vertices.size(), mesh.indices.size() / 3U,
            mesh.skeleton.bones.size(), pkg.sculpt_strokes.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rebuild Mesh & Textures")) {
            actions.rebuild_preview = true;
        }
    }

    int run_cli(const CharacterStudioCliArgs& args) override {
        const std::filesystem::path pkg_path = args.character_path.empty()
            ? default_character_package_path(args.project_directory, args.character_slug)
            : args.character_path;
        CharacterPackage pkg{};
        std::string err;
        if (!load_or_create_character_package(pkg_path, args.character_slug, pkg, err)) {
            std::cerr << "vulkan_char_gen: failed to load package: " << err << "\n";
            return 1;
        }

        if (const auto v = args.find_option("name")) {
            pkg.identity.display_name = std::string(*v);
        }
        if (const auto v = args.find_option("role")) {
            if (const auto r = parse_character_role(*v)) {
                pkg.identity.role = *r;
            }
        }
        if (const auto v = args.find_option("seed")) {
            try {
                pkg.identity.seed = std::stoull(std::string(*v));
            } catch (...) {}
        }
        if (const auto prompt = args.find_option("prompt")) {
            CharacterAiGenerationResult ai_res{};
            if (!ai_generate_body_spec(*prompt, pkg.identity.seed, args.ollama, pkg, ai_res)) {
                std::cerr << "vulkan_char_gen: AI generation error: " << ai_res.diagnostic << "\n";
                return 1;
            }
            std::cout << "[ai] " << ai_res.summary << "\n";
        }
        if (const auto v = args.find_option("gender")) {
            if (const auto g = parse_character_gender(*v)) {
                pkg.body.gender = *g;
            } else {
                std::cerr << "vulkan_char_gen: invalid --gender value: " << *v << "\n";
                return 1;
            }
        }
        if (const auto v = args.find_option("archetype")) {
            if (const auto a = parse_character_archetype(*v)) {
                pkg.body.archetype = *a;
            }
        }
        if (const auto v = args.find_option("height")) {
            try {
                pkg.body.height_meters = std::stof(std::string(*v));
            } catch (...) {}
        }
        if (const auto v = args.find_option("musculature")) {
            try {
                pkg.body.musculature = std::stof(std::string(*v));
            } catch (...) {}
        }
        if (const auto v = args.find_option("skin")) {
            if (const auto rgb = parse_hex_rgb(*v)) {
                pkg.body.skin_color = *rgb;
            }
        }

        sanitize_character_package(pkg);
        if (!save_character_package_file(pkg_path, pkg, err)) {
            std::cerr << "vulkan_char_gen: save failed: " << err << "\n";
            return 1;
        }
        std::cout << "Saved character package: " << pkg_path.generic_string() << "\n";

        if (args.cook_after_cli) {
            CharacterCookReport report{};
            if (!cook_character_package_to_project(
                    pkg, args.project_directory, CharacterCookOptions{}, report, err)) {
                std::cerr << "vulkan_char_gen: cook failed: " << err << "\n";
                return 1;
            }
            std::cout << "Cooked " << report.cooked_file_count << " assets -> "
                      << report.model_asset_relative.generic_string() << "\n";
        }
        return 0;
    }

    void print_cli_help() const override {
        std::cout
            << "vulkan_char_gen - VCR Single-Purpose Character Generator & 3D Sculptor\n"
            << "Usage:\n"
            << "  vulkan_char_gen [options]\n\n"
            << "Options:\n"
            << "  --project <dir>         Target game project directory\n"
            << "  --character <path|slug> .vcrchar.json path or character slug\n"
            << "  --cli                   Run headless Unix CLI mode without opening GUI\n"
            << "  --prompt <text>         Generate anatomy/face/skin via Ollama or offline fallback\n"
            << "  --name <display_name>   Override display name\n"
            << "  --role <role>           npc|player|companion|vendor|boss\n"
            << "  --gender <female|male>  Anatomical base model gender\n"
            << "  --archetype <type>      human_balanced|human_athletic|human_heavy|human_slender|stylized_hero|veteran_wanderer\n"
            << "  --height <meters>       Body height in meters\n"
            << "  --musculature <0..1>    Muscle definition\n"
            << "  --skin <#RRGGBB>        Base skin color hex\n"
            << "  --cook                  Also cook into <project>/assets/\n"
            << "  --smoke-frames <N>      Open Vulkan GUI for N frames then exit cleanly\n"
            << "  --screenshot <png>      Capture Vulkan preview screenshot PNG and exit\n"
            << "  --focus-face            Start camera framed closely on the face\n"
            << "  --mcp-stdio             Enable JSON-RPC 2.0 MCP server on stdin/stdout\n";
    }

    bool handle_mcp_custom_method(
        std::string_view method,
        std::string_view params_json,
        CharacterPackage& package,
        const OllamaBridgeConfig& ollama,
        std::string& out_result_json,
        bool& out_rebuild_preview,
        std::string& out_error) override {
        if (method == "ai_generate_body") {
            Json p = Json::object();
            try {
                if (!params_json.empty()) {
                    p = Json::parse(params_json);
                }
            } catch (...) {}
            const std::string prompt = p.value("prompt", std::string{"balanced wanderer"});
            const std::uint64_t seed = p.value("seed", package.identity.seed);
            CharacterAiGenerationResult res{};
            if (!ai_generate_body_spec(prompt, seed, ollama, package, res)) {
                out_error = res.diagnostic;
                return false;
            }
            if (const std::string g_str = p.value("gender", std::string{}); !g_str.empty()) {
                if (const auto g = parse_character_gender(g_str)) {
                    package.body.gender = *g;
                    sanitize_character_package(package);
                }
            }
            out_rebuild_preview = true;
            out_result_json = Json{
                {"used_ollama", res.used_ollama},
                {"summary", res.summary},
                {"gender", std::string(to_token(package.body.gender))},
                {"archetype", std::string(to_token(package.body.archetype))},
            }.dump();
            return true;
        }
        out_error = "Unknown vulkan_char_gen MCP method: " + std::string(method);
        return false;
    }

private:
    std::array<char, 96> name_buf_{};
    std::array<char, 64> slug_buf_{};
    std::array<char, 512> prompt_buf_{"athletic ranger with sharp jaw and weathered skin"};
    SculptBrushKind brush_ = SculptBrushKind::inflate;
    float brush_radius_ = 0.085F;
    float brush_strength_ = 0.65F;
    bool mirror_x_ = true;
    bool has_last_stroke_pos_ = false;
    glm::vec3 last_stroke_pos_{0.0F};
};

} // namespace
} // namespace vcr

int main(int argc, char** argv) {
    try {
        vcr::CharGenTool tool;
        vcr::CharacterStudioShell shell;
        return shell.run(argc, argv, tool);
    } catch (const std::exception& ex) {
        std::cerr << "vulkan_char_gen fatal error: " << ex.what() << '\n';
        return 1;
    }
}
