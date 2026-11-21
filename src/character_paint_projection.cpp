#include "character_paint_projection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vcr {
namespace {
constexpr float missing = std::numeric_limits<float>::infinity();

struct WorldBox {
    glm::vec3 lo{missing}, hi{-missing};

    bool overlaps(glm::vec3 center, float radius) const noexcept {
        if (lo.x > hi.x) return false;
        const auto delta = center - glm::clamp(center, lo, hi);
        return glm::dot(delta, delta) <= radius * radius;
    }
};

constexpr std::size_t texel_index(std::uint32_t x, std::uint32_t y, std::size_t stride) noexcept {
    return y * stride + x;
}

float cross2(glm::vec2 a, glm::vec2 b) noexcept { return a.x * b.y - a.y * b.x; }

// Propagate sampled coordinates into the filter gutter. A pass reads only
// preceding rings, so results do not depend on scan direction.
void fill_gutter(std::vector<glm::vec3>& positions, std::uint32_t resolution) {
    std::vector<unsigned char> ring(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) ring[i] = std::isfinite(positions[i].x) ? 0 : 255;
    for (unsigned char step = 1; step <= 3; ++step) {
        for (std::uint32_t y = 0; y < resolution; ++y) {
            for (std::uint32_t x = 0; x < resolution; ++x) {
                const auto i = texel_index(x, y, resolution);
                if (ring[i] != 255) continue;
                const auto copy = [&](std::size_t n) {
                    if (ring[n] >= step) return false;
                    positions[i] = positions[n];
                    ring[i] = step;
                    return true;
                };
                if (x && copy(i - 1)) continue;
                if (x + 1 < resolution && copy(i + 1)) continue;
                if (y && copy(i - resolution)) continue;
                if (y + 1 < resolution) copy(i + resolution);
            }
        }
    }
}
} // namespace

