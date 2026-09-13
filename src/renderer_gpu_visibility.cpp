#include "renderer.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

namespace vcr {
namespace {
constexpr std::size_t source_capacity = max_scene_nodes + 1ULL,
                      group_capacity = max_scene_nodes + 2ULL;
constexpr VkDeviceSize metadata_offset = 0;
constexpr VkDeviceSize groups_offset = source_capacity * sizeof(VisibilitySource);
constexpr VkDeviceSize commands_offset = groups_offset + group_capacity * sizeof(VisibilityGroup);
constexpr VkDeviceSize command_capacity = group_capacity * 3 * sizeof(VkDrawIndexedIndirectCommand);
void barrier(VkCommandBuffer command, VkBuffer buffer, VkPipelineStageFlags2 src,
             VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
    const VkBufferMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                   .srcStageMask = src,
                                   .srcAccessMask = src_access,
                                   .dstStageMask = dst,
                                   .dstAccessMask = dst_access,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .buffer = buffer,
                                   .offset = 0,
                                   .size = VK_WHOLE_SIZE};
    const VkDependencyInfo info{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .bufferMemoryBarrierCount = 1,
                                .pBufferMemoryBarriers = &b};
    vkCmdPipelineBarrier2(command, &info);
}
} // namespace
void Renderer::create_gpu_visibility() {
    if (!authored_scene_enabled_) {
        return;
    }
    // CPU visibility uses the same vertex indirection without requiring the
    // optional compute/indirect-draw features below.
    for (auto &frame : frames_) {
        frame.visibility.visible.create(*context_, 32 + source_capacity * 3 * 4,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "frame visible instance indices");
    }
    const auto &limits = context_->properties().limits;
    if (!context_->indirect_first_instance() || limits.maxPerStageDescriptorStorageBuffers < 6 ||
        limits.maxDescriptorSetStorageBuffers < 6 || limits.maxComputeWorkGroupInvocations < 128 ||
        limits.maxComputeWorkGroupSize[0] < 128 ||
        limits.maxComputeWorkGroupCount[0] < (source_capacity + 127) / 128) {
        return;
    }
    visibility_sources_.reserve(source_capacity);
    visibility_groups_.reserve(group_capacity);
    visibility_commands_.reserve(group_capacity * 3);
    record_copies_.reserve(source_capacity);
    node_record_heads_.resize(max_scene_nodes, UINT32_MAX);
    node_record_next_.reserve(source_capacity);
    const auto device_buffer = [this](GpuBuffer &buffer, VkDeviceSize size,
                                      VkBufferUsageFlags usage, const char *name) {
        buffer.create(*context_, size, usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, name);
    };
    const auto mapped_buffer = [this](GpuBuffer &buffer, VkDeviceSize size,
                                      VkBufferUsageFlags usage, const char *name,
                                      bool readback = false) {
        buffer.create(*context_, size, usage, VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_MAPPED_BIT |
                          (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                    : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT),
                      name);
    };
    constexpr auto storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    device_buffer(visibility_lod_history_, source_capacity * 16, storage, "visibility LOD history");
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    for (unsigned i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       .descriptorCount = 1,
                       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    const VkDescriptorSetLayoutCreateInfo layout{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data()};
    VK_CHECK(vkCreateDescriptorSetLayout(
        context_->device(), &layout, nullptr,
        visibility_layout_.put(context_->device(), vkDestroyDescriptorSetLayout)));
    const auto set_layout = visibility_layout_.get();
    const VkPipelineLayoutCreateInfo pipeline_layout{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout};
    VK_CHECK(vkCreatePipelineLayout(
        context_->device(), &pipeline_layout, nullptr,
        visibility_pipeline_layout_.put(context_->device(), vkDestroyPipelineLayout)));
    const auto shader = load_shader("scene_visibility.comp.spv");
    const VkComputePipelineCreateInfo pipeline{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader.get(),
                  .pName = "main"},
        .layout = visibility_pipeline_layout_.get()};
    VK_CHECK(
        vkCreateComputePipelines(context_->device(), pipeline_cache_, 1, &pipeline, nullptr,
                                 visibility_pipeline_.put(context_->device(), vkDestroyPipeline)));
    const std::array pools{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, max_frames_in_flight},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, max_frames_in_flight * 6}};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = max_frames_in_flight,
                                          .poolSizeCount = 2,
                                          .pPoolSizes = pools.data()};
    VK_CHECK(
        vkCreateDescriptorPool(context_->device(), &pool, nullptr,
                               visibility_pool_.put(context_->device(), vkDestroyDescriptorPool)));
    for (auto &frame : frames_) {
        auto &f = frame.visibility;
        device_buffer(f.indirect, command_capacity,
                      storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      "GPU indirect draws");
        device_buffer(f.metadata, groups_offset, storage, "GPU visibility source bounds");
        device_buffer(f.groups, group_capacity * sizeof(VisibilityGroup), storage,
                      "GPU visibility draw groups");
        mapped_buffer(f.uniform, sizeof(VisibilityUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      "GPU visibility uniforms");
        mapped_buffer(f.staging, commands_offset + command_capacity,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "GPU visibility static upload");
        mapped_buffer(f.readback, 32 + command_capacity, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      "GPU visibility statistics", true);
        f.dirty_records.reserve(source_capacity);
        f.dirty_flags.resize(source_capacity, 0);
        const VkDescriptorSetAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = visibility_pool_.get(),
            .descriptorSetCount = 1,
            .pSetLayouts = &set_layout};
        VK_CHECK(vkAllocateDescriptorSets(context_->device(), &allocate, &f.descriptor));
        context_->set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET,
                                 reinterpret_cast<std::uint64_t>(f.descriptor),
                                 "frame GPU visibility descriptors");
        std::array<VkDescriptorBufferInfo, 7> infos{};
        const std::array buffers{std::cref(f.uniform),
                                 std::cref(frame.scene_instance_buffer),
                                 std::cref(f.metadata),
                                 std::cref(f.groups),
                                 std::cref(f.visible),
                                 std::cref(f.indirect),
                                 std::cref(visibility_lod_history_)};
        std::array<VkWriteDescriptorSet, 7> writes{};
        for (unsigned i = 0; i < infos.size(); ++i) {
            infos[i] = {buffers[i].get().handle(), 0, buffers[i].get().size()};
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         .dstSet = f.descriptor,
                         .dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = bindings[i].descriptorType,
                         .pBufferInfo = &infos[i]};
        }
        vkUpdateDescriptorSets(context_->device(), static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    context_->set_debug_name(VK_OBJECT_TYPE_PIPELINE,
                             reinterpret_cast<std::uint64_t>(visibility_pipeline_.get()),
                             "GPU visibility and LOD");
    context_->set_debug_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                             reinterpret_cast<std::uint64_t>(visibility_pipeline_layout_.get()),
                             "GPU visibility pipeline layout");
    context_->set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                             reinterpret_cast<std::uint64_t>(visibility_layout_.get()),
                             "GPU visibility descriptor layout");
    context_->set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_POOL,
                             reinterpret_cast<std::uint64_t>(visibility_pool_.get()),
                             "GPU visibility descriptor pool");
}
void Renderer::destroy_gpu_visibility() noexcept {
    visibility_pipeline_.reset();
    visibility_pipeline_layout_.reset();
    visibility_pool_.reset();
    visibility_layout_.reset();
    for (auto &frame : frames_) {
        auto &f = frame.visibility;
        f.visible.destroy();
        f.indirect.destroy();
        f.metadata.destroy();
        f.groups.destroy();
        f.uniform.destroy();
        f.staging.destroy();
        f.readback.destroy();
        f.descriptor = VK_NULL_HANDLE;
    }
    visibility_lod_history_.destroy();
    gpu_visibility_active_ = false;
}
void Renderer::build_gpu_visibility_scene(std::uint32_t plane_count, std::uint32_t plane_first,
                                          std::int32_t plane_vertex, std::uint32_t cube_count) {
    if (visibility_pipeline_.get() == VK_NULL_HANDLE) {
        return;
    }
    visibility_sources_.resize(scene_metadata_.size());
    visibility_groups_.clear();
    visibility_commands_.clear();
    const auto count = static_cast<std::uint32_t>(scene_metadata_.size());
    const auto group = [&](std::uint32_t first, std::uint32_t instances, glm::uvec4 counts,
                           glm::uvec4 firsts, glm::vec4 errors) {
        const auto g = static_cast<std::uint32_t>(visibility_groups_.size());
        visibility_groups_.push_back({{first, instances, 0, 0}, counts, firsts, errors});
        for (auto i = first; i < first + instances; ++i) {
            const auto &bounds = scene_metadata_[i].local_bounds;
            visibility_sources_[i] = {{bounds.center, float(g)}, {bounds.extent, 0}};
        }
        for (glm::length_t lod = 0; lod < 3; ++lod) {
            visibility_commands_.push_back({counts[lod], 0, firsts[lod],
                                            static_cast<std::int32_t>(counts.w),
                                            first + static_cast<unsigned>(lod) * count});
        }
    };
    group(0, editor_ground_count_, {plane_count, 0, 0, static_cast<unsigned>(plane_vertex)},
          {plane_first, 0, 0, 0}, {});
    group(editor_ground_count_, editor_cube_count_, {cube_count, 0, 0, 0}, {}, {});
    for (const auto &draw : editor_model_draws_) {
        const auto &mesh = editor_model_assets_[draw.asset_index].submeshes[draw.submesh_index];
        group(draw.first_instance, draw.instance_count,
              {mesh.lod_count[0], mesh.lod_count[1], mesh.lod_count[2], 0},
              {mesh.lod_first[0], mesh.lod_first[1], mesh.lod_first[2], 0},
              {mesh.lod_error[0], mesh.lod_error[1], mesh.lod_error[2], 0});
    }
    std::fill(node_record_heads_.begin(), node_record_heads_.end(), UINT32_MAX);
    node_record_next_.assign(count, UINT32_MAX);
    std::array<std::pair<float, std::uint32_t>, 64> candidates{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto id = scene_metadata_[i].id;
        if (id && id.index < max_scene_nodes) {
            node_record_next_[i] = node_record_heads_[id.index];
            node_record_heads_[id.index] = i;
        }
        if (scene_metadata_[i].occluder) {
            const auto &instance = editor_scene_instances_[i];
            const auto bounds =
                transform_bounds(scene_metadata_[i].local_bounds, instance.model_row_0,
                                 instance.model_row_1, instance.model_row_2);
            const float score = glm::length(bounds.extent);
            const auto lowest = std::min_element(candidates.begin(), candidates.end());
            if (score > lowest->first) {
                *lowest = {score, i};
            }
        }
    }
    occluder_candidate_count_ = 0;
    for (const auto &[score, index] : candidates) {
        if (score > 0) {
            occluder_candidates_[occluder_candidate_count_++] = index;
        }
    }
    ++gpu_scene_revision_;
    gpu_packet_revision_ = 0;
    reset_lod_history_ = true;
}
void Renderer::prepare_gpu_scene_packet(const glm::mat4 &vp, glm::vec3 camera, VkExtent2D extent) {
    const bool fade_enabled = lod_fade_pattern_ != LodFadePattern::off && visibility_settings_.lod &&
                              !editor_model_draws_.empty();
    // Optional fading advances without a camera change. It never requires
    // synchronous readback of the active-transition count.
    const bool changed = fade_enabled || reset_lod_history_ || gpu_packet_revision_ != gpu_scene_revision_ ||
                         packet_document_revision_ != editor_scene_revision_ ||
                         packet_runtime_revision_ != runtime_transforms_.revision() ||
                         packet_extent_.width != extent.width ||
                         packet_extent_.height != extent.height || vp != packet_view_projection_;
    if (changed) {
        ++gpu_visibility_revision_;
    }
    if (!changed && frames_[frame_index_].visibility.view_revision == gpu_visibility_revision_) {
        return;
    }
    const auto update_record = [&](std::uint32_t i) {
        auto current = editor_scene_instances_[i];
        const auto id = scene_metadata_[i].id;
        TransformRows previous{current.model_row_0, current.model_row_1, current.model_row_2};
        if (runtime_transforms_.contains(id)) {
            const auto transform = runtime_transforms_.current(id);
            previous = runtime_transforms_.previous(id);
            current.model_row_0 = transform.x;
            current.model_row_1 = transform.y;
            current.model_row_2 = transform.z;
        }
        current.material.reserved = static_cast<float>(id ? id.index : i);
        scene_packet_[i] = {current, previous};
        for (auto &frame : frames_) {
            auto &f = frame.visibility;
            if (!f.dirty_flags[i]) {
                f.dirty_flags[i] = 1;
                f.dirty_records.push_back(i);
            }
        }
    };
    if (gpu_packet_revision_ != gpu_scene_revision_) {
        scene_packet_.resize(scene_metadata_.size());
        for (auto &frame : frames_) {
            frame.scene_revision = 0;
        }
        for (std::uint32_t i = 0; i < scene_metadata_.size(); ++i) {
            update_record(i);
        }
        gpu_packet_revision_ = gpu_scene_revision_;
        ++packet_revision_;
    } else if (!runtime_transforms_.dirty_indices().empty()) {
        for (auto node : runtime_transforms_.dirty_indices()) {
            for (auto i = node_record_heads_[node]; i != UINT32_MAX; i = node_record_next_[i]) {
                update_record(i);
            }
        }
        ++packet_revision_;
    }
    const auto guard = glm::vec2(2) / glm::vec2(extent.width, extent.height);
    auto padded = vp;
    for (int c = 0; c < 4; ++c) {
        padded[c][0] *= 1 - guard.x;
        padded[c][1] *= 1 - guard.y;
    }
    const ViewFrustum frustum(padded);
    OcclusionGrid grid;
    grid.clear();
    if (visibility_settings_.occlusion) {
        std::array<std::pair<float, std::uint32_t>, 16> best{};
        for (unsigned c = 0; c < occluder_candidate_count_; ++c) {
            const auto i = occluder_candidates_[c];
            const auto &instance = scene_packet_[i].current;
            const auto bounds =
                transform_bounds(scene_metadata_[i].local_bounds, instance.model_row_0,
                                 instance.model_row_1, instance.model_row_2);
            if (!frustum.intersects(bounds)) {
                continue;
            }
            const float score =
                glm::length(bounds.extent) / std::max(glm::length(bounds.center - camera), .01F);
            const auto lowest = std::min_element(best.begin(), best.end());
            if (score > .05F && score > lowest->first) {
                *lowest = {score, i};
            }
        }
        for (const auto &[score, i] : best) {
            if (score == 0) {
                continue;
            }
            const auto &instance = scene_packet_[i].current;
            auto corners = bounds_corners(scene_metadata_[i].local_bounds);
            for (auto &p : corners) {
                const glm::vec4 point(p, 1);
                p = {glm::dot(instance.model_row_0, point), glm::dot(instance.model_row_1, point),
                     glm::dot(instance.model_row_2, point)};
            }
            grid.add_box(corners, padded, guard);
        }
    }
    const float pixels = .5F * float(extent.height) / std::tan(glm::radians(32.5F));
    const bool occlusion_enabled = visibility_settings_.occlusion && grid.has_coverage();
    const bool automatic_cubes =
        visibility_settings_.backend == VisibilitySettings::Backend::automatic &&
        editor_model_draws_.empty();
    if (!automatic_cubes) {
        gpu_visibility_bypassed_ = false;
    }
    if (automatic_cubes) {
        // For cheap cubes, indirect indexing costs more than modest rejection.
        // A bounded sample only chooses to DRAW EXTRA instances; it never
        // decides visibility. Unsuitable predictions cannot create holes.
        constexpr std::uint32_t samples = 64;
        std::uint32_t retained = 0;
        for (std::uint32_t sample = 0; sample < samples; ++sample) {
            const auto i = static_cast<std::size_t>((std::uint64_t(sample) * 2654435761ULL) %
                                                    scene_packet_.size());
            const auto &instance = scene_packet_[i].current;
            const auto bounds =
                transform_bounds(scene_metadata_[i].local_bounds, instance.model_row_0,
                                 instance.model_row_1, instance.model_row_2);
            if ((!visibility_settings_.frustum || frustum.intersects(bounds)) &&
                (!occlusion_enabled || !grid.occluded(bounds, padded, guard))) {
                ++retained;
            }
        }
        gpu_visibility_bypassed_ = retained > samples / 2;
    }
    if (automatic_cubes && gpu_visibility_bypassed_) {
        auto &stats = visibility_statistics_;
        stats = {};
        stats.submitted = static_cast<std::uint32_t>(scene_metadata_.size());
        stats.visible = stats.submitted;
        stats.lod_instances[0] = stats.submitted;
        stats.triangles =
            std::uint64_t(editor_ground_count_) * 2 + std::uint64_t(editor_cube_count_) * 12;
        stats.packet_revision = packet_revision_;
    }
    const VisibilityUniforms uniform{padded,
                                     frustum.planes(),
                                     {camera, pixels},
                                     {guard, visibility_settings_.lod_pixel_error,
                                      fade_enabled ? std::clamp(fsr_frame_time_ms_ * 0.001F / lod_fade_seconds_, 0.0F, 1.0F) : 0.0F},
                                     {static_cast<unsigned>(scene_metadata_.size()),
                                      visibility_settings_.frustum, occlusion_enabled,
                                      unsigned(visibility_settings_.lod) |
                                        (fade_enabled ? (unsigned(lod_fade_pattern_) << 1U) : 0U)},
                                     grid.depths()};
    if (!gpu_visibility_bypassed_) {
        frames_[frame_index_].visibility.uniform.write(std::as_bytes(std::span{&uniform, 1}));
    }
    const auto terrain_projection =
        lod_projection(terrain_bounds_, vp, {extent.width, extent.height});
    terrain_lod_ = visibility_settings_.lod
                       ? select_lod(terrain_lod_error_, 1, terrain_projection.nearest_depth,
                                    terrain_projection.pixels, terrain_lod_,
                                    visibility_settings_.lod_pixel_error)
                       : 0;
    packet_view_projection_ = vp;
    packet_extent_ = extent;
    packet_runtime_revision_ = runtime_transforms_.revision();
    packet_document_revision_ = editor_scene_revision_;
}
void Renderer::upload_gpu_scene_records(VkCommandBuffer command, FrameContext &frame) {
    auto &f = frame.visibility;
    record_copies_.clear();
    if (frame.scene_revision == 0) {
        const auto bytes = std::as_bytes(std::span{scene_packet_});
        if (!bytes.empty()) {
            frame.scene_upload_buffer.write(bytes);
            record_copies_.push_back({0, 0, bytes.size()});
        }
    } else {
        for (auto i : f.dirty_records) {
            const auto offset = VkDeviceSize(i) * sizeof(RuntimeGpuInstance);
            frame.scene_upload_buffer.write(std::as_bytes(std::span{&scene_packet_[i], 1}), offset);
            record_copies_.push_back({offset, offset, sizeof(RuntimeGpuInstance)});
        }
    }
    if (!record_copies_.empty()) {
        vkCmdCopyBuffer(command, frame.scene_upload_buffer.handle(),
                        frame.scene_instance_buffer.handle(),
                        static_cast<unsigned>(record_copies_.size()), record_copies_.data());
        barrier(command, frame.scene_instance_buffer.handle(), VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    for (auto i : f.dirty_records) {
        f.dirty_flags[i] = 0;
    }
    f.dirty_records.clear();
    frame.scene_revision = packet_revision_;
    frame.scene_source_revision = 0; // GPU dirty-record uploads own this version.
}
void Renderer::record_gpu_visibility(VkCommandBuffer command, FrameContext &frame) {
    if (!gpu_visibility_active_ || mode_ == RunMode::instancing) {
        return;
    }
    auto &f = frame.visibility;
    if (gpu_visibility_bypassed_) {
        f.view_revision = gpu_visibility_revision_;
        f.statistics_valid = false;
        return;
    }
    if (f.view_revision == gpu_visibility_revision_) {
        return;
    }
    const auto commands_bytes = visibility_commands_.size() * sizeof(VkDrawIndexedIndirectCommand);
    if (f.scene_revision != gpu_scene_revision_) {
        f.staging.write(std::as_bytes(std::span{visibility_sources_}), metadata_offset);
        f.staging.write(std::as_bytes(std::span{visibility_groups_}), groups_offset);
        f.staging.write(std::as_bytes(std::span{visibility_commands_}), commands_offset);
        const VkBufferCopy metadata{metadata_offset, 0,
                                    visibility_sources_.size() * sizeof(VisibilitySource)};
        const VkBufferCopy groups{groups_offset, 0,
                                  visibility_groups_.size() * sizeof(VisibilityGroup)};
        if (metadata.size) {
            vkCmdCopyBuffer(command, f.staging.handle(), f.metadata.handle(), 1, &metadata);
        }
        vkCmdCopyBuffer(command, f.staging.handle(), f.groups.handle(), 1, &groups);
        for (auto buffer : {f.metadata.handle(), f.groups.handle()}) {
            barrier(command, buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        }
        f.scene_revision = gpu_scene_revision_;
    }
    const VkBufferCopy commands{commands_offset, 0, commands_bytes};
    vkCmdCopyBuffer(command, f.staging.handle(), f.indirect.handle(), 1, &commands);
    vkCmdFillBuffer(command, f.visible.handle(), 0, 32, 0);
    for (auto buffer : {f.indirect.handle(), f.visible.handle()}) {
        barrier(command, buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    barrier(command, visibility_lod_history_.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            reset_lod_history_ ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
                               : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            reset_lod_history_
                ? VK_ACCESS_2_TRANSFER_WRITE_BIT
                : VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (reset_lod_history_) {
        vkCmdFillBuffer(command, visibility_lod_history_.handle(), 0, VK_WHOLE_SIZE, 0);
        barrier(command, visibility_lod_history_.handle(), VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        reset_lod_history_ = false;
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, visibility_pipeline_.get());
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            visibility_pipeline_layout_.get(), 0, 1, &f.descriptor, 0, nullptr);
    vkCmdDispatch(command, (static_cast<unsigned>(scene_metadata_.size()) + 127) / 128, 1, 1);
    barrier(command, f.visible.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(command, f.indirect.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferCopy counters{0, 0, 32}, read_commands{0, 32, commands_bytes};
    vkCmdCopyBuffer(command, f.visible.handle(), f.readback.handle(), 1, &counters);
    vkCmdCopyBuffer(command, f.indirect.handle(), f.readback.handle(), 1, &read_commands);
    barrier(command, f.readback.handle(), VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
    f.statistics_valid = true;
    f.statistics_sources = static_cast<unsigned>(scene_metadata_.size());
    f.statistics_groups = static_cast<unsigned>(visibility_groups_.size());
    f.view_revision = gpu_visibility_revision_;
}
void Renderer::read_gpu_visibility(FrameContext &frame) {
    auto &f = frame.visibility;
    if (!f.statistics_valid) {
        return;
    }
    f.statistics_valid = false;
    if (!gpu_visibility_active_ || f.scene_revision != gpu_scene_revision_) {
        return;
    }
    // The frame fence has already completed. No GPU wait or allocation here.
    const auto bytes = f.readback.read_completed_bytes(
        32 + std::size_t(f.statistics_groups) * 3 * sizeof(VkDrawIndexedIndirectCommand));
    std::array<std::uint32_t, 8> counters{};
    std::memcpy(counters.data(), bytes.data(), 32);
    auto &stats = visibility_statistics_;
    stats = {};
    stats.submitted = f.statistics_sources;
    stats.visible = counters[0];
    stats.frustum_rejected = counters[1];
    stats.occlusion_rejected = counters[2];
    stats.lod_instances = {counters[3], counters[4], counters[5]};
    stats.packet_revision = packet_revision_;
    for (unsigned i = 0; i < f.statistics_groups * 3; ++i) {
        VkDrawIndexedIndirectCommand draw;
        std::memcpy(&draw, bytes.data() + 32 + i * sizeof(draw), sizeof(draw));
        stats.triangles += std::uint64_t(draw.indexCount / 3) * draw.instanceCount;
    }
    assert(stats.visible + stats.frustum_rejected + stats.occlusion_rejected == stats.submitted);
}
} // namespace vcr
