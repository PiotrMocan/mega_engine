#include "game_ui_runtime.hpp"

#include "gameplay_framework.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string_view>

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kColorBgBackdrop = pack_rgba8(8U, 12U, 20U, 170U);
constexpr std::uint32_t kColorPanelFill = pack_rgba8(16U, 22U, 34U, 232U);
constexpr std::uint32_t kColorPanelBorder = pack_rgba8(74U, 108U, 152U, 235U);
constexpr std::uint32_t kColorAccent = pack_rgba8(72U, 186U, 255U, 255U);
constexpr std::uint32_t kColorAccentFill = pack_rgba8(32U, 78U, 122U, 235U);
constexpr std::uint32_t kColorItemBg = pack_rgba8(24U, 33U, 49U, 210U);
constexpr std::uint32_t kColorTextPrimary = pack_rgba8(238U, 244U, 252U, 255U);
constexpr std::uint32_t kColorTextMuted = pack_rgba8(162U, 178U, 198U, 240U);
constexpr std::uint32_t kColorHealthBg = pack_rgba8(22U, 18U, 24U, 215U);
constexpr std::uint32_t kColorHealthFill = pack_rgba8(216U, 64U, 74U, 240U);
constexpr std::uint32_t kColorSubtitleBg = pack_rgba8(10U, 14U, 20U, 195U);

constexpr std::array<InputAction, 8U> kRebindableActions{{
    InputAction::move_forward,
    InputAction::move_backward,
    InputAction::move_left,
    InputAction::move_right,
    InputAction::run,
    InputAction::interact,
    InputAction::pause,
    InputAction::quick_reset,
}};

constexpr std::array<std::string_view, 8U> kRebindActionLocKeys{{
    "ui.action.move_forward",
    "ui.action.move_backward",
    "ui.action.move_left",
    "ui.action.move_right",
    "ui.action.sprint",
    "ui.action.interact",
    "ui.action.pause",
    "ui.action.quick_reset",
}};

[[nodiscard]] std::uint32_t apply_alpha(const std::uint32_t rgba8, const float alpha_factor) noexcept {
    const float src_a = static_cast<float>((rgba8 >> 24U) & 0xFFU);
    const auto a = static_cast<std::uint32_t>(
        std::clamp(std::lround(src_a * std::clamp(alpha_factor, 0.0F, 1.0F)), 0L, 255L));
    return (rgba8 & 0x00FFFFFFU) | (a << 24U);
}

[[nodiscard]] std::string_view resolve_loc(
    const LocalizationSystem* loc,
    const std::string_view key_or_text) noexcept {
    if (loc != nullptr) {
        return loc->resolve(key_or_text);
    }
    if (!key_or_text.empty() && key_or_text.front() == '@') {
        return key_or_text.substr(1U);
    }
    return key_or_text;
}

[[nodiscard]] UiRect compute_menu_panel_rect(
    const UiScaler& scaler,
    const bool wide_panel) noexcept {
    const glm::vec2 panel_size(wide_panel ? 940.0F : 560.0F, wide_panel ? 690.0F : 500.0F);
    return scaler.resolve(UiAnchorPreset::center, glm::vec2(0.0F, 0.0F), panel_size, glm::vec2(0.5F, 0.5F), true);
}

[[nodiscard]] UiRect compute_settings_tab_rect(
    const UiScaler& scaler,
    const UiRect& panel,
    const std::size_t tab_index) noexcept {
    const float pad_x = scaler.px(28.0F);
    const float tabs_top = panel.min.y + scaler.px(82.0F);
    const float tab_h = scaler.px(38.0F);
    const float gap = scaler.px(8.0F);
    const float avail_w = std::max((panel.max.x - panel.min.x) - pad_x * 2.0F, 40.0F);
    const float tab_w = (avail_w - gap * 3.0F) * 0.25F;
    const float x0 = panel.min.x + pad_x + static_cast<float>(tab_index) * (tab_w + gap);
    return UiRect{glm::vec2(x0, tabs_top), glm::vec2(x0 + tab_w, tabs_top + tab_h)};
}

[[nodiscard]] UiRect compute_menu_row_rect(
    const UiScaler& scaler,
    const UiRect& panel,
    const bool is_settings,
    const std::size_t row_index,
    const std::size_t total_rows) noexcept {
    const float pad_x = scaler.px(32.0F);
    const float top_y = panel.min.y + scaler.px(is_settings ? 136.0F : 124.0F);
    const float bottom_pad = scaler.px(52.0F);
    const float avail_h = std::max(panel.max.y - top_y - bottom_pad, 60.0F);
    const std::size_t safe_rows = std::max<std::size_t>(total_rows, 1U);
    const float max_step = scaler.px(is_settings ? 42.0F : 58.0F);
    const float step_y = std::min(max_step, avail_h / static_cast<float>(safe_rows));
    const float row_h = std::max(step_y - scaler.px(6.0F), 20.0F);
    const float y0 = top_y + static_cast<float>(row_index) * step_y;
    return UiRect{glm::vec2(panel.min.x + pad_x, y0), glm::vec2(panel.max.x - pad_x, y0 + row_h)};
}

[[nodiscard]] UiRect compute_dialogue_panel_rect(const UiScaler& scaler) noexcept {
    return scaler.resolve(
        UiAnchorPreset::bottom_center,
        glm::vec2(0.0F, -28.0F),
        glm::vec2(960.0F, 290.0F),
        glm::vec2(0.5F, 1.0F),
        true);
}

[[nodiscard]] UiRect compute_dialogue_choice_rect(
    const UiScaler& scaler,
    const UiRect& panel,
    const std::size_t choice_idx) noexcept {
    const float pad_x = scaler.px(28.0F);
    const float y0 = panel.min.y + scaler.px(126.0F) + static_cast<float>(choice_idx) * scaler.px(36.0F);
    return UiRect{
        glm::vec2(panel.min.x + pad_x, y0),
        glm::vec2(panel.max.x - pad_x, y0 + scaler.px(31.0F)),
    };
}

[[nodiscard]] std::string_view marker_icon_text(const UiMarkerKind kind) noexcept {
    switch (kind) {
    case UiMarkerKind::objective:
        return "!";
    case UiMarkerKind::waypoint:
        return "*";
    case UiMarkerKind::interact:
        return "E";
    case UiMarkerKind::speaker:
        return "...";
    case UiMarkerKind::hazard:
        return "/!\\";
    case UiMarkerKind::info:
        return "i";
    }
    return "!";
}

[[nodiscard]] std::string_view scale_mode_loc_key(const UiScaleMode mode) noexcept {
    switch (mode) {
    case UiScaleMode::fit:
        return "ui.scale_mode.fit";
    case UiScaleMode::match_height:
        return "ui.scale_mode.match_height";
    case UiScaleMode::match_width:
        return "ui.scale_mode.match_width";
    case UiScaleMode::pixel_constant:
        return "ui.scale_mode.pixel";
    }
    return "ui.scale_mode.fit";
}

} // namespace

GameUiRuntime::GameUiRuntime() {
    projected_markers_.reserve(64U);
    world_pos_cache_.reserve(256U);
    reset_session_state();
}

void GameUiRuntime::configure(const GameUiConfig& config) {
    config_ = config;
    if (config_.start_in_main_menu && screen_ == GameUiScreen::in_game) {
        screen_ = GameUiScreen::main_menu;
        settings_return_screen_ = GameUiScreen::main_menu;
        has_started_gameplay_ = false;
        selected_index_ = 0U;
    }
}

void GameUiRuntime::reset_session_state() noexcept {
    screen_ = config_.start_in_main_menu ? GameUiScreen::main_menu : GameUiScreen::in_game;
    settings_return_screen_ = config_.start_in_main_menu ? GameUiScreen::main_menu : GameUiScreen::pause_menu;
    settings_tab_ = GameUiSettingsTab::graphics;
    selected_index_ = 0U;
    has_started_gameplay_ = !config_.start_in_main_menu;
    quit_requested_ = false;
    awaiting_rebind_ = false;
    nav_repeat_cooldown_ = 0.0F;
    clear_subtitles();
    toast_text_.fill('\0');
    toast_remaining_seconds_ = 0.0F;
    active_dialogue_graph_ = SIZE_MAX;
    active_dialogue_node_ = SIZE_MAX;
    using_inline_dialogue_ = false;
    dialogue_choice_index_ = 0U;
    projected_markers_.clear();
    nearest_interact_marker_ = SIZE_MAX;
}

void GameUiRuntime::open_screen(const GameUiScreen screen) noexcept {
    awaiting_rebind_ = false;
    if (screen == GameUiScreen::settings && screen_ != GameUiScreen::settings) {
        settings_return_screen_ =
            (screen_ == GameUiScreen::main_menu) ? GameUiScreen::main_menu : GameUiScreen::pause_menu;
    }
    screen_ = screen;
    selected_index_ = 0U;
    dialogue_choice_index_ = 0U;
}

void GameUiRuntime::open_settings_tab(const GameUiSettingsTab tab) noexcept {
    settings_tab_ = tab;
    selected_index_ = 0U;
    awaiting_rebind_ = false;
}

