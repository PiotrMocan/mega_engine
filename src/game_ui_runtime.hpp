#pragma once

#include "audio_engine.hpp"
#include "game_overlay.hpp"
#include "input_actions.hpp"
#include "localization.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

class GameplayWorld;

enum class GameUiScreen : std::uint8_t {
    in_game = 0U,
    main_menu = 1U,
    pause_menu = 2U,
    settings = 3U,
    dialogue = 4U,
};

enum class GameUiSettingsTab : std::uint8_t {
    graphics = 0U,
    audio = 1U,
    controls = 2U,
    interface_loc = 3U,
};

struct GameUiConfig {
    bool start_in_main_menu = false;
    bool auto_pause_enabled = true;
    bool show_default_hud = true;
    bool show_crosshair = true;
    bool show_fps = false;
    bool show_subtitles = true;
    bool show_world_markers = true;
    bool auto_trigger_marker_dialogue = true;
    UiScaleConfig scale_config{};
    UiCrosshairStyle crosshair_style{};
    std::string title_text{"ui.menu.title"};
    std::string subtitle_text{"ui.menu.subtitle"};
};

struct GameUiHostState {
    std::array<char, 32U> present_mode_label{"Auto"};
    bool fullscreen = false;
    std::array<char, 32U> anti_aliasing_label{"Off"};
    bool dynamic_shadows = true;
    std::uint32_t shadow_resolution = 2048U;
    bool sky_enabled = true;
    float exposure_ev = 0.0F;
    bool bloom_enabled = true;

    void set_present_mode_label(const std::string_view label) noexcept {
        static_cast<void>(component_detail::set_fixed_cstr(present_mode_label, label));
    }
    void set_anti_aliasing_label(const std::string_view label) noexcept {
        static_cast<void>(component_detail::set_fixed_cstr(anti_aliasing_label, label));
    }
};

struct GameUiHostCallbacks {
    void* user_data = nullptr;
    void (*query_host_state)(void* user_data, GameUiHostState& out_state) noexcept = nullptr;
    void (*cycle_present_mode)(void* user_data, int delta) noexcept = nullptr;
    void (*set_fullscreen)(void* user_data, bool enabled) noexcept = nullptr;
    void (*cycle_anti_aliasing)(void* user_data, int delta) noexcept = nullptr;
    void (*set_dynamic_shadows)(void* user_data, bool enabled) noexcept = nullptr;
    void (*cycle_shadow_resolution)(void* user_data, int delta) noexcept = nullptr;
    void (*set_sky_enabled)(void* user_data, bool enabled) noexcept = nullptr;
    void (*adjust_exposure_ev)(void* user_data, float delta_ev) noexcept = nullptr;
    void (*set_bloom_enabled)(void* user_data, bool enabled) noexcept = nullptr;
    void (*on_new_game)(void* user_data) noexcept = nullptr;
    void (*on_restart_game)(void* user_data) noexcept = nullptr;
    void (*on_quit_requested)(void* user_data) noexcept = nullptr;
};

struct UiSubtitleEntry {
    std::array<char, 64U> speaker{};
    std::array<char, 256U> text{};
    std::uint32_t color_rgba8 = pack_rgba8(230U, 240U, 255U, 255U);
    float duration_seconds = 3.5F;
    float remaining_seconds = 0.0F;
    bool active = false;
};

struct DialogueChoice {
    std::string text{};
    std::string next_node_id{};
    std::string set_variable_name{};
    float set_variable_value = 0.0F;
    bool has_variable_effect = false;
    bool close_dialogue = false;
};

struct DialogueNode {
    std::string node_id{};
    std::string speaker{};
    std::string text{};
    std::vector<DialogueChoice> choices{};
};

struct DialogueGraph {
    std::string dialogue_id{};
    std::string entry_node_id{};
    std::vector<DialogueNode> nodes{};
};

