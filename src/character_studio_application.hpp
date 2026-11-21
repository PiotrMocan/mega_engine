#pragma once

#include "character_studio_shell.hpp"
#include "studio_brush_history.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

class CharacterStudioApplication final : public ICharacterStudioTool {
public:
    [[nodiscard]] int run(int argc, char** argv);

    [[nodiscard]] CharacterStudioToolMeta meta() const noexcept override;

    void on_package_loaded(const CharacterPackage& package) override;
    void on_workspace_changed(
        CharacterStudioShell& shell,
        CharacterStudioWorkspace workspace) override;
    void on_frame_tick(
        CharacterStudioShell& shell,
        float dt_seconds) override;
    void handle_viewport_interaction(
        CharacterStudioShell& shell,
        const ViewportInteractionContext& ctx,
        CharacterToolFrameActions& actions) override;

    void draw_left_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override;
    void draw_right_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override;
    void draw_bottom_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override;

    [[nodiscard]] int run_cli(const CharacterStudioCliArgs& args) override;
    void print_cli_help() const override;

    [[nodiscard]] bool handle_mcp_custom_method(
        std::string_view method,
        std::string_view params_json,
        CharacterPackage& package,
        const OllamaBridgeConfig& ollama,
        std::string& out_result_json,
        CharacterDocumentChange& out_change,
        std::string& out_error) override;

private:
    struct ChoiceEditBuffers final {
        std::array<char, 256> text_en{};
        std::array<char, 256> text_ru{};
        std::array<char, 64> next_node{};
    };

    void sync_dialogue_node_buffers(const CharacterPackage& package);
    void sync_preview_brush(CharacterStudioShell& shell) const noexcept;

    void draw_anatomy_left(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);
    void draw_anatomy_right(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);
    void draw_anatomy_bottom(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);

    void draw_wardrobe_left(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);
    void draw_wardrobe_right(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);
    void draw_wardrobe_bottom(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);

    void draw_acting_left(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);
    void draw_acting_right(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);
    void draw_acting_bottom(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);

    void draw_cook_left(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);
    void draw_cook_right(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);
    void draw_cook_bottom(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions);

    // Workspace 1: Anatomy, Sculpt & Groom
    std::array<char, 96> name_buf_{};
    std::array<char, 64> slug_buf_{};
    std::array<char, 512> body_prompt_buf_{"athletic ranger with sharp jaw and weathered skin"};
    bool full_ai_include_outfit_ = false;
    bool sculpt_enabled_ = true;
    SculptBrushKind sculpt_brush_ = SculptBrushKind::inflate;
    float sculpt_radius_ = 0.085F;
    float sculpt_strength_ = 0.65F;
    bool sculpt_mirror_x_ = true;
    bool has_last_stroke_pos_ = false;
    StudioBrushHistory<SculptStrokeRecord> sculpt_history_{};
    glm::vec3 last_stroke_pos_{0.0F};

    // Workspace 2: Wardrobe & 3D PBR Paint
    std::array<char, 512> outfit_prompt_buf_{"emerald scout trench coat with leather boots and bronze trim"};
    bool paint_enabled_ = true;
    CharacterPaintMode paint_mode_ = CharacterPaintMode::color;
    CharacterPaintTarget paint_pinned_target_ = CharacterPaintTarget::outfit_primary;
    bool paint_auto_target_ = true;
    float paint_radius_uv_ = 0.045F;
    bool paint_skin_in_3d_ = true;
    float paint_skin_radius_m_ = 0.045F;
    bool paint_mirror_x_ = false;
    float paint_hardness_ = 0.78F;
    float paint_strength_ = 0.85F;
    glm::vec4 paint_color_{0.88F, 0.28F, 0.16F, 1.0F};
    bool paint_mirror_u_ = false;
    bool has_last_uv_ = false;
    StudioBrushHistory<CharacterPaintStroke> paint_history_{};
    glm::vec2 last_uv_{0.0F};
    glm::vec3 last_paint_center_{};
    CharacterPaintTarget last_paint_target_{};
    bool last_paint_in_3d_ = false;

    // Workspace 3: Acting, Voice & Dialogue
    bool sync_facial_from_package_requested_ = true;
    bool preview_russian_ = false;
    std::size_t selected_node_idx_ = 0U;
    std::array<char, 512> act_prompt_buf_{"calm tactical commander, warm low voice, bilingual greeting"};
    std::array<char, 64> node_id_buf_{};
    std::array<char, 96> speaker_buf_{};
    std::array<char, 512> text_en_buf_{};
    std::array<char, 512> text_ru_buf_{};
    std::vector<ChoiceEditBuffers> choice_bufs_{};

    // Workspace 4: Cook & Verify
    int cook_tex_res_idx_ = 3;
    bool cook_bake_voice_ = true;
    bool cook_generate_prefabs_ = true;
    bool cook_generate_localization_ = true;
    bool cook_verify_outputs_ = true;
    bool dry_run_valid_ = false;
    std::string dry_run_summary_{};
};

} // namespace vcr