void GameUiRuntime::push_subtitle(
    const std::string_view speaker,
    const std::string_view text_or_key,
    const float duration_seconds,
    const std::uint32_t color_rgba8) noexcept {
    if (text_or_key.empty()) {
        return;
    }
    // Shift existing entries if full so newest appears cleanly at bottom
    std::size_t slot = kMaxSubtitleSlots;
    for (std::size_t i = 0U; i < kMaxSubtitleSlots; ++i) {
        if (!subtitles_[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxSubtitleSlots) {
        for (std::size_t i = 0U; i + 1U < kMaxSubtitleSlots; ++i) {
            subtitles_[i] = subtitles_[i + 1U];
        }
        slot = kMaxSubtitleSlots - 1U;
    }

    UiSubtitleEntry& dst = subtitles_[slot];
    dst.active = true;
    dst.color_rgba8 = color_rgba8;
    dst.duration_seconds = std::clamp(duration_seconds, 0.5F, 30.0F);
    dst.remaining_seconds = dst.duration_seconds;
    static_cast<void>(component_detail::set_fixed_cstr(dst.speaker, speaker));
    static_cast<void>(component_detail::set_fixed_cstr(dst.text, text_or_key));
}

void GameUiRuntime::clear_subtitles() noexcept {
    for (auto& sub : subtitles_) {
        sub.active = false;
        sub.remaining_seconds = 0.0F;
        sub.speaker.fill('\0');
        sub.text.fill('\0');
    }
}

std::size_t GameUiRuntime::active_subtitle_count() const noexcept {
    std::size_t count = 0U;
    for (const auto& sub : subtitles_) {
        if (sub.active && sub.remaining_seconds > 0.0F) {
            ++count;
        }
    }
    return count;
}

void GameUiRuntime::show_toast(
    const std::string_view text_or_key,
    const float duration_seconds) noexcept {
    if (text_or_key.empty()) {
        return;
    }
    static_cast<void>(component_detail::set_fixed_cstr(toast_text_, text_or_key));
    toast_duration_seconds_ = std::clamp(duration_seconds, 0.4F, 15.0F);
    toast_remaining_seconds_ = toast_duration_seconds_;
}

void GameUiRuntime::clear_dialogues() noexcept {
    dialogues_.clear();
    if (screen_ == GameUiScreen::dialogue) {
        close_dialogue();
    }
}

void GameUiRuntime::register_dialogue(DialogueGraph graph) {
    if (graph.dialogue_id.empty() || graph.nodes.empty()) {
        return;
    }
    for (auto& existing : dialogues_) {
        if (existing.dialogue_id == graph.dialogue_id) {
            existing = std::move(graph);
            return;
        }
    }
    dialogues_.push_back(std::move(graph));
}

bool GameUiRuntime::load_dialogue_file(
    const std::filesystem::path& path,
    std::string* error_message) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (error_message != nullptr) {
            *error_message = "Failed to open dialogue JSON: " + path.string();
        }
        return false;
    }
    Json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        if (error_message != nullptr) {
            *error_message = "Failed to parse dialogue JSON '" + path.string() + "': " + e.what();
        }
        return false;
    }
    if (!root.is_object() || !root.contains("nodes") || !root["nodes"].is_array()) {
        if (error_message != nullptr) {
            *error_message = "Dialogue JSON missing 'nodes' array: " + path.string();
        }
        return false;
    }

    DialogueGraph graph{};
    std::string default_id = path.stem().string();
    if (default_id.ends_with(".vcrdlg")) {
        default_id.erase(default_id.size() - 7U);
    }
    if (root.contains("dialogue_id") && root["dialogue_id"].is_string()) {
        graph.dialogue_id = root["dialogue_id"].get<std::string>();
    } else if (root.contains("id") && root["id"].is_string()) {
        graph.dialogue_id = root["id"].get<std::string>();
    } else {
        graph.dialogue_id = std::move(default_id);
    }
    if (root.contains("entry_node") && root["entry_node"].is_string()) {
        graph.entry_node_id = root["entry_node"].get<std::string>();
    } else if (root.contains("entry") && root["entry"].is_string()) {
        graph.entry_node_id = root["entry"].get<std::string>();
    }
    for (const auto& jnode : root["nodes"]) {
        if (!jnode.is_object()) {
            continue;
        }
        DialogueNode node{};
        node.node_id = jnode.value("id", std::string{});
        node.speaker = jnode.value("speaker", std::string{});
        node.text = jnode.value("text", std::string{});
        if (jnode.contains("choices") && jnode["choices"].is_array()) {
            for (const auto& jchoice : jnode["choices"]) {
                if (!jchoice.is_object() || node.choices.size() >= kMaxDialogueChoices) {
                    continue;
                }
                DialogueChoice ch{};
                ch.text = jchoice.value("text", std::string{"..."});
                ch.next_node_id = jchoice.value("next", std::string{});
                ch.close_dialogue = jchoice.value("close", ch.next_node_id.empty());
                if (jchoice.contains("set_var") && jchoice["set_var"].is_string()) {
                    ch.set_variable_name = jchoice["set_var"].get<std::string>();
                    if (jchoice.contains("value") && jchoice["value"].is_number()) {
                        ch.set_variable_value = jchoice["value"].get<float>();
                    } else if (jchoice.contains("set_value") && jchoice["set_value"].is_number()) {
                        ch.set_variable_value = jchoice["set_value"].get<float>();
                    } else {
                        ch.set_variable_value = 1.0F;
                    }
                    ch.has_variable_effect = !ch.set_variable_name.empty();
                }
                node.choices.push_back(std::move(ch));
            }
        }
        if (!node.node_id.empty()) {
            if (graph.entry_node_id.empty()) {
                graph.entry_node_id = node.node_id;
            }
            graph.nodes.push_back(std::move(node));
        }
    }
    if (graph.nodes.empty()) {
        if (error_message != nullptr) {
            *error_message = "Dialogue JSON contained zero valid nodes: " + path.string();
        }
        return false;
    }
    register_dialogue(std::move(graph));
    return true;
}

std::size_t GameUiRuntime::load_dialogue_directory(
    const std::filesystem::path& dir,
    std::string* error_message) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return 0U;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string fname = entry.path().filename().string();
        if (fname.ends_with(".vcrdlg.json") || entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::size_t count = 0U;
    for (const auto& file : files) {
        if (load_dialogue_file(file, error_message)) {
            ++count;
        }
    }
    return count;
}

bool GameUiRuntime::start_dialogue(
    const std::string_view dialogue_id,
    const std::string_view entry_node) {
    for (std::size_t gi = 0U; gi < dialogues_.size(); ++gi) {
        const DialogueGraph& g = dialogues_[gi];
        if (g.dialogue_id != dialogue_id || g.nodes.empty()) {
            continue;
        }
        const std::string_view target_node =
            entry_node.empty() ? std::string_view(g.entry_node_id) : entry_node;
        std::size_t ni = 0U;
        for (std::size_t k = 0U; k < g.nodes.size(); ++k) {
            if (g.nodes[k].node_id == target_node) {
                ni = k;
                break;
            }
        }
        using_inline_dialogue_ = false;
        active_dialogue_graph_ = gi;
        active_dialogue_node_ = ni;
        dialogue_choice_index_ = 0U;
        screen_ = GameUiScreen::dialogue;
        return true;
    }
    return false;
}

bool GameUiRuntime::start_inline_dialogue(
    const std::string_view speaker,
    const std::string_view text_or_key,
    const std::string_view choice_label_or_key) noexcept {
    if (text_or_key.empty()) {
        return false;
    }
    using_inline_dialogue_ = true;
    active_dialogue_graph_ = SIZE_MAX;
    active_dialogue_node_ = SIZE_MAX;
    dialogue_choice_index_ = 0U;
    static_cast<void>(component_detail::set_fixed_cstr(
        inline_speaker_, speaker.empty() ? std::string_view("ui.common.speaker") : speaker));
    static_cast<void>(component_detail::set_fixed_cstr(inline_text_, text_or_key));
    static_cast<void>(component_detail::set_fixed_cstr(
        inline_choice_,
        choice_label_or_key.empty() ? std::string_view("ui.common.yes") : choice_label_or_key));
    screen_ = GameUiScreen::dialogue;
    return true;
}

void GameUiRuntime::select_dialogue_choice(
    const std::size_t choice_index,
    GameplayWorld* world) noexcept {
    if (screen_ != GameUiScreen::dialogue) {
        return;
    }
    if (using_inline_dialogue_) {
        close_dialogue();
        return;
    }
    const DialogueNode* node = current_dialogue_node();
    if (node == nullptr) {
        close_dialogue();
        return;
    }
    if (node->choices.empty()) {
        close_dialogue();
        return;
    }
    if (choice_index >= node->choices.size()) {
        return;
    }
    const DialogueChoice& ch = node->choices[choice_index];
    if (world != nullptr && ch.has_variable_effect && !ch.set_variable_name.empty()) {
        world->set_variable(ch.set_variable_name, ch.set_variable_value);
    }
    if (ch.close_dialogue || ch.next_node_id.empty()) {
        close_dialogue();
        return;
    }
    const DialogueGraph& g = dialogues_[active_dialogue_graph_];
    for (std::size_t k = 0U; k < g.nodes.size(); ++k) {
        if (g.nodes[k].node_id == ch.next_node_id) {
            active_dialogue_node_ = k;
            dialogue_choice_index_ = 0U;
            return;
        }
    }
    close_dialogue();
}

void GameUiRuntime::close_dialogue() noexcept {
    using_inline_dialogue_ = false;
    active_dialogue_graph_ = SIZE_MAX;
    active_dialogue_node_ = SIZE_MAX;
    dialogue_choice_index_ = 0U;
    if (screen_ == GameUiScreen::dialogue) {
        screen_ = GameUiScreen::in_game;
    }
}

