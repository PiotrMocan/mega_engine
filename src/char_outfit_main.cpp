#include "character_ai_bridge.hpp"
#include "character_package.hpp"
#include "character_studio_shell.hpp"

#include <glm/geometric.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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

class CharOutfitTool final : public ICharacterStudioTool {
public:
    [[nodiscard]] CharacterStudioToolMeta meta() const noexcept override {
        return CharacterStudioToolMeta{
            .tool_id = "char_outfit",
            .window_title = "VCR Char Outfit Studio (Wardrobe, Hair & PBR Paint)",
            .short_badge = "02 // OUTFIT & PBR",
            .cli_program_name = "vulkan_char_outfit",
            .exe_base_name = "vulkan_char_outfit",
        };
    }

    void on_package_loaded(const CharacterPackage& package) override {
        if (!package.identity.prompt.empty() && prompt_buf_[0] == '\0') {
            const std::size_t n = std::min(package.identity.prompt.size(), prompt_buf_.size() - 1U);
            std::memcpy(prompt_buf_.data(), package.identity.prompt.data(), n);
            prompt_buf_[n] = '\0';
        }
    }

    void handle_viewport_interaction(
        CharacterStudioShell& shell,
        const ViewportInteractionContext& ctx,
        CharacterToolFrameActions& actions) override {
        shell.set_preview_brush_radius(brush_radius_uv_ * 1.4F);
        if (ctx.left_released) {
            has_last_uv_ = false;
            return;
        }
        if (!ctx.hit || !ctx.hit->hit) {
            return;
        }
        if (!ctx.left_clicked && !ctx.left_down) {
            return;
        }
        if (!ctx.left_clicked && has_last_uv_ &&
            glm::length(ctx.hit->uv - last_uv_) < 0.015F) {
            return;
        }

        CharacterPaintStroke stroke{};
        stroke.target = auto_target_ ? ctx.hit->target : pinned_target_;
        stroke.mode = ctx.ctrl_down ? CharacterPaintMode::erase : paint_mode_;
        stroke.uv = ctx.hit->uv;
        stroke.radius = brush_radius_uv_;
        stroke.hardness = brush_hardness_;
        stroke.strength = brush_strength_;
        stroke.color = brush_color_;
        stroke.mirror_u = mirror_u_;

        CharacterPackage& pkg = shell.package();
        if (pkg.paint_strokes.size() < 512U) {
            pkg.paint_strokes.push_back(stroke);
            last_uv_ = ctx.hit->uv;
            has_last_uv_ = true;
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
            actions.status_message = "Painted " +
                std::string(to_token(stroke.target)) + " (" +
                std::string(to_token(stroke.mode)) + ") #" +
                std::to_string(pkg.paint_strokes.size());
            actions.status_tone = EditorTone::info;
        }
    }

