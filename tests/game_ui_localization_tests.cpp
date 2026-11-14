#include "cli_options.hpp"
#include "editor_scene_tools.hpp"
#include "fly_camera.hpp"
#include "game_overlay.hpp"
#include "game_ui_runtime.hpp"
#include "gameplay_framework.hpp"
#include "gameplay_session.hpp"
#include "input_actions.hpp"
#include "localization.hpp"
#include "mcp_control.hpp"
#include "prefab_io.hpp"
#include "project_system.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"
#include "scene_uuid.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void require_check(
    const bool condition,
    const char* const expression,
    const int line,
    const std::string_view detail = {}) {
    if (condition) {
        return;
    }
    std::cerr << "FAIL line " << line << ": " << expression;
    if (!detail.empty()) {
        std::cerr << " (" << detail << ")";
    }
    std::cerr << '\n';
    ++g_failures;
}

#define REQUIRE(expr) require_check(static_cast<bool>(expr), #expr, __LINE__)
#define REQUIRE_MSG(expr, msg) require_check(static_cast<bool>(expr), #expr, __LINE__, (msg))

[[nodiscard]] bool close_to(
    const float a,
    const float b,
    const float eps = 1.0e-3F) noexcept {
    return std::abs(a - b) <= eps;
}

class ScopedTempDir final {
public:
    ScopedTempDir() {
        static std::atomic<std::uint64_t> counter{0U};
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto id = counter.fetch_add(1U, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
            ("vcr_ui_loc_test_" + std::to_string(tick) + "_" + std::to_string(id));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDir() {
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

void write_text_file(const std::filesystem::path& path, const std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
}

void test_sdf_font_atlas_and_canvas() {
    const vcr::GameUiFontAtlas& atlas = vcr::default_game_ui_font_atlas();
    REQUIRE(atlas.width == vcr::game_ui_sdf_atlas_size);
    REQUIRE(atlas.height == vcr::game_ui_sdf_atlas_size);
    REQUIRE(atlas.sdf);
    REQUIRE(atlas.pixels_r8.size() ==
            static_cast<std::size_t>(atlas.width) * atlas.height);

    const vcr::GameUiGlyphMetrics* glyph_a = atlas.find_glyph(U'A');
    const vcr::GameUiGlyphMetrics* glyph_cyr_upper = atlas.find_glyph(0x0414U); // 'Д'
    const vcr::GameUiGlyphMetrics* glyph_cyr_lower = atlas.find_glyph(0x044FU); // 'я'
    const vcr::GameUiGlyphMetrics* glyph_bullet = atlas.find_glyph(0x2022U);    // '•'
    REQUIRE(glyph_a != nullptr && glyph_a->advance_px > 0.0F);
    REQUIRE(glyph_cyr_upper != nullptr && glyph_cyr_upper->advance_px > 0.0F);
    REQUIRE(glyph_cyr_lower != nullptr && glyph_cyr_lower->advance_px > 0.0F);
    REQUIRE(glyph_bullet != nullptr && glyph_bullet->advance_px > 0.0F);
    REQUIRE(glyph_a->uv_max.x > glyph_a->uv_min.x);
    REQUIRE(glyph_a->uv_max.y > glyph_a->uv_min.y);

    const ScopedTempDir temp;
    const std::filesystem::path font_json = temp.path() / "ui_font.vcrfont.json";
    std::string error;
    REQUIRE_MSG(vcr::save_game_ui_font_atlas_file(font_json, atlas, &error), error);
    REQUIRE(std::filesystem::is_regular_file(font_json));
    REQUIRE(std::filesystem::is_regular_file(temp.path() / "ui_font.png"));

    vcr::GameUiFontAtlas reloaded{};
    REQUIRE_MSG(vcr::load_game_ui_font_atlas_file(font_json, reloaded, &error), error);
    REQUIRE(reloaded.width == atlas.width);
    REQUIRE(reloaded.height == atlas.height);
    REQUIRE(reloaded.sdf == atlas.sdf);
    REQUIRE(reloaded.glyphs.size() == atlas.glyphs.size());
    REQUIRE(reloaded.pixels_r8 == atlas.pixels_r8);
    REQUIRE(reloaded.find_glyph(0x0414U) != nullptr);

    vcr::GameOverlayCanvas canvas;
    canvas.set_viewport(1920.0F, 1080.0F);
    canvas.set_font_atlas(&reloaded);
    canvas.clear();
    canvas.draw_rect({10.0F, 10.0F}, {110.0F, 60.0F}, vcr::pack_rgba8(20U, 30U, 40U, 200U));
    canvas.draw_rounded_panel(
        {24.0F, 24.0F},
        {360.0F, 180.0F},
        vcr::pack_rgba8(15U, 24U, 36U, 225U),
        vcr::pack_rgba8(80U, 160U, 240U, 255U),
        2.0F);
    canvas.draw_crosshair({960.0F, 540.0F}, vcr::UiCrosshairStyle{});
    canvas.draw_text_px(
        {40.0F, 44.0F},
        [] {
            constexpr std::u8string_view kU8 = u8"Привет, Engine UI! 123%";
            return std::string(kU8.begin(), kU8.end());
        }(),
        24.0F,
        vcr::pack_rgba8(255U, 255U, 255U, 255U),
        true);
    canvas.draw_text_wrapped(
        {40.0F, 92.0F},
        280.0F,
        "First second third fourth fifth sixth seventh eighth",
        18.0F,
        vcr::pack_rgba8(220U, 230U, 240U, 255U),
        1.25F,
        false);
    REQUIRE(!canvas.truncated());
    REQUIRE(canvas.vertex_count() > 60U);
    REQUIRE(canvas.measure_text_px("Hello", 20.0F).x > 0.0F);
    REQUIRE(canvas.measure_text_wrapped(
        "WordOne WordTwo WordThree WordFour WordFive", 90.0F, 18.0F).y > 18.0F);
}

void test_ui_scaler_anchors_and_world_projection() {
    const vcr::UiScaleConfig cfg{
        .reference_resolution = {1920.0F, 1080.0F},
        .mode = vcr::UiScaleMode::fit,
        .user_scale = 1.0F,
        .min_scale = 0.5F,
        .max_scale = 3.0F,
        .safe_area_fraction = 0.05F,
    };

    const vcr::UiScaler s720({1280.0F, 720.0F}, cfg);
    const vcr::UiScaler s1080({1920.0F, 1080.0F}, cfg);
    const vcr::UiScaler s_ultrawide({3440.0F, 1440.0F}, cfg);
    const vcr::UiScaler s4k({3840.0F, 2160.0F}, cfg);

    REQUIRE(close_to(s720.scale(), 720.0F / 1080.0F));
    REQUIRE(close_to(s1080.scale(), 1.0F));
    REQUIRE(close_to(s_ultrawide.scale(), 1440.0F / 1080.0F));
    REQUIRE(close_to(s4k.scale(), 2.0F));

    vcr::UiScaleConfig w_cfg = cfg;
    w_cfg.mode = vcr::UiScaleMode::match_width;
    const vcr::UiScaler sw({3440.0F, 1440.0F}, w_cfg);
    REQUIRE(close_to(sw.scale(), 3440.0F / 1920.0F));

    vcr::UiScaleConfig h_cfg = cfg;
    h_cfg.mode = vcr::UiScaleMode::match_height;
    const vcr::UiScaler sh({1280.0F, 1080.0F}, h_cfg);
    REQUIRE(close_to(sh.scale(), 1.0F));

    vcr::UiScaleConfig p_cfg = cfg;
    p_cfg.mode = vcr::UiScaleMode::pixel_constant;
    p_cfg.user_scale = 1.25F;
    const vcr::UiScaler sp({3840.0F, 2160.0F}, p_cfg);
    REQUIRE(close_to(sp.scale(), 1.25F));

    const vcr::UiRect safe = s1080.safe_rect();
    REQUIRE(close_to(safe.min.x, 96.0F));
    REQUIRE(close_to(safe.min.y, 54.0F));
    REQUIRE(close_to(safe.max.x, 1824.0F));
    REQUIRE(close_to(safe.max.y, 1026.0F));

    const std::array<vcr::UiAnchorPreset, 10U> presets{{
        vcr::UiAnchorPreset::top_left,
        vcr::UiAnchorPreset::top_center,
        vcr::UiAnchorPreset::top_right,
        vcr::UiAnchorPreset::middle_left,
        vcr::UiAnchorPreset::center,
        vcr::UiAnchorPreset::middle_right,
        vcr::UiAnchorPreset::bottom_left,
        vcr::UiAnchorPreset::bottom_center,
        vcr::UiAnchorPreset::bottom_right,
        vcr::UiAnchorPreset::stretch_fill,
    }};
    for (const vcr::UiAnchorPreset p : presets) {
        const glm::vec2 pt = s1080.anchor_point(p, true);
        REQUIRE(pt.x >= safe.min.x - 0.1F && pt.x <= safe.max.x + 0.1F);
        REQUIRE(pt.y >= safe.min.y - 0.1F && pt.y <= safe.max.y + 0.1F);
    }
    const vcr::UiRect stretched = s1080.resolve(
        vcr::UiAnchorPreset::stretch_fill, {10.0F, 10.0F}, {0.0F, 0.0F});
    REQUIRE(stretched.size().x > 1000.0F && stretched.size().y > 800.0F);

    const glm::vec3 cam_pos{0.0F, 2.0F, 10.0F};
    const glm::vec3 cam_fwd{0.0F, 0.0F, -1.0F};
    const glm::vec3 cam_up{0.0F, 1.0F, 0.0F};
    const glm::mat4 vp = vcr::make_ui_view_projection(
        cam_pos, cam_fwd, cam_up, 1920.0F / 1080.0F);

    const vcr::WorldScreenProjection front = vcr::project_world_indicator(
        {0.0F, 2.0F, 0.0F}, cam_pos, vp, {1920.0F, 1080.0F}, 48.0F, true);
    REQUIRE(front.in_front);
    REQUIRE(front.visible_on_screen);
    REQUIRE(!front.clamped_to_edge);
    REQUIRE(close_to(front.screen_px.x, 960.0F, 2.0F));
    REQUIRE(close_to(front.screen_px.y, 540.0F, 2.0F));

    const vcr::WorldScreenProjection behind = vcr::project_world_indicator(
        {0.0F, 2.0F, 25.0F}, cam_pos, vp, {1920.0F, 1080.0F}, 48.0F, true);
    REQUIRE(!behind.in_front);
    REQUIRE(!behind.visible_on_screen);
    REQUIRE(behind.clamped_to_edge);
    REQUIRE(behind.screen_px.x >= 47.9F && behind.screen_px.x <= 1872.1F);
    REQUIRE(behind.screen_px.y >= 47.9F && behind.screen_px.y <= 1032.1F);
}

void test_localization_system() {
    vcr::LocalizationSystem loc;
    REQUIRE(loc.locale() == "en");
    REQUIRE(loc.contains("ui.pause.title"));
    REQUIRE(!loc.get("ui.pause.title").empty());

    REQUIRE(loc.set_locale("ru"));
    REQUIRE(loc.locale() == "ru");
    REQUIRE(loc.contains("ui.pause.title"));
    REQUIRE(loc.get("ui.pause.title") != "ui.pause.title");

    // Fallback resolution when active locale omits a custom key
    loc.set_entry("en", "quest.only_in_en", "Visit sector {sector} (day {day})");
    REQUIRE(loc.set_fallback_locale("en"));
    REQUIRE(loc.get("quest.only_in_en") == "Visit sector {sector} (day {day})");

    std::array<char, 128U> fmt_buf{};
    const std::array<vcr::LocalizationArg, 2U> args{{
        {.name = "sector", .value = "7G"},
        {.name = "day", .value = "4"},
    }};
    const std::string_view formatted = loc.format_into(fmt_buf, "quest.only_in_en", args);
    REQUIRE(formatted == "Visit sector 7G (day 4)");
    REQUIRE(loc.resolve("Literal text") == "Literal text");

    const ScopedTempDir temp;
    const std::filesystem::path loc_dir = temp.path() / "localization";
    std::filesystem::create_directories(loc_dir);
    loc.set_entry("fr", "ui.greeting", "Bonjour {name}", "Francais");
    std::string error;
    const std::filesystem::path fr_file = loc_dir / "fr.vcrloc.json";
    REQUIRE_MSG(loc.save_file(fr_file, "fr", &error), error);
    REQUIRE(std::filesystem::is_regular_file(fr_file));

    vcr::LocalizationSystem loaded;
    const std::size_t loaded_files = loaded.load_directory(loc_dir, &error);
    REQUIRE_MSG(loaded_files == 1U, error);
    REQUIRE(loaded.set_locale("fr"));
    REQUIRE(loaded.display_name("fr") == "Francais");
    const std::array<vcr::LocalizationArg, 1U> fr_arg{{
        {.name = "name", .value = "Ada"},
    }};
    REQUIRE(loaded.format_into(fmt_buf, "ui.greeting", fr_arg) == "Bonjour Ada");
}

void test_ui_marker_scene_prefab_duplicate_and_shipping() {
    vcr::SceneDocument scene;
    const vcr::SceneNodeId root = scene.create_node("Beacon", vcr::SceneNodeKind::folder);
    REQUIRE(root);
    if (auto node = scene.node(root)) {
        node->get().transform.translation = {3.0F, 1.5F, -4.0F};
    }

    vcr::UiMarkerComponent marker{};
    marker.kind = vcr::UiMarkerKind::speaker;
    marker.enabled = true;
    marker.clamp_to_screen = true;
    marker.show_distance = true;
    marker.show_health_bar = true;
    marker.world_offset = {0.0F, 2.2F, 0.0F};
    marker.max_distance = 75.0F;
    marker.fade_start_distance = 50.0F;
    marker.color_rgba8 = vcr::pack_rgba8(90U, 210U, 255U, 255U);
    REQUIRE(marker.set_label("Guide"));
    REQUIRE(marker.set_subtitle_text("Welcome traveler"));
    REQUIRE(marker.set_dialogue_id("intro_talk"));
    REQUIRE(vcr::valid_ui_marker_component(marker));

    REQUIRE(scene.set_ui_marker(root, marker));
    REQUIRE(scene.has_ui_marker(root));
    REQUIRE(scene.ui_marker(root)->get() == marker);

    const ScopedTempDir temp;
    const std::filesystem::path scene_path = temp.path() / "ui_test.vcrscene.json";
    std::string error;
    REQUIRE_MSG(vcr::save_scene_file(scene, scene_path, error), error);

    vcr::SceneDocument loaded_scene;
    REQUIRE_MSG(vcr::load_scene_file(scene_path, loaded_scene, error), error);
    const auto uuid = scene.uuid(root);
    REQUIRE(uuid.has_value());
    const vcr::SceneNodeId loaded_root = loaded_scene.find_by_uuid(*uuid);
    REQUIRE(loaded_root);
    REQUIRE(loaded_scene.has_ui_marker(loaded_root));
    REQUIRE(loaded_scene.ui_marker(loaded_root)->get() == marker);

    // Prefab save + override round-trip
    const std::filesystem::path asset_root = temp.path() / "project_assets";
    std::filesystem::create_directories(asset_root);
    const std::string prefab_rel = "prefabs/marker_beacon.vcrprefab.json";
    REQUIRE_MSG(vcr::save_prefab_file(scene, root, asset_root, prefab_rel, error), error);

    vcr::UiMarkerComponent override_marker = marker;
    override_marker.kind = vcr::UiMarkerKind::hazard;
    REQUIRE(override_marker.set_label("Overridden Hazard"));
    const std::array<vcr::PrefabNodeOverride, 1U> overrides{{
        vcr::PrefabNodeOverride{
            .source_node_uuid = *uuid,
            .ui_marker = override_marker,
        },
    }};
    vcr::PrefabInstantiateResult inst_res{};
    REQUIRE_MSG(vcr::instantiate_prefab_file(
        loaded_scene,
        asset_root,
        prefab_rel,
        vcr::PrefabInstantiateOptions{.overrides = overrides},
        inst_res,
        error),
        error);
    REQUIRE(inst_res.root_id);
    REQUIRE(loaded_scene.has_ui_marker(inst_res.root_id));
    REQUIRE(loaded_scene.ui_marker(inst_res.root_id)->get() == override_marker);

    // Duplicate subtree preserves UiMarkerComponent
    std::uint32_t duplicated_count = 0U;
    const vcr::SceneNodeId dup_root = vcr::duplicate_scene_subtree(
        loaded_scene, inst_res.root_id, duplicated_count, error);
    REQUIRE_MSG(dup_root, error);
    REQUIRE(duplicated_count >= 1U);
    REQUIRE(loaded_scene.has_ui_marker(dup_root));
    REQUIRE(loaded_scene.ui_marker(dup_root)->get() == override_marker);

    // Shipping test with UI assets (.vcrloc.json, .vcrdlg.json, .vcrfont.json + .png)
    vcr::CreateProjectResult created{};
    REQUIRE_MSG(
        vcr::create_project_on_disk(
            vcr::CreateProjectOptions{
                .name = "UI Ship Check",
                .parent_directory = temp.path() / "ship_src",
                .template_kind = vcr::ProjectTemplateKind::standard,
                .bundle_runtime = false,
            },
            created,
            error),
        error);

    const std::filesystem::path proj = created.project_directory;
    write_text_file(
        proj / "assets/localization/custom.vcrloc.json",
        R"({"locale":"de","display_name":"Deutsch","entries":{"ui.test":"Hallo"}})");
    write_text_file(
        proj / "assets/dialogues/npc.vcrdlg.json",
        R"({"id":"npc_intro","entry":"n0","nodes":[{"id":"n0","speaker":"NPC","text":"Hi","choices":[{"text":"Bye","close":true}]}]})");
    REQUIRE_MSG(
        vcr::save_game_ui_font_atlas_file(
            proj / "assets/fonts/hud.vcrfont.json",
            vcr::default_game_ui_font_atlas(),
            &error),
        error);

    const std::filesystem::path ship_dest = temp.path() / "ship_out" / "game";
    vcr::ShipProjectResult shipped{};
    REQUIRE_MSG(vcr::ship_project(proj, ship_dest, shipped, error), error);
    REQUIRE(std::filesystem::is_regular_file(ship_dest / "assets/localization/custom.vcrloc.json"));
    REQUIRE(std::filesystem::is_regular_file(ship_dest / "assets/dialogues/npc.vcrdlg.json"));
    REQUIRE(std::filesystem::is_regular_file(ship_dest / "assets/fonts/hud.vcrfont.json"));
    REQUIRE(std::filesystem::is_regular_file(ship_dest / "assets/fonts/hud.png"));

    // Corrupt font image reference must fail ship_project cleanly
    std::filesystem::remove(proj / "assets/fonts/hud.png");
    const std::filesystem::path ship_fail = temp.path() / "ship_out_fail" / "game";
    REQUIRE(!vcr::ship_project(proj, ship_fail, shipped, error));
    REQUIRE(!std::filesystem::exists(ship_fail));
}

void test_cli_dialogue_behavior_runtime_mcp_and_compatibility() {
    // CLI flags --main-menu and --locale
    const std::array<const char*, 4U> argv{{
        "vulkan_cube.exe",
        "--main-menu",
        "--locale",
        "ru",
    }};
    const vcr::CliParseResult cli = vcr::parse_cli_options(
        static_cast<int>(argv.size()), argv.data());
    REQUIRE_MSG(static_cast<bool>(cli), cli.error);
    REQUIRE(cli.options.start_in_main_menu);
    REQUIRE(cli.options.locale == "ru");

    // "dialogue" gameplay behavior
    vcr::SceneDocument scene;
    const vcr::SceneNodeId speaker_node = scene.create_node("Villager", vcr::SceneNodeKind::folder);
    vcr::GameplayComponent gp{};
    gp.enabled = true;
    gp.interaction_radius = 4.0F;
    REQUIRE(gp.set_behavior("dialogue"));
    REQUIRE(scene.set_gameplay(speaker_node, gp));

    vcr::UiMarkerComponent marker{};
    marker.kind = vcr::UiMarkerKind::speaker;
    REQUIRE(marker.set_label("Villager"));
    REQUIRE(marker.set_subtitle_text("Press E"));
    REQUIRE(marker.set_dialogue_id("quest_intro"));
    marker.world_offset = {0.0F, 1.5F, 0.0F};
    REQUIRE(scene.set_ui_marker(speaker_node, marker));

    vcr::GameplayWorld world;
    world.start(scene);
    world.update(1.0F / 60.0F, scene, {0.0F, 0.0F, 0.0F}, true, {}, nullptr, nullptr, nullptr, nullptr, {});
    REQUIRE(world.get_variable("dialogue.interactions") >= 1.0F);
    REQUIRE(close_to(world.get_variable("dialogue.requested"), 1.0F));
    world.set_variable("dialogue.requested", 0.0F);

    // GameUiRuntime full flow
    vcr::LocalizationSystem loc;
    vcr::InputActionSystem input;
    input.set_hardware_polling_enabled(false);

    vcr::GameUiRuntime ui;
    struct HostTrack {
        int new_game_calls = 0;
        int restart_calls = 0;
        int quit_calls = 0;
    } track{};

    vcr::GameUiHostCallbacks cb{};
    cb.user_data = &track;
    cb.on_new_game = [](void* ud) noexcept {
        ++static_cast<HostTrack*>(ud)->new_game_calls;
    };
    cb.on_restart_game = [](void* ud) noexcept {
        ++static_cast<HostTrack*>(ud)->restart_calls;
    };
    cb.on_quit_requested = [](void* ud) noexcept {
        ++static_cast<HostTrack*>(ud)->quit_calls;
    };
    ui.set_host_callbacks(cb);

    vcr::GameUiConfig cfg{};
    cfg.start_in_main_menu = true;
    cfg.show_fps = true;
    ui.configure(cfg);
    REQUIRE(ui.screen() == vcr::GameUiScreen::main_menu);
    REQUIRE(ui.blocks_gameplay_simulation());
    REQUIRE(ui.wants_mouse_cursor());

    std::vector<vcr::SceneWorldNode> world_nodes;
    std::vector<std::uint32_t> scratch;
    scene.build_world_nodes(world_nodes, scratch);

    const auto make_input = [&](const float dt) {
        return vcr::GameUiUpdateInput{
            .delta_seconds = dt,
            .viewport_extent = {1920U, 1080U},
            .display_fps = 120.0F,
            .camera_pos = {0.0F, 1.5F, 5.0F},
            .camera_forward = {0.0F, 0.0F, -1.0F},
            .camera_up = {0.0F, 1.0F, 0.0F},
            .player_pos = {0.0F, 0.0F, 1.0F},
            .player_health = 80.0F,
            .player_max_health = 100.0F,
            .scene = &scene,
            .world_nodes = world_nodes,
            .gameplay_world = &world,
            .input_actions = &input,
            .localization = &loc,
        };
    };

    // Press Enter on row 0 ("New Game") in main menu
    input.set_context(vcr::InputContext::ui);
    input.pulse_key(vcr::InputKey::enter);
    input.begin_frame(1.0F / 60.0F);
    ui.update(make_input(1.0F / 60.0F));
    REQUIRE(track.new_game_calls == 1);
    REQUIRE(ui.screen() == vcr::GameUiScreen::in_game);
    REQUIRE(!ui.blocks_gameplay_simulation());

    // World marker projection + rendering in HUD
    vcr::GameOverlayCanvas canvas;
    canvas.set_viewport(1920.0F, 1080.0F);
    canvas.set_font_atlas(&vcr::default_game_ui_font_atlas());
    ui.refresh_markers(make_input(1.0F / 60.0F));
    REQUIRE(!ui.projected_markers().empty());
    ui.render(canvas, make_input(1.0F / 60.0F));
    REQUIRE(canvas.vertex_count() > 20U);

    // Pause via Escape (pause key in gameplay context)
    input.set_context(vcr::InputContext::gameplay);
    input.pulse_key(vcr::InputKey::escape);
    input.begin_frame(1.0F / 60.0F);
    ui.update(make_input(1.0F / 60.0F));
    REQUIRE(ui.screen() == vcr::GameUiScreen::pause_menu);
    REQUIRE(ui.blocks_gameplay_simulation());

    // Open Settings -> Controls tab, trigger keyboard + gamepad rebind on row 3 (move_forward)
    ui.open_screen(vcr::GameUiScreen::settings);
    ui.open_settings_tab(vcr::GameUiSettingsTab::controls);
    input.set_context(vcr::InputContext::ui);
    for (int i = 0; i < 3; ++i) {
        input.pulse_key(vcr::InputKey::down);
        input.begin_frame(1.0F / 60.0F);
        ui.update(make_input(1.0F / 60.0F));
        input.begin_frame(1.0F / 60.0F);
    }
    REQUIRE(ui.selected_index() == 3U);

    // Start rebind
    input.pulse_key(vcr::InputKey::enter);
    input.begin_frame(1.0F / 60.0F);
    ui.update(make_input(1.0F / 60.0F));
    REQUIRE(ui.awaiting_rebind());

    // Capture keyboard key 'I'
    input.pulse_key(vcr::InputKey::i);
    input.begin_frame(1.0F / 60.0F);
    ui.update(make_input(1.0F / 60.0F));
    REQUIRE(!ui.awaiting_rebind());
    REQUIRE(input.binding(vcr::InputAction::move_forward).primary_key == vcr::InputKey::i);

    // Re-enter rebind and capture GamepadButton::north via synthetic gamepad rising edge
    input.begin_frame(1.0F / 60.0F);
    input.pulse_key(vcr::InputKey::enter);
    input.begin_frame(1.0F / 60.0F);
    ui.update(make_input(1.0F / 60.0F));
    REQUIRE(ui.awaiting_rebind());

    vcr::GamepadState gp_idle{};
    gp_idle.connected = true;
    input.set_synthetic_gamepad(gp_idle);
    input.begin_frame(1.0F / 60.0F);
    vcr::GamepadState gp_press = gp_idle;
    gp_press.buttons_mask = vcr::gamepad_button_bit(vcr::GamepadButton::north);
    input.set_synthetic_gamepad(gp_press);
    input.begin_frame(1.0F / 60.0F);
    ui.update(make_input(1.0F / 60.0F));
    REQUIRE(!ui.awaiting_rebind());
    REQUIRE(input.binding(vcr::InputAction::move_forward).gamepad_button ==
            vcr::GamepadButton::north);
    input.set_synthetic_gamepad(std::nullopt);

    // Interface & Language tab -> switch locale en -> ru via row 0
    ui.open_settings_tab(vcr::GameUiSettingsTab::interface_loc);
    REQUIRE(ui.selected_index() == 0U);
    input.pulse_key(vcr::InputKey::right);
    input.begin_frame(1.0F / 60.0F);
    ui.update(make_input(1.0F / 60.0F));
    REQUIRE(loc.locale() == "ru");

    // Subtitles and toasts + expiry
    ui.open_screen(vcr::GameUiScreen::in_game);
    ui.push_subtitle("Guide", "ui.common.yes", 0.5F);
    ui.show_toast("ui.common.ok", 0.4F);
    REQUIRE(ui.active_subtitle_count() == 1U);
    for (int f = 0; f < 40; ++f) {
        input.begin_frame(0.025F);
        ui.update(make_input(0.025F));
    }
    REQUIRE(ui.active_subtitle_count() == 0U);

    // .vcrdlg.json multi-choice dialogue graph mutating GameplayWorld variable
    const ScopedTempDir dlg_temp;
    const std::filesystem::path dlg_path = dlg_temp.path() / "quest.vcrdlg.json";
    write_text_file(
        dlg_path,
        R"({
          "id": "quest_intro",
          "entry": "start",
          "nodes": [
            {
              "id": "start",
              "speaker": "Scout",
              "text": "Choose your path.",
              "choices": [
                {
                  "text": "Accept mission",
                  "next": "accepted",
                  "set_var": "mission.accepted",
                  "set_value": 1.0
                },
                {
                  "text": "Decline",
                  "close": true
                }
              ]
            },
            {
              "id": "accepted",
              "speaker": "Scout",
              "text": "Good luck!",
              "choices": [
                { "text": "Continue", "close": true }
              ]
            }
          ]
        })");
    std::string error;
    REQUIRE_MSG(ui.load_dialogue_file(dlg_path, &error), error);
    REQUIRE(ui.start_dialogue("quest_intro"));
    REQUIRE(ui.dialogue_active());
    REQUIRE(ui.blocks_player_control());
    ui.select_dialogue_choice(0U, &world);
    REQUIRE(close_to(world.get_variable("mission.accepted"), 1.0F));
    REQUIRE(ui.dialogue_active());
    ui.select_dialogue_choice(0U, &world);
    REQUIRE(!ui.dialogue_active());

    // MCP ControlPlane commands for the 3 new UI/Marker tools
    static_assert(std::is_trivially_copyable_v<vcr::McpCommand>);
    vcr::McpControlPlane mcp;
    vcr::McpCommand cmd_set{};
    cmd_set.token = 101U;
    cmd_set.type = vcr::McpCommandType::editor_set_ui_marker;
    cmd_set.node_id = speaker_node;
    cmd_set.ui_marker = marker;
    REQUIRE(mcp.submit(cmd_set));

    vcr::McpCommand cmd_rm{};
    cmd_rm.token = 102U;
    cmd_rm.type = vcr::McpCommandType::editor_remove_ui_marker;
    cmd_rm.node_id = speaker_node;
    REQUIRE(mcp.submit(cmd_rm));

    vcr::McpCommand cmd_cfg{};
    cmd_cfg.token = 103U;
    cmd_cfg.type = vcr::McpCommandType::editor_configure_ui;
    cmd_cfg.ui_config.mask = vcr::McpUiConfigPayload::flag_show_fps;
    cmd_cfg.ui_config.show_fps = true;
    REQUIRE(mcp.submit(cmd_cfg));

    vcr::McpCommand popped{};
    REQUIRE(mcp.try_next_command(popped) && popped.token == 101U &&
            popped.type == vcr::McpCommandType::editor_set_ui_marker &&
            popped.ui_marker == marker);
    REQUIRE(mcp.try_next_command(popped) && popped.token == 102U &&
            popped.type == vcr::McpCommandType::editor_remove_ui_marker);
    REQUIRE(mcp.try_next_command(popped) && popped.token == 103U &&
            popped.type == vcr::McpCommandType::editor_configure_ui &&
            popped.ui_config.show_fps);

    // GameplaySession custom extension compatibility policy
    vcr::GameplaySession custom_session;
    vcr::GameplayExtensionCallbacks custom_ext{};
    custom_ext.on_camera_audio_overlay = [](
        void*, vcr::GameplaySession&, vcr::GameplayUpdateContext&) noexcept {};
    custom_session.set_extension_callbacks(custom_ext);

    std::vector<vcr::SceneGpuInstance> cubes;
    std::vector<vcr::SceneModelInstance> models;
    std::vector<vcr::ScenePunctualLight> lights;
    const vcr::SceneRenderView view = scene.build_render_view(cubes, models, lights);
    const vcr::FlyCamera startup_cam;
    vcr::FlyCamera output_cam;
    REQUIRE_MSG(
        custom_session.start(scene, view, startup_cam, output_cam, error),
        error);
    REQUIRE(!custom_session.game_ui().config().auto_pause_enabled);
    REQUIRE(!custom_session.game_ui().config().show_default_hud);
    custom_session.stop();
}

} // namespace

int main() {
    test_sdf_font_atlas_and_canvas();
    test_ui_scaler_anchors_and_world_projection();
    test_localization_system();
    test_ui_marker_scene_prefab_duplicate_and_shipping();
    test_cli_dialogue_behavior_runtime_mcp_and_compatibility();
    if (g_failures != 0) {
        std::cerr << g_failures << " test check(s) failed.\n";
        return 1;
    }
    std::cout << "All game UI & localization tests passed.\n";
    return 0;
}