const DialogueNode* GameUiRuntime::current_dialogue_node() const noexcept {
    if (using_inline_dialogue_ ||
        active_dialogue_graph_ >= dialogues_.size()) {
        return nullptr;
    }
    const DialogueGraph& g = dialogues_[active_dialogue_graph_];
    if (active_dialogue_node_ >= g.nodes.size()) {
        return nullptr;
    }
    return &g.nodes[active_dialogue_node_];
}

std::size_t GameUiRuntime::current_menu_row_count() const noexcept {
    switch (screen_) {
    case GameUiScreen::main_menu:
        return has_started_gameplay_ ? 4U : 3U; // Continue(opt), New Game, Settings, Quit
    case GameUiScreen::pause_menu:
        return 5U; // Resume, Restart, Settings, Main Menu, Quit
    case GameUiScreen::settings:
        switch (settings_tab_) {
        case GameUiSettingsTab::graphics:
            return 9U;
        case GameUiSettingsTab::audio:
            return 7U;
        case GameUiSettingsTab::controls:
            return 13U;
        case GameUiSettingsTab::interface_loc:
            return 10U;
        }
        return 1U;
    case GameUiScreen::dialogue: {
        if (using_inline_dialogue_) {
            return 1U;
        }
        const DialogueNode* n = current_dialogue_node();
        return n != nullptr && !n->choices.empty() ? n->choices.size() : 1U;
    }
    case GameUiScreen::in_game:
        return 0U;
    }
    return 0U;
}

void GameUiRuntime::update_subtitles_and_toast(const float dt) noexcept {
    const float clamped_dt = std::clamp(dt, 0.0F, 0.25F);
    for (auto& sub : subtitles_) {
        if (!sub.active) {
            continue;
        }
        sub.remaining_seconds -= clamped_dt;
        if (sub.remaining_seconds <= 0.0F) {
            sub.active = false;
            sub.remaining_seconds = 0.0F;
        }
    }
    if (toast_remaining_seconds_ > 0.0F) {
        toast_remaining_seconds_ = std::max(0.0F, toast_remaining_seconds_ - clamped_dt);
    }
}

void GameUiRuntime::update_world_markers(const GameUiUpdateInput& in) noexcept {
    projected_markers_.clear();
    nearest_interact_marker_ = SIZE_MAX;

    if (!config_.show_world_markers || in.scene == nullptr ||
        in.viewport_extent.x < 2U || in.viewport_extent.y < 2U) {
        return;
    }

    const auto& pool = in.scene->ui_markers();
    if (pool.empty()) {
        return;
    }

    const std::span<const SceneNodeId> ids = pool.owners();
    const std::span<const UiMarkerComponent> values = pool.values();

    // Index world transforms without dynamic allocations per frame once sized
    std::uint32_t max_idx = 0U;
    for (const SceneWorldNode& wn : in.world_nodes) {
        max_idx = std::max(max_idx, wn.id.index);
    }
    if (world_pos_cache_.size() <= max_idx) {
        world_pos_cache_.resize(static_cast<std::size_t>(max_idx) + 1U);
    }
    ++world_pos_stamp_;
    if (world_pos_stamp_ == 0U) {
        world_pos_stamp_ = 1U;
    }
    for (const SceneWorldNode& wn : in.world_nodes) {
        if (wn.world_visible) {
            world_pos_cache_[wn.id.index].pos = glm::vec3(wn.world_matrix[3]);
            world_pos_cache_[wn.id.index].stamp = world_pos_stamp_;
        }
    }

    const glm::vec2 viewport_f(
        static_cast<float>(in.viewport_extent.x),
        static_cast<float>(in.viewport_extent.y));
    UiScaler scaler(viewport_f, config_.scale_config);
    const glm::mat4 vp = make_ui_view_projection(
        in.camera_pos,
        in.camera_forward,
        in.camera_up,
        viewport_f.x / std::max(viewport_f.y, 1.0F));
    const float edge_margin = scaler.px(44.0F);

    float best_interact_dist = 1.0e9F;

    for (std::size_t i = 0U; i < ids.size() && i < values.size(); ++i) {
        const UiMarkerComponent& marker = values[i];
        if (!marker.enabled) {
            continue;
        }
        const SceneNodeId nid = ids[i];
        if (nid.index >= world_pos_cache_.size() ||
            world_pos_cache_[nid.index].stamp != world_pos_stamp_) {
            continue;
        }

        const glm::vec3 world_pos = world_pos_cache_[nid.index].pos + marker.world_offset;
        const WorldScreenProjection proj = project_world_indicator(
            world_pos,
            in.camera_pos,
            vp,
            viewport_f,
            edge_margin,
            marker.clamp_to_screen);

        if (proj.distance_m > marker.max_distance) {
            continue;
        }
        if (!proj.visible_on_screen && !marker.clamp_to_screen) {
            continue;
        }

        float alpha = 1.0F;
        if (marker.max_distance > marker.fade_start_distance + 0.1F &&
            proj.distance_m > marker.fade_start_distance) {
            alpha = 1.0F - (proj.distance_m - marker.fade_start_distance) /
                               (marker.max_distance - marker.fade_start_distance);
            alpha = std::clamp(alpha, 0.0F, 1.0F);
        }
        if (alpha <= 0.01F) {
            continue;
        }

        float health_frac = 1.0F;
        bool has_health = false;
        float interact_radius = 3.2F;
        if (in.gameplay_world != nullptr) {
            if (const GameplayEntityState* entity = in.gameplay_world->find_entity(nid)) {
                if (entity->destroyed) {
                    continue;
                }
                has_health = true;
                health_frac = entity->max_health > 0.0F
                    ? std::clamp(entity->health / entity->max_health, 0.0F, 1.0F)
                    : 0.0F;
                interact_radius = std::max(entity->interaction_radius, 1.5F);
            }
        } else if (const auto gp = in.scene->gameplay(nid)) {
            interact_radius = std::max(gp->get().interaction_radius, 1.5F);
        }

        const float player_dist = glm::length(world_pos_cache_[nid.index].pos - in.player_pos);
        const bool can_interact =
            (marker.kind == UiMarkerKind::interact || marker.kind == UiMarkerKind::speaker) &&
            player_dist <= interact_radius &&
            (!marker.dialogue_id_view().empty() || !marker.subtitle_text_view().empty());

        ProjectedMarkerDraw draw{};
        draw.node_id = nid;
        draw.kind = marker.kind;
        draw.screen_px = proj.screen_px;
        draw.edge_dir = proj.edge_dir;
        draw.distance_m = proj.distance_m;
        draw.alpha = alpha;
        draw.visible_on_screen = proj.visible_on_screen;
        draw.clamped_to_edge = proj.clamped_to_edge;
        draw.show_distance = marker.show_distance;
        draw.show_health_bar = marker.show_health_bar && has_health;
        draw.interactive = can_interact;
        draw.health_fraction = health_frac;
        draw.color_rgba8 = marker.color_rgba8;
        draw.label = marker.label;
        draw.subtitle_text = marker.subtitle_text;
        draw.dialogue_id = marker.dialogue_id;
        projected_markers_.push_back(draw);

        if (can_interact && player_dist < best_interact_dist) {
            best_interact_dist = player_dist;
            nearest_interact_marker_ = projected_markers_.size() - 1U;
        }
    }
}

void GameUiRuntime::update_rebind_capture(const GameUiUpdateInput& in) noexcept {
    if (in.input_actions == nullptr) {
        awaiting_rebind_ = false;
        return;
    }
    const InputKey key = in.input_actions->consume_last_pressed_key();
    const GamepadButton gp_btn = in.input_actions->consume_last_pressed_gamepad_button();
    if (key == InputKey::escape) {
        awaiting_rebind_ = false;
        return;
    }
    if (key != InputKey::none) {
        ActionBinding b = in.input_actions->binding(rebind_target_action_);
        b.primary_key = key;
        in.input_actions->set_binding(rebind_target_action_, b);
        awaiting_rebind_ = false;
        return;
    }
    if (gp_btn != GamepadButton::none) {
        ActionBinding b = in.input_actions->binding(rebind_target_action_);
        b.gamepad_button = gp_btn;
        in.input_actions->set_binding(rebind_target_action_, b);
        awaiting_rebind_ = false;
    }
}

void GameUiRuntime::activate_menu_item(
    const GameUiUpdateInput& in,
    const std::size_t row,
    const int adjust_dir) noexcept {
    (void)in;
    if (screen_ == GameUiScreen::main_menu) {
        if (adjust_dir != 0) {
            return;
        }
        std::size_t idx = row;
        if (!has_started_gameplay_) {
            ++idx; // Skip Continue
        }
        switch (idx) {
        case 0U: // Continue
            screen_ = GameUiScreen::in_game;
            break;
        case 1U: // New Game
            has_started_gameplay_ = true;
            screen_ = GameUiScreen::in_game;
            if (callbacks_.on_new_game != nullptr) {
                callbacks_.on_new_game(callbacks_.user_data);
            }
            break;
        case 2U: // Settings
            open_screen(GameUiScreen::settings);
            break;
        case 3U: // Quit
            quit_requested_ = true;
            if (callbacks_.on_quit_requested != nullptr) {
                callbacks_.on_quit_requested(callbacks_.user_data);
            }
            break;
        default:
            break;
        }
        return;
    }

    if (screen_ == GameUiScreen::pause_menu) {
        if (adjust_dir != 0) {
            return;
        }
        switch (row) {
        case 0U: // Resume
            screen_ = GameUiScreen::in_game;
            break;
        case 1U: // Restart
            screen_ = GameUiScreen::in_game;
            if (callbacks_.on_restart_game != nullptr) {
                callbacks_.on_restart_game(callbacks_.user_data);
            }
            break;
        case 2U: // Settings
            open_screen(GameUiScreen::settings);
            break;
        case 3U: // Main Menu
            open_screen(GameUiScreen::main_menu);
            break;
        case 4U: // Quit
            quit_requested_ = true;
            if (callbacks_.on_quit_requested != nullptr) {
                callbacks_.on_quit_requested(callbacks_.user_data);
            }
            break;
        default:
            break;
        }
    }
}