    void draw_left_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override {
        CharacterPackage& pkg = shell.package();
        const EditorUiKit& kit = shell.ui_kit();
        shell.set_preview_brush_radius(brush_radius_uv_ * 1.4F);

        kit.section_header(EditorIcon::material, "Modular Wardrobe");

        static constexpr std::array<TopGarmentStyle, 6> tops{
            TopGarmentStyle::none, TopGarmentStyle::tunic, TopGarmentStyle::jacket,
            TopGarmentStyle::trench_coat, TopGarmentStyle::armor_cuirass, TopGarmentStyle::hoodie};
        if (ImGui::BeginCombo("Top Garment", to_token(pkg.wardrobe.top_style).data())) {
            for (const auto s : tops) {
                if (ImGui::Selectable(to_token(s).data(), s == pkg.wardrobe.top_style)) {
                    pkg.wardrobe.top_style = s;
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
            ImGui::EndCombo();
        }

        static constexpr std::array<BottomGarmentStyle, 6> bottoms{
            BottomGarmentStyle::none, BottomGarmentStyle::trousers, BottomGarmentStyle::cargo_pants,
            BottomGarmentStyle::tactical_greaves, BottomGarmentStyle::skirt, BottomGarmentStyle::shorts};
        if (ImGui::BeginCombo("Bottom Garment", to_token(pkg.wardrobe.bottom_style).data())) {
            for (const auto s : bottoms) {
                if (ImGui::Selectable(to_token(s).data(), s == pkg.wardrobe.bottom_style)) {
                    pkg.wardrobe.bottom_style = s;
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
            ImGui::EndCombo();
        }

        static constexpr std::array<FootwearStyle, 5> footwear{
            FootwearStyle::barefoot, FootwearStyle::boots, FootwearStyle::combat_boots,
            FootwearStyle::greave_boots, FootwearStyle::sneakers};
        if (ImGui::BeginCombo("Footwear", to_token(pkg.wardrobe.footwear_style).data())) {
            for (const auto s : footwear) {
                if (ImGui::Selectable(to_token(s).data(), s == pkg.wardrobe.footwear_style)) {
                    pkg.wardrobe.footwear_style = s;
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
            ImGui::EndCombo();
        }

        static constexpr std::array<HairStyle, 7> hairs{
            HairStyle::bald, HairStyle::short_crop, HairStyle::spiky, HairStyle::bob,
            HairStyle::ponytail, HairStyle::mohawk, HairStyle::hood};
        if (ImGui::BeginCombo("Hair Style", to_token(pkg.wardrobe.hair_style).data())) {
            for (const auto s : hairs) {
                if (ImGui::Selectable(to_token(s).data(), s == pkg.wardrobe.hair_style)) {
                    pkg.wardrobe.hair_style = s;
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
            ImGui::EndCombo();
        }

        auto toggle = [&](const char* label, bool* v) {
            if (ImGui::Checkbox(label, v)) {
                actions.mark_dirty = true;
                actions.rebuild_preview = true;
            }
        };
        toggle("Belt", &pkg.wardrobe.has_belt);
        ImGui::SameLine();
        toggle("Gloves", &pkg.wardrobe.has_gloves);
        toggle("Pauldrons", &pkg.wardrobe.has_pauldrons);
        ImGui::SameLine();
        toggle("Cape", &pkg.wardrobe.has_cape);
        toggle("Beard", &pkg.wardrobe.has_beard);
        toggle("Occlude Covered Body", &pkg.wardrobe.occlude_covered_body);

        ImGui::Separator();
        kit.section_header(EditorIcon::renderer, "PBR Materials & Palette");
        static constexpr std::array<OutfitMaterialPreset, 6> mats{
            OutfitMaterialPreset::cloth_linen, OutfitMaterialPreset::leather,
            OutfitMaterialPreset::denim, OutfitMaterialPreset::tactical_weave,
            OutfitMaterialPreset::brushed_steel, OutfitMaterialPreset::gilded_plate};
        if (ImGui::BeginCombo("Primary Mat", to_token(pkg.wardrobe.primary_material).data())) {
            for (const auto m : mats) {
                if (ImGui::Selectable(to_token(m).data(), m == pkg.wardrobe.primary_material)) {
                    pkg.wardrobe.primary_material = m;
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo("Secondary Mat", to_token(pkg.wardrobe.secondary_material).data())) {
            for (const auto m : mats) {
                if (ImGui::Selectable(to_token(m).data(), m == pkg.wardrobe.secondary_material)) {
                    pkg.wardrobe.secondary_material = m;
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
            ImGui::EndCombo();
        }

        auto color_edit = [&](const char* label, float* c) {
            if (ImGui::ColorEdit3(label, c)) {
                actions.mark_dirty = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                actions.rebuild_preview = true;
            }
        };
        color_edit("Primary Color", &pkg.wardrobe.primary_color.x);
        color_edit("Secondary Color", &pkg.wardrobe.secondary_color.x);
        color_edit("Accent Trim", &pkg.wardrobe.accent_color.x);
        color_edit("Footwear Color", &pkg.wardrobe.footwear_color.x);
        color_edit("Hair Color", &pkg.wardrobe.hair_color.x);
        if (ImGui::SliderFloat("Wear & Tear", &pkg.wardrobe.wear_and_tear, 0.0F, 1.0F)) {
            actions.mark_dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            actions.rebuild_preview = true;
        }
    }

    void draw_right_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override {
        CharacterPackage& pkg = shell.package();
        const EditorUiKit& kit = shell.ui_kit();

        kit.section_header(EditorIcon::add, "AI Wardrobe Stylist");
        ImGui::InputTextMultiline(
            "##OutfitPrompt", prompt_buf_.data(), prompt_buf_.size(),
            ImVec2(-1.0F, 64.0F));
        if (kit.action_button(
                "GenOutfitAi", EditorIcon::play,
                shell.ai_busy() ? "Generating..." : "Style Outfit (AI)",
                EditorButtonStyle::primary, !shell.ai_busy())) {
            const std::string p = prompt_buf_.data();
            const std::uint64_t seed = pkg.identity.seed;
            shell.enqueue_ai_generation("Wardrobe & Palette",
                [p, seed](const OllamaBridgeConfig& cfg,
                          CharacterPackage& target,
                          CharacterAiGenerationResult& res) {
                    return ai_generate_outfit_spec(p, seed, cfg, target, res);
                });
        }

        ImGui::Separator();
        kit.section_header(EditorIcon::material, "3D PBR Texture Paint (LMB on Model)");
        static constexpr std::array<CharacterPaintMode, 4> modes{
            CharacterPaintMode::color, CharacterPaintMode::scar,
            CharacterPaintMode::metallic_accent, CharacterPaintMode::erase};
        if (ImGui::BeginCombo("Paint Mode", to_token(paint_mode_).data())) {
            for (const auto m : modes) {
                if (ImGui::Selectable(to_token(m).data(), m == paint_mode_)) {
                    paint_mode_ = m;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Auto-Pick Hit Surface", &auto_target_);
        if (!auto_target_) {
            static constexpr std::array<CharacterPaintTarget, 4> targets{
                CharacterPaintTarget::skin, CharacterPaintTarget::outfit_primary,
                CharacterPaintTarget::outfit_secondary, CharacterPaintTarget::hair};
            if (ImGui::BeginCombo("Target Slot", to_token(pinned_target_).data())) {
                for (const auto t : targets) {
                    if (ImGui::Selectable(to_token(t).data(), t == pinned_target_)) {
                        pinned_target_ = t;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SliderFloat("UV Radius", &brush_radius_uv_, 0.01F, 0.18F, "%.3f");
        ImGui::SliderFloat("Hardness", &brush_hardness_, 0.05F, 1.0F, "%.2f");
        ImGui::SliderFloat("Strength", &brush_strength_, 0.05F, 1.0F, "%.2f");
        ImGui::ColorEdit4("Paint Color", &brush_color_.x);
        ImGui::Checkbox("Mirror U", &mirror_u_);

        if (ImGui::Button("Undo Paint") && !pkg.paint_strokes.empty()) {
            pkg.paint_strokes.pop_back();
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Paint") && !pkg.paint_strokes.empty()) {
            pkg.paint_strokes.clear();
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
        }
    }

    void draw_bottom_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override {
        CharacterPackage& pkg = shell.package();
        const CharacterMeshBuildResult& mesh = shell.preview_mesh();
        ImGui::Text("Submeshes (%zu) | Paint Strokes (%zu) | Quick Presets:",
            mesh.submeshes.size(), pkg.paint_strokes.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Steel Knight")) {
            pkg.wardrobe.top_style = TopGarmentStyle::armor_cuirass;
            pkg.wardrobe.bottom_style = BottomGarmentStyle::tactical_greaves;
            pkg.wardrobe.footwear_style = FootwearStyle::greave_boots;
            pkg.wardrobe.primary_material = OutfitMaterialPreset::brushed_steel;
            pkg.wardrobe.has_pauldrons = true;
            pkg.wardrobe.has_cape = true;
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Street Coat")) {
            pkg.wardrobe.top_style = TopGarmentStyle::trench_coat;
            pkg.wardrobe.bottom_style = BottomGarmentStyle::cargo_pants;
            pkg.wardrobe.footwear_style = FootwearStyle::combat_boots;
            pkg.wardrobe.primary_material = OutfitMaterialPreset::leather;
            pkg.wardrobe.has_pauldrons = false;
            pkg.wardrobe.has_cape = false;
            actions.mark_dirty = true;
            actions.rebuild_preview = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cyber Hoodie")) {
            pkg.wardrobe.top_style = TopGarmentStyle::hoodie;
            pkg.wardrobe.bottom_style = BottomGarmentStyle::trousers;
            pkg.wardrobe.footwear_style = FootwearStyle::sneakers;
            pkg.wardrobe.primary_material = OutfitMaterialPreset::tactical_weave;
            pkg.wardrobe.hair_style = HairStyle::hood;
            actions.mark_dirty = true;
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
            std::cerr << "vulkan_char_outfit: failed to load package: " << err << "\n";
            return 1;
        }

        if (const auto prompt = args.find_option("prompt")) {
            CharacterAiGenerationResult ai_res{};
            if (!ai_generate_outfit_spec(*prompt, pkg.identity.seed, args.ollama, pkg, ai_res)) {
                std::cerr << "vulkan_char_outfit: AI generation error: " << ai_res.diagnostic << "\n";
                return 1;
            }
            std::cout << "[ai] " << ai_res.summary << "\n";
        }
        if (const auto v = args.find_option("top")) {
            if (const auto s = parse_top_garment_style(*v)) pkg.wardrobe.top_style = *s;
        }
        if (const auto v = args.find_option("bottom")) {
            if (const auto s = parse_bottom_garment_style(*v)) pkg.wardrobe.bottom_style = *s;
        }
        if (const auto v = args.find_option("footwear")) {
            if (const auto s = parse_footwear_style(*v)) pkg.wardrobe.footwear_style = *s;
        }
        if (const auto v = args.find_option("hair")) {
            if (const auto s = parse_hair_style(*v)) pkg.wardrobe.hair_style = *s;
        }
        if (const auto v = args.find_option("material")) {
            if (const auto m = parse_outfit_material_preset(*v)) pkg.wardrobe.primary_material = *m;
        }
        if (const auto v = args.find_option("primary-color")) {
            if (const auto rgb = parse_hex_rgb(*v)) pkg.wardrobe.primary_color = *rgb;
        }
        if (const auto v = args.find_option("secondary-color")) {
            if (const auto rgb = parse_hex_rgb(*v)) pkg.wardrobe.secondary_color = *rgb;
        }
        if (const auto v = args.find_option("accent-color")) {
            if (const auto rgb = parse_hex_rgb(*v)) pkg.wardrobe.accent_color = *rgb;
        }
        if (args.has_flag("cape")) pkg.wardrobe.has_cape = true;
        if (args.has_flag("no-cape")) pkg.wardrobe.has_cape = false;
        if (args.has_flag("pauldrons")) pkg.wardrobe.has_pauldrons = true;
        if (args.has_flag("no-pauldrons")) pkg.wardrobe.has_pauldrons = false;
        if (args.has_flag("gloves")) pkg.wardrobe.has_gloves = true;
        if (args.has_flag("no-gloves")) pkg.wardrobe.has_gloves = false;
        if (args.has_flag("belt")) pkg.wardrobe.has_belt = true;
        if (args.has_flag("no-belt")) pkg.wardrobe.has_belt = false;
        if (args.has_flag("beard")) pkg.wardrobe.has_beard = true;
        if (args.has_flag("no-beard")) pkg.wardrobe.has_beard = false;

        sanitize_character_package(pkg);
        if (!save_character_package_file(pkg_path, pkg, err)) {
            std::cerr << "vulkan_char_outfit: save failed: " << err << "\n";
            return 1;
        }
        std::cout << "Saved character wardrobe: " << pkg_path.generic_string() << "\n";

        if (args.cook_after_cli) {
            CharacterCookReport report{};
            if (!cook_character_package_to_project(
                    pkg, args.project_directory, CharacterCookOptions{}, report, err)) {
                std::cerr << "vulkan_char_outfit: cook failed: " << err << "\n";
                return 1;
            }
            std::cout << "Cooked " << report.cooked_file_count << " assets -> "
                      << report.model_asset_relative.generic_string() << "\n";
        }
        return 0;
    }

    void print_cli_help() const override {
        std::cout
            << "vulkan_char_outfit - VCR Single-Purpose Wardrobe & PBR Texture Painter\n"
            << "Usage:\n"
            << "  vulkan_char_outfit [options]\n\n"
            << "Options:\n"
            << "  --project <dir>           Target game project directory\n"
            << "  --character <path|slug>   .vcrchar.json path or character slug\n"
            << "  --cli                     Run headless CLI mode\n"
            << "  --prompt <text>           Style outfit via Ollama or offline fallback\n"
            << "  --top <style>             none|tunic|jacket|trench_coat|armor_cuirass|hoodie\n"
            << "  --bottom <style>          none|trousers|cargo_pants|tactical_greaves|skirt|shorts\n"
            << "  --footwear <style>        barefoot|boots|combat_boots|greave_boots|sneakers\n"
            << "  --hair <style>            bald|short_crop|spiky|bob|ponytail|mohawk|hood\n"
            << "  --material <preset>       cloth_linen|leather|denim|tactical_weave|brushed_steel|gilded_plate\n"
            << "  --primary-color <#RRGGBB> Primary outfit color\n"
            << "  --cape / --no-cape        Enable or disable cape mesh\n"
            << "  --belt / --no-belt        Enable or disable waist belt\n"
            << "  --gloves / --no-gloves    Enable or disable gloves\n"
            << "  --pauldrons / --no-pauldrons Enable or disable shoulder armor\n"
            << "  --beard / --no-beard      Enable or disable sculpted beard\n"
            << "  --cook                    Cook updated character into <project>/assets/\n"
            << "  --smoke-frames <N>        Run N Vulkan GUI frames and exit\n";
    }

    bool handle_mcp_custom_method(
        std::string_view method,
        std::string_view params_json,
        CharacterPackage& package,
        const OllamaBridgeConfig& ollama,
        std::string& out_result_json,
        bool& out_rebuild_preview,
        std::string& out_error) override {
        if (method == "ai_generate_outfit") {
            Json p = Json::object();
            try {
                if (!params_json.empty()) p = Json::parse(params_json);
            } catch (...) {}
            const std::string prompt = p.value("prompt", std::string{"ranger leather coat"});
            CharacterAiGenerationResult res{};
            if (!ai_generate_outfit_spec(prompt, package.identity.seed, ollama, package, res)) {
                out_error = res.diagnostic;
                return false;
            }
            out_rebuild_preview = true;
            out_result_json = Json{
                {"used_ollama", res.used_ollama},
                {"summary", res.summary},
                {"top_style", std::string(to_token(package.wardrobe.top_style))},
            }.dump();
            return true;
        }
        out_error = "Unknown vulkan_char_outfit MCP method: " + std::string(method);
        return false;
    }

private:
    std::array<char, 512> prompt_buf_{"emerald scout trench coat with leather boots and bronze trim"};
    CharacterPaintMode paint_mode_ = CharacterPaintMode::color;
    CharacterPaintTarget pinned_target_ = CharacterPaintTarget::outfit_primary;
    bool auto_target_ = true;
    float brush_radius_uv_ = 0.045F;
    float brush_hardness_ = 0.78F;
    float brush_strength_ = 0.85F;
    glm::vec4 brush_color_{0.88F, 0.28F, 0.16F, 1.0F};
    bool mirror_u_ = false;
    bool has_last_uv_ = false;
    glm::vec2 last_uv_{0.0F};
};

} // namespace
} // namespace vcr

int main(int argc, char** argv) {
    try {
        vcr::CharOutfitTool tool;
        vcr::CharacterStudioShell shell;
        return shell.run(argc, argv, tool);
    } catch (const std::exception& ex) {
        std::cerr << "vulkan_char_outfit fatal error: " << ex.what() << '\n';
        return 1;
    }
}
