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
    {
        const PunctualLight point{.color = {1.0F, 0.5F, 0.25F},
                                  .intensity_candela = 120.0F,
                                  .range = 4.0F};
        expect(valid_punctual_light(point, PunctualLightType::point),
               "finite point-light photometric contract");
        expect(std::abs(punctual_distance_attenuation(1.0F, 2.0F) - 15.0F / 16.0F) < 1e-6F &&
                   std::abs(punctual_distance_attenuation(1.0F, 4.0F) - 255.0F / 256.0F) < 1e-6F &&
                   std::abs(punctual_distance_attenuation(2.0F, 4.0F) - 15.0F / 64.0F) < 1e-6F &&
                   punctual_distance_attenuation(4.0F, 4.0F) == 0.0F,
               "punctual cutoff follows the KHR inverse-square reference");
        expect(std::abs(punctual_distance_attenuation(0.0F, 4.0F) - 100.0F) < 0.001F,
               "punctual singularity is bounded at the documented source radius");

        PunctualLight spot = point;
        spot.inner_cone_degrees = 20.0F;
        spot.outer_cone_degrees = 30.0F;
        const float inner_cosine = std::cos(glm::radians(spot.inner_cone_degrees));
        const float outer_cosine = std::cos(glm::radians(spot.outer_cone_degrees));
        expect(valid_punctual_light(spot, PunctualLightType::spot) &&
                   punctual_spot_attenuation(inner_cosine, 20.0F, 30.0F) == 1.0F &&
                   punctual_spot_attenuation(outer_cosine, 20.0F, 30.0F) == 0.0F &&
                   punctual_spot_attenuation(-1.0F, 20.0F, 30.0F) == 0.0F,
               "spot cone has finite complementary endpoints");

        glm::mat4 world(1.0F);
        world[0][0] = 2.0F;
        world[1][1] = 3.0F;
        world[2][2] = 4.0F;
        world[3] = glm::vec4(3.0F, 2.0F, -7.0F, 1.0F);
        GpuPunctualLight gpu{};
        expect(make_gpu_punctual_light(spot, PunctualLightType::spot, world, gpu) &&
                   glm::vec3(gpu.position_range) == glm::vec3(3.0F, 2.0F, -7.0F) &&
                   gpu.position_range.w == 4.0F &&
                   glm::vec3(gpu.direction_type) == glm::vec3(0.0F, 0.0F, -1.0F) &&
                   gpu.direction_type.w == 1.0F && gpu.color_intensity.w == 120.0F,
               "GPU local light ignores node scale for direction and range");

        spot.outer_cone_degrees = spot.inner_cone_degrees;
        expect(!valid_punctual_light(spot, PunctualLightType::spot) &&
                   !make_gpu_punctual_light(spot, PunctualLightType::spot, world, gpu),
               "invalid spot cone is rejected before GPU packing");
    }
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
    expect(settings.shadow_resolution == 2048, "default atlas size preserves existing rendering");
    for (const auto resolution : shadow_resolutions) {
        settings.shadow_resolution = resolution;
        expect(valid_lighting_settings(settings), "all shadow presets accepted");
    }
    for (const auto invalid : {0U, 512U, 2049U, 16384U, 0xffffffffU}) {
        settings.shadow_resolution = invalid;
        expect(!valid_lighting_settings(settings), "invalid shadow sizes rejected");
    }
    settings.shadow_resolution = 2048;
    const double initial_hour = settings.hour;
    for (bool gi : {false, true}) {
        settings.voxel_gi = gi;
        settings.hour = initial_hour;
        advance_lighting_clock(settings, 10.0, true);
        expect(settings.hour == initial_hour && settings.cycle,
               "pause freezes time without disabling cycle, with GI on and off");
        advance_lighting_clock(settings, 10.0, false);
        expect(settings.hour > initial_hour, "resume advances the same lighting clock");
        settings.cycle = false;
        const auto stopped_hour = settings.hour;
        advance_lighting_clock(settings, 10.0, false);
        expect(settings.hour == stopped_hour, "Advance time off stays stopped after resume");
        settings.cycle = true;
    }
    settings.hour = initial_hour;
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
    for (const auto resolution : shadow_resolutions) {
        const auto tile = resolution / 2;
        for (float aspect : {0.5F, 16.0F / 9, 4.0F})
            for (auto direction : {glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
                                   glm::normalize(glm::vec3(1, .01F, -.35F))}) {
                const auto matrices = shadow_cascades(view, aspect, direction, tile);
                for (int cascade = 0; cascade < 4; ++cascade)
                    for (float z :
                         {cascade == 0 ? depths[0] : depths[cascade] * 0.9F, depths[cascade + 1]})
                        for (float x : {-1.0F, 1.0F})
                            for (float y : {-1.0F, 1.0F}) {
                                const float tangent = std::tan(glm::radians(65.0F) * .5F);
                                auto clip =
                                    matrices[cascade] * inverse *
                                    glm::vec4(x * z * tangent * aspect, y * z * tangent, -z, 1);
                                clip /= clip.w;
                                expect(
                                    std::isfinite(clip.x) &&
                                        std::abs(clip.x) <= 1.0F - 4.0F / tile &&
                                        std::abs(clip.y) <= 1.0F - 4.0F / tile && clip.z >= 0 &&
                                        clip.z <= 1,
                                    "cascade covers receiver frustum even at vertical sun/extreme "
                                    "aspect");
                            }
            }
        const auto direction = glm::normalize(glm::vec3(1, 2, -1));
        const auto reference = shadow_cascades(view, 16.0F / 9, direction, tile);
        for (unsigned pose = 0; pose < 100; ++pose) {
            const auto position =
                glm::vec3(static_cast<float>(pose) * 2.17F, 7, -static_cast<float>(pose));
            const auto moved_view = glm::lookAt(
                position, position + glm::vec3(0.1F * pose, -1, -10), glm::vec3(0, 1, 0));
            const auto moved = shadow_cascades(moved_view, 16.0F / 9, direction, tile);
            for (unsigned cascade = 0; cascade < 4; ++cascade) {
                for (unsigned column = 0; column < 3; ++column)
                    expect(glm::length(moved[cascade][column] - reference[cascade][column]) < 1e-7F,
                           "camera pose cannot change shadow scale or light basis");
                for (unsigned axis = 0; axis < 2; ++axis) {
                    const float shift =
                        (moved[cascade][3][axis] - reference[cascade][3][axis]) * (tile / 2);
                    expect(std::abs(shift - std::round(shift)) < 0.002F,
                           "camera moves shadows by integer texels only");
                }
            }
        }
    }
    std::puts("Lighting invariants passed");
}
