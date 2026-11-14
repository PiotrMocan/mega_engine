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
constexpr VkDeviceSize command_capacity =
    group_capacity * model_lod_count * sizeof(VkDrawIndexedIndirectCommand);
constexpr VkDeviceSize shadow_command_capacity =
    VkDeviceSize{max_shadow_draw_groups} * 16ULL * sizeof(VkDrawIndexedIndirectCommand);
constexpr std::size_t visibility_depth_bins = 16ULL;
constexpr std::size_t max_binned_commands =
    std::size_t{max_shadow_draw_groups} * model_lod_count;
constexpr std::size_t bin_counts_uint_offset = 20ULL * source_capacity;
constexpr std::size_t scratch_uint_offset =
    bin_counts_uint_offset + max_binned_commands * visibility_depth_bins;
constexpr VkDeviceSize visible_capacity =
    32ULL +
    (20ULL * source_capacity + max_binned_commands * visibility_depth_bins +
     4ULL * source_capacity) *
        sizeof(std::uint32_t);
static_assert(source_capacity == 100'001ULL,
              "Update kMaxSceneSources in shaders/visibility_bins_common.glsl");
static_assert(max_binned_commands == 4096ULL * 4ULL,
              "Update kMaxBinnedCommands in shaders/visibility_bins_common.glsl");
static_assert(visibility_depth_bins == 16ULL,
              "Update kDepthBins in shaders/visibility_bins_common.glsl");

void barrier(VkCommandBuffer command, VkBuffer buffer, VkPipelineStageFlags2 src,
             VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access,
             VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) {
    const VkBufferMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                   .srcStageMask = src,
                                   .srcAccessMask = src_access,
                                   .dstStageMask = dst,
                                   .dstAccessMask = dst_access,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .buffer = buffer,
                                   .offset = offset,
                                   .size = size};
    const VkDependencyInfo info{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .bufferMemoryBarrierCount = 1,
                                .pBufferMemoryBarriers = &b};
    vkCmdPipelineBarrier2(command, &info);
}

void image_barrier(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect,
                   VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags2 src,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access,
                   std::uint32_t base_mip = 0U,
                   std::uint32_t level_count = VK_REMAINING_MIP_LEVELS) {
    const VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = src,
                                  .srcAccessMask = src_access,
                                  .dstStageMask = dst,
                                  .dstAccessMask = dst_access,
                                  .oldLayout = old_layout,
                                  .newLayout = new_layout,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = {.aspectMask = aspect,
                                                       .baseMipLevel = base_mip,
                                                       .levelCount = level_count,
                                                       .baseArrayLayer = 0U,
                                                       .layerCount = 1U}};
    const VkDependencyInfo info{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = 1U,
                                .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(command, &info);
}

struct HizPushConstants {
    glm::uvec2 src_size{};
    glm::uvec2 dst_size{};
};
static_assert(sizeof(HizPushConstants) == 16);
} // namespace