struct ProjectedMarkerDraw {
    SceneNodeId node_id{};
    UiMarkerKind kind = UiMarkerKind::objective;
    glm::vec2 screen_px{0.0F, 0.0F};
    glm::vec2 edge_dir{0.0F, -1.0F};
    float distance_m = 0.0F;
    float alpha = 1.0F;
    bool visible_on_screen = false;
    bool clamped_to_edge = false;
    bool show_distance = true;
    bool show_health_bar = false;
    bool interactive = false;
    float health_fraction = 1.0F;
    std::uint32_t color_rgba8 = pack_rgba8(92U, 225U, 255U, 255U);
    std::array<char, 128U> label{};
    std::array<char, 128U> subtitle_text{};
    std::array<char, 64U> dialogue_id{};
};

struct GameUiUpdateInput {
    float delta_seconds = 0.0F;
    glm::uvec2 viewport_extent{1280U, 720U};
    glm::vec2 mouse_pos_px{-1.0F, -1.0F};
    bool mouse_left_pressed = false;
    float display_fps = 0.0F;
    glm::vec3 camera_pos{0.0F};
    glm::vec3 camera_forward{0.0F, 0.0F, -1.0F};
    glm::vec3 camera_up{0.0F, 1.0F, 0.0F};
    glm::vec3 player_pos{0.0F};
    float player_health = 100.0F;
    float player_max_health = 100.0F;
    const SceneDocument* scene = nullptr;
    std::span<const SceneWorldNode> world_nodes{};
    GameplayWorld* gameplay_world = nullptr;
    InputActionSystem* input_actions = nullptr;
    AudioEngine* audio_engine = nullptr;
    LocalizationSystem* localization = nullptr;
};

class GameUiRuntime final {
public:
    static constexpr std::size_t kMaxSubtitleSlots = 4U;
    static constexpr std::size_t kMaxDialogueChoices = 4U;

    GameUiRuntime();

    void configure(const GameUiConfig& config);
    [[nodiscard]] const GameUiConfig& config() const noexcept { return config_; }
    [[nodiscard]] GameUiConfig& config_mut() noexcept { return config_; }

    void set_host_callbacks(const GameUiHostCallbacks& callbacks) noexcept {
        callbacks_ = callbacks;
    }

    void reset_session_state() noexcept;
    void open_screen(GameUiScreen screen) noexcept;
    void open_settings_tab(GameUiSettingsTab tab) noexcept;

    [[nodiscard]] GameUiScreen screen() const noexcept { return screen_; }
    [[nodiscard]] GameUiSettingsTab settings_tab() const noexcept { return settings_tab_; }
    [[nodiscard]] bool blocks_gameplay_simulation() const noexcept {
        return screen_ == GameUiScreen::main_menu ||
               screen_ == GameUiScreen::pause_menu ||
               screen_ == GameUiScreen::settings;
    }
    [[nodiscard]] bool blocks_player_control() const noexcept {
        return blocks_gameplay_simulation() || screen_ == GameUiScreen::dialogue;
    }
    [[nodiscard]] bool wants_mouse_cursor() const noexcept {
        return screen_ != GameUiScreen::in_game;
    }
    [[nodiscard]] bool handles_escape_key() const noexcept {
        return config_.auto_pause_enabled || screen_ != GameUiScreen::in_game;
    }
    [[nodiscard]] bool awaiting_rebind() const noexcept { return awaiting_rebind_; }
    [[nodiscard]] std::size_t selected_index() const noexcept { return selected_index_; }
    [[nodiscard]] std::size_t selected_choice_index() const noexcept { return dialogue_choice_index_; }

    void push_subtitle(
        std::string_view speaker,
        std::string_view text_or_key,
        float duration_seconds = 3.5F,
        std::uint32_t color_rgba8 = pack_rgba8(230U, 240U, 255U, 255U)) noexcept;
    void clear_subtitles() noexcept;
    [[nodiscard]] std::size_t active_subtitle_count() const noexcept;

    void show_toast(std::string_view text_or_key, float duration_seconds = 2.5F) noexcept;

    void clear_dialogues() noexcept;
    void register_dialogue(DialogueGraph graph);
    bool load_dialogue_file(const std::filesystem::path& path, std::string* error_message = nullptr);
    std::size_t load_dialogue_directory(const std::filesystem::path& dir, std::string* error_message = nullptr);

