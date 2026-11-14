#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t game_ui_atlas_size = 256U;
inline constexpr std::uint32_t game_ui_glyph_cell_size = 16U;
inline constexpr std::uint32_t game_ui_glyph_grid_dim = 16U;
inline constexpr std::size_t game_ui_max_vertices = 32'768U;

struct GameUiVertex {
    glm::vec2 pos_px{};
    glm::vec2 uv{};
    std::uint32_t rgba8 = 0xFFFFFFFFU;
};

static_assert(sizeof(GameUiVertex) == 20U);

[[nodiscard]] inline constexpr std::uint32_t pack_rgba8(
    const std::uint8_t r,
    const std::uint8_t g,
    const std::uint8_t b,
    const std::uint8_t a = 255U) noexcept {
    return static_cast<std::uint32_t>(r) |
        (static_cast<std::uint32_t>(g) << 8U) |
        (static_cast<std::uint32_t>(b) << 16U) |
        (static_cast<std::uint32_t>(a) << 24U);
}

inline constexpr std::uint32_t game_ui_sdf_atlas_size = 512U;
inline constexpr std::uint32_t game_ui_sdf_cell_size = 32U;
inline constexpr std::uint32_t game_ui_sdf_grid_dim = 16U;

struct GameUiGlyphMetrics {
    std::uint32_t codepoint = 0U;
    glm::vec2 uv_min{0.0F, 0.0F};
    glm::vec2 uv_max{0.0F, 0.0F};
    glm::vec2 size_px{0.0F, 0.0F};
    glm::vec2 bearing_px{0.0F, 0.0F};
    float advance_px = 0.0F;
};

class GameUiFontAtlas final {
public:
    std::uint32_t width = game_ui_sdf_atlas_size;
    std::uint32_t height = game_ui_sdf_atlas_size;
    float base_pixel_size = 20.0F;
    float cap_height_px = 16.0F;
    float line_height_px = 22.0F;
    bool sdf = true;
    float sdf_spread_px = 5.0F;
    glm::vec2 solid_uv_min{0.004F, 0.004F};
    glm::vec2 solid_uv_max{0.058F, 0.058F};
    std::vector<std::uint8_t> pixels_r8{};
    std::vector<GameUiGlyphMetrics> glyphs{};

    void rebuild_lookup() noexcept;
    [[nodiscard]] const GameUiGlyphMetrics* find_glyph(std::uint32_t codepoint) const noexcept;
    [[nodiscard]] const GameUiGlyphMetrics& fallback_glyph() const noexcept;

    [[nodiscard]] static GameUiFontAtlas build_default_sdf();
    [[nodiscard]] static GameUiFontAtlas build_bitmap_fallback();

private:
    std::array<std::int16_t, 128> ascii_lookup_{};
    std::int16_t question_index_ = -1;
};

[[nodiscard]] const GameUiFontAtlas& default_game_ui_font_atlas();

bool save_game_ui_font_atlas_file(
    const std::filesystem::path& json_path,
    const GameUiFontAtlas& atlas,
    std::string* error_message = nullptr);

bool load_game_ui_font_atlas_file(
    const std::filesystem::path& json_path,
    GameUiFontAtlas& out_atlas,
    std::string* error_message = nullptr);

enum class UiScaleMode : std::uint8_t {
    fit = 0U,
    match_height = 1U,
    match_width = 2U,
    pixel_constant = 3U,
};

enum class UiAnchorPreset : std::uint8_t {
    top_left = 0U,
    top_center = 1U,
    top_right = 2U,
    middle_left = 3U,
    center = 4U,
    middle_right = 5U,
    bottom_left = 6U,
    bottom_center = 7U,
    bottom_right = 8U,
    stretch_fill = 9U,
};

struct UiScaleConfig {
    glm::vec2 reference_resolution{1920.0F, 1080.0F};
    UiScaleMode mode = UiScaleMode::fit;
    float user_scale = 1.0F;
    float min_scale = 0.55F;
    float max_scale = 3.0F;
    float safe_area_fraction = 0.025F;
};