void GameUiRuntime::activate_settings_item(
    const GameUiUpdateInput& in,
    const std::size_t row,
    const int adjust_dir) noexcept {
    const int step = adjust_dir != 0 ? adjust_dir : 1;

    if (settings_tab_ == GameUiSettingsTab::graphics) {
        switch (row) {
        case 0U:
            if (callbacks_.cycle_present_mode != nullptr) {
                callbacks_.cycle_present_mode(callbacks_.user_data, step);
            }
            break;
        case 1U:
            if (callbacks_.set_fullscreen != nullptr) {
                callbacks_.set_fullscreen(callbacks_.user_data, !host_state_.fullscreen);
            }
            break;
        case 2U:
            if (callbacks_.cycle_anti_aliasing != nullptr) {
                callbacks_.cycle_anti_aliasing(callbacks_.user_data, step);
            }
            break;
        case 3U:
            if (callbacks_.set_dynamic_shadows != nullptr) {
                callbacks_.set_dynamic_shadows(callbacks_.user_data, !host_state_.dynamic_shadows);
            }
            break;
        case 4U:
            if (callbacks_.cycle_shadow_resolution != nullptr) {
                callbacks_.cycle_shadow_resolution(callbacks_.user_data, step);
            }
            break;
        case 5U:
            if (callbacks_.set_sky_enabled != nullptr) {
                callbacks_.set_sky_enabled(callbacks_.user_data, !host_state_.sky_enabled);
            }
            break;
        case 6U:
            if (callbacks_.adjust_exposure_ev != nullptr) {
                callbacks_.adjust_exposure_ev(callbacks_.user_data, static_cast<float>(step) * 0.25F);
            }
            break;
        case 7U:
            if (callbacks_.set_bloom_enabled != nullptr) {
                callbacks_.set_bloom_enabled(callbacks_.user_data, !host_state_.bloom_enabled);
            }
            break;
        case 8U:
            if (adjust_dir == 0) {
                open_screen(settings_return_screen_);
            }
            break;
        default:
            break;
        }
        return;
    }

    if (settings_tab_ == GameUiSettingsTab::audio) {
        auto adjust_bus = [&](const AudioBusId bus) {
            if (in.audio_engine == nullptr) {
                return;
            }
            const float delta = static_cast<float>(step) * 0.05F;
            const float next = std::clamp(in.audio_engine->bus_volume(bus) + delta, 0.0F, 1.0F);
            in.audio_engine->set_bus_volume(bus, next);
        };
        switch (row) {
        case 0U: adjust_bus(AudioBusId::master); break;
        case 1U: adjust_bus(AudioBusId::sfx); break;
        case 2U: adjust_bus(AudioBusId::voice); break;
        case 3U: adjust_bus(AudioBusId::music); break;
        case 4U: adjust_bus(AudioBusId::ui); break;
        case 5U:
            if (in.audio_engine != nullptr) {
                in.audio_engine->set_bus_muted(
                    AudioBusId::master, !in.audio_engine->bus_muted(AudioBusId::master));
            }
            break;
        case 6U:
            if (adjust_dir == 0) {
                open_screen(settings_return_screen_);
            }
            break;
        default:
            break;
        }
        return;
    }

    if (settings_tab_ == GameUiSettingsTab::controls) {
        if (in.input_actions != nullptr) {
            GamepadConfig cfg = in.input_actions->gamepad_config();
            if (row == 0U) {
                cfg.look_speed_pixels_per_sec = std::clamp(
                    cfg.look_speed_pixels_per_sec + static_cast<float>(step) * 80.0F, 160.0F, 2400.0F);
                in.input_actions->set_gamepad_config(cfg);
                return;
            }
            if (row == 1U) {
                cfg.stick_inner_deadzone = std::clamp(
                    cfg.stick_inner_deadzone + static_cast<float>(step) * 0.03F, 0.04F, 0.45F);
                in.input_actions->set_gamepad_config(cfg);
                return;
            }
            if (row == 2U) {
                cfg.invert_look_y = !cfg.invert_look_y;
                in.input_actions->set_gamepad_config(cfg);
                return;
            }
            if (row >= 3U && row < 3U + kRebindableActions.size()) {
                if (adjust_dir == 0) {
                    static_cast<void>(in.input_actions->consume_last_pressed_key());
                    static_cast<void>(in.input_actions->consume_last_pressed_gamepad_button());
                    rebind_target_action_ = kRebindableActions[row - 3U];
                    awaiting_rebind_ = true;
                }
                return;
            }
            if (row == 3U + kRebindableActions.size() && adjust_dir == 0) {
                in.input_actions->reset_bindings_to_default();
                return;
            }
        }
        if (row == 12U && adjust_dir == 0) {
            open_screen(settings_return_screen_);
        }
        return;
    }

    if (settings_tab_ == GameUiSettingsTab::interface_loc) {
        switch (row) {
        case 0U:
            if (in.localization != nullptr) {
                const auto locales = in.localization->available_locales();
                if (!locales.empty()) {
                    std::size_t current = 0U;
                    for (std::size_t i = 0U; i < locales.size(); ++i) {
                        if (locales[i].locale == in.localization->locale()) {
                            current = i;
                            break;
                        }
                    }
                    const std::size_t next = step >= 0
                        ? (current + 1U) % locales.size()
                        : (current + locales.size() - 1U) % locales.size();
                    static_cast<void>(in.localization->set_locale(locales[next].locale));
                }
            }
            break;
        case 1U: {
            const int cur = static_cast<int>(config_.scale_config.mode);
            const int nxt = (cur + (step >= 0 ? 1 : 3)) % 4;
            config_.scale_config.mode = static_cast<UiScaleMode>(nxt);
            break;
        }
        case 2U:
            config_.scale_config.user_scale = std::clamp(
                config_.scale_config.user_scale + static_cast<float>(step) * 0.10F, 0.60F, 2.00F);
            break;
        case 3U:
            config_.scale_config.safe_area_fraction = std::clamp(
                config_.scale_config.safe_area_fraction + static_cast<float>(step) * 0.01F, 0.0F, 0.08F);
            break;
        case 4U: config_.show_default_hud = !config_.show_default_hud; break;
        case 5U: config_.show_crosshair = !config_.show_crosshair; break;
        case 6U: config_.show_fps = !config_.show_fps; break;
        case 7U: config_.show_subtitles = !config_.show_subtitles; break;
        case 8U: config_.show_world_markers = !config_.show_world_markers; break;
        case 9U:
            if (adjust_dir == 0) {
                open_screen(settings_return_screen_);
            }
            break;
        default:
            break;
        }
    }
}

void GameUiRuntime::handle_dialogue_input(const GameUiUpdateInput& in) noexcept {
    const glm::vec2 viewport_f(
        static_cast<float>(std::max(in.viewport_extent.x, 1U)),
        static_cast<float>(std::max(in.viewport_extent.y, 1U)));
    UiScaler scaler(viewport_f, config_.scale_config);
    const std::size_t choice_count = std::max<std::size_t>(current_menu_row_count(), 1U);
    if (dialogue_choice_index_ >= choice_count) {
        dialogue_choice_index_ = 0U;
    }

    // Mouse hover + click over choice rows
    if (in.mouse_pos_px.x >= 0.0F && in.mouse_pos_px.y >= 0.0F) {
        const UiRect panel = compute_dialogue_panel_rect(scaler);
        for (std::size_t i = 0U; i < choice_count && i < kMaxDialogueChoices; ++i) {
            const UiRect cr = compute_dialogue_choice_rect(scaler, panel, i);
            if (cr.contains(in.mouse_pos_px)) {
                dialogue_choice_index_ = i;
                if (in.mouse_left_pressed) {
                    select_dialogue_choice(i, in.gameplay_world);
                    return;
                }
            }
        }
    }

    if (in.input_actions == nullptr) {
        return;
    }
    const glm::vec2 step = in.input_actions->ui_navigation_step();
    if (in.input_actions->action_pressed(InputAction::ui_up) || step.y > 0.5F) {
        dialogue_choice_index_ = (dialogue_choice_index_ + choice_count - 1U) % choice_count;
    } else if (in.input_actions->action_pressed(InputAction::ui_down) || step.y < -0.5F) {
        dialogue_choice_index_ = (dialogue_choice_index_ + 1U) % choice_count;
    }

    if (in.input_actions->action_pressed(InputAction::ui_confirm) ||
        in.input_actions->action_pressed(InputAction::interact)) {
        select_dialogue_choice(dialogue_choice_index_, in.gameplay_world);
        return;
    }
    if (in.input_actions->action_pressed(InputAction::ui_cancel) ||
        (config_.auto_pause_enabled && in.input_actions->action_pressed(InputAction::pause))) {
        close_dialogue();
    }
}