void Renderer::create_gpu_visibility() {
    if (!authored_scene_enabled_) {
        return;
    }
    // CPU visibility uses the same vertex indirection without requiring the
    // optional compute/indirect-draw features below.
    for (auto &frame : frames_) {
        frame.visibility.visible.create(
            *context_, visible_capacity,
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
    visibility_commands_.reserve(group_capacity * model_lod_count);
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
    hiz_fallback_.create_sampled(*context_, 1U, 1U, VK_FORMAT_R32_SFLOAT, VK_FILTER_NEAREST,
                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, "Hi-Z fallback pyramid");
    hiz_fallback_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
    for (unsigned i = 0; i < 7U; ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       .descriptorCount = 1,
                       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    bindings[7] = {.binding = 7U,
                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   .descriptorCount = 1U,
                   .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
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
    const auto binned_shader = load_shader("scene_visibility_binned.comp.spv");
    const auto prefix_shader = load_shader("scene_visibility_prefix.comp.spv");
    const auto scatter_shader = load_shader("scene_visibility_scatter.comp.spv");
    const VkComputePipelineCreateInfo pipeline{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader.get(),
                  .pName = "main"},
        .layout = visibility_pipeline_layout_.get()};
    const VkComputePipelineCreateInfo binned_pipeline{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = binned_shader.get(),
                  .pName = "main"},
        .layout = visibility_pipeline_layout_.get()};
    const VkComputePipelineCreateInfo prefix_pipeline{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = prefix_shader.get(),
                  .pName = "main"},
        .layout = visibility_pipeline_layout_.get()};
    const VkComputePipelineCreateInfo scatter_pipeline{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = scatter_shader.get(),
                  .pName = "main"},
        .layout = visibility_pipeline_layout_.get()};
    VK_CHECK(
        vkCreateComputePipelines(context_->device(), pipeline_cache_, 1, &pipeline, nullptr,
                                 visibility_pipeline_.put(context_->device(), vkDestroyPipeline)));
    VK_CHECK(vkCreateComputePipelines(
        context_->device(), pipeline_cache_, 1, &binned_pipeline, nullptr,
        visibility_binned_pipeline_.put(context_->device(), vkDestroyPipeline)));
    VK_CHECK(vkCreateComputePipelines(
        context_->device(), pipeline_cache_, 1, &prefix_pipeline, nullptr,
        visibility_prefix_pipeline_.put(context_->device(), vkDestroyPipeline)));
    VK_CHECK(vkCreateComputePipelines(
        context_->device(), pipeline_cache_, 1, &scatter_pipeline, nullptr,
        visibility_scatter_pipeline_.put(context_->device(), vkDestroyPipeline)));

    std::array<VkDescriptorSetLayoutBinding, 6> shadow_bindings{};
    for (unsigned i = 0; i < shadow_bindings.size(); ++i) {
        shadow_bindings[i] = {.binding = i,
                              .descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                       : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              .descriptorCount = 1U,
                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    const VkDescriptorSetLayoutCreateInfo shadow_layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(shadow_bindings.size()),
        .pBindings = shadow_bindings.data()};
    VK_CHECK(vkCreateDescriptorSetLayout(
        context_->device(), &shadow_layout_info, nullptr,
        shadow_visibility_layout_.put(context_->device(), vkDestroyDescriptorSetLayout)));
    const auto shadow_set_layout = shadow_visibility_layout_.get();
    const VkPipelineLayoutCreateInfo shadow_pipeline_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1U,
        .pSetLayouts = &shadow_set_layout};
    VK_CHECK(vkCreatePipelineLayout(
        context_->device(), &shadow_pipeline_layout_info, nullptr,
        shadow_visibility_pipeline_layout_.put(context_->device(), vkDestroyPipelineLayout)));
    const auto shadow_init_shader = load_shader("shadow_init.comp.spv");
    const auto shadow_vis_shader = load_shader("shadow_visibility.comp.spv");
    const std::array<VkComputePipelineCreateInfo, 2> shadow_pipelines{{
        {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = shadow_init_shader.get(),
                   .pName = "main"},
         .layout = shadow_visibility_pipeline_layout_.get()},
        {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = shadow_vis_shader.get(),
                   .pName = "main"},
         .layout = shadow_visibility_pipeline_layout_.get()},
    }};
    std::array<VkPipeline, 2> created_shadow_pipelines{};
    VK_CHECK(vkCreateComputePipelines(
        context_->device(), pipeline_cache_,
        static_cast<std::uint32_t>(shadow_pipelines.size()), shadow_pipelines.data(), nullptr,
        created_shadow_pipelines.data()));
    *shadow_init_pipeline_.put(context_->device(), vkDestroyPipeline) = created_shadow_pipelines[0];
    *shadow_visibility_pipeline_.put(context_->device(), vkDestroyPipeline) =
        created_shadow_pipelines[1];

    if (hiz_supported_) {
        const std::array<VkDescriptorSetLayoutBinding, 2> hiz_bindings{{
            {.binding = 0U,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1U,
             .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 1U,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .descriptorCount = 1U,
             .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        }};
        const VkDescriptorSetLayoutCreateInfo hiz_layout_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<std::uint32_t>(hiz_bindings.size()),
            .pBindings = hiz_bindings.data()};
        VK_CHECK(vkCreateDescriptorSetLayout(
            context_->device(), &hiz_layout_info, nullptr,
            hiz_downsample_layout_.put(context_->device(), vkDestroyDescriptorSetLayout)));
        const auto hiz_set_layout = hiz_downsample_layout_.get();
        const VkPushConstantRange hiz_push{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0U,
            .size = sizeof(HizPushConstants),
        };
        const VkPipelineLayoutCreateInfo hiz_pipe_layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1U,
            .pSetLayouts = &hiz_set_layout,
            .pushConstantRangeCount = 1U,
            .pPushConstantRanges = &hiz_push};
        VK_CHECK(vkCreatePipelineLayout(
            context_->device(), &hiz_pipe_layout_info, nullptr,
            hiz_downsample_pipeline_layout_.put(context_->device(), vkDestroyPipelineLayout)));
        const auto hiz_shader = load_shader("hiz_downsample.comp.spv");
        const VkComputePipelineCreateInfo hiz_pipe_info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = hiz_shader.get(),
                      .pName = "main"},
            .layout = hiz_downsample_pipeline_layout_.get()};
        VK_CHECK(vkCreateComputePipelines(
            context_->device(), pipeline_cache_, 1U, &hiz_pipe_info, nullptr,
            hiz_downsample_pipeline_.put(context_->device(), vkDestroyPipeline)));
        context_->set_debug_name(
            VK_OBJECT_TYPE_PIPELINE,
            reinterpret_cast<std::uint64_t>(hiz_downsample_pipeline_.get()),
            "Hi-Z depth downsample");
        context_->set_debug_name(
            VK_OBJECT_TYPE_PIPELINE_LAYOUT,
            reinterpret_cast<std::uint64_t>(hiz_downsample_pipeline_layout_.get()),
            "Hi-Z downsample pipeline layout");
        context_->set_debug_name(
            VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
            reinterpret_cast<std::uint64_t>(hiz_downsample_layout_.get()),
            "Hi-Z downsample descriptor layout");
    }

    const std::array pools{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, max_frames_in_flight * 2U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, max_frames_in_flight * 11U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_frames_in_flight}};
    const VkDescriptorPoolCreateInfo pool{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = max_frames_in_flight * 2U,
        .poolSizeCount = static_cast<std::uint32_t>(pools.size()),
        .pPoolSizes = pools.data()};
    VK_CHECK(
        vkCreateDescriptorPool(context_->device(), &pool, nullptr,
                               visibility_pool_.put(context_->device(), vkDestroyDescriptorPool)));
    for (auto &frame : frames_) {
        auto &f = frame.visibility;
        device_buffer(f.indirect, command_capacity, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                      "GPU indirect draws");
        device_buffer(f.shadow_indirect, shadow_command_capacity,
                      storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "GPU shadow indirect draws");
        device_buffer(f.metadata, groups_offset, storage, "GPU visibility source bounds");
        device_buffer(f.groups, group_capacity * sizeof(VisibilityGroup), storage,
                      "GPU visibility draw groups");
        mapped_buffer(f.uniform, sizeof(VisibilityUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      "GPU visibility uniforms");
        mapped_buffer(f.shadow_uniform, sizeof(ShadowVisibilityUniforms),
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "GPU shadow visibility uniforms");
        mapped_buffer(f.staging, commands_offset + command_capacity,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "GPU visibility static upload");
        mapped_buffer(f.readback, 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      "GPU visibility statistics", true);
        f.dirty_records.reserve(source_capacity);
        f.dirty_flags.resize(source_capacity, 0);

        const std::array<VkDescriptorSetLayout, 2> frame_layouts{set_layout, shadow_set_layout};
        std::array<VkDescriptorSet, 2> allocated_sets{};
        const VkDescriptorSetAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = visibility_pool_.get(),
            .descriptorSetCount = static_cast<std::uint32_t>(frame_layouts.size()),
            .pSetLayouts = frame_layouts.data()};
        VK_CHECK(vkAllocateDescriptorSets(context_->device(), &allocate, allocated_sets.data()));
        f.descriptor = allocated_sets[0];
        f.shadow_descriptor = allocated_sets[1];
        context_->set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET,
                                 reinterpret_cast<std::uint64_t>(f.descriptor),
                                 "frame GPU visibility descriptors");
        context_->set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET,
                                 reinterpret_cast<std::uint64_t>(f.shadow_descriptor),
                                 "frame GPU shadow visibility descriptors");

        std::array<VkDescriptorBufferInfo, 7> infos{};
        const std::array buffers{std::cref(f.uniform),
                                 std::cref(frame.scene_instance_buffer),
                                 std::cref(f.metadata),
                                 std::cref(f.groups),
                                 std::cref(f.visible),
                                 std::cref(f.indirect),
                                 std::cref(visibility_lod_history_)};
        const GpuTexture &initial_hiz = hiz_pyramid_ ? hiz_pyramid_ : hiz_fallback_;
        const VkDescriptorImageInfo hiz_image_info{
            .sampler = initial_hiz.sampler(),
            .imageView = initial_hiz.view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        std::array<VkWriteDescriptorSet, 8> writes{};
        for (unsigned i = 0; i < infos.size(); ++i) {
            infos[i] = {buffers[i].get().handle(), 0, buffers[i].get().size()};
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         .dstSet = f.descriptor,
                         .dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = bindings[i].descriptorType,
                         .pBufferInfo = &infos[i]};
        }
        writes[7] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = f.descriptor,
                     .dstBinding = 7U,
                     .descriptorCount = 1U,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     .pImageInfo = &hiz_image_info};
        vkUpdateDescriptorSets(context_->device(), static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        std::array<VkDescriptorBufferInfo, 6> shadow_infos{};
        const std::array shadow_buffers{std::cref(f.shadow_uniform),
                                        std::cref(frame.scene_instance_buffer),
                                        std::cref(f.metadata),
                                        std::cref(f.groups),
                                        std::cref(f.visible),
                                        std::cref(f.shadow_indirect)};
        std::array<VkWriteDescriptorSet, 6> shadow_writes{};
        for (unsigned i = 0; i < shadow_infos.size(); ++i) {
            shadow_infos[i] = {shadow_buffers[i].get().handle(), 0, shadow_buffers[i].get().size()};
            shadow_writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = f.shadow_descriptor,
                                .dstBinding = i,
                                .descriptorCount = 1U,
                                .descriptorType = shadow_bindings[i].descriptorType,
                                .pBufferInfo = &shadow_infos[i]};
        }
        vkUpdateDescriptorSets(context_->device(),
                               static_cast<std::uint32_t>(shadow_writes.size()),
                               shadow_writes.data(), 0, nullptr);
    }
    update_hiz_descriptors();

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
    context_->set_debug_name(VK_OBJECT_TYPE_PIPELINE,
                             reinterpret_cast<std::uint64_t>(shadow_init_pipeline_.get()),
                             "GPU shadow command init");
    context_->set_debug_name(VK_OBJECT_TYPE_PIPELINE,
                             reinterpret_cast<std::uint64_t>(shadow_visibility_pipeline_.get()),
                             "GPU shadow cascade visibility and LOD");
    context_->set_debug_name(
        VK_OBJECT_TYPE_PIPELINE_LAYOUT,
        reinterpret_cast<std::uint64_t>(shadow_visibility_pipeline_layout_.get()),
        "GPU shadow visibility pipeline layout");
    context_->set_debug_name(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        reinterpret_cast<std::uint64_t>(shadow_visibility_layout_.get()),
        "GPU shadow visibility descriptor layout");
}