struct UiRect {
    glm::vec2 min{0.0F, 0.0F};
    glm::vec2 max{0.0F, 0.0F};

    [[nodiscard]] glm::vec2 size() const noexcept { return max - min; }
    [[nodiscard]] glm::vec2 center() const noexcept { return (min + max) * 0.5F; }
    [[nodiscard]] bool contains(glm::vec2 point) const noexcept {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
    }
};

class UiScaler final {
public:
    UiScaler() noexcept = default;
    explicit UiScaler(glm::vec2 viewport_px, const UiScaleConfig& config = {}) noexcept {
        configure(viewport_px, config);
    }

    void configure(glm::vec2 viewport_px, const UiScaleConfig& config = {}) noexcept;

    [[nodiscard]] float scale() const noexcept { return scale_; }
    [[nodiscard]] glm::vec2 viewport() const noexcept { return viewport_px_; }
    [[nodiscard]] UiRect safe_rect() const noexcept { return safe_rect_; }
    [[nodiscard]] float px(float ref_value) const noexcept { return ref_value * scale_; }
    [[nodiscard]] glm::vec2 px(glm::vec2 ref_value) const noexcept { return ref_value * scale_; }

    [[nodiscard]] glm::vec2 anchor_point(
        UiAnchorPreset preset,
        bool respect_safe_area = true) const noexcept;

    [[nodiscard]] UiRect resolve(
        UiAnchorPreset anchor,
        glm::vec2 offset_ref,
        glm::vec2 size_ref,
        glm::vec2 pivot = {0.0F, 0.0F},
        bool respect_safe_area = true) const noexcept;

private:
    glm::vec2 viewport_px_{1280.0F, 720.0F};
    UiRect safe_rect_{glm::vec2{32.0F, 18.0F}, glm::vec2{1248.0F, 702.0F}};
    float scale_ = 1.0F;
};

struct UiCrosshairStyle {
    float gap_px = 5.0F;
    float arm_length_px = 8.0F;
    float thickness_px = 2.0F;
    float center_dot_radius_px = 1.5F;
    std::uint32_t color_rgba8 = pack_rgba8(240U, 245U, 250U, 230U);
    std::uint32_t outline_rgba8 = pack_rgba8(12U, 16U, 22U, 190U);
};

void build_game_ui_font_atlas(
    std::array<std::uint8_t, game_ui_atlas_size * game_ui_atlas_size>& out_pixels) noexcept;

[[nodiscard]] glm::mat4 make_ui_view_projection(
    const glm::vec3& camera_pos,
    glm::vec3 camera_forward,
    glm::vec3 camera_up,
    float aspect,
    float near_plane = 0.1F,
    float far_plane = 500.0F,
    float vertical_fov_deg = 65.0F) noexcept;

struct WorldScreenProjection {
    glm::vec2 screen_px{0.0F, 0.0F};
    glm::vec2 edge_dir{0.0F, -1.0F};
    float distance_m = 0.0F;
    bool in_front = false;
    bool visible_on_screen = false;
    bool clamped_to_edge = false;
};

[[nodiscard]] std::optional<glm::vec2> project_world_to_screen(
    const glm::vec3& world_pos,
    const glm::mat4& view_projection,
    glm::vec2 viewport_px) noexcept;

[[nodiscard]] WorldScreenProjection project_world_indicator(
    const glm::vec3& world_pos,
    const glm::vec3& camera_pos,
    const glm::mat4& view_projection,
    glm::vec2 viewport_px,
    float edge_margin_px = 48.0F,
    bool clamp_to_screen = true) noexcept;

class GameOverlayCanvas final {
public:
    GameOverlayCanvas();

    void reserve(std::size_t max_vertices);
    void clear() noexcept;

    void set_viewport(float width_px, float height_px) noexcept;
    [[nodiscard]] float viewport_width() const noexcept { return viewport_width_; }
    [[nodiscard]] float viewport_height() const noexcept { return viewport_height_; }