void GameUiRuntime::handle_menu_navigation(const GameUiUpdateInput& in) noexcept {
    if (callbacks_.query_host_state != nullptr) {
        callbacks_.query_host_state(callbacks_.user_data, host_state_);
    }

    const glm::vec2 viewport_f(
        static_cast<float>(std::max(in.viewport_extent.x, 1U)),
        static_cast<float>(std::max(in.viewport_extent.y, 1U)));
    UiScaler scaler(viewport_f, config_.scale_config);
    const bool is_settings = (screen_ == GameUiScreen::settings);
    const UiRect panel = compute_menu_panel_rect(scaler, is_settings);
    const std::size_t row_count = std::max<std::size_t>(current_menu_row_count(), 1U);
    if (selected_index_ >= row_count) {
        selected_index_ = 0U;
    }

    // Mouse hover + click on Settings tabs and Menu/Settings rows
    if (in.mouse_pos_px.x >= 0.0F && in.mouse_pos_px.y >= 0.0F) {
        if (is_settings) {
            for (std::size_t t = 0U; t < 4U; ++t) {
                if (compute_settings_tab_rect(scaler, panel, t).contains(in.mouse_pos_px) &&
                    in.mouse_left_pressed) {
                    open_settings_tab(static_cast<GameUiSettingsTab>(t));
                    return;
                }
            }
        }
        for (std::size_t r = 0U; r < row_count; ++r) {
            const UiRect row_rect = compute_menu_row_rect(scaler, panel, is_settings, r, row_count);
            if (row_rect.contains(in.mouse_pos_px)) {
                selected_index_ = r;
                if (in.mouse_left_pressed) {
                    if (is_settings) {
                        const float mid_x = (row_rect.min.x + row_rect.max.x) * 0.5F;
                        const int dir = (in.mouse_pos_px.x >= mid_x + scaler.px(40.0F)) ? 1 : 0;
                        activate_settings_item(in, r, dir);
                    } else {
                        activate_menu_item(in, r, 0);
                    }
                    return;
                }
            }
        }
    }

    if (in.input_actions == nullptr) {
        return;
    }

    const glm::vec2 nav = in.input_actions->ui_navigation_step();
    if (in.input_actions->action_pressed(InputAction::ui_up) || nav.y > 0.5F) {
        selected_index_ = (selected_index_ + row_count - 1U) % row_count;
    } else if (in.input_actions->action_pressed(InputAction::ui_down) || nav.y < -0.5F) {
        selected_index_ = (selected_index_ + 1U) % row_count;
    }

    if (is_settings && in.input_actions->action_pressed(InputAction::recover)) {
        const std::size_t t = (static_cast<std::size_t>(settings_tab_) + 1U) % 4U;
        open_settings_tab(static_cast<GameUiSettingsTab>(t));
        return;
    }

    int adjust = 0;
    if (in.input_actions->action_pressed(InputAction::ui_left) || nav.x < -0.5F) {
        adjust = -1;
    } else if (in.input_actions->action_pressed(InputAction::ui_right) || nav.x > 0.5F) {
        adjust = 1;
    }

    if (adjust != 0) {
        if (is_settings) {
            activate_settings_item(in, selected_index_, adjust);
        }
        return;
    }

    if (in.input_actions->action_pressed(InputAction::ui_confirm)) {
        if (is_settings) {
            activate_settings_item(in, selected_index_, 0);
        } else {
            activate_menu_item(in, selected_index_, 0);
        }
        return;
    }

    if (in.input_actions->action_pressed(InputAction::ui_cancel) ||
        in.input_actions->action_pressed(InputAction::pause)) {
        if (is_settings) {
            open_screen(settings_return_screen_);
        } else if (screen_ == GameUiScreen::pause_menu) {
            open_screen(GameUiScreen::in_game);
        }
    }
}

void GameUiRuntime::update(const GameUiUpdateInput& in) noexcept {
    update_subtitles_and_toast(in.delta_seconds);

    if (awaiting_rebind_) {
        update_rebind_capture(in);
        return;
    }

    if (screen_ == GameUiScreen::main_menu ||
        screen_ == GameUiScreen::pause_menu ||
        screen_ == GameUiScreen::settings) {
        handle_menu_navigation(in);
        return;
    }

    if (screen_ == GameUiScreen::dialogue) {
        handle_dialogue_input(in);
        update_world_markers(in);
        return;
    }

    // In-game HUD mode
    if (config_.auto_pause_enabled && in.input_actions != nullptr &&
        (in.input_actions->action_pressed(InputAction::pause) ||
         in.input_actions->action_pressed(InputAction::ui_cancel))) {
        open_screen(GameUiScreen::pause_menu);
        return;
    }

    update_world_markers(in);

    // Check script-driven dialogue trigger from GameplayWorld behavior
    if (in.gameplay_world != nullptr &&
        in.gameplay_world->get_variable("dialogue.requested") > 0.5F) {
        in.gameplay_world->set_variable("dialogue.requested", 0.0F);
        const auto target_idx = static_cast<std::uint32_t>(
            std::max(0.0F, in.gameplay_world->get_variable("dialogue.last_node_index")));
        for (const ProjectedMarkerDraw& m : projected_markers_) {
            if (m.node_id.index == target_idx) {
                const std::string_view dlg_id(m.dialogue_id.data());
                const std::string_view sub_txt(m.subtitle_text.data());
                const std::string_view lbl_txt(m.label.data());
                if (!dlg_id.empty() && start_dialogue(dlg_id)) {
                    return;
                }
                if (!sub_txt.empty()) {
                    static_cast<void>(start_inline_dialogue(lbl_txt, sub_txt));
                    return;
                }
            }
        }
    }

    if (in.input_actions != nullptr &&
        in.input_actions->action_pressed(InputAction::interact) &&
        nearest_interact_marker_ < projected_markers_.size()) {
        const ProjectedMarkerDraw& m = projected_markers_[nearest_interact_marker_];
        const std::string_view dlg_id(m.dialogue_id.data());
        const std::string_view sub_txt(m.subtitle_text.data());
        const std::string_view lbl_txt(m.label.data());
        if (!dlg_id.empty() && start_dialogue(dlg_id)) {
            return;
        }
        if (!sub_txt.empty()) {
            if (m.kind == UiMarkerKind::speaker && config_.auto_trigger_marker_dialogue) {
                static_cast<void>(start_inline_dialogue(lbl_txt, sub_txt));
            } else {
                push_subtitle(lbl_txt, sub_txt, 4.0F, m.color_rgba8);
            }
        }
    }
}

void GameUiRuntime::refresh_markers(const GameUiUpdateInput& in) noexcept {
    update_world_markers(in);
}

void GameUiRuntime::render(
    GameOverlayCanvas& canvas,
    const GameUiUpdateInput& in) const noexcept {
    const glm::vec2 viewport_f(
        static_cast<float>(std::max(in.viewport_extent.x, 1U)),
        static_cast<float>(std::max(in.viewport_extent.y, 1U)));
    UiScaler scaler(viewport_f, config_.scale_config);

    if (screen_ == GameUiScreen::in_game || screen_ == GameUiScreen::dialogue) {
        render_world_markers(canvas, scaler, in);
        render_hud(canvas, scaler, in);
        render_subtitles(canvas, scaler, in);
    }

    if (screen_ == GameUiScreen::dialogue) {
        render_dialogue(canvas, scaler, in);
    } else if (screen_ != GameUiScreen::in_game) {
        render_menu_overlay(canvas, scaler, in);
    }
}

void GameUiRuntime::render_world_markers(
    GameOverlayCanvas& canvas,
    const UiScaler& scaler,
    const GameUiUpdateInput& in) const noexcept {
    if (!config_.show_world_markers || projected_markers_.empty()) {
        return;
    }

    std::array<char, 64U> dist_buf{};
    for (std::size_t i = 0U; i < projected_markers_.size(); ++i) {
        const ProjectedMarkerDraw& m = projected_markers_[i];
        const std::uint32_t col = apply_alpha(m.color_rgba8, m.alpha);
        const std::uint32_t bg = apply_alpha(pack_rgba8(12U, 16U, 24U, 200U), m.alpha);
        const std::uint32_t txt_col = apply_alpha(kColorTextPrimary, m.alpha);
        const float box_r = scaler.px(13.0F);
        const glm::vec2 p = m.screen_px;

        canvas.draw_rounded_panel(
            p - glm::vec2(box_r), p + glm::vec2(box_r), bg, col, scaler.px(1.8F));

        const std::string_view icon = marker_icon_text(m.kind);
        const float icon_h = scaler.px(15.0F);
        const glm::vec2 icon_sz = canvas.measure_text_px(icon, icon_h);
        canvas.draw_text_px(p - icon_sz * 0.5F, icon, icon_h, col, false);

        if (m.clamped_to_edge) {
            const float arrow_dist = box_r + scaler.px(9.0F);
            const glm::vec2 tip = p + m.edge_dir * arrow_dist;
            canvas.draw_line(p + m.edge_dir * box_r, tip, scaler.px(2.2F), col);
        }

        const std::string_view label_txt = resolve_loc(in.localization, std::string_view(m.label.data()));
        const float label_h = scaler.px(14.0F);
        float text_y = p.y - box_r - label_h - scaler.px(5.0F);
        if (m.show_health_bar) {
            const float bar_w = scaler.px(58.0F);
            const float bar_h = scaler.px(5.0F);
            const glm::vec2 b0(p.x - bar_w * 0.5F, text_y - scaler.px(4.0F));
            canvas.draw_progress_bar(
                b0,
                b0 + glm::vec2(bar_w, bar_h),
                m.health_fraction,
                bg,
                apply_alpha(kColorHealthFill, m.alpha),
                col,
                1.0F);
            text_y -= scaler.px(10.0F);
        }

        if (!label_txt.empty()) {
            const glm::vec2 lsz = canvas.measure_text_px(label_txt, label_h);
            canvas.draw_text_px(
                glm::vec2(p.x - lsz.x * 0.5F, text_y), label_txt, label_h, txt_col, true);
        }

        if (m.show_distance) {
            const int meters = static_cast<int>(std::lround(m.distance_m));
            const std::string_view unit = resolve_loc(in.localization, "ui.common.meters_short");
            std::snprintf(dist_buf.data(), dist_buf.size(), "%d %.*s", meters, static_cast<int>(unit.size()), unit.data());
            const float dh = scaler.px(12.0F);
            const glm::vec2 dsz = canvas.measure_text_px(dist_buf.data(), dh);
            canvas.draw_text_px(
                glm::vec2(p.x - dsz.x * 0.5F, p.y + box_r + scaler.px(3.0F)),
                dist_buf.data(),
                dh,
                apply_alpha(kColorTextMuted, m.alpha),
                true);
        }
    }
}