    bool start_dialogue(std::string_view dialogue_id, std::string_view entry_node = {});
    bool start_inline_dialogue(
        std::string_view speaker,
        std::string_view text_or_key,
        std::string_view choice_label_or_key = "ui.common.yes") noexcept;
    void select_dialogue_choice(std::size_t choice_index, GameplayWorld* world = nullptr) noexcept;
    void close_dialogue() noexcept;
    [[nodiscard]] bool dialogue_active() const noexcept {
        return screen_ == GameUiScreen::dialogue;
    }

    [[nodiscard]] std::span<const ProjectedMarkerDraw> projected_markers() const noexcept {
        return projected_markers_;
    }
    [[nodiscard]] SceneNodeId interactive_marker_node() const noexcept {
        return nearest_interact_marker_ < projected_markers_.size()
            ? projected_markers_[nearest_interact_marker_].node_id
            : SceneNodeId{};
    }

    void update(const GameUiUpdateInput& in) noexcept;
    void refresh_markers(const GameUiUpdateInput& in) noexcept;
    void render(GameOverlayCanvas& canvas, const GameUiUpdateInput& in) const noexcept;

private:
    struct CachedWorldPos {
        glm::vec3 pos{0.0F};
        std::uint64_t stamp = 0U;
    };

    void update_subtitles_and_toast(float dt) noexcept;
    void update_world_markers(const GameUiUpdateInput& in) noexcept;
    void update_rebind_capture(const GameUiUpdateInput& in) noexcept;
    void handle_menu_navigation(const GameUiUpdateInput& in) noexcept;
    void activate_menu_item(const GameUiUpdateInput& in, std::size_t row, int adjust_dir) noexcept;
    void activate_settings_item(const GameUiUpdateInput& in, std::size_t row, int adjust_dir) noexcept;
    void handle_dialogue_input(const GameUiUpdateInput& in) noexcept;

    [[nodiscard]] const DialogueNode* current_dialogue_node() const noexcept;
    [[nodiscard]] std::size_t current_menu_row_count() const noexcept;

    void render_world_markers(GameOverlayCanvas& canvas, const UiScaler& scaler, const GameUiUpdateInput& in) const noexcept;
    void render_hud(GameOverlayCanvas& canvas, const UiScaler& scaler, const GameUiUpdateInput& in) const noexcept;
    void render_subtitles(GameOverlayCanvas& canvas, const UiScaler& scaler, const GameUiUpdateInput& in) const noexcept;
    void render_dialogue(GameOverlayCanvas& canvas, const UiScaler& scaler, const GameUiUpdateInput& in) const noexcept;
    void render_menu_overlay(GameOverlayCanvas& canvas, const UiScaler& scaler, const GameUiUpdateInput& in) const noexcept;

    GameUiConfig config_{};
    GameUiHostCallbacks callbacks_{};
    GameUiHostState host_state_{};

    GameUiScreen screen_ = GameUiScreen::in_game;
    GameUiScreen settings_return_screen_ = GameUiScreen::pause_menu;
    GameUiSettingsTab settings_tab_ = GameUiSettingsTab::graphics;
    std::size_t selected_index_ = 0U;
    bool has_started_gameplay_ = false;
    bool quit_requested_ = false;

    bool awaiting_rebind_ = false;
    InputAction rebind_target_action_ = InputAction::interact;
    float nav_repeat_cooldown_ = 0.0F;

    std::array<UiSubtitleEntry, kMaxSubtitleSlots> subtitles_{};
    std::array<char, 256U> toast_text_{};
    float toast_remaining_seconds_ = 0.0F;
    float toast_duration_seconds_ = 2.5F;

    std::vector<DialogueGraph> dialogues_{};
    std::size_t active_dialogue_graph_ = SIZE_MAX;
    std::size_t active_dialogue_node_ = SIZE_MAX;
    bool using_inline_dialogue_ = false;
    std::array<char, 64U> inline_speaker_{};
    std::array<char, 256U> inline_text_{};
    std::array<char, 96U> inline_choice_{};
    std::size_t dialogue_choice_index_ = 0U;

    std::vector<ProjectedMarkerDraw> projected_markers_{};
    std::vector<CachedWorldPos> world_pos_cache_{};
    std::uint64_t world_pos_stamp_ = 1U;
    std::size_t nearest_interact_marker_ = SIZE_MAX;
};

} // namespace vcr
