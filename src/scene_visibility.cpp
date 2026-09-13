#include "scene_visibility.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vcr {
namespace {
glm::vec4 row(const glm::mat4 &m, int i) noexcept {
    return {m[0][i], m[1][i], m[2][i], m[3][i]};
}
struct Projection {
    std::array<glm::vec2, 8> points{};
    glm::vec2 low{1.0F}, high{-1.0F};
    float near_depth = 0.0F, far_depth = 1.0F;
};
bool project(const std::array<glm::vec3, 8> &corners, const glm::mat4 &matrix,
             Projection &result) noexcept {
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const glm::vec4 clip = matrix * glm::vec4(corners[i], 1.0F);
        // Clipping an occluder at the near plane requires new silhouette
        // vertices. Retain such objects instead of guessing that silhouette.
        if (!std::isfinite(clip.w) || clip.w <= 1.0e-5F ||
            clip.z >= clip.w || clip.z <= 0.0F) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z)) {
            return false;
        }
        result.points[i] = glm::vec2(ndc) * 0.5F + 0.5F;
        result.low = glm::min(result.low, result.points[i]);
        result.high = glm::max(result.high, result.points[i]);
        result.near_depth = std::max(result.near_depth, ndc.z);
        result.far_depth = std::min(result.far_depth, ndc.z);
    }
    return true;
}
float cross(glm::vec2 a, glm::vec2 b) noexcept {
    return a.x * b.y - a.y * b.x;
}
} // namespace

Bounds transform_bounds(const Bounds &b, const glm::vec4 &x, const glm::vec4 &y,
                        const glm::vec4 &z) noexcept {
    const glm::vec4 c(b.center, 1.0F);
    return {{glm::dot(x, c), glm::dot(y, c), glm::dot(z, c)},
            {glm::dot(glm::abs(glm::vec3(x)), b.extent), glm::dot(glm::abs(glm::vec3(y)), b.extent),
             glm::dot(glm::abs(glm::vec3(z)), b.extent)}};
}