void GameUiRuntime::render_hud(
    GameOverlayCanvas& canvas,
    const UiScaler& scaler,
    const GameUiUpdateInput& in) const noexcept {
    if (!config_.show_default_hud) {
        return;
    }

    // Bottom-left Health panel
    const UiRect hp_panel = scaler.resolve(
        UiAnchorPreset::bottom_left,
        glm::vec2(0.0F, 0.0F),
        glm::vec2(260.0F, 52.0F),
        glm::vec2(0.0F, 1.0F),
        true);
    canvas.draw_rounded_panel(
        hp_panel.min, hp_panel.max, kColorPanelFill, kColorPanelBorder, scaler.px(1.5F));

    const std::string_view hp_label = resolve_loc(in.localization, "ui.common.health");
    const float hp_lbl_h = scaler.px(14.0F);
    canvas.draw_text_px(
        hp_panel.min + glm::vec2(scaler.px(12.0F), scaler.px(8.0F)),
        hp_label,
        hp_lbl_h,
        kColorTextPrimary,
        true);

    std::array<char, 64U> num_buf{};
    std::snprintf(
        num_buf.data(),
        num_buf.size(),
        "%.0f / %.0f",
        static_cast<double>(std::max(0.0F, in.player_health)),
        static_cast<double>(std::max(1.0F, in.player_max_health)));
    const glm::vec2 num_sz = canvas.measure_text_px(num_buf.data(), hp_lbl_h);
    canvas.draw_text_px(
        glm::vec2(hp_panel.max.x - scaler.px(12.0F) - num_sz.x, hp_panel.min.y + scaler.px(8.0F)),
        num_buf.data(),
        hp_lbl_h,
        kColorTextMuted,
        true);

    const float frac = in.player_max_health > 0.0F
        ? std::clamp(in.player_health / in.player_max_health, 0.0F, 1.0F)
        : 0.0F;
    canvas.draw_progress_bar(
        glm::vec2(hp_panel.min.x + scaler.px(12.0F), hp_panel.min.y + scaler.px(30.0F)),
        glm::vec2(hp_panel.max.x - scaler.px(12.0F), hp_panel.max.y - scaler.px(10.0F)),
        frac,
        kColorHealthBg,
        kColorHealthFill,
        kColorPanelBorder,
        1.0F);

    // Optional top-right FPS readout
    if (config_.show_fps && in.display_fps > 0.5F) {
        const UiRect fps_panel = scaler.resolve(
            UiAnchorPreset::top_right,
            glm::vec2(0.0F, 0.0F),
            glm::vec2(148.0F, 34.0F),
            glm::vec2(1.0F, 0.0F),
            true);
        canvas.draw_rounded_panel(
            fps_panel.min, fps_panel.max, kColorPanelFill, kColorPanelBorder, 1.0F);
        const std::string_view fps_lbl = resolve_loc(in.localization, "ui.common.fps");
        std::snprintf(
            num_buf.data(),
            num_buf.size(),
            "%.*s: %.0f",
            static_cast<int>(fps_lbl.size()),
            fps_lbl.data(),
            static_cast<double>(in.display_fps));
        const float fh = scaler.px(15.0F);
        const glm::vec2 fsz = canvas.measure_text_px(num_buf.data(), fh);
        canvas.draw_text_px(fps_panel.center() - fsz * 0.5F, num_buf.data(), fh, kColorAccent, true);
    }

    // Center crosshair
    if (config_.show_crosshair && screen_ == GameUiScreen::in_game) {
        UiCrosshairStyle scaled_cross = config_.crosshair_style;
        scaled_cross.gap_px = scaler.px(config_.crosshair_style.gap_px);
        scaled_cross.arm_length_px = scaler.px(config_.crosshair_style.arm_length_px);
        scaled_cross.thickness_px = std::max(1.0F, scaler.px(config_.crosshair_style.thickness_px));
        scaled_cross.center_dot_radius_px = scaler.px(config_.crosshair_style.center_dot_radius_px);
        canvas.draw_crosshair(scaler.anchor_point(UiAnchorPreset::center, false), scaled_cross);
    }

    // Proximity interact prompt
    if (screen_ == GameUiScreen::in_game &&
        nearest_interact_marker_ < projected_markers_.size()) {
        const ProjectedMarkerDraw& m = projected_markers_[nearest_interact_marker_];
        const std::string_view key_tok = in.input_actions != nullptr
            ? to_token(in.input_actions->binding(InputAction::interact).primary_key)
            : std::string_view("E");
        const std::string_view action_lbl = resolve_loc(
            in.localization,
            std::string_view(m.label.data()).empty()
                ? std::string_view("ui.common.interact")
                : std::string_view(m.label.data()));

        std::array<char, 192U> prompt_buf{};
        std::string_view prompt_txt{};
        if (in.localization != nullptr) {
            const std::array<LocalizationArg, 2U> args{{
                {"key", key_tok},
                {"action", action_lbl},
            }};
            prompt_txt = in.localization->format_into(prompt_buf, "ui.hint.interact_key", args);
        } else {
            const int n = std::snprintf(
                prompt_buf.data(),
                prompt_buf.size(),
                "[%.*s] %.*s",
                static_cast<int>(key_tok.size()),
                key_tok.data(),
                static_cast<int>(action_lbl.size()),
                action_lbl.data());
            prompt_txt = std::string_view(prompt_buf.data(), n > 0 ? static_cast<std::size_t>(n) : 0U);
        }

        const float ph = scaler.px(17.0F);
        const glm::vec2 tsz = canvas.measure_text_px(prompt_txt, ph);
        const UiRect box = scaler.resolve(
            UiAnchorPreset::center,
            glm::vec2(0.0F, 92.0F),
            glm::vec2((tsz.x / std::max(scaler.scale(), 0.1F)) + 34.0F, 36.0F),
            glm::vec2(0.5F, 0.5F),
            false);
        canvas.draw_rounded_panel(box.min, box.max, kColorPanelFill, kColorAccent, scaler.px(1.5F));
        canvas.draw_text_px(box.center() - tsz * 0.5F, prompt_txt, ph, kColorTextPrimary, true);
    }

    // Toast banner
    if (toast_remaining_seconds_ > 0.0F && toast_text_[0] != '\0') {
        const float alpha = toast_remaining_seconds_ < 0.45F ? toast_remaining_seconds_ / 0.45F : 1.0F;
        const std::string_view toast_str = resolve_loc(in.localization, std::string_view(toast_text_.data()));
        const float th = scaler.px(16.0F);
        const glm::vec2 tsz = canvas.measure_text_px(toast_str, th);
        const UiRect box = scaler.resolve(
            UiAnchorPreset::top_center,
            glm::vec2(0.0F, 18.0F),
            glm::vec2((tsz.x / std::max(scaler.scale(), 0.1F)) + 44.0F, 36.0F),
            glm::vec2(0.5F, 0.0F),
            true);
        canvas.draw_rounded_panel(
            box.min,
            box.max,
            apply_alpha(kColorPanelFill, alpha),
            apply_alpha(kColorAccent, alpha),
            1.5F);
        canvas.draw_text_px(box.center() - tsz * 0.5F, toast_str, th, apply_alpha(kColorTextPrimary, alpha), false);
    }
}

