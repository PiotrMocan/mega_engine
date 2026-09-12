#include "lighting.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <tuple>

namespace vcr {
double wrap_day_hour(double hour) noexcept {
    if (!std::isfinite(hour))
        return 12.0;
    hour = std::fmod(hour, 24.0);
    return hour < 0 ? hour + 24.0 : hour;
}
void advance_lighting_clock(LightingSettings &settings, double seconds, bool paused) noexcept {
    if (!paused && settings.dynamic && settings.cycle && std::isfinite(seconds) && seconds > 0)
        settings.hour = wrap_day_hour(settings.hour + seconds * 24.0 / settings.day_seconds);
}
SunState evaluate_sun(double hour) noexcept {
    const float angle = static_cast<float>((wrap_day_hour(hour) - 6.0) * 3.141592653589793 / 12.0);
    const glm::vec3 d = glm::normalize(glm::vec3(std::cos(angle), std::sin(angle), -0.35F));
    const float day = glm::smoothstep(-0.08F, 0.18F, d.y);
    const float high = glm::smoothstep(0.0F, 0.5F, d.y);
    SunState result;
    result.direction_intensity = glm::vec4(d, 1.6F * glm::smoothstep(-0.015F, 0.12F, d.y));
    result.color =
        glm::vec4(glm::mix(glm::vec3(1.0F, 0.28F, 0.08F), glm::vec3(1.0F, 0.94F, 0.82F), high), 1);
    result.sky_zenith = glm::vec4(
        glm::mix(glm::vec3(0.001F, 0.002F, 0.006F), glm::vec3(0.12F, 0.32F, 0.72F), day), 1);
    result.sky_horizon = glm::vec4(
        glm::mix(glm::vec3(0.003F, 0.005F, 0.012F),
                 glm::mix(glm::vec3(0.8F, 0.24F, 0.06F), glm::vec3(0.52F, 0.67F, 0.82F), high),
                 day),
        1);
    return result;
}
void DirtyVoxelBricks::reset(glm::vec3 origin, float cell) noexcept {
    origin_ = origin;
    cell_ = cell;
    dirty_.set();
}
void DirtyVoxelBricks::mark(const Bounds &b) noexcept {
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(b.center[axis]) || !std::isfinite(b.extent[axis])) {
            dirty_.set(); // Conservative invalidation; never convert NaN to an index.
            return;
        }
    }
    const glm::vec3 lo =
        (b.center - b.extent - origin_ - glm::vec3(cell_ * 0.002F)) / (cell_ * gi_brick_size);
    const glm::vec3 hi =
        (b.center + b.extent - origin_ + glm::vec3(cell_ * 0.002F)) / (cell_ * gi_brick_size);
    if (glm::any(glm::lessThan(hi, glm::vec3(0))) ||
        glm::any(glm::greaterThanEqual(lo, glm::vec3(8))))
        return;
    const auto first = glm::ivec3(glm::clamp(glm::floor(lo), glm::vec3(0), glm::vec3(7)));
    const auto last = glm::ivec3(glm::clamp(glm::floor(hi), glm::vec3(0), glm::vec3(7)));
    for (int z = first.z; z <= last.z; ++z)
        for (int y = first.y; y <= last.y; ++y)
            for (int x = first.x; x <= last.x; ++x)
                dirty_.set(static_cast<std::size_t>(x) +
                           8 * (static_cast<std::size_t>(y) + 8 * static_cast<std::size_t>(z)));
}
std::array<std::uint32_t, gi_brick_count> DirtyVoxelBricks::take() noexcept {
    std::array<std::uint32_t, gi_brick_count> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = dirty_[i] ? 1U : 0U;
    dirty_.reset();
    return result;
}
void VoxelSceneTracker::reserve() {
    current_.reserve(max_scene_nodes + 2ULL);
    next_.reserve(max_scene_nodes + 2ULL);
    world_bounds_.reserve(max_scene_nodes + 2ULL);
}
void VoxelSceneTracker::append(VoxelSurface surface) {
    assert(next_.size() < next_.capacity());
    next_.push_back(surface);
}
namespace {
auto surface_key(const VoxelSurface &s) noexcept {
    return std::tuple(s.id.index, s.id.generation, s.first, s.count);
}
bool same_surface(const VoxelSurface &a, const VoxelSurface &b) noexcept {
    return surface_key(a) == surface_key(b) && a.transform == b.transform && a.color == b.color &&
           a.local_bounds.center == b.local_bounds.center &&
           a.local_bounds.extent == b.local_bounds.extent;
}
} // namespace
void VoxelSceneTracker::finish_snapshot(DirtyVoxelBricks &dirty) {
    std::sort(next_.begin(), next_.end(),
              [](const auto &a, const auto &b) { return surface_key(a) < surface_key(b); });
    std::size_t old = 0, next = 0;
    while (old < current_.size() || next < next_.size()) {
        if (next == next_.size() ||
            (old < current_.size() && surface_key(current_[old]) < surface_key(next_[next])))
            dirty.mark(current_[old++].world_bounds());
        else if (old == current_.size() || surface_key(next_[next]) < surface_key(current_[old]))
            dirty.mark(next_[next++].world_bounds());
        else {
            if (!same_surface(current_[old], next_[next])) {
                dirty.mark(current_[old].world_bounds());
                dirty.mark(next_[next].world_bounds());
            }
            ++old;
            ++next;
        }
    }
    current_.swap(next_);
    world_bounds_.clear();
    for (const auto &surface : current_)
        world_bounds_.push_back(surface.world_bounds());
}
void VoxelSceneTracker::set_transform(SceneNodeId id, TransformRows transform,
                                      DirtyVoxelBricks &dirty) noexcept {
    auto it = std::lower_bound(
        current_.begin(), current_.end(), id.index,
        [](const auto &surface, unsigned index) { return surface.id.index < index; });
    for (; it != current_.end() && it->id.index == id.index; ++it)
        if (it->id == id && it->transform != transform) {
            auto &bounds = world_bounds_[static_cast<std::size_t>(it - current_.begin())];
            dirty.mark(bounds);
            it->transform = transform;
            bounds = it->world_bounds();
            dirty.mark(bounds);
        }
}
std::array<glm::mat4, 4> shadow_cascades(const glm::mat4 &view, float aspect, glm::vec3 direction,
                                         unsigned tile_resolution) noexcept {
    assert(tile_resolution > 8);
    const glm::mat4 inverse = glm::inverse(view);
    constexpr std::array<float, 5> splits{0.1F, 12, 32, 80, 192};
    std::array<glm::mat4, 4> result{};
    const glm::vec3 up = std::abs(direction.y) > 0.98F ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const float tangent = std::tan(glm::radians(65.0F) * 0.5F);
    // The light basis must not depend on subtraction of two large world positions.
    const auto light_rotation = glm::lookAt(glm::vec3(0), -direction, up);
    for (std::size_t cascade = 0; cascade < result.size(); ++cascade) {
        // The shader starts blending before the preceding split. Both maps must
        // cover that receiver band, including their complete PCF footprints.
        const float near_depth = cascade == 0 ? splits[0] : splits[cascade] * 0.9F;
        const float far_depth = splits[cascade + 1];
        const auto center = inverse * glm::vec4(0, 0, -(near_depth + far_depth) * 0.5F, 1);
        // Compute the radius in camera space so translation/rotation cannot change it.
        float radius = glm::length(glm::vec3(far_depth * tangent * aspect, far_depth * tangent,
                                             (far_depth - near_depth) * 0.5F));
        radius = std::ceil(radius * 16.0F) / 16.0F;
        const float resolution = static_cast<float>(tile_resolution);
        radius *= resolution / (resolution - 6.0F);
        const float texel = 2.0F * radius / resolution;
        auto light_center = glm::vec3(light_rotation * center);
        light_center = glm::round(light_center / texel) * texel;
        auto light_view = light_rotation;
        light_view[3] =
            glm::vec4(-light_center.x, -light_center.y, -light_center.z - radius - 192.0F, 1);
        auto projection =
            glm::ortho(-radius, radius, -radius, radius, 0.1F, 2.0F * radius + 384.0F);
        projection[1][1] *= -1;
        result[cascade] = projection * light_view;
    }
    return result;
}
} // namespace vcr