CharacterPaintProjection::CharacterPaintProjection(const CharacterMeshBuildResult& mesh,
    const CharacterSubmeshBuild& sub, std::span<const CharacterPaintStroke> strokes,
    std::uint32_t resolution) : tile_size_(resolution > 1024U ? 32U : 16U), resolution_(resolution), columns_(0) {
    static_assert(maximum_character_paint_samples <= UINT16_MAX);
    if (strokes.empty() || strokes.size() > maximum_character_paint_samples ||
        resolution == 0 || resolution > character_default_texture_size) return;
    columns_ = (resolution + tile_size_ - 1) / tile_size_;
    tiles_.resize(columns_ * columns_);
    const float scale = static_cast<float>(resolution);
    const auto pixel = [&](float uv) {
        return std::min(resolution - 1, static_cast<std::uint32_t>(std::clamp(uv, 0.0F, 1.0F) * scale));
    };
    const bool surface = std::any_of(strokes.begin(), strokes.end(), [](const auto& s) { return s.skin_surface.has_value(); });
    if (surface) {
        const std::size_t width = resolution;
        positions_.assign(width * width, glm::vec3{missing, 0, 0});
        centers_.resize(strokes.size());
        std::vector<float> closest(strokes.size(), missing);
        struct Anchor { std::uint16_t stroke; glm::vec3 bind_center; };
        std::vector<std::vector<Anchor>> anchors(tiles_.size());
        for (std::size_t s = 0; s < strokes.size(); ++s) {
            const auto& shape = strokes[s].skin_surface;
            if (!shape) continue;
            centers_[s] = shape->bind_center;
            const auto uv = strokes[s].uv;
            anchors[pixel(uv.y) / tile_size_ * columns_ + pixel(uv.x) / tile_size_].push_back(
                {static_cast<std::uint16_t>(s), shape->bind_center});
        }

        // Resolve the stored material UV against the current sculpted/morphed
        // mesh. The saved pick disambiguates coincident charts at their edges.
        const std::size_t first = sub.first_index, count = sub.index_count;
        if (first > mesh.indices.size() || count > mesh.indices.size() - first) return;
        for (std::size_t f = first; f + 2 < first + count; f += 3) {
            std::array<glm::vec2, 3> uv{};
            std::array<glm::vec3, 3> world{};
            bool valid = true;
            for (std::size_t k = 0; k < 3; ++k) {
                const auto i = mesh.indices[f + k];
                if (i >= mesh.vertices.size()) { valid = false; break; }
                const auto& v = mesh.vertices[i];
                uv[k] = {v.texcoord[0], 1.0F - v.texcoord[1]};
                world[k] = {v.position[0], v.position[1], v.position[2]};
                if (!std::isfinite(uv[k].x) || !std::isfinite(uv[k].y) || !std::isfinite(world[k].x) ||
                    !std::isfinite(world[k].y) || !std::isfinite(world[k].z)) { valid = false; break; }
            }
            if (!valid) continue;
            const auto e1 = uv[1] - uv[0], e2 = uv[2] - uv[0];
            const float det = cross2(e1, e2);
            if (!std::isfinite(det) || std::abs(det) <= 1e-14F) continue;
            const float inv = 1.0F / det;
            const auto point = [&](glm::vec2 sample, glm::vec3& p) {
                const auto d = sample - uv[0];
                const float b = cross2(d, e2) * inv, c = cross2(e1, d) * inv;
                constexpr float eps = 1e-5F;
                if (b < -eps || c < -eps || b + c > 1.0F + eps) return false;
                p = world[0] + b * (world[1] - world[0]) + c * (world[2] - world[0]);
                return true;
            };
            const auto lo = glm::min(uv[0], glm::min(uv[1], uv[2])), hi = glm::max(uv[0], glm::max(uv[1], uv[2]));
            const auto x0 = pixel(lo.x), y0 = pixel(lo.y), x1 = pixel(hi.x), y1 = pixel(hi.y);
            for (auto ty = y0 / tile_size_; ty <= y1 / tile_size_; ++ty) {
                for (auto tx = x0 / tile_size_; tx <= x1 / tile_size_; ++tx) {
                    for (const auto& anchor : anchors[ty * columns_ + tx]) {
                        const auto s = anchor.stroke;
                        glm::vec3 p{};
                        if (!point(strokes[s].uv, p)) continue;
                        const auto delta = p - anchor.bind_center;
                        const float distance2 = glm::dot(delta, delta);
                        if (distance2 < closest[s]) { closest[s] = distance2; centers_[s] = p; }
                    }
                }
            }
            for (auto y = y0; y <= y1; ++y) {
                for (auto x = x0; x <= x1; ++x) {
                    point((glm::vec2(x, y) + 0.5F) / scale, positions_[texel_index(x, y, resolution)]);
                }
            }
        }
        fill_gutter(positions_, resolution);
    }

    for (std::uint32_t ty = 0; ty < columns_; ++ty) {
        for (std::uint32_t tx = 0; tx < columns_; ++tx) {
            const auto x0 = tx * tile_size_, y0 = ty * tile_size_;
            const auto x1 = std::min(resolution, x0 + tile_size_), y1 = std::min(resolution, y0 + tile_size_);
            WorldBox box;
            if (surface) {
                for (auto y = y0; y < y1; ++y) {
                    for (auto x = x0; x < x1; ++x) {
                        const auto p = positions_[texel_index(x, y, resolution)];
                        if (!std::isfinite(p.x)) continue;
                        box.lo = glm::min(box.lo, p); box.hi = glm::max(box.hi, p);
                    }
                }
            }
            const glm::vec2 uv_lo = glm::vec2(x0, y0) / scale, uv_hi = glm::vec2(x1, y1) / scale;
            auto& tile = tiles_[ty * columns_ + tx];
            for (std::size_t i = 0; i < strokes.size(); ++i) {
                const auto& s = strokes[i];
                bool overlaps = false;
                if (s.skin_surface) {
                    const auto c = centers_[i];
                    overlaps = box.overlaps(c, s.skin_surface->radius_m) || (s.skin_surface->mirror_x &&
                        box.overlaps({-c.x, c.y, c.z}, s.skin_surface->radius_m));
                } else {
                    const auto inside = [&](glm::vec2 p) {
                        const auto d = p - glm::clamp(p, uv_lo, uv_hi);
                        return glm::dot(d, d) <= s.radius * s.radius;
                    };
                    overlaps = inside(s.uv) || (s.mirror_u && inside({1.0F - s.uv.x, s.uv.y}));
                }
                if (overlaps) tile.push_back(static_cast<std::uint16_t>(i));
            }
        }
    }
}

glm::vec3 CharacterPaintProjection::offset(std::size_t stroke, std::uint32_t x, std::uint32_t y, bool mirror) const noexcept {
    auto c = centers_[stroke];
    const auto p = positions_[texel_index(x, y, resolution_)];
    if (mirror) c.x = std::copysign(std::abs(c.x), p.x);
    return p - c;
}

glm::vec2 CharacterPaintProjection::tangent_offset(glm::vec3 delta, std::uint32_t x, std::uint32_t y) const noexcept {
    const auto at = [&](std::uint32_t u, std::uint32_t v) noexcept { return positions_[texel_index(u, v, resolution_)]; };
    const auto p = at(x, y);
    const auto component = [&](glm::vec3 plus, glm::vec3 minus) noexcept {
        const auto forward = plus - p, back = p - minus;
        const float f = glm::dot(forward, forward), b = glm::dot(back, back);
        if (std::isfinite(f) && f > 1e-14F && (f <= b || b <= 1e-14F)) return glm::dot(delta, forward) / std::sqrt(f);
        if (std::isfinite(b) && b > 1e-14F) return glm::dot(delta, back) / std::sqrt(b);
        return 0.0F;
    };
    return {component(at(std::min(x + 1, resolution_ - 1), y), at(x ? x - 1 : x, y)),
            component(at(x, std::min(y + 1, resolution_ - 1)), at(x, y ? y - 1 : y))};
}
} // namespace vcr