void GameUiRuntime::render_subtitles(
    GameOverlayCanvas& canvas,
    const UiScaler& scaler,
    const GameUiUpdateInput& in) const noexcept {
    if (!config_.show_subtitles) {
        return;
    }

    const float font_h = scaler.px(17.0F);
    const float max_w = scaler.px(820.0F);
    float bottom_y = scaler.anchor_point(UiAnchorPreset::bottom_center, true).y - scaler.px(66.0F);
    if (screen_ == GameUiScreen::dialogue) {
        bottom_y = compute_dialogue_panel_rect(scaler).min.y - scaler.px(14.0F);
    }

    std::array<char, 340U> line_buf{};
    for (std::size_t idx = kMaxSubtitleSlots; idx > 0U; --idx) {
        const UiSubtitleEntry& sub = subtitles_[idx - 1U];
        if (!sub.active || sub.remaining_seconds <= 0.0F) {
            continue;
        }
        const float alpha = sub.remaining_seconds < 0.45F ? (sub.remaining_seconds / 0.45F) : 1.0F;
        const std::string_view spk = resolve_loc(in.localization, std::string_view(sub.speaker.data()));
        const std::string_view txt = resolve_loc(in.localization, std::string_view(sub.text.data()));
        std::string_view full_line{};
        if (!spk.empty()) {
            const int n = std::snprintf(
                line_buf.data(),
                line_buf.size(),
                "%.*s: %.*s",
                static_cast<int>(spk.size()),
                spk.data(),
                static_cast<int>(txt.size()),
                txt.data());
            full_line = std::string_view(line_buf.data(), n > 0 ? static_cast<std::size_t>(n) : 0U);
        } else {
            full_line = txt;
        }

        const glm::vec2 wrapped_sz = canvas.measure_text_wrapped(full_line, max_w, font_h, 1.25F);
        const float pad_x = scaler.px(18.0F);
        const float pad_y = scaler.px(8.0F);
        const float box_w = std::min(max_w + pad_x * 2.0F, wrapped_sz.x + pad_x * 2.0F);
        const float box_h = wrapped_sz.y + pad_y * 2.0F;
        const float cx = scaler.anchor_point(UiAnchorPreset::bottom_center, true).x;
        const glm::vec2 b0(cx - box_w * 0.5F, bottom_y - box_h);
        const glm::vec2 b1(cx + box_w * 0.5F, bottom_y);

        canvas.draw_rounded_panel(
            b0, b1, apply_alpha(kColorSubtitleBg, alpha), apply_alpha(kColorPanelBorder, alpha), 1.0F);
        canvas.draw_text_wrapped(
            b0 + glm::vec2(pad_x, pad_y),
            max_w,
            full_line,
            font_h,
            apply_alpha(sub.color_rgba8, alpha),
            1.25F,
            true);
        bottom_y = b0.y - scaler.px(8.0F);
    }
}

void GameUiRuntime::render_dialogue(
    GameOverlayCanvas& canvas,
    const UiScaler& scaler,
    const GameUiUpdateInput& in) const noexcept {
    const UiRect panel = compute_dialogue_panel_rect(scaler);
    canvas.draw_rounded_panel(
        panel.min, panel.max, kColorPanelFill, kColorPanelBorder, scaler.px(2.0F));

    std::string_view speaker_raw{};
    std::string_view body_raw{};
    const DialogueNode* node = current_dialogue_node();
    if (using_inline_dialogue_) {
        speaker_raw = std::string_view(inline_speaker_.data());
        body_raw = std::string_view(inline_text_.data());
    } else if (node != nullptr) {
        speaker_raw = node->speaker;
        body_raw = node->text;
    }

    const std::string_view speaker = resolve_loc(in.localization, speaker_raw);
    const std::string_view body = resolve_loc(in.localization, body_raw);

    const float pad_x = scaler.px(28.0F);
    canvas.draw_text_px(
        glm::vec2(panel.min.x + pad_x, panel.min.y + scaler.px(16.0F)),
        speaker,
        scaler.px(20.0F),
        kColorAccent,
        true);

    canvas.draw_text_wrapped(
        glm::vec2(panel.min.x + pad_x, panel.min.y + scaler.px(46.0F)),
        (panel.max.x - panel.min.x) - pad_x * 2.0F,
        body,
        scaler.px(17.0F),
        kColorTextPrimary,
        1.25F,
        true);

    const std::size_t choice_count = std::max<std::size_t>(current_menu_row_count(), 1U);
    std::array<char, 192U> cbuf{};
    for (std::size_t i = 0U; i < choice_count && i < kMaxDialogueChoices; ++i) {
        const UiRect cr = compute_dialogue_choice_rect(scaler, panel, i);
        const bool selected = (i == dialogue_choice_index_);
        canvas.draw_rounded_panel(
            cr.min,
            cr.max,
            selected ? kColorAccentFill : kColorItemBg,
            selected ? kColorAccent : kColorPanelBorder,
            selected ? scaler.px(2.0F) : 1.0F);

        std::string_view ctext_raw = using_inline_dialogue_
            ? std::string_view(inline_choice_.data())
            : (node != nullptr && i < node->choices.size() ? std::string_view(node->choices[i].text)
                                                           : std::string_view("ui.common.yes"));
        const std::string_view ctext = resolve_loc(in.localization, ctext_raw);
        const int n = std::snprintf(
            cbuf.data(),
            cbuf.size(),
            "%zu. %.*s",
            i + 1U,
            static_cast<int>(ctext.size()),
            ctext.data());
        const std::string_view label(cbuf.data(), n > 0 ? static_cast<std::size_t>(n) : 0U);
        const float ch_h = scaler.px(16.0F);
        canvas.draw_text_px(
            glm::vec2(cr.min.x + scaler.px(14.0F), cr.center().y - ch_h * 0.55F),
            label,
            ch_h,
            kColorTextPrimary,
            false);
    }

    const std::string_view hint = resolve_loc(in.localization, "ui.hint.dialogue_nav");
    canvas.draw_text_px(
        glm::vec2(panel.min.x + pad_x, panel.max.y - scaler.px(22.0F)),
        hint,
        scaler.px(12.0F),
        kColorTextMuted,
        false);
}

