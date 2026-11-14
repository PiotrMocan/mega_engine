#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t game_ui_atlas_size = 256U;
inline constexpr std::uint32_t game_ui_glyph_cell_size = 16U;
inline constexpr std::uint32_t game_ui_glyph_grid_dim = 16U;
inline constexpr std::size_t game_ui_max_vertices = 16'384U;

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

void build_game_ui_font_atlas(
    std::array<std::uint8_t, game_ui_atlas_size * game_ui_atlas_size>& out_pixels) noexcept;

[[nodiscard]] std::optional<glm::vec2> project_world_to_screen(
    const glm::vec3& world_pos,
    const glm::mat4& view_projection,
    glm::vec2 viewport_px) noexcept;

class GameOverlayCanvas final {
public:
    GameOverlayCanvas();

    void reserve(std::size_t max_vertices);
    void clear() noexcept;

    void set_viewport(float width_px, float height_px) noexcept;
    [[nodiscard]] float viewport_width() const noexcept { return viewport_width_; }
    [[nodiscard]] float viewport_height() const noexcept { return viewport_height_; }

    void draw_rect(glm::vec2 min_px, glm::vec2 max_px, std::uint32_t rgba8) noexcept;
    void draw_rect_outline(
        glm::vec2 min_px,
        glm::vec2 max_px,
        float thickness_px,
        std::uint32_t rgba8) noexcept;
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

    void draw_text(
        glm::vec2 top_left_px,
        std::string_view utf8_text,
        float scale,
        std::uint32_t rgba8) noexcept;

    [[nodiscard]] glm::vec2 measure_text(
        std::string_view utf8_text,
        float scale) const noexcept;

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
    std::size_t max_vertices_ = game_ui_max_vertices;
    float viewport_width_ = 1280.0F;
    float viewport_height_ = 720.0F;
    bool truncated_ = false;
};

} // namespace vcr
