#include "renderer.hpp"
#include "coarse_depth_order.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>
#include <glm/gtc/matrix_access.hpp>

namespace vcr {
bool Renderer::set_lod_fade(LodFadePattern pattern, float seconds) noexcept {
    if ((pattern != LodFadePattern::off && pattern != LodFadePattern::bayer4 &&
         pattern != LodFadePattern::bayer8) || !std::isfinite(seconds) ||
        seconds < 0.05F || seconds > 2.0F) return false;
    if (pattern == lod_fade_pattern_ && seconds == lod_fade_seconds_) return true;
    lod_fade_pattern_ = pattern;
    lod_fade_seconds_ = seconds;
#if VCR_EDITOR_ENABLED
    reset_lod_fades_ = true;
    reset_lod_history_ = true;
    packet_document_revision_ = 0;
#endif
    invalidate_temporal_history();
    exposure_reset_pending_ = true;
    return true;
}
#if VCR_EDITOR_ENABLED
bool Renderer::set_runtime_transform(SceneNodeId id, TransformRows transform) noexcept {
    if (!runtime_transforms_.contains(id)) {
        return false;
    }
    const auto revision = runtime_transforms_.revision();
    const bool result = runtime_transforms_.set(id, transform);
    if (revision != runtime_transforms_.revision()) {
        shadow_dirty_ = true;
        if (gi_provisioned_)
            voxel_scene_.set_transform(id, transform, voxel_dirty_);
    }
    return result;
}
void Renderer::set_visibility_settings(VisibilitySettings settings) noexcept {
    settings.lod_pixel_error = std::isfinite(settings.lod_pixel_error)
                                   ? std::clamp(settings.lod_pixel_error, 0.1F, 8.0F)
                                   : 1.0F;
    visibility_settings_ = settings;
    packet_document_revision_ = 0;
    shadow_dirty_ = true;
}
void Renderer::prepare_scene_packet(const glm::mat4 &vp, glm::vec3 camera, VkExtent2D extent) {
    const auto forward = glm::normalize(glm::vec3(glm::row(vp, 3)));
    // Large discontinuities and long pauses snap instead of fading stale LODs.
    // Normal continuous motion advances in seconds, independent of frame rate.
    if (lod_fade_pattern_ != LodFadePattern::off &&
        (!lod_camera_valid_ || glm::length(camera - lod_camera_position_) > 25.0F ||
         glm::dot(forward, lod_camera_forward_) < 0.70710678F || fsr_frame_time_ms_ > 250.0F)) {
        reset_lod_fades_ = true;
        reset_lod_history_ = true;
    }
    lod_camera_position_ = camera;
    lod_camera_forward_ = forward;
    lod_camera_valid_ = lod_fade_pattern_ != LodFadePattern::off;
    const bool gpu = visibility_pipeline_.get() != VK_NULL_HANDLE && !scene_metadata_.empty() &&
                     visibility_settings_.backend != VisibilitySettings::Backend::cpu &&
                     (visibility_settings_.backend == VisibilitySettings::Backend::gpu ||
                      scene_metadata_.size() >= 1024);
    if (gpu) {
        if (!gpu_visibility_active_) {
            gpu_packet_revision_ = 0;
            reset_lod_history_ = true;
        }
        gpu_visibility_active_ = true;
        prepare_gpu_scene_packet(vp, camera, extent);
        return;
    }
    if (gpu_visibility_active_) {
        gpu_visibility_active_ = false;
        reset_lod_fades_ = true;
        packet_document_revision_ = 0;
        for (auto &frame : frames_) {
            frame.scene_revision = 0;
        }
    }
    const bool data_changed = packet_runtime_revision_ != runtime_transforms_.revision() ||
                              packet_document_revision_ != editor_scene_revision_;
    const bool fade_enabled = lod_fade_pattern_ != LodFadePattern::off && visibility_settings_.lod;
    const bool reset_fade = reset_lod_fades_ || packet_document_revision_ != editor_scene_revision_;
    // Without camera-dependent membership/LOD, only clip.w can change the
    // coarse keys. Exact equality also permits lateral motion and AA jitter
    // to reuse the packet when that row remains bit-for-bit unchanged.
    const bool depth_order_unchanged = !visibility_settings_.coarse_front_to_back ||
        glm::row(vp, 3) == glm::row(packet_view_projection_, 3);
    if (!data_changed && !lod_fades_active_ && !reset_fade && depth_order_unchanged && !visibility_settings_.frustum &&
        (!visibility_settings_.occlusion ||
         scene_metadata_.size() < visibility_settings_.occlusion_threshold) &&
        (!visibility_settings_.lod || (editor_model_draws_.empty() && !editor_terrain_visible_))) {
        return;
    }
    if (!data_changed && !lod_fades_active_ && !reset_fade && extent.width == packet_extent_.width &&
        extent.height == packet_extent_.height && vp == packet_view_projection_) {
        return;
    }
    assert(scene_metadata_.size() == editor_scene_instances_.size());
    const auto count = static_cast<std::uint32_t>(scene_metadata_.size());
    const auto guard = glm::vec2(2.0F) / glm::vec2(extent.width, extent.height);
    auto padded_vp = vp;
    for (int column = 0; column < 4; ++column) {
        padded_vp[column][0] *= 1.0F - guard.x;
        padded_vp[column][1] *= 1.0F - guard.y;
    }
    const ViewFrustum frustum(padded_vp);
    const auto terrain_projection =
        lod_projection(terrain_bounds_, vp, {extent.width, extent.height});
    terrain_lod_ = visibility_settings_.lod
                       ? select_lod(terrain_lod_error_, 1, terrain_projection.nearest_depth,
                                    terrain_projection.pixels, terrain_lod_,
                                    visibility_settings_.lod_pixel_error)
                       : 0;
    const bool use_occlusion =
        visibility_settings_.occlusion && count >= visibility_settings_.occlusion_threshold &&
        (count <= 2048 || visibility_settings_.backend == VisibilitySettings::Backend::cpu);
    OcclusionGrid occlusion;
    std::array<std::pair<float, std::uint32_t>, 16> occluders{};
    if (data_changed) {
        for (std::uint32_t i = 0; i < count; ++i) {
            auto &instance = editor_scene_instances_[i];
            const auto &metadata = scene_metadata_[i];
            if (runtime_transforms_.contains(metadata.id)) {
                const auto t = runtime_transforms_.current(metadata.id);
                instance.model_row_0 = t.x;
                instance.model_row_1 = t.y;
                instance.model_row_2 = t.z;
            }
            world_bounds_[i] = transform_bounds(metadata.local_bounds, instance.model_row_0,
                                                instance.model_row_1, instance.model_row_2);
        }
    }
    bounds_hierarchy_active_ = false;
    if (visibility_settings_.frustum && visibility_settings_.cpu_hierarchy) {
        if (hierarchy_rebuild_required_ || (data_changed && !world_hierarchy_.valid())) {
            // Topology changes are cold scene preparation. Transform changes
            // retain the balanced topology and only refit its enclosure.
            static_cast<void>(world_hierarchy_.build(world_bounds_));
            hierarchy_rebuild_required_ = false;
        } else if (data_changed) {
            static_cast<void>(world_hierarchy_.refit(world_bounds_));
        }
        bounds_hierarchy_active_ = world_hierarchy_.query(frustum, instance_visibility_);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto &metadata = scene_metadata_[i];
        const auto &b = world_bounds_[i];
        const bool visible = bounds_hierarchy_active_ ? instance_visibility_[i] != 0 :
            (!visibility_settings_.frustum || frustum.intersects(b));
        instance_visibility_[i] = visible ? 1U : 0U;
        if (use_occlusion && visible && metadata.occluder) {
            const float distance = glm::length(b.center - camera);
            const float score = glm::length(b.extent) / std::max(distance, 0.01F);
            if (score > 0.05F) {
                auto worst = std::min_element(occluders.begin(), occluders.end());
                if (score > worst->first) {
                    *worst = {score, i};
                }
            }
        }
    }
    if (use_occlusion) {
        occlusion.clear();
        for (const auto [score, index] : occluders) {
            if (score == 0.0F) {
                continue;
            }
            const auto &instance = editor_scene_instances_[index];
            auto corners = bounds_corners(scene_metadata_[index].local_bounds);
            for (auto &point : corners) {
                const glm::vec4 p(point, 1);
                point = {glm::dot(instance.model_row_0, p), glm::dot(instance.model_row_1, p),
                         glm::dot(instance.model_row_2, p)};
            }
            occlusion.add_box(corners, padded_vp, guard);
        }
    }
    const bool test_occlusion = use_occlusion && occlusion.has_coverage();
    visibility_statistics_ = {};
    visibility_statistics_.submitted = count;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!instance_visibility_[i]) {
            ++visibility_statistics_.frustum_rejected;
        } else if (test_occlusion && occlusion.occluded(world_bounds_[i], padded_vp, guard)) {
            instance_visibility_[i] = 0;
            ++visibility_statistics_.occlusion_rejected;
        }
    }
    visibility_scratch_.clear();
    if (visibility_settings_.coarse_front_to_back) {
        coarse_depth_keys_.resize(count);
    }
    const auto sort_range = [this, &vp](std::size_t first) {
        if (!visibility_settings_.coarse_front_to_back || visibility_scratch_.size() - first < 2) {
            return;
        }
        // Preserve batching boundaries and source indices. Only the appended
        // visible packet changes order; the shadow/source packet stays intact.
        // Calculate the finite/overflow-safe key once per visible instance,
        // instead of repeating its double arithmetic for every comparison.
        for (auto position = first; position < visibility_scratch_.size(); ++position) {
            const auto index = lod_visibility_source(visibility_scratch_[position]);
            coarse_depth_keys_[index] = coarse_depth_key(world_bounds_[index], vp);
        }
        std::sort(visibility_scratch_.begin() + static_cast<std::ptrdiff_t>(first),
                  visibility_scratch_.end(), [this](std::uint32_t left, std::uint32_t right) {
                      left = lod_visibility_source(left);
                      right = lod_visibility_source(right);
                      return std::pair{coarse_depth_keys_[left], left} <
                             std::pair{coarse_depth_keys_[right], right};
                  });
    };
    const auto add_range = [this, &sort_range](std::uint32_t begin, std::uint32_t end) {
        const auto first = visibility_scratch_.size();
        for (auto i = begin; i < end; ++i) {
            if (instance_visibility_[i]) {
                visibility_scratch_.push_back(i);
            }
        }
        sort_range(first);
        return static_cast<std::uint32_t>(visibility_scratch_.size() - first);
    };
    visible_ground_count_ = add_range(0, editor_ground_count_);
    visible_cube_count_ =
        add_range(editor_ground_count_, editor_ground_count_ + editor_cube_count_);
    visibility_statistics_.triangles = visible_ground_count_ * 2ULL + visible_cube_count_ * 12ULL;
    visibility_statistics_.lod_instances[0] = visible_ground_count_ + visible_cube_count_;
    visible_model_draws_.clear();
    bool lod_changed = false;
    lod_fades_active_ = false;
    for (const auto &draw : editor_model_draws_) {
        const auto &asset = editor_model_assets_[draw.asset_index];
        const auto &submesh = asset.submeshes[draw.submesh_index];
        for (auto i = draw.first_instance; i < draw.first_instance + draw.instance_count; ++i) {
            const auto &instance = editor_scene_instances_[i];
            // Frobenius norm is an upper bound on the largest singular value,
            // including shear. A conservative scale avoids excessive LOD error.
            const float scale = std::sqrt(
                glm::dot(glm::vec3(instance.model_row_0), glm::vec3(instance.model_row_0)) +
                glm::dot(glm::vec3(instance.model_row_1), glm::vec3(instance.model_row_1)) +
                glm::dot(glm::vec3(instance.model_row_2), glm::vec3(instance.model_row_2)));
            const auto &b = world_bounds_[i];
            const auto lod_view = lod_projection(b, vp, {extent.width, extent.height});
            const unsigned lod =
                visibility_settings_.lod
                    ? select_lod(submesh.lod_error, scale, lod_view.nearest_depth, lod_view.pixels,
                                 instance_lods_[i], visibility_settings_.lod_pixel_error)
                    : 0;
            // Finite error also represents intentional removal in authored LODs.
            const auto usable = std::isfinite(submesh.lod_error[lod]) ? lod : 0;
            lod_changed |= instance_lods_[i] != usable;
            instance_lods_[i] = static_cast<std::uint8_t>(usable);
            if (fade_enabled) {
                auto &fade = instance_lod_fades_[i];
                advance_lod_fade(fade, usable, fsr_frame_time_ms_ * 0.001F,
                                 lod_fade_seconds_, true,
                                 reset_fade || instance_visibility_[i] == 0);
                lod_fades_active_ |= fade.previous != fade.current && fade.progress < 1.0F;
            }
        }
        for (unsigned lod = 0; lod < 3; ++lod) {
            const auto first = visibility_scratch_.size();
            auto visible = draw;
            visible.first_instance = static_cast<std::uint32_t>(visibility_scratch_.size());
            visible.instance_count = 0;
            visible.first_index = lod == 0 ? draw.first_index : submesh.lod_first[lod];
            visible.index_count = lod == 0 ? draw.index_count : submesh.lod_count[lod];
            for (auto i = draw.first_instance; i < draw.first_instance + draw.instance_count; ++i) {
                const auto &fade = instance_lod_fades_[i];
                const bool transitioning = fade_enabled && fade.previous != fade.current && fade.progress < 1.0F;
                const auto current_lod = fade_enabled ? fade.current : instance_lods_[i];
                if (instance_visibility_[i] && (current_lod == lod ||
                    (transitioning && fade.previous == lod))) {
                    const auto token = transitioning
                        ? encode_lod_visibility(i, fade.progress, fade.previous == lod, lod_fade_pattern_)
                        : std::optional<std::uint32_t>{i};
                    assert(token.has_value());
                    visibility_scratch_.push_back(*token);
                    ++visible.instance_count;
                }
            }
            sort_range(first);
            if (visible.instance_count) {
                visible_model_draws_.push_back(visible);
                visibility_statistics_.lod_instances[lod] += visible.instance_count;
                visibility_statistics_.triangles +=
                    std::uint64_t(visible.index_count / 3) * visible.instance_count;
            }
        }
    }
    // Draw emission counts include both LODs; visibility counts unique sources.
    visibility_statistics_.visible = count - visibility_statistics_.frustum_rejected -
                                      visibility_statistics_.occlusion_rejected;
    reset_lod_fades_ = false;
    if (data_changed) {
        scene_packet_.clear();
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto &current = editor_scene_instances_[i];
            const auto id = scene_metadata_[i].id;
            const auto previous =
                runtime_transforms_.contains(id)
                    ? runtime_transforms_.previous(id)
                    : TransformRows{current.model_row_0, current.model_row_1, current.model_row_2};
            scene_packet_.push_back({current, previous});
            // Stable source index survives visibility/LOD compaction. The
            // material's reserved GPU lane is not part of authored factors.
            scene_packet_.back().current.material.reserved = static_cast<float>(id ? id.index : i);
        }
        ++source_packet_revision_;
    }
    if (data_changed || lod_changed || visible_indices_ != visibility_scratch_) {
        // Camera/LOD changes only rebuild indices. Original records retain
        // their transforms and material data for both camera and shadow draws.
        visible_indices_.assign(visibility_scratch_.begin(), visibility_scratch_.end());
        ++packet_revision_;
    }
    visibility_statistics_.packet_revision = packet_revision_;
    packet_runtime_revision_ = runtime_transforms_.revision();
    packet_document_revision_ = editor_scene_revision_;
    packet_view_projection_ = vp;
    packet_extent_ = extent;
}
#endif
} // namespace vcr