void Renderer::destroy_gpu_visibility() noexcept {
    hiz_downsample_pipeline_.reset();
    hiz_downsample_pipeline_layout_.reset();
    hiz_downsample_pool_.reset();
    hiz_downsample_layout_.reset();
    hiz_first_pass_sets_.fill(VK_NULL_HANDLE);
    hiz_mip_sets_.fill(VK_NULL_HANDLE);

    shadow_visibility_pipeline_.reset();
    shadow_init_pipeline_.reset();
    shadow_visibility_pipeline_layout_.reset();
    shadow_visibility_layout_.reset();

    visibility_scatter_pipeline_.reset();
    visibility_prefix_pipeline_.reset();
    visibility_binned_pipeline_.reset();
    visibility_pipeline_.reset();
    visibility_pipeline_layout_.reset();
    visibility_pool_.reset();
    visibility_layout_.reset();
    for (auto &frame : frames_) {
        auto &f = frame.visibility;
        f.visible.destroy();
        f.indirect.destroy();
        f.shadow_indirect.destroy();
        f.metadata.destroy();
        f.groups.destroy();
        f.uniform.destroy();
        f.shadow_uniform.destroy();
        f.staging.destroy();
        f.readback.destroy();
        f.descriptor = VK_NULL_HANDLE;
        f.shadow_descriptor = VK_NULL_HANDLE;
    }
    visibility_lod_history_.destroy();
    hiz_fallback_.destroy();
    hiz_fallback_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    hiz_valid_ = false;
    packet_used_hiz_ = false;
    gpu_visibility_active_ = false;
}

