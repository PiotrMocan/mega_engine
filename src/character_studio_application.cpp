#include "character_studio_application.hpp"

#include "character_ai_bridge.hpp"
#include "character_package.hpp"

#include <glm/geometric.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
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

void copy_to_buf(char* dst, const std::size_t dst_size, const std::string_view src) noexcept {
    if (dst == nullptr || dst_size == 0U) {
        return;
    }
    const std::size_t n = std::min(src.size(), dst_size - 1U);
    if (n > 0U) {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
}

[[nodiscard]] std::uint32_t tex_resolution_from_index(const int idx) noexcept {
    switch (idx) {
    case 0:
        return 256U;
    case 1:
        return 512U;
    case 2:
    default:
        return 1024U;
    }
}

[[nodiscard]] std::uint64_t next_character_seed(const std::uint64_t current_seed) noexcept {
    const std::uint64_t t = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t z = current_seed ^ t ^ 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31U);
    return z == 0ULL ? 1337ULL : z;
}

[[nodiscard]] bool run_full_character_ai_pipeline(
    const std::string_view prompt,
    const bool include_outfit,
    const std::uint64_t seed,
    const OllamaBridgeConfig& cfg,
    CharacterPackage& target,
    CharacterAiGenerationResult& out_res) {
    CharacterAiGenerationResult body_res{};
    if (!ai_generate_body_spec(prompt, seed, cfg, target, body_res)) {
        out_res = std::move(body_res);
        return false;
    }

    bool used_ollama = body_res.used_ollama;
    bool used_fallback = body_res.used_fallback;
    std::string combined_summary = body_res.summary;

    if (include_outfit) {
        CharacterAiGenerationResult outfit_res{};
        if (!ai_generate_outfit_spec(prompt, seed, cfg, target, outfit_res)) {
            out_res = std::move(outfit_res);
            return false;
        }
        used_ollama = used_ollama || outfit_res.used_ollama;
        used_fallback = used_fallback || outfit_res.used_fallback;
        if (!outfit_res.summary.empty()) {
            combined_summary += " | " + outfit_res.summary;
        }
    }

    CharacterAiGenerationResult act_res{};
    if (!ai_generate_acting_spec(prompt, seed, cfg, target, act_res)) {
        out_res = std::move(act_res);
        return false;
    }
    used_ollama = used_ollama || act_res.used_ollama;
    used_fallback = used_fallback || act_res.used_fallback;
    if (!act_res.summary.empty()) {
        combined_summary += " | " + act_res.summary;
    }

    out_res.used_ollama = used_ollama;
    out_res.used_fallback = used_fallback;
    out_res.model_used = body_res.model_used.empty() ? act_res.model_used : body_res.model_used;
    out_res.summary = std::move(combined_summary);
    return true;
}

} // namespace

int CharacterStudioApplication::run(int argc, char** argv) {
    CharacterStudioShell shell;
    return shell.run(argc, argv, *this);
}

CharacterStudioToolMeta CharacterStudioApplication::meta() const noexcept {
    return CharacterStudioToolMeta{
        .tool_id = "character_studio",
        .window_title = "VCR Character Studio",
        .short_badge = "CHAR STUDIO",
        .cli_program_name = "vulkan_character_studio",
        .exe_base_name = "vulkan_character_studio",
    };
}

void CharacterStudioApplication::sync_dialogue_node_buffers(const CharacterPackage& package) {
    if (package.dialogue.nodes.empty() ||
        selected_node_idx_ >= package.dialogue.nodes.size()) {
        node_id_buf_[0] = '\0';
        speaker_buf_[0] = '\0';
        text_en_buf_[0] = '\0';
        text_ru_buf_[0] = '\0';
        choice_bufs_.clear();
        return;
    }
    const DialogueNodeSpec& n = package.dialogue.nodes[selected_node_idx_];
    copy_to_buf(node_id_buf_.data(), node_id_buf_.size(), n.id);
    copy_to_buf(speaker_buf_.data(), speaker_buf_.size(), n.speaker);
    copy_to_buf(text_en_buf_.data(), text_en_buf_.size(), n.text);
    copy_to_buf(text_ru_buf_.data(), text_ru_buf_.size(), n.text_ru);

    choice_bufs_.resize(n.choices.size());
    for (std::size_t i = 0U; i < n.choices.size(); ++i) {
        copy_to_buf(
            choice_bufs_[i].text_en.data(),
            choice_bufs_[i].text_en.size(),
            n.choices[i].text);
        copy_to_buf(
            choice_bufs_[i].text_ru.data(),
            choice_bufs_[i].text_ru.size(),
            n.choices[i].text_ru);
        copy_to_buf(
            choice_bufs_[i].next_node.data(),
            choice_bufs_[i].next_node.size(),
            n.choices[i].next_node_id);
    }
}

void CharacterStudioApplication::sync_preview_brush(
    CharacterStudioShell& shell) const noexcept {
    switch (shell.workspace()) {
    case CharacterStudioWorkspace::anatomy_groom:
        shell.set_preview_brush(sculpt_radius_, sculpt_enabled_);
        break;
    case CharacterStudioWorkspace::wardrobe_paint:
        shell.set_preview_brush(paint_radius_uv_ * 1.4F, paint_enabled_);
        break;
    case CharacterStudioWorkspace::acting_dialogue:
    case CharacterStudioWorkspace::cook_export:
        shell.set_preview_brush(0.0F, false);
        break;
    }
}

void CharacterStudioApplication::on_package_loaded(const CharacterPackage& package) {
    copy_to_buf(name_buf_.data(), name_buf_.size(), package.identity.display_name);
    copy_to_buf(slug_buf_.data(), slug_buf_.size(), package.identity.slug);
    if (!package.identity.prompt.empty()) {
        copy_to_buf(body_prompt_buf_.data(), body_prompt_buf_.size(), package.identity.prompt);
    }
    if (selected_node_idx_ >= package.dialogue.nodes.size()) {
        selected_node_idx_ = 0U;
    }
    sync_dialogue_node_buffers(package);
    sync_facial_from_package_requested_ = true;
    has_last_stroke_pos_ = false;
    has_last_uv_ = false;
    dry_run_valid_ = false;
    dry_run_summary_.clear();
}

void CharacterStudioApplication::on_workspace_changed(
    CharacterStudioShell& shell,
    const CharacterStudioWorkspace /*workspace*/) {
    has_last_stroke_pos_ = false;
    has_last_uv_ = false;
    sync_preview_brush(shell);
}

void CharacterStudioApplication::on_frame_tick(
    CharacterStudioShell& shell,
    float /*dt_seconds*/) {
    if (sync_facial_from_package_requested_) {
        const ActingMotionSpec& act = shell.package().acting;
        FacialPreviewParams& f = shell.preview_facial();
        f = FacialPreviewParams{};
        f.smile = act.default_smile;
        f.brow_raise = act.default_brow_raise;
        f.frown = act.default_frown;
        if (!act.default_idle_clip.empty()) {
            shell.set_preview_clip(act.default_idle_clip);
        }
        sync_facial_from_package_requested_ = false;
    }
}