void GameUiRuntime::render_menu_overlay(
    GameOverlayCanvas& canvas,
    const UiScaler& scaler,
    const GameUiUpdateInput& in) const noexcept {
    // Full-screen dim backdrop
    canvas.draw_rect(glm::vec2(0.0F, 0.0F), scaler.viewport(), kColorBgBackdrop);

    const bool is_settings = (screen_ == GameUiScreen::settings);
    const UiRect panel = compute_menu_panel_rect(scaler, is_settings);
    canvas.draw_rounded_panel(
        panel.min, panel.max, kColorPanelFill, kColorPanelBorder, scaler.px(2.0F));

    const std::string_view title_key =
        screen_ == GameUiScreen::main_menu ? std::string_view(config_.title_text)
        : screen_ == GameUiScreen::pause_menu ? std::string_view("ui.pause.title")
                                              : std::string_view("ui.settings.title");
    const std::string_view title = resolve_loc(in.localization, title_key);
    const float title_h = scaler.px(28.0F);
    const glm::vec2 tsz = canvas.measure_text_px(title, title_h);
    canvas.draw_text_px(
        glm::vec2(panel.center().x - tsz.x * 0.5F, panel.min.y + scaler.px(20.0F)),
        title,
        title_h,
        kColorTextPrimary,
        true);

    if (screen_ == GameUiScreen::main_menu && !config_.subtitle_text.empty()) {
        const std::string_view sub = resolve_loc(in.localization, config_.subtitle_text);
        const float sub_h = scaler.px(14.0F);
        const glm::vec2 ssz = canvas.measure_text_px(sub, sub_h);
        canvas.draw_text_px(
            glm::vec2(panel.center().x - ssz.x * 0.5F, panel.min.y + scaler.px(56.0F)),
            sub,
            sub_h,
            kColorAccent,
            true);
    }

    if (is_settings) {
        constexpr std::array<std::string_view, 4U> kTabKeys{{
            "ui.settings.tab.graphics",
            "ui.settings.tab.audio",
            "ui.settings.tab.controls",
            "ui.settings.tab.interface",
        }};
        for (std::size_t t = 0U; t < 4U; ++t) {
            const UiRect tr = compute_settings_tab_rect(scaler, panel, t);
            const bool active_tab = (static_cast<std::size_t>(settings_tab_) == t);
            canvas.draw_rounded_panel(
                tr.min,
                tr.max,
                active_tab ? kColorAccentFill : kColorItemBg,
                active_tab ? kColorAccent : kColorPanelBorder,
                active_tab ? scaler.px(2.0F) : 1.0F);
            const std::string_view tab_lbl = resolve_loc(in.localization, kTabKeys[t]);
            const float th = scaler.px(16.0F);
            const glm::vec2 lsz = canvas.measure_text_px(tab_lbl, th);
            canvas.draw_text_px(
                tr.center() - lsz * 0.5F,
                tab_lbl,
                th,
                active_tab ? kColorTextPrimary : kColorTextMuted,
                false);
        }
    }

    const std::size_t row_count = current_menu_row_count();
    const std::string_view on_str = resolve_loc(in.localization, "ui.common.on");
    const std::string_view off_str = resolve_loc(in.localization, "ui.common.off");
    auto bool_str = [&](const bool v) noexcept { return v ? on_str : off_str; };

    std::array<char, 160U> val_buf{};
    for (std::size_t r = 0U; r < row_count; ++r) {
        const UiRect rr = compute_menu_row_rect(scaler, panel, is_settings, r, row_count);
        const bool selected = (r == selected_index_);
        canvas.draw_rounded_panel(
            rr.min,
            rr.max,
            selected ? kColorAccentFill : kColorItemBg,
            selected ? kColorAccent : kColorPanelBorder,
            selected ? scaler.px(2.0F) : 1.0F);

        std::string_view left_lbl{};
        std::string_view right_val{};

        if (screen_ == GameUiScreen::main_menu) {
            const std::size_t mapped = has_started_gameplay_ ? r : (r + 1U);
            constexpr std::array<std::string_view, 4U> kKeys{{
                "ui.menu.continue", "ui.menu.new_game", "ui.menu.settings", "ui.menu.quit"}};
            left_lbl = resolve_loc(in.localization, kKeys[mapped]);
        } else if (screen_ == GameUiScreen::pause_menu) {
            constexpr std::array<std::string_view, 5U> kKeys{{
                "ui.pause.resume", "ui.pause.restart", "ui.pause.settings",
                "ui.pause.main_menu", "ui.pause.quit"}};
            left_lbl = resolve_loc(in.localization, kKeys[r]);
        } else if (settings_tab_ == GameUiSettingsTab::graphics) {
            switch (r) {
            case 0U:
                left_lbl = resolve_loc(in.localization, "ui.settings.present_mode");
                right_val = std::string_view(host_state_.present_mode_label.data());
                break;
            case 1U:
                left_lbl = resolve_loc(in.localization, "ui.settings.fullscreen");
                right_val = bool_str(host_state_.fullscreen);
                break;
            case 2U:
                left_lbl = resolve_loc(in.localization, "ui.settings.anti_aliasing");
                right_val = std::string_view(host_state_.anti_aliasing_label.data());
                break;
            case 3U:
                left_lbl = resolve_loc(in.localization, "ui.settings.shadows");
                right_val = bool_str(host_state_.dynamic_shadows);
                break;
            case 4U:
                left_lbl = resolve_loc(in.localization, "ui.settings.shadow_res");
                std::snprintf(val_buf.data(), val_buf.size(), "%u", host_state_.shadow_resolution);
                right_val = val_buf.data();
                break;
            case 5U:
                left_lbl = resolve_loc(in.localization, "ui.settings.sky");
                right_val = bool_str(host_state_.sky_enabled);
                break;
            case 6U:
                left_lbl = resolve_loc(in.localization, "ui.settings.exposure");
                std::snprintf(val_buf.data(), val_buf.size(), "%+.2f EV", static_cast<double>(host_state_.exposure_ev));
                right_val = val_buf.data();
                break;
            case 7U:
                left_lbl = resolve_loc(in.localization, "ui.settings.bloom");
                right_val = bool_str(host_state_.bloom_enabled);
                break;
            case 8U:
                left_lbl = resolve_loc(in.localization, "ui.settings.back");
                break;
            default:
                break;
            }
        } else if (settings_tab_ == GameUiSettingsTab::audio) {
            auto fmt_bus = [&](const AudioBusId bus) noexcept -> std::string_view {
                const float v = in.audio_engine != nullptr ? in.audio_engine->bus_volume(bus) : 1.0F;
                std::snprintf(val_buf.data(), val_buf.size(), "%d%%", static_cast<int>(std::lround(v * 100.0F)));
                return val_buf.data();
            };
            switch (r) {
            case 0U: left_lbl = resolve_loc(in.localization, "ui.settings.master_volume"); right_val = fmt_bus(AudioBusId::master); break;
            case 1U: left_lbl = resolve_loc(in.localization, "ui.settings.sfx_volume"); right_val = fmt_bus(AudioBusId::sfx); break;
            case 2U: left_lbl = resolve_loc(in.localization, "ui.settings.voice_volume"); right_val = fmt_bus(AudioBusId::voice); break;
            case 3U: left_lbl = resolve_loc(in.localization, "ui.settings.music_volume"); right_val = fmt_bus(AudioBusId::music); break;
            case 4U: left_lbl = resolve_loc(in.localization, "ui.settings.ui_volume"); right_val = fmt_bus(AudioBusId::ui); break;
            case 5U:
                left_lbl = resolve_loc(in.localization, "ui.settings.mute_all");
                right_val = bool_str(in.audio_engine != nullptr && in.audio_engine->bus_muted(AudioBusId::master));
                break;
            case 6U: left_lbl = resolve_loc(in.localization, "ui.settings.back"); break;
            default: break;
            }
        } else if (settings_tab_ == GameUiSettingsTab::controls) {
            const GamepadConfig gp = in.input_actions != nullptr ? in.input_actions->gamepad_config() : GamepadConfig{};
            if (r == 0U) {
                left_lbl = resolve_loc(in.localization, "ui.settings.look_sensitivity");
                std::snprintf(val_buf.data(), val_buf.size(), "%.0f", static_cast<double>(gp.look_speed_pixels_per_sec));
                right_val = val_buf.data();
            } else if (r == 1U) {
                left_lbl = resolve_loc(in.localization, "ui.settings.gamepad_deadzone");
                std::snprintf(val_buf.data(), val_buf.size(), "%.2f", static_cast<double>(gp.stick_inner_deadzone));
                right_val = val_buf.data();
            } else if (r == 2U) {
                left_lbl = resolve_loc(in.localization, "ui.settings.invert_y");
                right_val = bool_str(gp.invert_look_y);
            } else if (r >= 3U && r < 3U + kRebindableActions.size()) {
                const std::size_t ai = r - 3U;
                left_lbl = resolve_loc(in.localization, kRebindActionLocKeys[ai]);
                const InputAction act = kRebindableActions[ai];
                if (awaiting_rebind_ && rebind_target_action_ == act) {
                    right_val = resolve_loc(in.localization, "ui.settings.press_key_prompt");
                } else if (in.input_actions != nullptr) {
                    const ActionBinding b = in.input_actions->binding(act);
                    const std::string_view k = to_token(b.primary_key);
                    const std::string_view g = to_token(b.gamepad_button);
                    std::snprintf(
                        val_buf.data(),
                        val_buf.size(),
                        "[%.*s] / %.*s",
                        static_cast<int>(k.size()),
                        k.data(),
                        static_cast<int>(g.size()),
                        g.data());
                    right_val = val_buf.data();
                }
            } else if (r == 3U + kRebindableActions.size()) {
                left_lbl = resolve_loc(in.localization, "ui.settings.reset_bindings");
            } else {
                left_lbl = resolve_loc(in.localization, "ui.settings.back");
            }
        } else if (settings_tab_ == GameUiSettingsTab::interface_loc) {
            switch (r) {
            case 0U:
                left_lbl = resolve_loc(in.localization, "ui.settings.language");
                if (in.localization != nullptr) {
                    const std::string_view dn = in.localization->display_name(in.localization->locale());
                    right_val = !dn.empty() ? dn : in.localization->locale();
                } else {
                    right_val = "en";
                }
                break;
            case 1U:
                left_lbl = resolve_loc(in.localization, "ui.settings.scale_mode");
                right_val = resolve_loc(in.localization, scale_mode_loc_key(config_.scale_config.mode));
                break;
            case 2U:
                left_lbl = resolve_loc(in.localization, "ui.settings.ui_scale");
                std::snprintf(val_buf.data(), val_buf.size(), "%.2fx", static_cast<double>(config_.scale_config.user_scale));
                right_val = val_buf.data();
                break;
            case 3U:
                left_lbl = resolve_loc(in.localization, "ui.settings.safe_area");
                std::snprintf(val_buf.data(), val_buf.size(), "%.0f%%", static_cast<double>(config_.scale_config.safe_area_fraction * 100.0F));
                right_val = val_buf.data();
                break;
            case 4U: left_lbl = resolve_loc(in.localization, "ui.settings.show_hud"); right_val = bool_str(config_.show_default_hud); break;
            case 5U: left_lbl = resolve_loc(in.localization, "ui.settings.show_crosshair"); right_val = bool_str(config_.show_crosshair); break;
            case 6U: left_lbl = resolve_loc(in.localization, "ui.settings.show_fps"); right_val = bool_str(config_.show_fps); break;
            case 7U: left_lbl = resolve_loc(in.localization, "ui.settings.show_subtitles"); right_val = bool_str(config_.show_subtitles); break;
            case 8U: left_lbl = resolve_loc(in.localization, "ui.settings.show_markers"); right_val = bool_str(config_.show_world_markers); break;
            case 9U: left_lbl = resolve_loc(in.localization, "ui.settings.back"); break;
            default: break;
            }
        }

        const float item_h = scaler.px(is_settings ? 16.0F : 20.0F);
        if (right_val.empty() && !is_settings) {
            const glm::vec2 lsz = canvas.measure_text_px(left_lbl, item_h);
            canvas.draw_text_px(
                rr.center() - glm::vec2(lsz.x * 0.5F, item_h * 0.55F),
                left_lbl,
                item_h,
                kColorTextPrimary,
                false);
        } else {
            canvas.draw_text_px(
                glm::vec2(rr.min.x + scaler.px(16.0F), rr.center().y - item_h * 0.55F),
                left_lbl,
                item_h,
                kColorTextPrimary,
                false);
            if (!right_val.empty()) {
                const glm::vec2 rsz = canvas.measure_text_px(right_val, item_h);
                canvas.draw_text_px(
                    glm::vec2(rr.max.x - scaler.px(16.0F) - rsz.x, rr.center().y - item_h * 0.55F),
                    right_val,
                    item_h,
                    selected ? kColorAccent : kColorTextMuted,
                    false);
            }
        }
    }

    const std::string_view hint = resolve_loc(in.localization, "ui.hint.menu_nav");
    const float hint_h = scaler.px(12.0F);
    const glm::vec2 hsz = canvas.measure_text_px(hint, hint_h);
    canvas.draw_text_px(
        glm::vec2(panel.center().x - hsz.x * 0.5F, panel.max.y - scaler.px(28.0F)),
        hint,
        hint_h,
        kColorTextMuted,
        false);
}

} // namespace vcr