void Renderer::create_hiz_targets() {
    destroy_hiz_targets();
    if (!hiz_supported_ || !authored_scene_enabled_) {
        return;
    }
    const VkExtent2D extent = scene_output_extent();
    if (extent.width == 0U || extent.height == 0U) {
        return;
    }
    const std::uint32_t mip0_width = std::max(1U, extent.width / 2U);
    const std::uint32_t mip0_height = std::max(1U, extent.height / 2U);
    std::uint32_t mips = 1U;
    for (std::uint32_t s = std::max(mip0_width, mip0_height); s > 1U; s >>= 1U) {
        ++mips;
    }
    mips = std::min<std::uint32_t>(mips, static_cast<std::uint32_t>(max_hiz_mips));
    hiz_pyramid_.create_storage_pyramid(*context_, mip0_width, mip0_height, mips,
                                        VK_FORMAT_R32_SFLOAT, "Hi-Z occlusion pyramid");
    for (std::uint32_t mip = 0U; mip < mips; ++mip) {
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = hiz_pyramid_.image(),
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R32_SFLOAT,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = mip,
                                 .levelCount = 1U,
                                 .baseArrayLayer = 0U,
                                 .layerCount = 1U}};
        VK_CHECK(vkCreateImageView(
            context_->device(), &view_info, nullptr,
            hiz_mip_views_[mip].put(context_->device(), vkDestroyImageView)));
        context_->set_debug_name(
            VK_OBJECT_TYPE_IMAGE_VIEW,
            reinterpret_cast<std::uint64_t>(hiz_mip_views_[mip].get()),
            "Hi-Z mip view");
    }
    hiz_mip_count_ = mips;
    hiz_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    hiz_valid_ = false;
    packet_used_hiz_ = false;
    update_hiz_descriptors();
}

