#include "lighting.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
namespace {
void expect(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}
} // namespace
int main() {
    using namespace vcr;
    expect(wrap_day_hour(-1) == 23 && wrap_day_hour(48) == 0, "day wraps both directions");
    expect(std::isfinite(wrap_day_hour(std::numeric_limits<double>::infinity())),
           "nonfinite hours stay safe");
    expect(evaluate_sun(12).direction_intensity.w > 1 && evaluate_sun(0).direction_intensity.w == 0,
           "no solar energy at midnight");
    expect(glm::length(evaluate_sun(0).sky_zenith - evaluate_sun(24).sky_zenith) < 1e-6F,
           "midnight sky is periodic");
    for (int minute = 0; minute < 1440; ++minute) {
        const auto sun = evaluate_sun(minute / 60.0);
        expect(std::abs(glm::length(glm::vec3(sun.direction_intensity)) - 1) < 1e-5F,
               "normalized finite sun direction");
        expect(glm::length(sun.sky_zenith - evaluate_sun((minute + 1) / 60.0).sky_zenith) < 0.04F,
               "continuous day/night sky");
    }
    LightingSettings settings;
    expect(valid_lighting_settings(settings), "defaults valid");
    settings.dynamic = false;
    expect(!valid_lighting_settings(settings), "voxel GI requires dynamic lighting");
    settings.voxel_gi = false;
    expect(valid_lighting_settings(settings), "legacy fallback valid");
    settings.gi_extent = 0;
    expect(!valid_lighting_settings(settings), "reject zero volume");
    settings.gi_extent = 64;
    settings.gi_update_hz = std::numeric_limits<float>::quiet_NaN();
    expect(!valid_lighting_settings(settings), "reject nonfinite update rate");
    DirtyVoxelBricks dirty;
    dirty.reset({0, 0, 0}, 1);
    expect(dirty.count() == 512, "new volume requires complete initialization");
    static_cast<void>(dirty.take());
    dirty.mark({{-20, -20, -20}, {1, 1, 1}});
    expect(dirty.count() == 0, "outside changes do not dirty volume");
    dirty.mark({{8, 4, 4}, {0, 1, 1}});
    expect(dirty.count() == 2, "conservative voxelization at brick seam clears both sides");
    static_cast<void>(dirty.take());
    VoxelSceneTracker tracker;
    tracker.reserve();
    VoxelSurface box{{3, 1}, 0, 12, {}, {{0, 0, 0}, {1, 1, 1}}, glm::vec4(1)};
    box.transform.x.w = 4;
    box.transform.y.w = 4;
    box.transform.z.w = 4;
    tracker.begin_snapshot();
    tracker.append(box);
    tracker.finish_snapshot(dirty);
    expect(dirty.count() == 1, "one new cube dirties one brick");
    static_cast<void>(dirty.take());
    tracker.begin_snapshot();
    tracker.append(box);
    tracker.finish_snapshot(dirty);
    expect(dirty.count() == 0, "unchanged snapshot reuses voxel geometry");
    auto moved = box.transform;
    moved.x.w = 20;
    tracker.set_transform(box.id, moved, dirty);
    expect(dirty.count() == 2, "movement dirties old and new positions");
    static_cast<void>(dirty.take());
    tracker.begin_snapshot();
    tracker.finish_snapshot(dirty);
    auto mask = dirty.take();
    expect(mask[2] == 1 && mask[0] == 0,
           "deletion uses latest runtime position, not authored snapshot");
    box.id.generation = 2;
    tracker.begin_snapshot();
    tracker.append(box);
    tracker.finish_snapshot(dirty);
    static_cast<void>(dirty.take());
    tracker.set_transform({3, 1}, moved, dirty);
    expect(dirty.count() == 0, "stale generation cannot move reused node");
    box.color.x = 0;
    tracker.begin_snapshot();
    tracker.append(box);
    tracker.finish_snapshot(dirty);
    expect(dirty.count() == 1, "material change rebuilds surface albedo");
    static_cast<void>(dirty.take());
    // A submesh outside the first submesh's AABB must also be invalidated.
    auto second = box;
    second.first = 12;
    second.local_bounds.center.x = 16;
    tracker.begin_snapshot();
    tracker.append(second);
    tracker.append(box);
    tracker.finish_snapshot(dirty);
    static_cast<void>(dirty.take());
    moved.x.w = 36;
    tracker.set_transform(box.id, moved, dirty);
    expect(dirty.count() == 4, "all submeshes participate in runtime invalidation");
    expect(tracker.world_bounds().size() == tracker.surfaces().size(),
           "SoA candidate bounds cover all surfaces");
    for (std::size_t i = 0; i < tracker.surfaces().size(); ++i) {
        expect(tracker.world_bounds()[i].center == tracker.surfaces()[i].world_bounds().center,
               "candidate bounds follow moved submeshes");
    }
    const auto view = glm::lookAt(glm::vec3(13, 7, 21), glm::vec3(3, 2, -5), glm::vec3(0, 1, 0));
    const auto inverse = glm::inverse(view);
    constexpr float depths[]{0.1F, 12, 32, 80, 192};
    for (float aspect : {0.5F, 16.0F / 9, 4.0F})
        for (auto direction :
             {glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::normalize(glm::vec3(1, .01F, -.35F))}) {
            const auto matrices = shadow_cascades(view, aspect, direction);
            for (int cascade = 0; cascade < 4; ++cascade)
                for (float z : {depths[cascade], depths[cascade + 1]})
                    for (float x : {-1.0F, 1.0F})
                        for (float y : {-1.0F, 1.0F}) {
                            const float tangent = std::tan(glm::radians(65.0F) * .5F);
                            auto clip = matrices[cascade] * inverse *
                                        glm::vec4(x * z * tangent * aspect, y * z * tangent, -z, 1);
                            clip /= clip.w;
                            expect(std::isfinite(clip.x) && std::abs(clip.x) <= 1.001F &&
                                       std::abs(clip.y) <= 1.001F && clip.z >= 0 && clip.z <= 1,
                                   "cascade covers receiver frustum even at vertical sun/extreme "
                                   "aspect");
                        }
        }
    std::puts("Lighting invariants passed");
}
