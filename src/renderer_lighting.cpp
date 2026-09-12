#include "image_barrier.hpp"
#include "renderer.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace vcr {
namespace {
constexpr VkFormat radiance_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkPipelineStageFlags2 compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 storage_rw =
    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 source, VkAccessFlags2 reads,
                    VkPipelineStageFlags2 destination, VkAccessFlags2 writes) {
    const VkMemoryBarrier2 barrier{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, source, reads, destination, writes};
    const VkDependencyInfo dependency{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 1, &barrier, 0, nullptr, 0, nullptr};
    vkCmdPipelineBarrier2(cmd, &dependency);
}
bool intersects(const Bounds &a, const Bounds &b) noexcept {
    return glm::all(glm::lessThanEqual(glm::abs(a.center - b.center), a.extent + b.extent));
}
} // namespace
bool Renderer::set_lighting_settings(LightingSettings settings) noexcept {
    if (!valid_lighting_settings(settings) || (settings.voxel_gi && !gi_provisioned_) ||
        !supports_shadow_resolution(settings.shadow_resolution))
        return false;
    settings.hour = wrap_day_hour(settings.hour);
    if (settings.dynamic != lighting_settings_.dynamic ||
        settings.hour != lighting_settings_.hour) {
        shadow_dirty_ = true;
        lighting_shadow_initialized_ = false;
        temporal_history_valid_ = false;
        fsr3_upscaler_.request_reset();
    }
    if (settings.gi_extent != lighting_settings_.gi_extent)
        gi_scene_revision_ = 0;
    gi_light_elapsed_ = 1000;
    lighting_settings_ = settings;
    return true;
}
void Renderer::create_lighting_resources() {
    const auto &limits = context_->properties().limits;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context_->physical_device(), radiance_format, &properties);
    constexpr auto features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                              VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                              VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkImageFormatProperties image_properties{};
    const auto support = vkGetPhysicalDeviceImageFormatProperties(
        context_->physical_device(), radiance_format, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        0, &image_properties);
    if (support != VK_SUCCESS && support != VK_ERROR_FORMAT_NOT_SUPPORTED)
        VK_CHECK(support);
    if (support != VK_SUCCESS || (properties.optimalTilingFeatures & features) != features)
        fatal("Lighting requires filterable RGBA16F 3D storage images");
    gi_provisioned_ = lighting_settings_.voxel_gi;
    if (gi_provisioned_ &&
        (limits.maxStorageBufferRange < sizeof(GiTriangle) * gi_max_triangles ||
         limits.maxComputeWorkGroupInvocations < 64 || limits.maxComputeWorkGroupSize[0] < 64 ||
         limits.maxComputeWorkGroupCount[0] < 4096 ||
         image_properties.maxExtent.width < gi_resolution))
        fatal("Voxel GI exceeds compute/storage limits; start with --gi off");
    constexpr auto host =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    voxel_geometry_.create(
        *context_, gi_provisioned_ ? 4ULL * gi_resolution * gi_resolution * gi_resolution : 32ULL,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "GI voxel surfaces");
    voxel_triangles_.create(*context_,
                            sizeof(GiTriangle) * (gi_provisioned_ ? gi_max_triangles : 1U),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "GI triangle cache");
    voxel_radiance_.create_volume(*context_, gi_provisioned_ ? gi_resolution : 2U,
                                  gi_provisioned_ ? 7U : 1U, radiance_format,
                                  "GI radiance mip volume");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = voxel_radiance_.image();
    view.viewType = VK_IMAGE_VIEW_TYPE_3D;
    view.format = radiance_format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(context_->device(), &view, nullptr,
                               voxel_storage_view_.put(context_->device(), vkDestroyImageView)));
    context_->set_debug_name(VK_OBJECT_TYPE_IMAGE_VIEW, handle_value(voxel_storage_view_.get()),
                             "GI mip zero storage view");
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                     nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
        static_cast<std::uint32_t>(bindings.size()), bindings.data()};
    VK_CHECK(vkCreateDescriptorSetLayout(
        context_->device(), &layout, nullptr,
        lighting_layout_.put(context_->device(), vkDestroyDescriptorSetLayout)));
    context_->set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                             handle_value(lighting_layout_.get()), "lighting descriptors");
    for (auto &frame : frames_) {
        frame.lighting_uniform_buffer.create(
            *context_, sizeof(LightingUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO, host, "lighting frame uniforms");
        const LightingUniforms initial{};
        frame.lighting_uniform_buffer.write(std::as_bytes(std::span{&initial, std::size_t{1}}));
        frame.lighting_was_dynamic = false;
        frame.voxel_job_buffer.create(
            *context_, sizeof(GiJob) * (gi_provisioned_ ? max_scene_nodes + 2ULL : 1ULL),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, host,
            "GI frame triangle jobs");
        frame.voxel_dirty_buffer.create(*context_, sizeof(voxel_job_mask_),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                        host, "GI frame dirty bricks");
    }
    if (gi_provisioned_) {
        voxel_jobs_.reserve(max_scene_nodes + 2ULL);
        voxel_batch_.reserve(max_scene_nodes + 2ULL);
        voxel_scene_.reserve();
    }
}
void Renderer::create_lighting_pipelines() {
    const std::array sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
    };
    const VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,     0, 2,
        static_cast<std::uint32_t>(sizes.size()),      sizes.data()};
    VK_CHECK(
        vkCreateDescriptorPool(context_->device(), &pool, nullptr,
                               lighting_pool_.put(context_->device(), vkDestroyDescriptorPool)));
    context_->set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_POOL, handle_value(lighting_pool_.get()),
                             "lighting descriptor pool");
    const auto layout = lighting_layout_.get();
    for (auto &frame : frames_) {
        const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                     nullptr, lighting_pool_.get(), 1, &layout};
        VK_CHECK(vkAllocateDescriptorSets(context_->device(), &allocation,
                                          &frame.lighting_descriptor_set));
        context_->set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET,
                                 handle_value(frame.lighting_descriptor_set),
                                 "lighting frame descriptors");
        const std::array buffers{
            VkDescriptorBufferInfo{frame.lighting_uniform_buffer.handle(), 0,
                                   sizeof(LightingUniforms)},
            VkDescriptorBufferInfo{voxel_geometry_.handle(), 0, voxel_geometry_.size()},
            VkDescriptorBufferInfo{voxel_triangles_.handle(), 0, voxel_triangles_.size()},
            VkDescriptorBufferInfo{frame.voxel_job_buffer.handle(), 0,
                                   frame.voxel_job_buffer.size()},
            VkDescriptorBufferInfo{frame.voxel_dirty_buffer.handle(), 0,
                                   frame.voxel_dirty_buffer.size()},
        };
        const std::array images{
            VkDescriptorImageInfo{voxel_radiance_.sampler(), voxel_radiance_.view(),
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, voxel_storage_view_.get(),
                                  VK_IMAGE_LAYOUT_GENERAL},
        };
        std::array<VkWriteDescriptorSet, 7> writes{};
        for (unsigned i = 0; i < 7; ++i) {
            auto &w = writes[i];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = frame.lighting_descriptor_set;
            w.dstBinding = i;
            w.descriptorCount = 1;
            if (i == 1 || i == 6) {
                w.descriptorType = i == 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                          : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &images[i == 1 ? 0 : 1];
            } else {
                w.descriptorType =
                    i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.pBufferInfo = &buffers[i == 0 ? 0 : i - 1];
            }
        }
        vkUpdateDescriptorSets(context_->device(), 7, writes.data(), 0, nullptr);
    }
    if (!gi_provisioned_)
        return;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    const VkPipelineLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &layout, 1, &push};
    VK_CHECK(vkCreatePipelineLayout(
        context_->device(), &info, nullptr,
        lighting_compute_layout_.put(context_->device(), vkDestroyPipelineLayout)));
    context_->set_debug_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                             handle_value(lighting_compute_layout_.get()), "voxel compute layout");
    for (unsigned i = 0; i < 2; ++i) {
        const auto shader =
            load_shader(i == 0 ? "voxel_geometry.comp.spv" : "voxel_light.comp.spv");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          nullptr,
                          0,
                          VK_SHADER_STAGE_COMPUTE_BIT,
                          shader.get(),
                          "main",
                          nullptr};
        pipeline.layout = lighting_compute_layout_.get();
        auto &destination = i == 0 ? voxel_geometry_pipeline_ : voxel_light_pipeline_;
        VK_CHECK(vkCreateComputePipelines(context_->device(), pipeline_cache_, 1, &pipeline,
                                          nullptr,
                                          destination.put(context_->device(), vkDestroyPipeline)));
        context_->set_debug_name(VK_OBJECT_TYPE_PIPELINE, handle_value(destination.get()),
                                 i == 0 ? "GI voxelize triangles" : "GI relight voxels");
    }
}
std::uint32_t Renderer::append_gi_triangles(std::span<const GiTriangle> triangles) {
    if (!gi_provisioned_ || triangles.empty())
        return 0;
    if (triangles.size() > gi_max_triangles - voxel_triangle_count_)
        fatal("GI mesh cache exceeds 1 million triangles; use --gi off");
    const auto first = voxel_triangle_count_;
    if (asset_upload_.recording())
        memory_barrier(asset_upload_.command_buffer(), compute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    upload_to_device_buffer(voxel_triangles_, std::as_bytes(triangles), sizeof(GiTriangle) * first);
    voxel_triangle_count_ += static_cast<std::uint32_t>(triangles.size());
    lighting_statistics_.mesh_bytes = sizeof(GiTriangle) * voxel_triangle_count_;
    return first;
}
std::size_t Renderer::gi_model_upload_bytes(const PreparedModel &prepared) const {
    if (!gi_provisioned_ || !prepared.model)
        return 0;
    std::uint64_t triangles = 0;
    for (const auto &submesh : prepared.model->submeshes)
        triangles += submesh.index_count / 3;
    if (triangles > gi_max_triangles - voxel_triangle_count_)
        fatal("GI mesh cache exceeds 1 million triangles; use --gi off");
    // Every submesh is staged separately; its alignment gap also consumes the arena.
    const auto bytes = triangles * sizeof(GiTriangle) +
                       prepared.model->submeshes.size() * (UploadBatch::alignment - 1);
    if (prepared.upload_bytes + bytes > 512ULL * 1024 * 1024)
        fatal("GI asset upload exceeds the 512 MiB batch budget");
    return static_cast<std::size_t>(bytes);
}
void Renderer::snapshot_voxel_scene() {
    voxel_scene_.begin_snapshot();
    const auto append = [&](std::uint32_t first, std::uint32_t count, std::uint32_t record,
                            glm::vec3 albedo = glm::vec3(1)) {
        if (!count)
            return;
        const auto &metadata = scene_metadata_[record];
        const auto &instance = editor_scene_instances_[record];
        const auto transform =
            metadata.id
                ? runtime_transforms_.current(metadata.id)
                : TransformRows{instance.model_row_0, instance.model_row_1, instance.model_row_2};
        voxel_scene_.append({metadata.id, first, count, transform, metadata.local_bounds,
                             instance.material.base_color *
                                 glm::vec4(albedo * (1 - instance.material.metallic), 1)});
    };
    for (unsigned i = 0; i < editor_ground_count_; ++i)
        append(gi_plane_first_, 2, i);
    for (unsigned i = 0; i < editor_cube_count_; ++i)
        append(gi_cube_first_, 12, editor_ground_count_ + i);
    for (const auto &draw : editor_model_draws_) {
        const auto &submesh = editor_model_assets_[draw.asset_index].submeshes[draw.submesh_index];
        for (unsigned i = 0; i < draw.instance_count; ++i)
            append(submesh.gi_first, submesh.gi_count, draw.first_instance + i, submesh.gi_albedo);
    }
    if (editor_terrain_visible_)
        voxel_scene_.append({{},
                             gi_terrain_first_,
                             gi_terrain_count_,
                             {},
                             terrain_bounds_,
                             glm::vec4(0.22F, 0.35F, 0.09F, 1)});
    voxel_scene_.finish_snapshot(voxel_dirty_);
}
void Renderer::start_voxel_job() {
    voxel_job_mask_ = voxel_dirty_.take();
    voxel_jobs_.clear();
    glm::vec3 low(1e30F), high(-1e30F);
    for (unsigned i = 0; i < gi_brick_count; ++i)
        if (voxel_job_mask_[i]) {
            const glm::vec3 p = voxel_dirty_.origin() +
                                glm::vec3(i % 8, (i / 8) % 8, i / 64) * (8 * voxel_dirty_.cell());
            low = glm::min(low, p);
            high = glm::max(high, p + glm::vec3(8 * voxel_dirty_.cell()));
        }
    const Bounds affected{(low + high) * 0.5F, (high - low) * 0.5F};
    const auto bounds = voxel_scene_.world_bounds();
    const auto surfaces = voxel_scene_.surfaces();
    for (std::size_t index = 0; index < bounds.size(); ++index)
        if (intersects(affected, bounds[index])) {
            const auto &surface = surfaces[index];
            voxel_jobs_.push_back(
                {surface.transform, surface.color, {surface.first, surface.count, 0, 0}});
        }
    gi_geometry_elapsed_ = 0;
    gi_geometry_urgent_ = false;
    voxel_job_index_ = 0;
    voxel_job_triangle_ = 0;
    voxel_job_start_ = true;
    voxel_job_active_ = true;
}
void Renderer::prepare_lighting(FrameContext &frame, const glm::mat4 &view, VkExtent2D extent) {
    if (!lighting_settings_.dynamic) {
        voxel_light_due_ = false;
        voxel_job_finishes_ = false;
        if (frame.lighting_was_dynamic) {
            lighting_uniforms_.options = {};
            frame.lighting_uniform_buffer.write(
                std::as_bytes(std::span{&lighting_uniforms_, std::size_t{1}}));
            frame.lighting_was_dynamic = false;
        }
        return;
    }
    frame.lighting_was_dynamic = true;
    const double dt = std::max(0.0, static_cast<double>(fsr_frame_time_ms_) * 0.001);
    advance_lighting_clock(lighting_settings_, dt, lighting_paused_);
    const auto sun = evaluate_sun(lighting_settings_.hour);
    auto &u = lighting_uniforms_;
    u.sun_direction_intensity = sun.direction_intensity;
    u.sun_color = sun.color;
    u.sky_zenith = sun.sky_zenith;
    u.sky_horizon = sun.sky_horizon;
    u.camera_view = view;
    if (authored_scene_enabled_ && !editor_sun_visible_)
        u.sun_direction_intensity.w = 0;
    const double shadow_hour = std::floor(lighting_settings_.hour * 3600.0) / 3600.0;
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    if (mode_ != RunMode::instancing &&
        (!lighting_shadow_initialized_ || lighting_shadow_view_ != view ||
         lighting_shadow_hour_ != shadow_hour || lighting_shadow_aspect_ != aspect)) {
        // One in-game second bounds solar stepping to a much smaller angle than
        // the old 15-second interval; paused lighting reuses the exact same atlas.
        const auto shadow_sun = evaluate_sun(shadow_hour);
        const auto tile_resolution = shadow_map_.width() / 2;
        const auto matrices = shadow_cascades(view, aspect, glm::vec3(shadow_sun.direction_intensity),
                                               tile_resolution);
        if (!lighting_shadow_initialized_ || matrices != u.shadow_matrices) {
            u.shadow_matrices = matrices;
            for (unsigned i = 0; i < matrices.size(); ++i) {
                const auto &matrix = matrices[i];
                // Convert a world-space shadow texel to depth once per matrix
                // update, instead of multiplying two uniform arrays per fragment.
                u.shadow_texel_depth[static_cast<int>(i)] =
                    2.0F * glm::length(glm::vec3(matrix[0][2], matrix[1][2], matrix[2][2])) /
                    (static_cast<float>(tile_resolution) *
                     glm::length(glm::vec3(matrix[0][0], matrix[1][0], matrix[2][0])));
            }
            shadow_dirty_ = true;
        }
        lighting_shadow_initialized_ = true;
        lighting_shadow_view_ = view;
        lighting_shadow_hour_ = shadow_hour;
        lighting_shadow_aspect_ = aspect;
    }
    gi_light_elapsed_ += dt;
    gi_geometry_elapsed_ += dt;
    voxel_batch_.clear();
    voxel_batch_triangles_ = 0;
    voxel_job_finishes_ = false;
    voxel_light_due_ = false;
    if (gi_provisioned_ && lighting_settings_.voxel_gi && authored_scene_enabled_ &&
        mode_ != RunMode::instancing) {
        const float cell = lighting_settings_.gi_extent / gi_resolution;
        const glm::vec3 camera = glm::vec3(glm::inverse(view)[3]);
        const glm::vec3 center =
            voxel_dirty_.origin() + glm::vec3(lighting_settings_.gi_extent * 0.5F);
        if (gi_scene_revision_ == 0 || cell != voxel_dirty_.cell() ||
            glm::any(glm::greaterThan(glm::abs(camera - center),
                                      glm::vec3(lighting_settings_.gi_extent * 0.25F)))) {
            const auto origin = glm::floor(camera / (8 * cell)) * (8 * cell) -
                                glm::vec3(lighting_settings_.gi_extent * 0.5F);
            voxel_dirty_.reset(origin, cell);
            voxel_job_active_ = false;
            voxel_ready_ = false;
        }
        const bool structural_change = gi_scene_revision_ != editor_scene_revision_;
        if (structural_change) {
            snapshot_voxel_scene();
            gi_scene_revision_ = editor_scene_revision_;
            gi_geometry_urgent_ = true;
        }
        if (voxel_dirty_.count() && gi_dirty_since_.time_since_epoch().count() == 0)
            gi_dirty_since_ = std::chrono::steady_clock::now();
        // Coalesce continuous movement at the GI rate. Topology edits and a new
        // camera volume are urgent; an already started batch always makes progress.
        if (!voxel_job_active_ && voxel_dirty_.count() &&
            (!voxel_ready_ || gi_geometry_urgent_ ||
             gi_geometry_elapsed_ >= 1.0 / lighting_settings_.gi_update_hz))
            start_voxel_job();
        if (voxel_job_active_) {
            auto budget = lighting_settings_.triangle_budget;
            while (voxel_job_index_ < voxel_jobs_.size() && budget) {
                auto job = voxel_jobs_[voxel_job_index_];
                const auto count = std::min(budget, job.range.y - voxel_job_triangle_);
                const auto total = job.range.y;
                job.range.x += voxel_job_triangle_;
                job.range.y = count;
                job.range.z = voxel_batch_triangles_;
                voxel_batch_.push_back(job);
                voxel_batch_triangles_ += count;
                budget -= count;
                voxel_job_triangle_ += count;
                if (voxel_job_triangle_ == total) {
                    ++voxel_job_index_;
                    voxel_job_triangle_ = 0;
                }
            }
            voxel_job_finishes_ = voxel_job_index_ == voxel_jobs_.size();
            if (!voxel_batch_.empty())
                frame.voxel_job_buffer.write(std::as_bytes(std::span{voxel_batch_}));
            frame.voxel_dirty_buffer.write(std::as_bytes(std::span{voxel_job_mask_}));
        }
        const bool sun_changed =
            gi_last_sun_.w != u.sun_direction_intensity.w ||
            (u.sun_direction_intensity.w > 0 &&
             (gi_last_sun_ != u.sun_direction_intensity || gi_last_sun_color_ != u.sun_color));
        voxel_light_due_ =
            voxel_job_finishes_ ||
            (!voxel_job_active_ &&
             (!voxel_ready_ ||
              (sun_changed && gi_light_elapsed_ >= 1.0 / lighting_settings_.gi_update_hz)));
        lighting_statistics_.pending_bricks = static_cast<std::uint32_t>(voxel_dirty_.count());
        if (voxel_job_active_)
            lighting_statistics_.pending_bricks += static_cast<std::uint32_t>(
                std::count(voxel_job_mask_.begin(), voxel_job_mask_.end(), 1U));
        lighting_statistics_.processed_triangles = voxel_batch_triangles_;
        lighting_statistics_.oldest_geometry_ms =
            gi_dirty_since_.time_since_epoch().count() == 0
                ? 0
                : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                            gi_dirty_since_)
                      .count();
    }
    u.volume_origin_cell = glm::vec4(voxel_dirty_.origin(), voxel_dirty_.cell());
    u.volume_parameters = glm::vec4(lighting_settings_.gi_extent, gi_resolution, 0, 0);
    u.options = glm::vec4(lighting_settings_.dynamic ? 1 : 0,
                          lighting_settings_.voxel_gi && (voxel_ready_ || voxel_job_finishes_) &&
                                  mode_ != RunMode::instancing
                              ? 1
                              : 0,
                          lighting_settings_.gi_strength, 1);
    frame.lighting_uniform_buffer.write(std::as_bytes(std::span{&u, std::size_t{1}}));
}
void Renderer::record_voxel_geometry(VkCommandBuffer command, FrameContext &frame) {
    frame.voxel_geometry_was_updated = gi_provisioned_ && voxel_job_active_ &&
                                       lighting_settings_.voxel_gi && mode_ != RunMode::instancing;
    if (!frame.voxel_geometry_was_updated)
        return;
    memory_barrier(command, compute, storage_rw, compute, storage_rw);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, voxel_geometry_pipeline_.get());
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, lighting_compute_layout_.get(),
                            0, 1, &frame.lighting_descriptor_set, 0, nullptr);
    if (voxel_job_start_) {
        const glm::uvec4 push(0, 0, 0, 0);
        vkCmdPushConstants(command, lighting_compute_layout_.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           16, &push);
        vkCmdDispatch(command, 4096, 1, 1);
        memory_barrier(command, compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, compute, storage_rw);
        voxel_job_start_ = false;
    }
    if (voxel_batch_triangles_) {
        const glm::uvec4 push(1, voxel_batch_triangles_, static_cast<unsigned>(voxel_batch_.size()),
                              0);
        vkCmdPushConstants(command, lighting_compute_layout_.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           16, &push);
        vkCmdDispatch(command, (voxel_batch_triangles_ + 63) / 64, 1, 1);
    }
    if (voxel_job_finishes_) {
        voxel_job_active_ = false;
        ++lighting_statistics_.geometry_updates;
        if (!voxel_dirty_.count())
            gi_dirty_since_ = {};
    }
}
void Renderer::record_voxel_light(VkCommandBuffer command, FrameContext &frame) {
    frame.voxel_light_was_updated = voxel_light_due_ && gi_provisioned_ &&
                                    lighting_settings_.voxel_gi && mode_ != RunMode::instancing;
    if (voxel_radiance_layout_ == VK_IMAGE_LAYOUT_UNDEFINED) {
        transition_image(command, voxel_radiance_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, voxel_radiance_.mip_levels());
        const VkClearColorValue clear{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                            voxel_radiance_.mip_levels(), 0, 1};
        vkCmdClearColorImage(command, voxel_radiance_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);
        transition_image(command, voxel_radiance_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, voxel_radiance_.mip_levels());
        voxel_radiance_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if (!frame.voxel_light_was_updated)
        return;
    memory_barrier(command, compute, storage_rw, compute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    transition_image(command, voxel_radiance_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, 1);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, voxel_light_pipeline_.get());
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, lighting_compute_layout_.get(),
                            0, 1, &frame.lighting_descriptor_set, 0, nullptr);
    vkCmdDispatch(command, 4096, 1, 1);
    transition_image(command, voxel_radiance_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, compute,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1);
    unsigned size = gi_resolution;
    for (unsigned level = 1; level < 7; ++level) {
        transition_image(
            command, voxel_radiance_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, level, 1);
        const unsigned next = size / 2;
        const VkImageBlit region{
            {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1},
            {{0, 0, 0}, {static_cast<int>(size), static_cast<int>(size), static_cast<int>(size)}},
            {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
            {{0, 0, 0}, {static_cast<int>(next), static_cast<int>(next), static_cast<int>(next)}}};
        vkCmdBlitImage(command, voxel_radiance_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       voxel_radiance_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                       VK_FILTER_LINEAR);
        transition_image(command, voxel_radiance_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, level, 1);
        size = next;
    }
    transition_image(command, voxel_radiance_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     0, 7);
    gi_last_sun_ = lighting_uniforms_.sun_direction_intensity;
    gi_last_sun_color_ = lighting_uniforms_.sun_color;
    voxel_ready_ = true;
    gi_light_elapsed_ = 0;
    ++lighting_statistics_.radiance_updates;
}
void Renderer::destroy_lighting() noexcept {
    voxel_geometry_pipeline_.reset();
    voxel_light_pipeline_.reset();
    lighting_compute_layout_.reset();
    lighting_pool_.reset();
    lighting_layout_.reset();
    voxel_storage_view_.reset();
    voxel_radiance_.destroy();
    voxel_geometry_.destroy();
    voxel_triangles_.destroy();
    for (auto &frame : frames_) {
        frame.lighting_uniform_buffer.destroy();
        frame.voxel_job_buffer.destroy();
        frame.voxel_dirty_buffer.destroy();
    }
    voxel_jobs_.clear();
    voxel_batch_.clear();
    voxel_scene_.clear();
    gi_provisioned_ = false;
    voxel_ready_ = false;
    voxel_job_active_ = false;
    voxel_radiance_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    voxel_triangle_count_ = 0;
    gi_scene_revision_ = 0;
    gi_geometry_urgent_ = false;
    lighting_shadow_initialized_ = false;
}
} // namespace vcr