void Renderer::destroy_hiz_targets() noexcept {
    hiz_downsample_pool_.reset();
    hiz_first_pass_sets_.fill(VK_NULL_HANDLE);
    hiz_mip_sets_.fill(VK_NULL_HANDLE);
    for (auto &view : hiz_mip_views_) {
        view.reset();
    }
    hiz_mip_count_ = 0U;
    hiz_pyramid_.destroy();
    hiz_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    hiz_valid_ = false;
    packet_used_hiz_ = false;
    if (context_ != nullptr && visibility_pipeline_.get() != VK_NULL_HANDLE && hiz_fallback_) {
        const VkDescriptorImageInfo fallback_info{
            .sampler = hiz_fallback_.sampler(),
            .imageView = hiz_fallback_.view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        for (auto &frame : frames_) {
            if (frame.visibility.descriptor != VK_NULL_HANDLE) {
                const VkWriteDescriptorSet write{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = frame.visibility.descriptor,
                    .dstBinding = 7U,
                    .descriptorCount = 1U,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &fallback_info};
                vkUpdateDescriptorSets(context_->device(), 1U, &write, 0U, nullptr);
            }
        }
    }
}

void Renderer::update_hiz_descriptors() {
    if (visibility_pipeline_.get() == VK_NULL_HANDLE) {
        return;
    }
    const GpuTexture &bound_hiz = hiz_pyramid_ ? hiz_pyramid_ : hiz_fallback_;
    if (bound_hiz) {
        const VkDescriptorImageInfo image_info{
            .sampler = bound_hiz.sampler(),
            .imageView = bound_hiz.view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        for (auto &frame : frames_) {
            if (frame.visibility.descriptor != VK_NULL_HANDLE) {
                const VkWriteDescriptorSet write{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = frame.visibility.descriptor,
                    .dstBinding = 7U,
                    .descriptorCount = 1U,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &image_info};
                vkUpdateDescriptorSets(context_->device(), 1U, &write, 0U, nullptr);
            }
        }
    }

    hiz_downsample_pool_.reset();
    hiz_first_pass_sets_.fill(VK_NULL_HANDLE);
    hiz_mip_sets_.fill(VK_NULL_HANDLE);
    if (!hiz_pyramid_ || hiz_mip_count_ == 0U ||
        hiz_downsample_layout_.get() == VK_NULL_HANDLE ||
        !anti_aliasing_frames_[0].depth_attachment) {
        return;
    }
    const std::uint32_t chain_sets = hiz_mip_count_ > 1U ? (hiz_mip_count_ - 1U) : 0U;
    const std::uint32_t total_sets = max_frames_in_flight + chain_sets;
    const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, total_sets},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, total_sets},
    }};
    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = total_sets,
        .poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data()};
    VK_CHECK(vkCreateDescriptorPool(
        context_->device(), &pool_info, nullptr,
        hiz_downsample_pool_.put(context_->device(), vkDestroyDescriptorPool)));
    context_->set_debug_name(
        VK_OBJECT_TYPE_DESCRIPTOR_POOL,
        reinterpret_cast<std::uint64_t>(hiz_downsample_pool_.get()),
        "Hi-Z downsample descriptor pool");

    std::array<VkDescriptorSetLayout, max_frames_in_flight + max_hiz_mips> layouts{};
    layouts.fill(hiz_downsample_layout_.get());
    std::array<VkDescriptorSet, max_frames_in_flight + max_hiz_mips> sets{};
    const VkDescriptorSetAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = hiz_downsample_pool_.get(),
        .descriptorSetCount = total_sets,
        .pSetLayouts = layouts.data()};
    VK_CHECK(vkAllocateDescriptorSets(context_->device(), &alloc_info, sets.data()));

    const auto write_pair = [this](VkDescriptorSet set, VkSampler src_sampler,
                                   VkImageView src_view, VkImageView dst_view) {
        const VkDescriptorImageInfo src_info{
            .sampler = src_sampler,
            .imageView = src_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        const VkDescriptorImageInfo dst_info{
            .sampler = VK_NULL_HANDLE,
            .imageView = dst_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const std::array<VkWriteDescriptorSet, 2> writes{{
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = set,
             .dstBinding = 0U,
             .descriptorCount = 1U,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &src_info},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = set,
             .dstBinding = 1U,
             .descriptorCount = 1U,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &dst_info},
        }};
        vkUpdateDescriptorSets(context_->device(), static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0U, nullptr);
    };

    for (std::uint32_t i = 0U; i < max_frames_in_flight; ++i) {
        hiz_first_pass_sets_[i] = sets[i];
        const auto &depth = anti_aliasing_frames_[i].depth_attachment;
        write_pair(hiz_first_pass_sets_[i], depth.sampler(), depth.view(),
                   hiz_mip_views_[0].get());
    }
    for (std::uint32_t mip = 0U; mip < chain_sets; ++mip) {
        hiz_mip_sets_[mip] = sets[max_frames_in_flight + mip];
        write_pair(hiz_mip_sets_[mip], hiz_pyramid_.sampler(), hiz_mip_views_[mip].get(),
                   hiz_mip_views_[mip + 1U].get());
    }
}

bool Renderer::should_use_hiz_occlusion() const noexcept {
    if (!hiz_supported_ || !hiz_pyramid_ || hiz_downsample_pipeline_.get() == VK_NULL_HANDLE ||
        hiz_first_pass_sets_[0] == VK_NULL_HANDLE || !gpu_visibility_active() ||
        gpu_visibility_bypassed_ || !visibility_settings_.occlusion ||
        scene_metadata_.size() < visibility_settings_.occlusion_threshold) {
        return false;
    }
    if (is_msaa(anti_aliasing_.method) && (!hiz_msaa_min_resolve_ || fog_active())) {
        return false;
    }
    return anti_aliasing_.method != AntiAliasingMethod::off ||
           (lighting_settings_.dynamic && mode_ != RunMode::instancing) ||
           subsurface_mode_actual() == SubsurfaceMode::full_resolution ||
           motion_blur_mode_actual() != MotionBlurMode::off ||
           fog_active();
}