    void set_font_atlas(const GameUiFontAtlas* atlas) noexcept;
    [[nodiscard]] const GameUiFontAtlas* font_atlas() const noexcept { return atlas_; }

    void draw_rect(glm::vec2 min_px, glm::vec2 max_px, std::uint32_t rgba8) noexcept;
    void draw_rect_outline(
        glm::vec2 min_px,
        glm::vec2 max_px,
        float thickness_px,
        std::uint32_t rgba8) noexcept;
    void draw_rounded_panel(
        glm::vec2 min_px,
        glm::vec2 max_px,
        std::uint32_t fill_rgba8,
        std::uint32_t border_rgba8 = 0U,
        float border_thickness_px = 1.0F) noexcept;
    void draw_line(
        glm::vec2 start_px,
        glm::vec2 end_px,
        float thickness_px,
        std::uint32_t rgba8) noexcept;
    void draw_progress_bar(
        glm::vec2 min_px,
        glm::vec2 max_px,
        float fraction,
        std::uint32_t bg_rgba8,
        std::uint32_t fill_rgba8,
        std::uint32_t border_rgba8 = 0U,
        float border_thickness_px = 1.0F) noexcept;
    void draw_arc(
        glm::vec2 center_px,
        float radius_px,
        float thickness_px,
        float start_radians,
        float end_radians,
        std::uint32_t rgba8,
        std::uint32_t segments = 24U) noexcept;
    void draw_crosshair(glm::vec2 center_px, const UiCrosshairStyle& style) noexcept;

    void draw_text(
        glm::vec2 top_left_px,
        std::string_view utf8_text,
        float scale,
        std::uint32_t rgba8) noexcept;
    void draw_text_shadowed(
        glm::vec2 top_left_px,
        std::string_view utf8_text,
        float scale,
        std::uint32_t rgba8,
        std::uint32_t shadow_rgba8 = pack_rgba8(0U, 0U, 0U, 190U),
        glm::vec2 shadow_offset_px = {1.5F, 1.5F}) noexcept;
    void draw_text_px(
        glm::vec2 top_left_px,
        std::string_view utf8_text,
        float height_px,
        std::uint32_t rgba8,
        bool shadowed = false) noexcept;
    void draw_text_wrapped(
        glm::vec2 top_left_px,
        float max_width_px,
        std::string_view utf8_text,
        float height_px,
        std::uint32_t rgba8,
        float line_spacing = 1.25F,
        bool shadowed = false) noexcept;

    [[nodiscard]] glm::vec2 measure_text(
        std::string_view utf8_text,
        float scale) const noexcept;
    [[nodiscard]] glm::vec2 measure_text_px(
        std::string_view utf8_text,
        float height_px) const noexcept;
    [[nodiscard]] glm::vec2 measure_text_wrapped(
        std::string_view utf8_text,
        float max_width_px,
        float height_px,
        float line_spacing = 1.25F) const noexcept;

    [[nodiscard]] std::span<const GameUiVertex> vertices() const noexcept {
        return vertices_;
    }
    [[nodiscard]] std::uint32_t vertex_count() const noexcept {
        return static_cast<std::uint32_t>(vertices_.size());
    }
    [[nodiscard]] std::size_t capacity_vertices() const noexcept {
        return max_vertices_;
    }
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

private:
    void push_quad(
        glm::vec2 p0,
        glm::vec2 p1,
        glm::vec2 p2,
        glm::vec2 p3,
        glm::vec2 uv_min,
        glm::vec2 uv_max,
        std::uint32_t rgba8) noexcept;
    void push_solid_quad(glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, std::uint32_t rgba8) noexcept;

    std::vector<GameUiVertex> vertices_{};
    const GameUiFontAtlas* atlas_ = nullptr;
    std::size_t max_vertices_ = game_ui_max_vertices;
    float viewport_width_ = 1280.0F;
    float viewport_height_ = 720.0F;
    bool truncated_ = false;
};

} // namespace vcr