void CharacterStudioApplication::handle_viewport_interaction(
    CharacterStudioShell& shell,
    const ViewportInteractionContext& ctx,
    CharacterToolFrameActions& actions) {
    sync_preview_brush(shell);

    switch (shell.workspace()) {
    case CharacterStudioWorkspace::anatomy_groom: {
        if (ctx.left_released) {
            has_last_stroke_pos_ = false;
            return;
        }
        if (!sculpt_enabled_ || !ctx.hit || !ctx.hit->hit) {
            return;
        }
        if (!ctx.left_clicked && !ctx.left_down) {
            return;
        }
        if (!ctx.left_clicked && has_last_stroke_pos_ &&
            glm::length(ctx.hit->position - last_stroke_pos_) < 0.022F) {
            return;
        }

        SculptBrushKind active_brush = sculpt_brush_;
        if (ctx.shift_down) {
            active_brush = SculptBrushKind::smooth;
        } else if (ctx.ctrl_down && active_brush == SculptBrushKind::inflate) {
            active_brush = SculptBrushKind::deflate;
        }

        glm::vec3 disp{0.0F};
        const float mag = 0.018F * sculpt_strength_;
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
        rec.radius = sculpt_radius_;
        rec.strength = sculpt_strength_;
        rec.mirror_x = sculpt_mirror_x_;

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
        break;
    }
    case CharacterStudioWorkspace::wardrobe_paint: {
        if (ctx.left_released) {
            has_last_uv_ = false;
            return;
        }
        if (!paint_enabled_ || !ctx.hit || !ctx.hit->hit) {
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
        stroke.target = paint_auto_target_ ? ctx.hit->target : paint_pinned_target_;
        stroke.mode = ctx.ctrl_down ? CharacterPaintMode::erase : paint_mode_;
        stroke.uv = ctx.hit->uv;
        stroke.radius = paint_radius_uv_;
        stroke.hardness = paint_hardness_;
        stroke.strength = paint_strength_;
        stroke.color = paint_color_;
        stroke.mirror_u = paint_mirror_u_;

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
        break;
    }
    case CharacterStudioWorkspace::acting_dialogue:
        if (ctx.left_clicked && ctx.hit && ctx.hit->hit) {
            if (ctx.hit->position.y > shell.package().body.height_meters * 0.78F) {
                shell.focus_camera_face();
                actions.status_message = "Focused 12-bone facial rig.";
                actions.status_tone = EditorTone::info;
            }
        }
        break;
    case CharacterStudioWorkspace::cook_export:
        break;
    }
}

void CharacterStudioApplication::draw_left_panel(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    sync_preview_brush(shell);
    ImGui::PushID(static_cast<int>(shell.workspace()));
    switch (shell.workspace()) {
    case CharacterStudioWorkspace::anatomy_groom:
        draw_anatomy_left(shell, actions);
        break;
    case CharacterStudioWorkspace::wardrobe_paint:
        draw_wardrobe_left(shell, actions);
        break;
    case CharacterStudioWorkspace::acting_dialogue:
        draw_acting_left(shell, actions);
        break;
    case CharacterStudioWorkspace::cook_export:
        draw_cook_left(shell, actions);
        break;
    }
    ImGui::PopID();
}

void CharacterStudioApplication::draw_right_panel(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    ImGui::PushID(static_cast<int>(shell.workspace()));
    switch (shell.workspace()) {
    case CharacterStudioWorkspace::anatomy_groom:
        draw_anatomy_right(shell, actions);
        break;
    case CharacterStudioWorkspace::wardrobe_paint:
        draw_wardrobe_right(shell, actions);
        break;
    case CharacterStudioWorkspace::acting_dialogue:
        draw_acting_right(shell, actions);
        break;
    case CharacterStudioWorkspace::cook_export:
        draw_cook_right(shell, actions);
        break;
    }
    ImGui::PopID();
}

void CharacterStudioApplication::draw_bottom_panel(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    ImGui::PushID(static_cast<int>(shell.workspace()));
    switch (shell.workspace()) {
    case CharacterStudioWorkspace::anatomy_groom:
        draw_anatomy_bottom(shell, actions);
        break;
    case CharacterStudioWorkspace::wardrobe_paint:
        draw_wardrobe_bottom(shell, actions);
        break;
    case CharacterStudioWorkspace::acting_dialogue:
        draw_acting_bottom(shell, actions);
        break;
    case CharacterStudioWorkspace::cook_export:
        draw_cook_bottom(shell, actions);
        break;
    }
    ImGui::PopID();
}

void CharacterStudioApplication::draw_anatomy_left(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    CharacterPackage& pkg = shell.package();
    const EditorUiKit& kit = shell.ui_kit();

    kit.section_header(EditorIcon::cube, "Identity & Archetype");
    if (ImGui::InputText("Name", name_buf_.data(), name_buf_.size())) {
        pkg.identity.display_name = name_buf_.data();
        actions.mark_dirty = true;
    }
    if (ImGui::InputText("Slug", slug_buf_.data(), slug_buf_.size())) {
        pkg.identity.slug = sanitize_character_slug(slug_buf_.data());
        actions.mark_dirty = true;
    }
    const std::string expected_fname = pkg.identity.slug + ".vcrchar.json";
    if (!pkg.identity.slug.empty() &&
        shell.character_path().filename().string() != expected_fname) {
        const std::string save_as_label = "Save As " + expected_fname;
        if (ImGui::SmallButton(save_as_label.c_str())) {
            std::string err;
            static_cast<void>(shell.save_character_as_slug(pkg.identity.slug, err));
        }
    }

    if (ImGui::InputScalar("Seed", ImGuiDataType_U64, &pkg.identity.seed)) {
        actions.mark_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Randomize")) {
        pkg.identity.seed = next_character_seed(pkg.identity.seed);
        actions.mark_dirty = true;
    }

    static constexpr std::array<CharacterRole, 5U> roles{
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

    static constexpr std::array<CharacterGender, 2U> genders{
        CharacterGender::female, CharacterGender::male};
    if (ImGui::BeginCombo("Gender", to_token(pkg.body.gender).data())) {
        for (const CharacterGender g : genders) {
            const bool sel = (g == pkg.body.gender);
            if (ImGui::Selectable(to_token(g).data(), sel)) {
                if (pkg.body.gender != g) {
                    pkg.body.gender = g;
                    if (g == CharacterGender::female) {
                        pkg.wardrobe.has_beard = false;
                    }
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
        }
        ImGui::EndCombo();
    }

    static constexpr std::array<CharacterArchetype, 6U> archetypes{
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
    kit.section_header(EditorIcon::cube, "3D Sculpt Brush");
    ImGui::Checkbox("Enable Sculpt Brush (LMB)", &sculpt_enabled_);
    static constexpr std::array<SculptBrushKind, 5U> brushes{
        SculptBrushKind::inflate, SculptBrushKind::deflate,
        SculptBrushKind::smooth, SculptBrushKind::grab,
        SculptBrushKind::flatten};
    if (ImGui::BeginCombo("Brush", to_token(sculpt_brush_).data())) {
        for (const SculptBrushKind b : brushes) {
            if (ImGui::Selectable(to_token(b).data(), b == sculpt_brush_)) {
                sculpt_brush_ = b;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SliderFloat("Brush Radius", &sculpt_radius_, 0.02F, 0.30F, "%.3f");
    ImGui::SliderFloat("Brush Strength", &sculpt_strength_, 0.1F, 1.5F, "%.2f");
    ImGui::Checkbox("Mirror X", &sculpt_mirror_x_);
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

void CharacterStudioApplication::draw_anatomy_right(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    CharacterPackage& pkg = shell.package();
    const EditorUiKit& kit = shell.ui_kit();

    kit.section_header(EditorIcon::add, "AI Character & Anatomy Generator");
    ImGui::InputTextMultiline(
        "##CharPrompt", body_prompt_buf_.data(), body_prompt_buf_.size(),
        ImVec2(-1.0F, 64.0F));
    const bool can_ai = !shell.ai_busy();
    if (kit.action_button(
            "GenBodyAi", EditorIcon::play,
            shell.ai_busy() ? "Generating..." : "Generate Anatomy & Face (AI)",
            EditorButtonStyle::primary, can_ai)) {
        const std::string prompt_str = body_prompt_buf_.data();
        const std::uint64_t seed = pkg.identity.seed;
        shell.enqueue_ai_generation("Body & Face",
            [prompt_str, seed](const OllamaBridgeConfig& cfg,
                               CharacterPackage& target,
                               CharacterAiGenerationResult& res) {
                return ai_generate_body_spec(prompt_str, seed, cfg, target, res);
            });
    }
    ImGui::Checkbox("Include AI Outfit", &full_ai_include_outfit_);
    if (kit.action_button(
            "GenFullAi", EditorIcon::add,
            shell.ai_busy() ? "Generating..." : "Generate Full Character (AI)",
            EditorButtonStyle::secondary, can_ai)) {
        const std::string prompt_str = body_prompt_buf_.data();
        const bool include_outfit = full_ai_include_outfit_;
        const std::uint64_t seed = pkg.identity.seed;
        shell.enqueue_ai_generation("Full Character",
            [prompt_str, include_outfit, seed](
                const OllamaBridgeConfig& cfg,
                CharacterPackage& target,
                CharacterAiGenerationResult& res) {
                return run_full_character_ai_pipeline(
                    prompt_str, include_outfit, seed, cfg, target, res);
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

void CharacterStudioApplication::draw_anatomy_bottom(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    const CharacterPackage& pkg = shell.package();
    ImGui::Text("| Sculpt: %zu", pkg.sculpt_strokes.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rebuild Mesh & Textures")) {
        actions.rebuild_preview = true;
    }
}

void CharacterStudioApplication::draw_wardrobe_left(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    CharacterPackage& pkg = shell.package();
    const EditorUiKit& kit = shell.ui_kit();

    kit.section_header(EditorIcon::material, "Modular Wardrobe");

    static constexpr std::array<TopGarmentStyle, 6U> tops{
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

    static constexpr std::array<BottomGarmentStyle, 6U> bottoms{
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

    static constexpr std::array<FootwearStyle, 5U> footwear{
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

    static constexpr std::array<HairStyle, 7U> hairs{
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
    toggle("3D Beard", &pkg.wardrobe.has_beard);
    ImGui::SameLine();
    toggle("Studio Underwear", &pkg.wardrobe.studio_underwear);
    toggle("Occlude Covered Body", &pkg.wardrobe.occlude_covered_body);

    auto thickness_slider = [&](const char* label, float* v) {
        if (ImGui::SliderFloat(label, v, 0.004F, 0.045F, "%.3f")) {
            actions.mark_dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            actions.rebuild_preview = true;
        }
    };
    thickness_slider("Top Thickness", &pkg.wardrobe.top_thickness);
    thickness_slider("Bottom Thickness", &pkg.wardrobe.bottom_thickness);

    ImGui::Separator();
    kit.section_header(EditorIcon::renderer, "PBR Materials & Palette");
    static constexpr std::array<OutfitMaterialPreset, 6U> mats{
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

void CharacterStudioApplication::draw_wardrobe_right(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    CharacterPackage& pkg = shell.package();
    const EditorUiKit& kit = shell.ui_kit();

    kit.section_header(EditorIcon::add, "AI Wardrobe Stylist");
    ImGui::InputTextMultiline(
        "##OutfitPrompt", outfit_prompt_buf_.data(), outfit_prompt_buf_.size(),
        ImVec2(-1.0F, 64.0F));
    if (kit.action_button(
            "GenOutfitAi", EditorIcon::play,
            shell.ai_busy() ? "Generating..." : "Style Outfit (AI)",
            EditorButtonStyle::primary, !shell.ai_busy())) {
        const std::string p = outfit_prompt_buf_.data();
        const std::uint64_t seed = pkg.identity.seed;
        shell.enqueue_ai_generation("Wardrobe & Palette",
            [p, seed](const OllamaBridgeConfig& cfg,
                      CharacterPackage& target,
                      CharacterAiGenerationResult& res) {
                return ai_generate_outfit_spec(p, seed, cfg, target, res);
            });
    }

    ImGui::Separator();
    kit.section_header(EditorIcon::material, "3D PBR Texture Paint");
    ImGui::Checkbox("Enable 3D Paint Brush (LMB)", &paint_enabled_);
    static constexpr std::array<CharacterPaintMode, 4U> modes{
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
    ImGui::Checkbox("Auto-Pick Hit Surface", &paint_auto_target_);
    if (!paint_auto_target_) {
        static constexpr std::array<CharacterPaintTarget, 4U> targets{
            CharacterPaintTarget::skin, CharacterPaintTarget::outfit_primary,
            CharacterPaintTarget::outfit_secondary, CharacterPaintTarget::hair};
        if (ImGui::BeginCombo("Target Slot", to_token(paint_pinned_target_).data())) {
            for (const auto t : targets) {
                if (ImGui::Selectable(to_token(t).data(), t == paint_pinned_target_)) {
                    paint_pinned_target_ = t;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SliderFloat("UV Radius", &paint_radius_uv_, 0.01F, 0.18F, "%.3f");
    ImGui::SliderFloat("Hardness", &paint_hardness_, 0.05F, 1.0F, "%.2f");
    ImGui::SliderFloat("Strength", &paint_strength_, 0.05F, 1.0F, "%.2f");
    ImGui::ColorEdit4("Paint Color", &paint_color_.x);
    ImGui::Checkbox("Mirror U", &paint_mirror_u_);

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

void CharacterStudioApplication::draw_wardrobe_bottom(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    CharacterPackage& pkg = shell.package();
    ImGui::Text("| Paint: %zu | Presets:", pkg.paint_strokes.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Studio Underwear")) {
        pkg.wardrobe.top_style = TopGarmentStyle::none;
        pkg.wardrobe.bottom_style = BottomGarmentStyle::none;
        pkg.wardrobe.footwear_style = FootwearStyle::barefoot;
        pkg.wardrobe.has_belt = false;
        pkg.wardrobe.has_gloves = false;
        pkg.wardrobe.has_pauldrons = false;
        pkg.wardrobe.has_cape = false;
        pkg.wardrobe.studio_underwear = true;
        actions.mark_dirty = true;
        actions.rebuild_preview = true;
    }
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

void CharacterStudioApplication::draw_acting_left(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    CharacterPackage& pkg = shell.package();
    const EditorUiKit& kit = shell.ui_kit();

    kit.section_header(EditorIcon::play, "12 Procedural Motion Clips");
    const auto& clips = shell.preview_clips();
    if (ImGui::BeginChild("##ClipList", ImVec2(-1.0F, 165.0F), ImGuiChildFlags_Borders)) {
        for (const AnimationClip& clip : clips) {
            const bool selected = (clip.name == shell.preview_clip());
            if (ImGui::Selectable(clip.name.c_str(), selected)) {
                shell.set_preview_clip(clip.name);
            }
        }
    }
    ImGui::EndChild();

    if (ImGui::BeginCombo("Default Idle Clip", pkg.acting.default_idle_clip.c_str())) {
        for (const AnimationClip& clip : clips) {
            const bool sel = (clip.name == pkg.acting.default_idle_clip);
            if (ImGui::Selectable(clip.name.c_str(), sel)) {
                pkg.acting.default_idle_clip = clip.name;
                shell.set_preview_clip(clip.name);
                actions.mark_dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    kit.section_header(EditorIcon::renderer, "Temperament & Locomotion");
    static constexpr std::array<CharacterTemperament, 6U> temps{
        CharacterTemperament::neutral, CharacterTemperament::warm,
        CharacterTemperament::stern, CharacterTemperament::cheerful,
        CharacterTemperament::guarded, CharacterTemperament::heroic};
    if (ImGui::BeginCombo("Temperament", to_token(pkg.acting.temperament).data())) {
        for (const auto t : temps) {
            if (ImGui::Selectable(to_token(t).data(), t == pkg.acting.temperament)) {
                pkg.acting.temperament = t;
                actions.mark_dirty = true;
                actions.rebuild_preview = true;
            }
        }
        ImGui::EndCombo();
    }
    auto acting_slider = [&](const char* label, float* v, float mn, float mx) {
        if (ImGui::SliderFloat(label, v, mn, mx, "%.2f")) {
            actions.mark_dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            actions.rebuild_preview = true;
        }
    };
    acting_slider("Walk m/s", &pkg.acting.walk_speed_mps, 0.8F, 3.5F);
    acting_slider("Run m/s", &pkg.acting.run_speed_mps, 2.5F, 8.5F);
    acting_slider("Idle Breath", &pkg.acting.idle_breath_amplitude, 0.0F, 2.0F);
    acting_slider("Stride Energy", &pkg.acting.stride_energy, 0.2F, 2.0F);
    acting_slider("Arm Swing", &pkg.acting.arm_swing, 0.0F, 2.0F);
    acting_slider("Blink Interval", &pkg.acting.blink_interval_seconds, 0.8F, 10.0F);

    ImGui::Separator();
    kit.section_header(EditorIcon::camera, "12-Bone Facial Rig & Visemes");
    FacialPreviewParams& fp = shell.preview_facial();
    ImGui::SliderFloat("Smile", &fp.smile, 0.0F, 1.0F);
    ImGui::SliderFloat("Frown", &fp.frown, 0.0F, 1.0F);
    ImGui::SliderFloat("Brow Raise", &fp.brow_raise, 0.0F, 1.0F);
    ImGui::SliderFloat("Eye Blink", &fp.eye_blink, 0.0F, 1.0F);
    ImGui::SliderFloat("Jaw Open", &fp.jaw_open, 0.0F, 1.0F);
    ImGui::SliderFloat("Lip Round", &fp.lip_round, 0.0F, 1.0F);
    ImGui::SliderFloat("Lip Wide", &fp.lip_wide, 0.0F, 1.0F);
    ImGui::SliderFloat("Eye Look X", &fp.eye_look.x, -1.0F, 1.0F);
    ImGui::SliderFloat("Eye Look Y", &fp.eye_look.y, -1.0F, 1.0F);
    if (ImGui::Button("Apply Defaults to Pkg")) {
        pkg.acting.default_smile = fp.smile;
        pkg.acting.default_brow_raise = fp.brow_raise;
        pkg.acting.default_frown = fp.frown;
        actions.mark_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Face")) {
        fp = FacialPreviewParams{};
    }
}

void CharacterStudioApplication::draw_acting_right(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    CharacterPackage& pkg = shell.package();
    const EditorUiKit& kit = shell.ui_kit();

    kit.section_header(EditorIcon::add, "AI Acting & Dialogue Writer");
    ImGui::InputTextMultiline(
        "##ActPrompt", act_prompt_buf_.data(), act_prompt_buf_.size(),
        ImVec2(-1.0F, 56.0F));
    if (kit.action_button(
            "GenActAi", EditorIcon::play,
            shell.ai_busy() ? "Writing..." : "Generate Acting & Dialogue (AI)",
            EditorButtonStyle::primary, !shell.ai_busy())) {
        const std::string p = act_prompt_buf_.data();
        const std::uint64_t seed = pkg.identity.seed;
        shell.enqueue_ai_generation("Acting, Voice & Dialogue",
            [p, seed](const OllamaBridgeConfig& cfg,
                      CharacterPackage& target,
                      CharacterAiGenerationResult& res) {
                return ai_generate_acting_spec(p, seed, cfg, target, res);
            });
    }

    ImGui::Separator();
    kit.section_header(EditorIcon::renderer, "Formant Voice Synthesizer");
    if (ImGui::SliderFloat("Pitch (Hz)", &pkg.voice.pitch_hz, 70.0F, 260.0F, "%.0f")) {
        actions.mark_dirty = true;
    }
    if (ImGui::SliderFloat("Speech Rate", &pkg.voice.speech_rate, 0.55F, 1.75F, "%.2f")) {
        actions.mark_dirty = true;
    }
    if (ImGui::SliderFloat("Cadence", &pkg.voice.cadence_variance, 0.0F, 1.0F, "%.2f")) {
        actions.mark_dirty = true;
    }
    if (ImGui::SliderFloat("Breathiness", &pkg.voice.breathiness, 0.0F, 1.0F, "%.2f")) {
        actions.mark_dirty = true;
    }
    if (ImGui::SliderFloat("Warmth", &pkg.voice.warmth, 0.0F, 1.0F, "%.2f")) {
        actions.mark_dirty = true;
    }

    ImGui::Checkbox("Audition Russian (RU) Line", &preview_russian_);

    ImGui::Separator();
    kit.section_header(EditorIcon::environment, "Dialogue Graph (EN / RU)");
    if (ImGui::BeginCombo("Entry Node", pkg.dialogue.entry_node_id.c_str())) {
        for (const DialogueNodeSpec& n : pkg.dialogue.nodes) {
            const bool sel = (n.id == pkg.dialogue.entry_node_id);
            if (ImGui::Selectable(n.id.c_str(), sel)) {
                pkg.dialogue.entry_node_id = n.id;
                actions.mark_dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button("+ Add Node")) {
        DialogueNodeSpec n{};
        n.id = "node_" + std::to_string(pkg.dialogue.nodes.size() + 1U);
        n.speaker = pkg.identity.display_name;
        n.text = "Keep moving, traveler.";
        n.text_ru = "Двигайся дальше, путник.";
        n.voice_clip_id = n.id;
        pkg.dialogue.nodes.push_back(std::move(n));
        selected_node_idx_ = pkg.dialogue.nodes.size() - 1U;
        sync_dialogue_node_buffers(pkg);
        actions.mark_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("- Remove Node") && pkg.dialogue.nodes.size() > 1U &&
        selected_node_idx_ < pkg.dialogue.nodes.size()) {
        pkg.dialogue.nodes.erase(
            pkg.dialogue.nodes.begin() + static_cast<std::ptrdiff_t>(selected_node_idx_));
        if (selected_node_idx_ >= pkg.dialogue.nodes.size()) {
            selected_node_idx_ = pkg.dialogue.nodes.size() - 1U;
        }
        sync_dialogue_node_buffers(pkg);
        actions.mark_dirty = true;
    }

    if (ImGui::BeginChild("##DlgNodes", ImVec2(-1.0F, 88.0F), ImGuiChildFlags_Borders)) {
        for (std::size_t ni = 0U; ni < pkg.dialogue.nodes.size(); ++ni) {
            const bool sel = (ni == selected_node_idx_);
            const std::string label = pkg.dialogue.nodes[ni].id + " [" +
                pkg.dialogue.nodes[ni].speaker + "]";
            if (ImGui::Selectable(label.c_str(), sel)) {
                selected_node_idx_ = ni;
                sync_dialogue_node_buffers(pkg);
            }
        }
    }
    ImGui::EndChild();

    if (selected_node_idx_ < pkg.dialogue.nodes.size()) {
        DialogueNodeSpec& node = pkg.dialogue.nodes[selected_node_idx_];
        if (ImGui::InputText("Node ID", node_id_buf_.data(), node_id_buf_.size())) {
            node.id = sanitize_character_slug(node_id_buf_.data());
            node.voice_clip_id = node.id;
            actions.mark_dirty = true;
        }
        if (ImGui::InputText("Speaker", speaker_buf_.data(), speaker_buf_.size())) {
            node.speaker = speaker_buf_.data();
            actions.mark_dirty = true;
        }
        static constexpr std::array<const char*, 4U> kExprClips{
            "talk", "wave", "idle", "attack"};
        if (ImGui::BeginCombo("Expression Clip", node.expression_clip.c_str())) {
            for (const char* expr : kExprClips) {
                const bool sel = (node.expression_clip == expr);
                if (ImGui::Selectable(expr, sel)) {
                    node.expression_clip = expr;
                    actions.mark_dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::SliderFloat("Node Smile", &node.smile, 0.0F, 1.0F, "%.2f")) {
            actions.mark_dirty = true;
        }
        if (ImGui::SliderFloat("Node Brow", &node.brow_raise, 0.0F, 1.0F, "%.2f")) {
            actions.mark_dirty = true;
        }
        if (ImGui::SliderFloat("Node Frown", &node.frown, 0.0F, 1.0F, "%.2f")) {
            actions.mark_dirty = true;
        }

        if (ImGui::InputTextMultiline("Text (EN)", text_en_buf_.data(), text_en_buf_.size(),
                ImVec2(-1.0F, 48.0F))) {
            node.text = text_en_buf_.data();
            actions.mark_dirty = true;
        }
        if (ImGui::InputTextMultiline("Text (RU)", text_ru_buf_.data(), text_ru_buf_.size(),
                ImVec2(-1.0F, 48.0F))) {
            node.text_ru = text_ru_buf_.data();
            actions.mark_dirty = true;
        }

        if (ImGui::CollapsingHeader("Dialogue Choices", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("+ Add Choice")) {
                DialogueChoiceSpec ch{};
                ch.text = "Continue";
                ch.text_ru = "Продолжить";
                ch.closes_dialogue = true;
                node.choices.push_back(std::move(ch));
                sync_dialogue_node_buffers(pkg);
                actions.mark_dirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("- Remove Choice") && !node.choices.empty()) {
                node.choices.pop_back();
                sync_dialogue_node_buffers(pkg);
                actions.mark_dirty = true;
            }

            const std::size_t choice_count =
                std::min(node.choices.size(), choice_bufs_.size());
            for (std::size_t ci = 0U; ci < choice_count; ++ci) {
                ImGui::PushID(static_cast<int>(ci));
                DialogueChoiceSpec& ch = node.choices[ci];
                ChoiceEditBuffers& cb = choice_bufs_[ci];
                ImGui::Separator();
                if (ImGui::InputText("Choice EN", cb.text_en.data(), cb.text_en.size())) {
                    ch.text = cb.text_en.data();
                    actions.mark_dirty = true;
                }
                if (ImGui::InputText("Choice RU", cb.text_ru.data(), cb.text_ru.size())) {
                    ch.text_ru = cb.text_ru.data();
                    actions.mark_dirty = true;
                }
                if (ImGui::InputText("Next Node", cb.next_node.data(), cb.next_node.size())) {
                    ch.next_node_id = sanitize_character_slug(cb.next_node.data());
                    actions.mark_dirty = true;
                }
                if (ImGui::Checkbox("Closes Dialogue", &ch.closes_dialogue)) {
                    actions.mark_dirty = true;
                }
                ImGui::PopID();
            }
        }

        if (kit.action_button(
                "SpeakLine", EditorIcon::play, "Synthesize, Speak & Lip-Sync",
                EditorButtonStyle::primary)) {
            const std::string& line = (preview_russian_ && !node.text_ru.empty())
                ? node.text_ru
                : node.text;
            SynthesizedSpeechClip speech = synthesize_dialogue_speech(
                node.id, line, pkg.voice, shell.preview_mesh().skeleton, pkg.identity.seed);
            const float dur = speech.duration_seconds;
            const std::size_t vc = speech.visemes.size();
            shell.set_preview_clip("talk");
            shell.preview_facial().smile = node.smile;
            shell.preview_facial().brow_raise = node.brow_raise;
            shell.preview_facial().frown = node.frown;
            shell.start_speech_preview(std::move(speech));
            actions.status_message = "Speaking '" + node.id + "' (" +
                std::to_string(dur) + "s, " + std::to_string(vc) + " visemes)";
            actions.status_tone = EditorTone::success;
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop Audio")) {
            shell.stop_speech_preview();
        }
    }
}

void CharacterStudioApplication::draw_acting_bottom(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& actions) {
    const CharacterPackage& pkg = shell.package();
    ImGui::Text(
        "| Rig: %zu | Speech: %s",
        shell.preview_mesh().skeleton.bones.size(),
        shell.speech_preview_active() ? "SPEAKING (Viseme Sync)" : "Idle");
    ImGui::SameLine();
    if (ImGui::SmallButton("Export Selected .wav") &&
        selected_node_idx_ < pkg.dialogue.nodes.size()) {
        const DialogueNodeSpec& node = pkg.dialogue.nodes[selected_node_idx_];
        const std::string& line = (preview_russian_ && !node.text_ru.empty())
            ? node.text_ru
            : node.text;
        const SynthesizedSpeechClip speech = synthesize_dialogue_speech(
            node.id, line, pkg.voice, shell.preview_mesh().skeleton, pkg.identity.seed);
        const std::filesystem::path out_wav = shell.project_directory() / "assets" / "audio" /
            (pkg.identity.slug + "_" + node.id + ".wav");
        std::string err;
        if (write_pcm_wav_file(out_wav, speech.mono_pcm, speech.sample_rate, err)) {
            actions.status_message = "Exported " + out_wav.generic_string();
            actions.status_tone = EditorTone::success;
        } else {
            actions.status_message = "WAV export failed: " + err;
            actions.status_tone = EditorTone::danger;
        }
    }
}

void CharacterStudioApplication::draw_cook_left(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& /*actions*/) {
    const CharacterPackage& pkg = shell.package();
    const EditorUiKit& kit = shell.ui_kit();

    kit.section_header(EditorIcon::import_asset, "Compiler & Publication Settings");
    ImGui::TextDisabled("Target Project:");
    ImGui::TextWrapped("%s", shell.project_directory().generic_string().c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("Source Package:");
    ImGui::TextWrapped("%s", shell.character_path().generic_string().c_str());

    ImGui::Separator();
    static constexpr std::array<const char*, 3U> res_labels{"256 px", "512 px", "1024 px"};
    ImGui::Combo("Texture Resolution", &cook_tex_res_idx_, res_labels.data(), 3);
    ImGui::Checkbox("Bake Formant Voice WAVs", &cook_bake_voice_);
    ImGui::Checkbox("Generate NPC & Player Prefabs", &cook_generate_prefabs_);
    ImGui::Checkbox("Generate EN / RU Localization", &cook_generate_localization_);
    ImGui::Checkbox("Verify Cooked Assets via Engine Loaders", &cook_verify_outputs_);

    ImGui::Spacing();
    if (kit.action_button(
            "CookPublishBtn", EditorIcon::import_asset,
            "Cook & Publish to Project", EditorButtonStyle::primary,
            true, false, ImVec2(-1.0F, 32.0F))) {
        CharacterCookOptions opts{};
        opts.texture_resolution = tex_resolution_from_index(cook_tex_res_idx_);
        opts.bake_voice_lines = cook_bake_voice_;
        opts.generate_prefabs = cook_generate_prefabs_;
        opts.generate_localization = cook_generate_localization_;
        opts.overwrite_existing = true;
        std::string err;
        static_cast<void>(shell.cook_to_project_with_options(opts, cook_verify_outputs_, err));
    }

    ImGui::Spacing();
    if (kit.action_button(
            "DryRunValidateBtn", EditorIcon::play,
            "Dry-Run Validate Mesh & Rig", EditorButtonStyle::secondary,
            true, false, ImVec2(-1.0F, 30.0F))) {
        CharacterPackage sanitized = pkg;
        sanitize_character_package(sanitized);
        const CharacterMeshBuildResult mesh = build_character_mesh(sanitized);
        dry_run_valid_ =
            !mesh.vertices.empty() &&
            !mesh.indices.empty() &&
            mesh.skeleton.bones.size() == character_total_bone_count;
        if (dry_run_valid_) {
            dry_run_summary_ = "Valid dry-run build: " + sanitized.identity.slug +
                " (" + std::to_string(mesh.vertices.size()) + " verts, " +
                std::to_string(mesh.indices.size() / 3U) + " tris, " +
                std::to_string(mesh.skeleton.bones.size()) + " bones, " +
                std::to_string(mesh.submeshes.size()) + " submeshes)";
            shell.set_status(dry_run_summary_, EditorTone::success);
        } else {
            dry_run_summary_ = "Dry-run validation failed: empty mesh or incomplete rig.";
            shell.set_status(dry_run_summary_, EditorTone::danger);
        }
    }
}

void CharacterStudioApplication::draw_cook_right(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& /*actions*/) {
    const EditorUiKit& kit = shell.ui_kit();
    kit.section_header(EditorIcon::folder, "Cooked Asset Manifest & Integrity");

    const std::optional<CharacterCookReport>& report = shell.last_cook_report();
    if (report.has_value()) {
        if (shell.last_cook_verified()) {
            kit.badge("VERIFIED", EditorTone::success);
        } else {
            kit.badge("COOKED (UNVERIFIED)", EditorTone::warning);
        }
        ImGui::SameLine();
        ImGui::Text("%s (%u files)",
            report->character_slug.c_str(), report->cooked_file_count);

        ImGui::Separator();
        ImGui::Text("Vertices: %u | Triangles: %u", report->vertex_count, report->triangle_count);
        ImGui::Text("Bones: %u | Submeshes: %u", report->bone_count, report->submesh_count);
        ImGui::Text("Audio Clips: %zu | Animation Clips: %zu",
            report->audio_assets_relative.size(), report->animation_clips_relative.size());

        ImGui::Separator();
        auto show_path = [](const char* label, const std::filesystem::path& p) {
            if (!p.empty()) {
                ImGui::TextDisabled("%s:", label);
                ImGui::TextWrapped("%s", p.generic_string().c_str());
            }
        };
        show_path("Model", report->model_asset_relative);
        show_path("NPC Prefab", report->npc_prefab_relative);
        show_path("Player Prefab", report->player_prefab_relative);
        show_path("Dialogue", report->dialogue_relative);
        show_path("Loc EN", report->loc_en_relative);
        show_path("Loc RU", report->loc_ru_relative);
    } else {
        kit.badge("UNCOOKED", EditorTone::neutral);
        ImGui::TextWrapped("Click 'Cook & Publish to Project' to emit engine assets into <project>/assets/characters/%s/.",
            shell.package().identity.slug.c_str());
    }

    if (!dry_run_summary_.empty()) {
        ImGui::Separator();
        kit.status_text(
            dry_run_summary_.c_str(),
            dry_run_valid_ ? EditorTone::success : EditorTone::danger);
    }
}

void CharacterStudioApplication::draw_cook_bottom(
    CharacterStudioShell& shell,
    CharacterToolFrameActions& /*actions*/) {
    if (shell.last_cook_report().has_value()) {
        ImGui::Text("| Output: %u files (%s)",
            shell.last_cook_report()->cooked_file_count,
            shell.last_cook_verified() ? "Verified" : "Cooked");
    } else {
        ImGui::TextDisabled("| Not yet cooked in session");
    }
}

int CharacterStudioApplication::run_cli(const CharacterStudioCliArgs& args) {
    const std::filesystem::path pkg_path = args.character_path.empty()
        ? default_character_package_path(args.project_directory, args.character_slug)
        : args.character_path;
    const bool json_out = args.has_flag("json");
    const bool dry_run = args.has_flag("dry-run");
    const bool verify = args.has_flag("verify");
    const bool do_cook = args.cook_after_cli || verify;

    std::error_code exist_ec;
    const bool file_existed_before =
        std::filesystem::is_regular_file(pkg_path, exist_ec) && !exist_ec;

    CharacterPackage pkg{};
    std::string err;
    if (file_existed_before) {
        if (!load_character_package_file(pkg_path, pkg, err)) {
            std::cerr << "vulkan_character_studio: failed to load package: " << err << "\n";
            return 1;
        }
    } else {
        pkg = make_default_character_package(args.character_slug, args.character_slug);
    }

    bool mutated = !file_existed_before;

    std::string stage = "gen";
    if (args.initial_workspace == CharacterStudioWorkspace::wardrobe_paint) {
        stage = "outfit";
    } else if (args.initial_workspace == CharacterStudioWorkspace::acting_dialogue) {
        stage = "act";
    } else if (args.initial_workspace == CharacterStudioWorkspace::cook_export) {
        stage = "cook";
    }
    if (const auto s = args.find_option("stage")) {
        stage = std::string(*s);
    }

    // Identity & Body options
    if (const auto v = args.find_option("name")) {
        pkg.identity.display_name = std::string(*v);
        mutated = true;
    }
    if (const auto v = args.find_option("role")) {
        if (const auto r = parse_character_role(*v)) {
            pkg.identity.role = *r;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("seed")) {
        try {
            pkg.identity.seed = std::stoull(std::string(*v));
            mutated = true;
        } catch (...) {}
    }

    if (const auto full_p = args.find_option("full-prompt")) {
        CharacterAiGenerationResult ai_res{};
        if (!run_full_character_ai_pipeline(
                *full_p, true, pkg.identity.seed, args.ollama, pkg, ai_res)) {
            std::cerr << "vulkan_character_studio: AI generation error: "
                      << ai_res.diagnostic << "\n";
            return 1;
        }
        mutated = true;
        if (!json_out) {
            std::cout << "[ai] " << ai_res.summary << "\n";
        }
    }
    if (const auto body_p = args.find_option("body-prompt")) {
        CharacterAiGenerationResult ai_res{};
        if (!ai_generate_body_spec(*body_p, pkg.identity.seed, args.ollama, pkg, ai_res)) {
            std::cerr << "vulkan_character_studio: AI body error: "
                      << ai_res.diagnostic << "\n";
            return 1;
        }
        mutated = true;
        if (!json_out) {
            std::cout << "[ai:body] " << ai_res.summary << "\n";
        }
    }
    if (const auto outfit_p = args.find_option("outfit-prompt")) {
        CharacterAiGenerationResult ai_res{};
        if (!ai_generate_outfit_spec(*outfit_p, pkg.identity.seed, args.ollama, pkg, ai_res)) {
            std::cerr << "vulkan_character_studio: AI outfit error: "
                      << ai_res.diagnostic << "\n";
            return 1;
        }
        mutated = true;
        if (!json_out) {
            std::cout << "[ai:outfit] " << ai_res.summary << "\n";
        }
    }
    if (const auto act_p = args.find_option("act-prompt")) {
        CharacterAiGenerationResult ai_res{};
        if (!ai_generate_acting_spec(*act_p, pkg.identity.seed, args.ollama, pkg, ai_res)) {
            std::cerr << "vulkan_character_studio: AI acting error: "
                      << ai_res.diagnostic << "\n";
            return 1;
        }
        mutated = true;
        if (!json_out) {
            std::cout << "[ai:act] " << ai_res.summary << "\n";
        }
    }
    if (const auto prompt = args.find_option("prompt")) {
        CharacterAiGenerationResult ai_res{};
        bool ok = false;
        if (stage == "all") {
            ok = run_full_character_ai_pipeline(
                *prompt, true, pkg.identity.seed, args.ollama, pkg, ai_res);
        } else if (stage == "outfit") {
            ok = ai_generate_outfit_spec(*prompt, pkg.identity.seed, args.ollama, pkg, ai_res);
        } else if (stage == "act") {
            ok = ai_generate_acting_spec(*prompt, pkg.identity.seed, args.ollama, pkg, ai_res);
        } else {
            ok = ai_generate_body_spec(*prompt, pkg.identity.seed, args.ollama, pkg, ai_res);
        }
        if (!ok) {
            std::cerr << "vulkan_character_studio: AI generation error: "
                      << ai_res.diagnostic << "\n";
            return 1;
        }
        mutated = true;
        if (!json_out) {
            std::cout << "[ai] " << ai_res.summary << "\n";
        }
    }

    if (const auto v = args.find_option("gender")) {
        if (const auto g = parse_character_gender(*v)) {
            pkg.body.gender = *g;
            if (*g == CharacterGender::female && !args.has_flag("beard")) {
                pkg.wardrobe.has_beard = false;
            }
            mutated = true;
        } else {
            std::cerr << "vulkan_character_studio: invalid --gender value: " << *v << "\n";
            return 1;
        }
    }
    if (const auto v = args.find_option("archetype")) {
        if (const auto a = parse_character_archetype(*v)) {
            pkg.body.archetype = *a;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("height")) {
        try {
            pkg.body.height_meters = std::stof(std::string(*v));
            mutated = true;
        } catch (...) {}
    }
    if (const auto v = args.find_option("musculature")) {
        try {
            pkg.body.musculature = std::stof(std::string(*v));
            mutated = true;
        } catch (...) {}
    }
    if (const auto v = args.find_option("skin")) {
        if (const auto rgb = parse_hex_rgb(*v)) {
            pkg.body.skin_color = *rgb;
            mutated = true;
        }
    }

    // Wardrobe options
    if (const auto v = args.find_option("top")) {
        if (const auto s = parse_top_garment_style(*v)) {
            pkg.wardrobe.top_style = *s;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("bottom")) {
        if (const auto s = parse_bottom_garment_style(*v)) {
            pkg.wardrobe.bottom_style = *s;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("footwear")) {
        if (const auto s = parse_footwear_style(*v)) {
            pkg.wardrobe.footwear_style = *s;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("hair")) {
        if (const auto s = parse_hair_style(*v)) {
            pkg.wardrobe.hair_style = *s;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("material")) {
        if (const auto m = parse_outfit_material_preset(*v)) {
            pkg.wardrobe.primary_material = *m;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("secondary-material")) {
        if (const auto m = parse_outfit_material_preset(*v)) {
            pkg.wardrobe.secondary_material = *m;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("primary-color")) {
        if (const auto rgb = parse_hex_rgb(*v)) {
            pkg.wardrobe.primary_color = *rgb;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("secondary-color")) {
        if (const auto rgb = parse_hex_rgb(*v)) {
            pkg.wardrobe.secondary_color = *rgb;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("accent-color")) {
        if (const auto rgb = parse_hex_rgb(*v)) {
            pkg.wardrobe.accent_color = *rgb;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("footwear-color")) {
        if (const auto rgb = parse_hex_rgb(*v)) {
            pkg.wardrobe.footwear_color = *rgb;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("hair-color")) {
        if (const auto rgb = parse_hex_rgb(*v)) {
            pkg.wardrobe.hair_color = *rgb;
            mutated = true;
        }
    }
    if (args.has_flag("cape")) { pkg.wardrobe.has_cape = true; mutated = true; }
    if (args.has_flag("no-cape")) { pkg.wardrobe.has_cape = false; mutated = true; }
    if (args.has_flag("pauldrons")) { pkg.wardrobe.has_pauldrons = true; mutated = true; }
    if (args.has_flag("no-pauldrons")) { pkg.wardrobe.has_pauldrons = false; mutated = true; }
    if (args.has_flag("gloves")) { pkg.wardrobe.has_gloves = true; mutated = true; }
    if (args.has_flag("no-gloves")) { pkg.wardrobe.has_gloves = false; mutated = true; }
    if (args.has_flag("belt")) { pkg.wardrobe.has_belt = true; mutated = true; }
    if (args.has_flag("no-belt")) { pkg.wardrobe.has_belt = false; mutated = true; }
    if (args.has_flag("beard")) { pkg.wardrobe.has_beard = true; mutated = true; }
    if (args.has_flag("no-beard")) { pkg.wardrobe.has_beard = false; mutated = true; }
    if (args.has_flag("studio-underwear")) { pkg.wardrobe.studio_underwear = true; mutated = true; }
    if (args.has_flag("no-studio-underwear")) { pkg.wardrobe.studio_underwear = false; mutated = true; }
    if (args.has_flag("occlude-body")) { pkg.wardrobe.occlude_covered_body = true; mutated = true; }
    if (args.has_flag("no-occlude-body")) { pkg.wardrobe.occlude_covered_body = false; mutated = true; }

    // Acting & Dialogue options
    if (const auto v = args.find_option("temperament")) {
        if (const auto t = parse_character_temperament(*v)) {
            pkg.acting.temperament = *t;
            mutated = true;
        }
    }
    if (const auto v = args.find_option("pitch")) {
        try { pkg.voice.pitch_hz = std::stof(std::string(*v)); mutated = true; } catch (...) {}
    }
    if (const auto v = args.find_option("rate")) {
        try { pkg.voice.speech_rate = std::stof(std::string(*v)); mutated = true; } catch (...) {}
    }
    if (const auto v = args.find_option("walk-speed")) {
        try { pkg.acting.walk_speed_mps = std::stof(std::string(*v)); mutated = true; } catch (...) {}
    }
    if (const auto v = args.find_option("run-speed")) {
        try { pkg.acting.run_speed_mps = std::stof(std::string(*v)); mutated = true; } catch (...) {}
    }
    if (const auto v = args.find_option("add-node")) {
        std::string_view sv = *v;
        const std::size_t p1 = sv.find(':');
        const std::size_t p2 = p1 != std::string_view::npos
            ? sv.find(':', p1 + 1U)
            : std::string_view::npos;
        if (p1 != std::string_view::npos && p2 != std::string_view::npos) {
            const std::size_t p3 = sv.find(':', p2 + 1U);
            DialogueNodeSpec node{};
            node.id = sanitize_character_slug(sv.substr(0U, p1));
            node.speaker = std::string(sv.substr(p1 + 1U, p2 - p1 - 1U));
            if (p3 != std::string_view::npos) {
                node.text = std::string(sv.substr(p2 + 1U, p3 - p2 - 1U));
                node.text_ru = std::string(sv.substr(p3 + 1U));
            } else {
                node.text = std::string(sv.substr(p2 + 1U));
            }
            node.voice_clip_id = node.id;
            pkg.dialogue.nodes.push_back(std::move(node));
            mutated = true;
        }
    }

    sanitize_character_package(pkg);

    if (dry_run) {
        const auto mesh = build_character_mesh(pkg);
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            std::cerr << "vulkan_character_studio: dry-run build failed (empty mesh).\n";
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

    const auto wav_out = args.find_option("export-wav");
    bool saved_pkg = false;
    if (mutated || (!do_cook && !wav_out)) {
        if (!save_character_package_file(pkg_path, pkg, err)) {
            std::cerr << "vulkan_character_studio: save failed: " << err << "\n";
            return 1;
        }
        saved_pkg = true;
        if (!json_out) {
            std::cout << "Saved character package: " << pkg_path.generic_string() << "\n";
        }
    }

    if (wav_out) {
        const CookedSkeletonAsset skel = build_character_skeleton(pkg);
        const std::string& text = pkg.dialogue.nodes.empty()
            ? std::string("Ready.")
            : pkg.dialogue.nodes.front().text;
        const SynthesizedSpeechClip clip = synthesize_dialogue_speech(
            "preview", text, pkg.voice, skel, pkg.identity.seed);
        if (!write_pcm_wav_file(
                std::filesystem::path(*wav_out), clip.mono_pcm, clip.sample_rate, err)) {
            std::cerr << "vulkan_character_studio: wav export failed: " << err << "\n";
            return 1;
        }
        if (!json_out) {
            std::cout << "Synthesized WAV: " << *wav_out
                      << " (" << clip.duration_seconds << "s, "
                      << clip.visemes.size() << " visemes)\n";
        }
    }

    if (do_cook) {
        CharacterCookOptions opts{};
        if (const auto v = args.find_option("tex-res")) {
            try {
                opts.texture_resolution = static_cast<std::uint32_t>(
                    std::stoul(std::string(*v)));
            } catch (...) {
                std::cerr << "vulkan_character_studio: invalid --tex-res value: "
                          << *v << "\n";
                return 2;
            }
        }
        if (args.has_flag("no-voice")) {
            opts.bake_voice_lines = false;
        }
        if (args.has_flag("no-prefabs")) {
            opts.generate_prefabs = false;
        }
        if (args.has_flag("no-loc")) {
            opts.generate_localization = false;
        }
        opts.overwrite_existing = true;

        CharacterCookReport report{};
        if (!cook_character_package_to_project(
                pkg, args.project_directory, opts, report, err)) {
            std::cerr << "vulkan_character_studio: cook failed: " << err << "\n";
            return 1;
        }
        if (verify && !verify_cooked_character_outputs(args.project_directory, report, err)) {
            std::cerr << "vulkan_character_studio: verification failed: " << err << "\n";
            return 1;
        }

        if (json_out) {
            std::cout << Json{
                {"status", "ok"},
                {"verified", verify},
                {"saved", saved_pkg},
                {"cooked", true},
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
    } else if (json_out) {
        std::cout << Json{
            {"status", "ok"},
            {"saved", saved_pkg},
            {"cooked", false},
            {"slug", pkg.identity.slug},
            {"package_path", pkg_path.generic_string()},
        }.dump(2) << "\n";
    }

    return 0;
}

void CharacterStudioApplication::print_cli_help() const {
    std::cout
        << "vulkan_character_studio - Unified Character Creation, Wardrobe, Acting & Cooker Studio\n"
        << "Usage:\n"
        << "  vulkan_character_studio [options]\n\n"
        << "General & GUI Options:\n"
        << "  --project <dir>           Target game project directory\n"
        << "  --character <path|slug>   .vcrchar.json path or character slug\n"
        << "  --input <path>            Alias for --character\n"
        << "  --tab <gen|outfit|act|cook> Initial workspace tab (also --workspace)\n"
        << "  --smoke-frames <N>        Run N Vulkan GUI frames and exit cleanly\n"
        << "  --screenshot <png>        Capture Vulkan viewport PNG and exit\n"
        << "  --focus-face              Start camera framed closely on the face\n"
        << "  --mcp-stdio               Enable JSON-RPC 2.0 MCP server on stdin/stdout\n\n"
        << "Headless CLI Options (--cli / --dry-run):\n"
        << "  --cli                     Run headless CLI pipeline without opening GUI\n"
        << "  --stage <gen|outfit|act|all> Stage for --prompt (default: active tab or gen)\n"
        << "  --prompt <text>           AI prompt for --stage\n"
        << "  --body-prompt <text>      AI anatomy/face/skin prompt\n"
        << "  --outfit-prompt <text>    AI wardrobe stylist prompt\n"
        << "  --act-prompt <text>       AI acting, voice & dialogue prompt\n"
        << "  --full-prompt <text>      AI full character generation (body + outfit + acting)\n"
        << "  --name / --role / --seed  Character identity fields\n"
        << "  --gender <female|male>    Anatomical base model\n"
        << "  --archetype <type>        human_balanced|human_athletic|human_heavy|human_slender|stylized_hero|veteran_wanderer\n"
        << "  --height / --musculature / --skin <#RRGGBB>\n"
        << "  --top / --bottom / --footwear / --hair / --material / --secondary-material\n"
        << "  --primary-color / --secondary-color / --accent-color / --footwear-color / --hair-color\n"
        << "  --cape / --no-cape / --belt / --no-belt / --gloves / --no-gloves\n"
        << "  --pauldrons / --no-pauldrons / --beard / --no-beard / --studio-underwear / --no-studio-underwear\n"
        << "  --temperament / --pitch / --rate / --walk-speed / --run-speed\n"
        << "  --add-node <id:sp:en[:ru]> Append bilingual dialogue node\n"
        << "  --export-wav <path>       Synthesize first dialogue node to PCM WAV\n"
        << "  --cook                    Publish cooked assets into <project>/assets/\n"
        << "  --tex-res <N>             Baked texture resolution: 256|512|1024 (default 1024)\n"
        << "  --no-voice / --no-prefabs / --no-loc\n"
        << "  --verify                  Cook & reload outputs via engine loaders to verify integrity\n"
        << "  --dry-run                 Validate package + build mesh without writing files\n"
        << "  --json                    Emit structured JSON summary to stdout\n";
}

bool CharacterStudioApplication::handle_mcp_custom_method(
    const std::string_view method,
    const std::string_view params_json,
    CharacterPackage& package,
    const OllamaBridgeConfig& ollama,
    std::string& out_result_json,
    bool& out_rebuild_preview,
    std::string& out_error) {
    Json p = Json::object();
    try {
        if (!params_json.empty()) {
            p = Json::parse(params_json);
        }
    } catch (...) {}

    if (method == "ai_generate_body") {
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

    if (method == "ai_generate_outfit") {
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

    if (method == "ai_generate_acting") {
        const std::string prompt = p.value("prompt", std::string{"stoic veteran"});
        CharacterAiGenerationResult res{};
        if (!ai_generate_acting_spec(prompt, package.identity.seed, ollama, package, res)) {
            out_error = res.diagnostic;
            return false;
        }
        out_rebuild_preview = true;
        out_result_json = Json{
            {"used_ollama", res.used_ollama},
            {"summary", res.summary},
            {"dialogue_nodes", package.dialogue.nodes.size()},
        }.dump();
        return true;
    }

    if (method == "ai_generate_full_character") {
        const std::string prompt = p.value("prompt", std::string{"athletic veteran ranger"});
        const bool include_outfit = p.value("include_outfit", true);
        const std::uint64_t seed = p.value("seed", package.identity.seed);
        CharacterAiGenerationResult res{};
        if (!run_full_character_ai_pipeline(
                prompt, include_outfit, seed, ollama, package, res)) {
            out_error = res.diagnostic;
            return false;
        }
        out_rebuild_preview = true;
        out_result_json = Json{
            {"used_ollama", res.used_ollama},
            {"summary", res.summary},
            {"gender", std::string(to_token(package.body.gender))},
            {"top_style", std::string(to_token(package.wardrobe.top_style))},
            {"dialogue_nodes", package.dialogue.nodes.size()},
        }.dump();
        return true;
    }

    out_error = "Unknown vulkan_character_studio MCP method: " + std::string(method);
    return false;
}

} // namespace vcr