void Renderer::record_hiz_pyramid(VkCommandBuffer command, FrameContext &frame,
                                  AntiAliasingFrameResources &aa_resources,
                                  VkExtent2D source_extent) {
    if (!should_use_hiz_occlusion() || source_extent.width == 0U || source_extent.height == 0U) {
        return;
    }
    image_barrier(
        command, aa_resources.depth_attachment.image(), VK_IMAGE_ASPECT_DEPTH_BIT,
        aa_resources.depth_attachment_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    aa_resources.depth_attachment_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    image_barrier(
        command, hiz_pyramid_.image(), VK_IMAGE_ASPECT_COLOR_BIT, hiz_layout_,
        VK_IMAGE_LAYOUT_GENERAL,
        hiz_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE
                                                 : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        hiz_layout_ == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_ACCESS_2_NONE
            : (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0U,
        hiz_mip_count_);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_downsample_pipeline_.get());

    glm::uvec2 src_size{source_extent.width, source_extent.height};
    glm::uvec2 dst_size{std::max(1U, src_size.x / 2U), std::max(1U, src_size.y / 2U)};
    {
        const HizPushConstants push{src_size, dst_size};
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                hiz_downsample_pipeline_layout_.get(), 0U, 1U,
                                &hiz_first_pass_sets_[frame_index_], 0U, nullptr);
        vkCmdPushConstants(command, hiz_downsample_pipeline_layout_.get(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
        vkCmdDispatch(command, (dst_size.x + 7U) / 8U, (dst_size.y + 7U) / 8U, 1U);
        image_barrier(command, hiz_pyramid_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      0U, 1U);
    }

    for (std::uint32_t mip = 1U; mip < hiz_mip_count_; ++mip) {
        src_size = dst_size;
        dst_size = {std::max(1U, src_size.x / 2U), std::max(1U, src_size.y / 2U)};
        const HizPushConstants push{src_size, dst_size};
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                hiz_downsample_pipeline_layout_.get(), 0U, 1U,
                                &hiz_mip_sets_[mip - 1U], 0U, nullptr);
        vkCmdPushConstants(command, hiz_downsample_pipeline_layout_.get(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
        vkCmdDispatch(command, (dst_size.x + 7U) / 8U, (dst_size.y + 7U) / 8U, 1U);
        image_barrier(command, hiz_pyramid_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      mip, 1U);
    }

    hiz_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    hiz_valid_ = true;
    hiz_view_projection_ = frame.rendered_view_projection;
    hiz_camera_ = frame.rendered_camera_position;
    hiz_source_extent_ = source_extent;
    hiz_runtime_revision_ = runtime_transforms_.revision();
    hiz_document_revision_ = editor_scene_revision_;
}

bool Renderer::shadow_gpu_visibility_active() const noexcept {
    return authored_scene_enabled_ && visibility_settings_.dynamic_shadows &&
           gpu_visibility_active() && !gpu_visibility_bypassed_ &&
           shadow_visibility_pipeline_.get() != VK_NULL_HANDLE &&
           shadow_init_pipeline_.get() != VK_NULL_HANDLE && !scene_metadata_.empty() &&
           visibility_groups_.size() <= max_shadow_draw_groups;
}

void Renderer::record_shadow_visibility(VkCommandBuffer command, FrameContext &frame,
                                        std::uint32_t update_mask, std::uint32_t cascades,
                                        std::uint32_t tile_resolution) {
    if (!shadow_gpu_visibility_active() || update_mask == 0U) {
        return;
    }
    auto &f = frame.visibility;
    const auto source_count = static_cast<std::uint32_t>(scene_metadata_.size());
    const auto group_count = static_cast<std::uint32_t>(visibility_groups_.size());

    ShadowVisibilityUniforms shadow_uniforms{};
    const float guard = 4.0F / static_cast<float>(std::max(tile_resolution, 16U));
    for (std::uint32_t c = 0U; c < cascades; ++c) {
        if ((update_mask & (1U << c)) == 0U) {
            continue;
        }
        auto matrix =
            cascades == 4U ? lighting_uniforms_.shadow_matrices[c] : light_view_projection_;
        const float scale_x = glm::length(glm::vec3(matrix[0][0], matrix[1][0], matrix[2][0]));
        const float scale_y = glm::length(glm::vec3(matrix[0][1], matrix[1][1], matrix[2][1]));
        shadow_uniforms.cascade_pixels_per_meter[static_cast<glm::length_t>(c)] =
            0.5F * static_cast<float>(tile_resolution) * std::max(scale_x, scale_y);
        for (int col = 0; col < 4; ++col) {
            matrix[col][0] *= 1.0F - guard;
            matrix[col][1] *= 1.0F - guard;
        }
        const ViewFrustum frustum(matrix);
        const auto &planes = frustum.planes();
        for (std::size_t p = 0U; p < planes.size(); ++p) {
            shadow_uniforms.cascade_planes[c * 6U + p] = planes[p];
        }
    }
    shadow_uniforms.config = {source_count, group_count, update_mask,
                              visibility_settings_.lod ? 1U : 0U};
    shadow_uniforms.params = {visibility_settings_.lod_pixel_error * 1.25F, 0.0F, 0.0F, 0.0F};
    f.shadow_uniform.write(std::as_bytes(std::span{&shadow_uniforms, 1}));

    const VkDeviceSize cmd_bytes =
        VkDeviceSize{group_count} * 16ULL * sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize shadow_visible_offset =
        32ULL + 4ULL * VkDeviceSize{source_count} * sizeof(std::uint32_t);
    const VkDeviceSize shadow_visible_bytes =
        16ULL * VkDeviceSize{source_count} * sizeof(std::uint32_t);

    barrier(command, f.shadow_indirect.handle(), VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0U, cmd_bytes);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, shadow_init_pipeline_.get());
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            shadow_visibility_pipeline_layout_.get(), 0U, 1U,
                            &f.shadow_descriptor, 0U, nullptr);
    vkCmdDispatch(command, (group_count * 16U + 63U) / 64U, 1U, 1U);

    barrier(command, f.shadow_indirect.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0U,
            cmd_bytes);
    barrier(command, f.visible.handle(), VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, shadow_visible_offset, shadow_visible_bytes);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, shadow_visibility_pipeline_.get());
    vkCmdDispatch(command, (source_count + 127U) / 128U, 1U, 1U);

    barrier(command, f.shadow_indirect.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, 0U, cmd_bytes);
    barrier(command, f.visible.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT, shadow_visible_offset, shadow_visible_bytes);
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
    const auto sanitize_error = [](std::uint32_t lod_indices, float err) {
        return std::isfinite(err) ? err : (lod_indices > 0U ? 0.0F : -1.0F);
    };
    const auto group = [&](std::uint32_t first, std::uint32_t instances,
                           std::int32_t vertex_offset, bool tessellated, glm::uvec4 counts,
                           glm::uvec4 firsts, glm::vec4 errors) {
        const auto g = static_cast<std::uint32_t>(visibility_groups_.size());
        visibility_groups_.push_back(
            {{first, instances, static_cast<std::uint32_t>(vertex_offset), tessellated ? 1U : 0U},
             counts,
             firsts,
             errors});
        for (auto i = first; i < first + instances; ++i) {
            const auto &bounds = scene_metadata_[i].local_bounds;
            visibility_sources_[i] = {{bounds.center, float(g)}, {bounds.extent, 0}};
        }
        for (glm::length_t lod = 0; lod < 4; ++lod) {
            visibility_commands_.push_back({counts[lod], 0U, firsts[lod], vertex_offset,
                                            first + static_cast<unsigned>(lod) * count});
        }
    };
    group(0, editor_ground_count_, plane_vertex, false, {plane_count, 0, 0, 0},
          {plane_first, 0, 0, 0}, {0.0F, -1.0F, -1.0F, -1.0F});
    group(editor_ground_count_, editor_cube_count_, 0, false, {cube_count, 0, 0, 0}, {},
          {0.0F, -1.0F, -1.0F, -1.0F});
    for (const auto &draw : editor_model_draws_) {
        const auto &mesh = editor_model_assets_[draw.asset_index].submeshes[draw.submesh_index];
        const bool tessellated = model_tessellation_enabled_ && draw.tessellation.enabled();
        const bool skinned = draw.uses_skinned_pool;
        group(draw.first_instance, draw.instance_count, draw.vertex_offset, tessellated,
              skinned
                  ? glm::uvec4{mesh.lod_count[0], 0U, 0U, 0U}
                  : glm::uvec4{
                        mesh.lod_count[0], mesh.lod_count[1], mesh.lod_count[2], mesh.lod_count[3]},
              skinned
                  ? glm::uvec4{mesh.lod_first[0], 0U, 0U, 0U}
                  : glm::uvec4{
                        mesh.lod_first[0], mesh.lod_first[1], mesh.lod_first[2], mesh.lod_first[3]},
              skinned
                  ? glm::vec4{
                        sanitize_error(mesh.lod_count[0], mesh.lod_error[0]), -1.0F, -1.0F, -1.0F}
                  : glm::vec4{
                        sanitize_error(mesh.lod_count[0], mesh.lod_error[0]),
                        sanitize_error(mesh.lod_count[1], mesh.lod_error[1]),
                        sanitize_error(mesh.lod_count[2], mesh.lod_error[2]),
                        sanitize_error(mesh.lod_count[3], mesh.lod_error[3])});
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
    hiz_valid_ = false;
    packet_used_hiz_ = false;
}

void Renderer::prepare_gpu_scene_packet(const glm::mat4 &vp, glm::vec3 camera, VkExtent2D extent) {
    const bool fade_enabled = lod_fade_pattern_ != LodFadePattern::off && visibility_settings_.lod &&
                              !editor_model_draws_.empty();
    const float camera_step = hiz_valid_ ? glm::length(camera - hiz_camera_) : 0.0F;
    const bool hiz_ready =
        should_use_hiz_occlusion() && hiz_valid_ && hiz_mip_count_ > 0U &&
        hiz_runtime_revision_ == runtime_transforms_.revision() &&
        hiz_document_revision_ == editor_scene_revision_ && camera_step <= 5.0F;
    // Optional fading advances without a camera change. It never requires
    // synchronous readback of the active-transition count.
    const bool changed = fade_enabled || reset_lod_history_ ||
                         (hiz_ready != packet_used_hiz_) ||
                         gpu_packet_revision_ != gpu_scene_revision_ ||
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
    if (visibility_settings_.occlusion && occluder_candidate_count_ > 0U) {
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
    const bool grid_occlusion_enabled = visibility_settings_.occlusion && grid.has_coverage();
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
                (!grid_occlusion_enabled || !grid.occluded(bounds, padded, guard))) {
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

    const bool use_hiz = hiz_ready && !gpu_visibility_bypassed_;
    const std::uint32_t active_mip0_w =
        use_hiz ? std::max(1U, hiz_source_extent_.width / 2U) : 1U;
    const std::uint32_t active_mip0_h =
        use_hiz ? std::max(1U, hiz_source_extent_.height / 2U) : 1U;
    std::uint32_t active_max_mip = 0U;
    if (use_hiz) {
        for (std::uint32_t s = std::max(active_mip0_w, active_mip0_h);
             s > 1U && active_max_mip + 1U < hiz_mip_count_; s >>= 1U) {
            ++active_max_mip;
        }
    }
    const std::uint32_t occlusion_flags =
        (grid_occlusion_enabled ? 1U : 0U) | (use_hiz ? 2U : 0U);
    const bool binned =
        visibility_settings_.coarse_front_to_back &&
        !scene_metadata_.empty() && !visibility_commands_.empty() &&
        visibility_groups_.size() <= max_shadow_draw_groups;
    const auto command_count = static_cast<unsigned>(visibility_commands_.size());
    const VisibilityUniforms uniform{
        padded,
        use_hiz ? hiz_view_projection_ : glm::mat4{1.0F},
        frustum.planes(),
        {camera, pixels},
        {guard, visibility_settings_.lod_pixel_error,
         fade_enabled ? std::clamp(fsr_frame_time_ms_ * 0.001F / lod_fade_seconds_, 0.0F, 1.0F)
                      : 0.0F},
        {static_cast<float>(active_mip0_w), static_cast<float>(active_mip0_h),
         static_cast<float>(active_max_mip), use_hiz ? camera_step : 0.0F},
        {static_cast<unsigned>(scene_metadata_.size()), visibility_settings_.frustum ? 1U : 0U,
         occlusion_flags,
         unsigned(visibility_settings_.lod) |
             (fade_enabled ? (unsigned(lod_fade_pattern_) << 1U) : 0U) |
             (binned ? (8U | (command_count << 4U)) : 0U)},
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
    packet_used_hiz_ = use_hiz;
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
    if (f.view_revision == gpu_visibility_revision_) {
        return;
    }
    if (hiz_pyramid_) {
        if (hiz_layout_ == VK_IMAGE_LAYOUT_UNDEFINED) {
            image_barrier(command, hiz_pyramid_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0U, hiz_mip_count_);
            hiz_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    } else if (hiz_fallback_ && hiz_fallback_layout_ == VK_IMAGE_LAYOUT_UNDEFINED) {
        image_barrier(command, hiz_fallback_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        hiz_fallback_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    const auto commands_bytes = visibility_commands_.size() * sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize main_visible_bytes =
        32ULL + 4ULL * VkDeviceSize{scene_metadata_.size()} * sizeof(std::uint32_t);
    const bool binned =
        visibility_settings_.coarse_front_to_back &&
        !scene_metadata_.empty() && !visibility_commands_.empty() &&
        visibility_groups_.size() <= max_shadow_draw_groups;
    const VkDeviceSize bin_counts_byte_offset =
        32ULL + bin_counts_uint_offset * sizeof(std::uint32_t);
    const VkDeviceSize bin_counts_bytes =
        VkDeviceSize{visibility_commands_.size()} * visibility_depth_bins * sizeof(std::uint32_t);
    const VkDeviceSize scratch_byte_offset =
        32ULL + scratch_uint_offset * sizeof(std::uint32_t);
    const VkDeviceSize scratch_bytes =
        4ULL * VkDeviceSize{scene_metadata_.size()} * sizeof(std::uint32_t);
    const VkBufferCopy commands{commands_offset, 0, commands_bytes};
    vkCmdCopyBuffer(command, f.staging.handle(), f.indirect.handle(), 1, &commands);
    vkCmdFillBuffer(command, f.visible.handle(), 0, 32, 0);
    if (binned) {
        vkCmdFillBuffer(command, f.visible.handle(), bin_counts_byte_offset, bin_counts_bytes, 0);
    }
    barrier(command, f.indirect.handle(), VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0U,
            commands_bytes);
    barrier(command, f.visible.handle(),
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0U,
            main_visible_bytes);
    if (binned) {
        barrier(command, f.visible.handle(),
                VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                bin_counts_byte_offset, bin_counts_bytes);
        barrier(command, f.visible.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                scratch_byte_offset, scratch_bytes);
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
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        binned ? visibility_binned_pipeline_.get() : visibility_pipeline_.get());
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            visibility_pipeline_layout_.get(), 0, 1, &f.descriptor, 0, nullptr);
    vkCmdDispatch(command, (static_cast<unsigned>(scene_metadata_.size()) + 127) / 128, 1, 1);
    if (binned) {
        barrier(command, f.visible.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                bin_counts_byte_offset, bin_counts_bytes);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          visibility_prefix_pipeline_.get());
        vkCmdDispatch(command,
                      (static_cast<unsigned>(visibility_commands_.size()) + 63U) / 64U, 1, 1);
        barrier(command, f.visible.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                bin_counts_byte_offset, bin_counts_bytes);
        barrier(command, f.visible.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT, scratch_byte_offset, scratch_bytes);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          visibility_scatter_pipeline_.get());
        vkCmdDispatch(command, (static_cast<unsigned>(scene_metadata_.size()) + 127U) / 128U, 1, 1);
    }
    barrier(command, f.visible.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, 0U,
            main_visible_bytes);
    barrier(command, f.indirect.handle(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, 0U, commands_bytes);
    const VkBufferCopy counters{0, 0, 32};
    vkCmdCopyBuffer(command, f.visible.handle(), f.readback.handle(), 1, &counters);
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
    const auto bytes = f.readback.read_completed_bytes(32);
    std::array<std::uint32_t, 8> counters{};
    std::memcpy(counters.data(), bytes.data(), 32);
    auto &stats = visibility_statistics_;
    stats = {};
    stats.submitted = f.statistics_sources;
    stats.visible = counters[0];
    stats.frustum_rejected = counters[1];
    stats.occlusion_rejected = counters[2];
    stats.lod_instances = {counters[3], counters[4], counters[5], counters[7]};
    stats.triangles = counters[6];
    stats.packet_revision = packet_revision_;
    assert(stats.visible + stats.frustum_rejected + stats.occlusion_rejected == stats.submitted);
}
} // namespace vcr