std::optional<float> ray_bounds_intersection(
    const glm::vec3 origin,
    const glm::vec3 direction,
    const Bounds& bounds) noexcept {
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        !std::isfinite(origin.z) || !std::isfinite(direction.x) ||
        !std::isfinite(direction.y) || !std::isfinite(direction.z) ||
        !std::isfinite(bounds.center.x) ||
        !std::isfinite(bounds.center.y) ||
        !std::isfinite(bounds.center.z) ||
        !std::isfinite(bounds.extent.x) ||
        !std::isfinite(bounds.extent.y) ||
        !std::isfinite(bounds.extent.z) ||
        bounds.extent.x < 0.0F || bounds.extent.y < 0.0F ||
        bounds.extent.z < 0.0F) {
        return std::nullopt;
    }
    float nearest = 0.0F;
    float farthest = std::numeric_limits<float>::infinity();
    constexpr float parallel_epsilon = 1.0e-7F;
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        const float minimum = bounds.center[axis] - bounds.extent[axis];
        const float maximum = bounds.center[axis] + bounds.extent[axis];
        if (std::abs(direction[axis]) <= parallel_epsilon) {
            if (origin[axis] < minimum || origin[axis] > maximum) {
                return std::nullopt;
            }
            continue;
        }
        float entry = (minimum - origin[axis]) / direction[axis];
        float exit = (maximum - origin[axis]) / direction[axis];
        if (entry > exit) {
            std::swap(entry, exit);
        }
        nearest = std::max(nearest, entry);
        farthest = std::min(farthest, exit);
        if (nearest > farthest) {
            return std::nullopt;
        }
    }
    return farthest >= 0.0F
        ? std::optional<float>(nearest)
        : std::nullopt;
}
ViewFrustum::ViewFrustum(const glm::mat4 &m) noexcept {
    const auto w = row(m, 3);
    planes_ = {w + row(m, 0), w - row(m, 0), w + row(m, 1),
               w - row(m, 1), row(m, 2),     w - row(m, 2)};
}
bool ViewFrustum::intersects(const Bounds &b) const noexcept {
    for (const auto &p : planes_) {
        const float distance = glm::dot(glm::vec3(p), b.center) + p.w;
        const float radius = glm::dot(glm::abs(glm::vec3(p)), b.extent);
        if (distance + radius < -1.0e-4F) {
            return false;
        }
    }
    return true;
}
std::array<glm::vec3, 8> bounds_corners(const Bounds &b) noexcept {
    std::array<glm::vec3, 8> out{};
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = b.center + b.extent * glm::vec3((i & 1U) ? 1.0F : -1.0F, (i & 2U) ? 1.0F : -1.0F,
                                                 (i & 4U) ? 1.0F : -1.0F);
    }
    return out;
}
void OcclusionGrid::clear() noexcept {
    depths_.fill(0.0F);
}
bool OcclusionGrid::has_coverage() const noexcept {
    return std::ranges::any_of(depths_, [](float depth) { return depth > 0.0F; });
}
void OcclusionGrid::add_box(const std::array<glm::vec3, 8> &corners, const glm::mat4 &vp,
                            const glm::vec2 guard) noexcept {
    Projection p;
    if (!project(corners, vp, p) || p.far_depth <= 0.0F) {
        return;
    }
    std::sort(p.points.begin(), p.points.end(),
              [](auto a, auto b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    std::array<glm::vec2, 16> hull{};
    unsigned count = 0;
    for (auto point : p.points) {
        while (count >= 2 &&
               cross(hull[count - 1] - hull[count - 2], point - hull[count - 1]) <= 0) {
            --count;
        }
        hull[count++] = point;
    }
    const unsigned lower = count + 1;
    for (int i = 6; i >= 0; --i) {
        const auto point = p.points[static_cast<std::size_t>(i)];
        while (count >= lower &&
               cross(hull[count - 1] - hull[count - 2], point - hull[count - 1]) <= 0) {
            --count;
        }
        hull[count++] = point;
    }
    if (count < 4) {
        return;
    }
    --count;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const glm::vec2 low = glm::vec2(x, y) / glm::vec2(width, height) - guard;
            const glm::vec2 high = glm::vec2(x + 1, y + 1) / glm::vec2(width, height) + guard;
            if (low.x < p.low.x || low.y < p.low.y || high.x > p.high.x || high.y > p.high.y) {
                continue;
            }
            bool inside = true;
            for (unsigned e = 0; e < count && inside; ++e) {
                const auto a = hull[e], edge = hull[(e + 1) % count] - a;
                for (const auto point :
                     std::array{low, high, glm::vec2(low.x, high.y), glm::vec2(high.x, low.y)}) {
                    if (cross(edge, point - a) <= 1.0e-6F) {
                        inside = false;
                        break;
                    }
                }
            }
            if (inside) {
                const auto cell = std::size_t(y) * width + x;
                depths_[cell] = std::max(depths_[cell], p.far_depth - 1.0e-5F);
            }
        }
    }
}
bool OcclusionGrid::occluded(const Bounds &bounds, const glm::mat4 &vp,
                             const glm::vec2 guard) const noexcept {
    Projection p;
    if (!project(bounds_corners(bounds), vp, p)) {
        return false;
    }
    p.low -= guard;
    p.high += guard;
    if (p.low.x < 0 || p.low.y < 0 || p.high.x >= 1 || p.high.y >= 1) {
        return false;
    }
    const auto x0 = static_cast<unsigned>(p.low.x * width),
               x1 = static_cast<unsigned>(p.high.x * width);
    const auto y0 = static_cast<unsigned>(p.low.y * height),
               y1 = static_cast<unsigned>(p.high.y * height);
    for (auto y = y0; y <= y1; ++y) {
        for (auto x = x0; x <= x1; ++x) {
            if (depths_[std::size_t(y) * width + x] <= p.near_depth + 1.0e-5F) {
                return false;
            }
        }
    }
    return true;
}
LodProjection lod_projection(const Bounds &bounds, const glm::mat4 &vp,
                             glm::vec2 resolution) noexcept {
    const auto x = row(vp, 0), y = row(vp, 1), w = row(vp, 3);
    const glm::vec4 center(bounds.center, 1);
    const float near_depth = glm::dot(w, center) - glm::dot(glm::abs(glm::vec3(w)), bounds.extent);
    if (near_depth <= 0) {
        return {};
    }
    const float focal = std::max(glm::length(glm::vec3(x)) * resolution.x * .5F,
                                 glm::length(glm::vec3(y)) * resolution.y * .5F);
    const float max_x =
        (std::abs(glm::dot(x, center)) + glm::dot(glm::abs(glm::vec3(x)), bounds.extent)) *
        resolution.x * .5F;
    const float max_y =
        (std::abs(glm::dot(y, center)) + glm::dot(glm::abs(glm::vec3(y)), bounds.extent)) *
        resolution.y * .5F;
    // Bound lateral and depth displacement, including off-axis perspective.
    return {near_depth, focal + std::max(max_x, max_y) / near_depth};
}
unsigned select_lod(std::span<const float> errors, float scale, float distance, float pixels,
                    unsigned previous, float budget) noexcept {
    if (distance <= 0.0F || errors.empty()) {
        return 0;
    }
    unsigned selected = 0;
    for (unsigned i = 1; i < errors.size(); ++i) {
        // Coarsening requires a margin; refinement immediately respects the
        // nominal budget. Error cannot grow past the advertised pixel limit.
        const float threshold = budget * (i > previous ? 0.8F : 1.0F);
        if (errors[i] * scale * pixels / distance <= threshold) {
            selected = i;
        }
    }
    return selected;
}
} // namespace vcr
