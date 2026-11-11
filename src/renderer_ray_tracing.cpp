#include "renderer.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace vcr {
namespace {

constexpr std::uint32_t rt_opacity_max_triangles = 524'288U;
constexpr std::uint32_t rt_alpha_max_maps = 32U;
constexpr std::uint32_t rt_alpha_texel_base_word =
    rt_opacity_max_triangles * static_cast<std::uint32_t>(sizeof(RtMaskedTriangle) / sizeof(std::uint32_t));
constexpr std::uint32_t rt_alpha_max_texel_words = rt_alpha_max_maps * rt_alpha_map_word_count;
constexpr VkDeviceSize rt_opacity_buffer_bytes =
    static_cast<VkDeviceSize>(rt_alpha_texel_base_word + rt_alpha_max_texel_words) *
    sizeof(std::uint32_t);
constexpr std::uint32_t rt_initial_tlas_capacity = 256U;
constexpr VkDeviceSize rt_vertex_stride_bytes = 24ULL;
constexpr VkDeviceSize rt_terrain_vertex_stride_bytes = 16ULL;
constexpr std::uint32_t rt_cube_vertex_count = 24U;
constexpr std::uint32_t rt_cube_triangle_count = 12U;
constexpr std::uint32_t rt_plane_vertex_count = 4U;
constexpr std::uint32_t rt_plane_triangle_count = 2U;
constexpr VkDeviceSize rt_plane_vertex_byte_offset =
    static_cast<VkDeviceSize>(rt_cube_vertex_count) * rt_vertex_stride_bytes;
constexpr VkDeviceSize rt_plane_index_byte_offset =
    static_cast<VkDeviceSize>(rt_cube_triangle_count * 3U) * sizeof(std::uint16_t);
constexpr std::uint32_t rt_sphere_vertex_count = 1'106U;
constexpr std::uint32_t rt_sphere_triangle_count = 2'208U;
constexpr std::uint32_t rt_terrain_cells_x = 256U;
constexpr std::uint32_t rt_terrain_cells_z = 192U;
constexpr std::uint32_t rt_terrain_vertices_x = rt_terrain_cells_x + 1U;
constexpr std::uint32_t rt_terrain_vertex_count =
    rt_terrain_vertices_x * (rt_terrain_cells_z + 1U);
constexpr std::uint32_t rt_terrain_triangle_count =
    rt_terrain_cells_x * rt_terrain_cells_z * 2U;

void rt_memory_barrier(
    const VkCommandBuffer command_buffer,
    const VkPipelineStageFlags2 src_stage,
    const VkAccessFlags2 src_access,
    const VkPipelineStageFlags2 dst_stage,
    const VkAccessFlags2 dst_access) noexcept {
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1U,
        .pMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

[[nodiscard]] VkDeviceSize align_up_device_size(
    const VkDeviceSize value,
    const VkDeviceSize alignment) noexcept {
    if (alignment <= 1ULL) {
        return value;
    }
    const VkDeviceSize mask = alignment - 1ULL;
    return (value + mask) & ~mask;
}

[[nodiscard]] VkTransformMatrixKHR to_vk_transform(const TransformRows& rows) noexcept {
    return VkTransformMatrixKHR{{
        {rows.x.x, rows.x.y, rows.x.z, rows.x.w},
        {rows.y.x, rows.y.y, rows.y.z, rows.y.w},
        {rows.z.x, rows.z.y, rows.z.z, rows.z.w},
    }};
}

class RtImmediateCommands final {
public:
    explicit RtImmediateCommands(VulkanContext& context)
        : context_(context) {
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = context_.queue_families().graphics,
        };
        VK_CHECK(vkCreateCommandPool(
            context_.device(),
            &pool_info,
            nullptr,
            command_pool_.put(context_.device(), vkDestroyCommandPool)));

        const VkCommandBufferAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = command_pool_.get(),
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1U,
        };
        VK_CHECK(vkAllocateCommandBuffers(
            context_.device(),
            &allocate_info,
            &command_buffer_));

        const VkFenceCreateInfo fence_info{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        VK_CHECK(vkCreateFence(
            context_.device(),
            &fence_info,
            nullptr,
            fence_.put(context_.device(), vkDestroyFence)));

        const VkCommandBufferBeginInfo begin_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VK_CHECK(vkBeginCommandBuffer(command_buffer_, &begin_info));
    }

    ~RtImmediateCommands() noexcept {
        if (pending_) {
            const VkResult result = vkDeviceWaitIdle(context_.device());
            if (result != VK_SUCCESS) {
                std::fprintf(stderr, "RT immediate cleanup wait failed: %d\n", result);
            }
        }
    }

    RtImmediateCommands(const RtImmediateCommands&) = delete;
    RtImmediateCommands& operator=(const RtImmediateCommands&) = delete;

    [[nodiscard]] VkCommandBuffer command_buffer() const noexcept { return command_buffer_; }

    void submit_and_wait() {
        VK_CHECK(vkEndCommandBuffer(command_buffer_));
        const VkCommandBufferSubmitInfo command_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = command_buffer_,
            .deviceMask = 1U,
        };
        const VkSubmitInfo2 submit_info{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1U,
            .pCommandBufferInfos = &command_info,
        };
        VK_CHECK(vkQueueSubmit2(context_.graphics_queue(), 1U, &submit_info, fence_.get()));
        pending_ = true;
        const VkFence fence = fence_.get();
        VK_CHECK(vkWaitForFences(context_.device(), 1U, &fence, VK_TRUE, UINT64_MAX));
        pending_ = false;
    }

private:
    VulkanContext& context_;
    DeviceObject<VkCommandPool> command_pool_{};
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    DeviceObject<VkFence> fence_{};
    bool pending_ = false;
};

[[nodiscard]] AccelerationStructure build_compacted_static_blas(
    VulkanContext& context,
    const VkDeviceAddress vertex_address,
    const VkDeviceSize vertex_stride,
    const std::uint32_t max_vertex,
    const VkDeviceAddress index_address,
    const VkIndexType index_type,
    const std::uint32_t triangle_count,
    const std::string_view debug_name) {
    const VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry = VkAccelerationStructureGeometryDataKHR{
            .triangles = VkAccelerationStructureGeometryTrianglesDataKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                .vertexData = {.deviceAddress = vertex_address},
                .vertexStride = vertex_stride,
                .maxVertex = max_vertex,
                .indexType = index_type,
                .indexData = {.deviceAddress = index_address},
            },
        },
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };
    VkAccelerationStructureBuildGeometryInfoKHR build_info{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
            VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1U,
        .pGeometries = &geometry,
    };
    VkAccelerationStructureBuildSizesInfoKHR sizes{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };
    vkGetAccelerationStructureBuildSizesKHR(
        context.device(),
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info,
        &triangle_count,
        &sizes);

    AccelerationStructure uncompacted{};
    uncompacted.create(
        context,
        sizes.accelerationStructureSize,
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        debug_name,
        triangle_count);

    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        context.acceleration_structure_properties().minAccelerationStructureScratchOffsetAlignment,
        256ULL);
    GpuBuffer scratch{};
    scratch.create(
        context,
        std::max<VkDeviceSize>(sizes.buildScratchSize, 256ULL),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "static BLAS scratch",
        scratch_alignment);

    DeviceObject<VkQueryPool> query_pool{};
    const VkQueryPoolCreateInfo query_info{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
        .queryCount = 1U,
    };
    VK_CHECK(vkCreateQueryPool(
        context.device(),
        &query_info,
        nullptr,
        query_pool.put(context.device(), vkDestroyQueryPool)));

    const VkAccelerationStructureBuildRangeInfoKHR range{
        .primitiveCount = triangle_count,
        .primitiveOffset = 0U,
        .firstVertex = 0U,
        .transformOffset = 0U,
    };
    const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
    {
        RtImmediateCommands commands(context);
        const VkCommandBuffer cmd = commands.command_buffer();
        vkCmdResetQueryPool(cmd, query_pool.get(), 0U, 1U);
        build_info.dstAccelerationStructure = uncompacted.handle();
        build_info.scratchData.deviceAddress = scratch.device_address();
        vkCmdBuildAccelerationStructuresKHR(cmd, 1U, &build_info, &range_ptr);
        rt_memory_barrier(
            cmd,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        const VkAccelerationStructureKHR uncompacted_handle = uncompacted.handle();
        vkCmdWriteAccelerationStructuresPropertiesKHR(
            cmd,
            1U,
            &uncompacted_handle,
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
            query_pool.get(),
            0U);
        commands.submit_and_wait();
    }
    scratch.destroy();

    VkDeviceSize compacted_size = 0ULL;
    VK_CHECK(vkGetQueryPoolResults(
        context.device(),
        query_pool.get(),
        0U,
        1U,
        sizeof(VkDeviceSize),
        &compacted_size,
        sizeof(VkDeviceSize),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    if (compacted_size == 0ULL || compacted_size >= uncompacted.size_bytes()) {
        return uncompacted;
    }

    AccelerationStructure compacted{};
    compacted.create(
        context,
        compacted_size,
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        debug_name,
        triangle_count);
    {
        RtImmediateCommands commands(context);
        const VkCommandBuffer cmd = commands.command_buffer();
        const VkCopyAccelerationStructureInfoKHR copy_info{
            .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
            .src = uncompacted.handle(),
            .dst = compacted.handle(),
            .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR,
        };
        vkCmdCopyAccelerationStructureKHR(cmd, &copy_info);
        rt_memory_barrier(
            cmd,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        commands.submit_and_wait();
    }
    return compacted;
}

} // namespace

void Renderer::create_ray_tracing_resources() {
    if (!rt_provisioned_ || context_ == nullptr) {
        return;
    }
    rt_opacity_micromaps_.create(
        *context_,
        rt_opacity_buffer_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "RT masked triangle & R8 alpha cache");
    const RtMaskedTriangle sentinel_opaque{};
    upload_to_device_buffer(
        rt_opacity_micromaps_,
        std::as_bytes(std::span{&sentinel_opaque, std::size_t{1U}}),
        0ULL);
    rt_opacity_triangle_count_ = 1U;
    rt_alpha_texel_words_used_ = 0U;

    blas_cube_ = build_compacted_static_blas(
        *context_,
        vertex_buffer_.device_address(),
        rt_vertex_stride_bytes,
        rt_cube_vertex_count - 1U,
        index_buffer_.device_address(),
        VK_INDEX_TYPE_UINT16,
        rt_cube_triangle_count,
        "cube BLAS");
    blas_plane_ = build_compacted_static_blas(
        *context_,
        vertex_buffer_.device_address() + rt_plane_vertex_byte_offset,
        rt_vertex_stride_bytes,
        rt_plane_vertex_count - 1U,
        index_buffer_.device_address() + rt_plane_index_byte_offset,
        VK_INDEX_TYPE_UINT16,
        rt_plane_triangle_count,
        "ground plane BLAS");
    blas_sphere_ = build_compacted_static_blas(
        *context_,
        pbr_sample_vertex_buffer_.device_address(),
        rt_vertex_stride_bytes,
        rt_sphere_vertex_count - 1U,
        pbr_sample_index_buffer_.device_address(),
        VK_INDEX_TYPE_UINT16,
        rt_sphere_triangle_count,
        "PBR sample sphere BLAS");

    {
        std::vector<std::uint32_t> terrain_triangles;
        terrain_triangles.reserve(static_cast<std::size_t>(rt_terrain_triangle_count) * 3ULL);
        for (std::uint32_t row = 0U; row < rt_terrain_cells_z; ++row) {
            for (std::uint32_t col = 0U; col < rt_terrain_cells_x; ++col) {
                const std::uint32_t a = row * rt_terrain_vertices_x + col;
                const std::uint32_t b = a + 1U;
                const std::uint32_t c = (row + 1U) * rt_terrain_vertices_x + col;
                const std::uint32_t d = c + 1U;
                terrain_triangles.push_back(c);
                terrain_triangles.push_back(a);
                terrain_triangles.push_back(d);
                terrain_triangles.push_back(d);
                terrain_triangles.push_back(a);
                terrain_triangles.push_back(b);
            }
        }
        GpuBuffer terrain_blas_indices{};
        terrain_blas_indices.create(
            *context_,
            static_cast<VkDeviceSize>(terrain_triangles.size()) * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            0,
            "terrain temporary BLAS index buffer");
        upload_to_device_buffer(terrain_blas_indices, std::as_bytes(std::span{terrain_triangles}));
        blas_terrain_ = build_compacted_static_blas(
            *context_,
            terrain_vertex_buffer_.device_address(),
            rt_terrain_vertex_stride_bytes,
            rt_terrain_vertex_count - 1U,
            terrain_blas_indices.device_address(),
            VK_INDEX_TYPE_UINT32,
            rt_terrain_triangle_count,
            "heightfield terrain BLAS");
    }

#if VCR_EDITOR_ENABLED
    for (const auto& entry : rt_alpha_maps_) {
        static_cast<void>(find_or_upload_rt_alpha_map(entry.relative_path, nullptr));
    }
    for (auto& asset : editor_model_assets_) {
        if (!asset.vertex_buffer || !asset.index_buffer) {
            continue;
        }
        for (auto& submesh : asset.submeshes) {
            if ((submesh.coverage & 1U) != 0U && !submesh.cpu_rt_triangles.empty() &&
                submesh.rt_opacity_count == 0U) {
                const std::uint32_t alpha_word_offset = submesh.rt_alpha_texture_path.empty()
                    ? 0U
                    : find_or_upload_rt_alpha_map(submesh.rt_alpha_texture_path, nullptr);
                if (!submesh.rt_alpha_texture_path.empty()) {
                    for (auto& tri : submesh.cpu_rt_triangles) {
                        if (tri.effective_cutoff <= 1.0F) {
                            tri.texel_word_offset = alpha_word_offset;
                        }
                    }
                }
                submesh.rt_opacity_first =
                    append_rt_masked_triangles(submesh.cpu_rt_triangles);
                submesh.rt_opacity_count =
                    static_cast<std::uint32_t>(submesh.cpu_rt_triangles.size());
            }
        }
        build_model_blas_on_upload(asset);
        compact_model_blas(asset);
    }
#endif

    for (auto& frame : frames_) {
        frame.tlas_capacity = 0U;
        frame.tlas_instance_count = 0U;
        frame.tlas_scene_revision = 0U;
        frame.tlas_runtime_revision = 0U;
        frame.tlas_terrain_visible = false;
        frame.tlas_built_once = false;
        frame.tlas_was_updated = false;
        ensure_frame_tlas_capacity(frame, rt_initial_tlas_capacity);
    }
    {
        RtImmediateCommands commands(*context_);
        const VkCommandBuffer cmd = commands.command_buffer();
        for (auto& frame : frames_) {
            const VkAccelerationStructureGeometryKHR geometry{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
                .geometry = VkAccelerationStructureGeometryDataKHR{
                    .instances = VkAccelerationStructureGeometryInstancesDataKHR{
                        .sType =
                            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                        .arrayOfPointers = VK_FALSE,
                        .data = {.deviceAddress = frame.tlas_instance_buffer.device_address()},
                    },
                },
            };
            const VkAccelerationStructureBuildGeometryInfoKHR build_info{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .dstAccelerationStructure = frame.tlas.handle(),
                .geometryCount = 1U,
                .pGeometries = &geometry,
                .scratchData = {.deviceAddress = frame.tlas_scratch_buffer.device_address()},
            };
            const VkAccelerationStructureBuildRangeInfoKHR range{
                .primitiveCount = 0U,
            };
            const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
            vkCmdBuildAccelerationStructuresKHR(cmd, 1U, &build_info, &range_ptr);
        }
        rt_memory_barrier(
            cmd,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        commands.submit_and_wait();
    }

    rt_tlas_instances_.reserve(4096U);
    refresh_ray_tracing_statistics();
}

void Renderer::provision_ray_tracing_resources() {
    rt_provision_pending_ = false;
    if (rt_provisioned_ || context_ == nullptr || !context_->ray_query_supported()) {
        return;
    }
#if VCR_EDITOR_ENABLED
    if (uploading_request_) {
        asset_upload_.wait();
        if (uploading_model_) {
            editor_model_assets_[*uploading_model_].ready = true;
        }
        asset_requests_[*uploading_request_].done = true;
        --asset_requests_pending_;
        uploading_model_.reset();
        uploading_request_.reset();
    }
#endif
    VK_CHECK(vkDeviceWaitIdle(context_->device()));

    rt_provisioned_ = true;
    create_ray_tracing_resources();

    destroy_subsurface_descriptors();
    destroy_anti_aliasing_descriptors();
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_->device(), descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
        for (auto& frame : frames_) {
            frame.descriptor_set = VK_NULL_HANDLE;
            frame.scene_descriptor_set = VK_NULL_HANDLE;
        }
    }
    destroy_pipeline();

    local_light_cluster_pipeline_.reset();
    voxel_geometry_pipeline_.reset();
    voxel_light_pipeline_.reset();
    lighting_compute_layout_.reset();
    lighting_pool_.reset();
    for (auto& frame : frames_) {
        frame.lighting_descriptor_set = VK_NULL_HANDLE;
    }
    lighting_layout_.reset();

    create_lighting_descriptor_layout();
    create_pipeline();
    create_lighting_pipelines();
    create_descriptors();
    create_anti_aliasing_descriptors();
    if (subsurface_resources_provisioned_) {
        create_subsurface_descriptors();
    }
    invalidate_temporal_history();
    exposure_reset_pending_ = true;
    voxel_ready_ = false;
}

void Renderer::destroy_ray_tracing() noexcept {
    for (auto& frame : frames_) {
        frame.tlas.destroy();
        frame.tlas_instance_buffer.destroy();
        frame.tlas_scratch_buffer.destroy();
        frame.tlas_capacity = 0U;
        frame.tlas_instance_count = 0U;
        frame.tlas_scene_revision = 0U;
        frame.tlas_runtime_revision = 0U;
        frame.tlas_terrain_visible = false;
        frame.tlas_built_once = false;
        frame.tlas_was_updated = false;
    }
#if VCR_EDITOR_ENABLED
    for (auto& entry : rt_alpha_maps_) {
        entry.gpu_word_offset = 0U;
    }
    for (auto& asset : editor_model_assets_) {
        for (auto& submesh : asset.submeshes) {
            submesh.blas.destroy();
            submesh.rt_opacity_first = 0U;
            submesh.rt_opacity_count = 0U;
        }
    }
#endif
    blas_terrain_.destroy();
    blas_sphere_.destroy();
    blas_plane_.destroy();
    blas_cube_.destroy();
    rt_upload_scratch_.destroy();
    rt_opacity_micromaps_.destroy();
    rt_opacity_triangle_count_ = 0U;
    rt_alpha_texel_words_used_ = 0U;
    rt_tlas_instances_.clear();
    rt_provisioned_ = false;
    rt_provision_pending_ = false;
    rt_tlas_build_due_ = false;
    gi_last_rt_active_ = false;
    lighting_statistics_.blas_count = 0U;
    lighting_statistics_.tlas_instances = 0U;
    lighting_statistics_.tlas_builds = 0ULL;
    lighting_statistics_.acceleration_structure_bytes = 0ULL;
    lighting_statistics_.opacity_micromap_bytes = 0ULL;
}

void Renderer::ensure_frame_tlas_capacity(
    FrameContext& frame,
    const std::uint32_t required_instances) {
    const std::uint64_t max_hw_instances =
        std::max<std::uint64_t>(context_->acceleration_structure_properties().maxInstanceCount, 1ULL);
    const std::uint32_t clamped_required = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(required_instances, max_hw_instances));
    if (frame.tlas && frame.tlas_capacity >= clamped_required && frame.tlas_capacity > 0U) {
        return;
    }
    std::uint32_t new_capacity = std::max(frame.tlas_capacity, rt_initial_tlas_capacity);
    while (new_capacity < clamped_required) {
        if (new_capacity > static_cast<std::uint32_t>(max_hw_instances / 2ULL)) {
            new_capacity = static_cast<std::uint32_t>(max_hw_instances);
            break;
        }
        new_capacity *= 2U;
    }

    const VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = VkAccelerationStructureGeometryDataKHR{
            .instances = VkAccelerationStructureGeometryInstancesDataKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                .arrayOfPointers = VK_FALSE,
            },
        },
    };
    const VkAccelerationStructureBuildGeometryInfoKHR build_info{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1U,
        .pGeometries = &geometry,
    };
    VkAccelerationStructureBuildSizesInfoKHR sizes{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };
    vkGetAccelerationStructureBuildSizesKHR(
        context_->device(),
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info,
        &new_capacity,
        &sizes);

    frame.tlas.create(
        *context_,
        sizes.accelerationStructureSize,
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        "frame scene TLAS",
        new_capacity);
    frame.tlas_instance_buffer.create(
        *context_,
        static_cast<VkDeviceSize>(new_capacity) * sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        "frame TLAS instances",
        16ULL);
    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        context_->acceleration_structure_properties().minAccelerationStructureScratchOffsetAlignment,
        256ULL);
    frame.tlas_scratch_buffer.create(
        *context_,
        std::max<VkDeviceSize>(sizes.buildScratchSize, 256ULL),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "frame TLAS scratch",
        scratch_alignment);
    frame.tlas_capacity = new_capacity;
    frame.tlas_built_once = false;

    if (frame.lighting_descriptor_set != VK_NULL_HANDLE) {
        const VkAccelerationStructureKHR tlas_handle = frame.tlas.handle();
        const VkWriteDescriptorSetAccelerationStructureKHR tlas_write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1U,
            .pAccelerationStructures = &tlas_handle,
        };
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = &tlas_write,
            .dstSet = frame.lighting_descriptor_set,
            .dstBinding = 10U,
            .descriptorCount = 1U,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        };
        vkUpdateDescriptorSets(context_->device(), 1U, &write, 0U, nullptr);
    }
    refresh_ray_tracing_statistics();
}

void Renderer::prepare_ray_tracing(FrameContext& frame) {
    if (!rt_provisioned_ || mode_ == RunMode::instancing) {
        rt_tlas_build_due_ = false;
        return;
    }

    const bool chart_fixture =
        fixture_ == RenderFixture::material_spheres ||
        fixture_ == RenderFixture::subsurface_profiles;
    std::uint64_t scene_revision = 1ULL;
    std::uint64_t runtime_revision = 1ULL;
    bool terrain_visible = !chart_fixture;
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_) {
        scene_revision = editor_scene_revision_;
        runtime_revision = runtime_transforms_.revision();
        terrain_visible = !chart_fixture && editor_terrain_visible_;
    }
#endif

    if (frame.tlas_built_once &&
        frame.tlas_scene_revision == scene_revision &&
        frame.tlas_runtime_revision == runtime_revision &&
        frame.tlas_terrain_visible == terrain_visible) {
        rt_tlas_build_due_ = false;
        lighting_statistics_.tlas_instances = frame.tlas_instance_count;
        return;
    }

    const std::uint64_t max_hw_instances =
        std::max<std::uint64_t>(context_->acceleration_structure_properties().maxInstanceCount, 1ULL);
    rt_tlas_instances_.clear();
    const auto push_instance = [&](
                                   const TransformRows& transform,
                                   const AccelerationStructure& blas,
                                   const std::uint32_t custom_index,
                                   const VkGeometryInstanceFlagsKHR flags) {
        if (!blas || blas.device_address() == 0ULL ||
            rt_tlas_instances_.size() >= max_hw_instances) {
            return;
        }
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform = to_vk_transform(transform);
        instance.instanceCustomIndex = custom_index & 0x00FFFFFFU;
        instance.mask = 0xFFU;
        instance.instanceShaderBindingTableRecordOffset = 0U;
        instance.flags = flags;
        instance.accelerationStructureReference = blas.device_address();
        rt_tlas_instances_.push_back(instance);
    };

#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_) {
        const auto resolve_transform = [&](const std::uint32_t record) noexcept -> TransformRows {
            const auto& metadata = scene_metadata_[record];
            const auto& instance = editor_scene_instances_[record];
            return metadata.id
                ? runtime_transforms_.current(metadata.id)
                : TransformRows{instance.model_row_0, instance.model_row_1, instance.model_row_2};
        };
        for (std::uint32_t i = 0U; i < editor_ground_count_; ++i) {
            push_instance(
                resolve_transform(i),
                blas_plane_,
                0U,
                VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR);
        }
        for (std::uint32_t i = 0U; i < editor_cube_count_; ++i) {
            push_instance(
                resolve_transform(editor_ground_count_ + i),
                blas_cube_,
                0U,
                VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR);
        }
        for (const auto& draw : editor_model_draws_) {
            const auto& asset = editor_model_assets_[draw.asset_index];
            if (!asset.ready) {
                continue;
            }
            const auto& submesh = asset.submeshes[draw.submesh_index];
            VkGeometryInstanceFlagsKHR flags =
                VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR;
            if ((submesh.coverage & 2U) != 0U) {
                flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            }
            const std::uint32_t custom_index =
                (submesh.coverage & 1U) != 0U ? submesh.rt_opacity_first : 0U;
            for (std::uint32_t i = 0U; i < draw.instance_count; ++i) {
                push_instance(
                    resolve_transform(draw.first_instance + i),
                    submesh.blas,
                    custom_index,
                    flags);
            }
        }
        if (terrain_visible) {
            push_instance(
                TransformRows{},
                blas_terrain_,
                0U,
                VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR |
                    VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
        }
    } else
#endif
    {
        if (!chart_fixture && !demo_rt_transforms_.empty()) {
            push_instance(
                demo_rt_transforms_[0],
                blas_plane_,
                0U,
                VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR);
        }
        const std::size_t sphere_end = std::min<std::size_t>(
            demo_rt_transforms_.size(),
            1ULL + static_cast<std::size_t>(pbr_sample_instance_count_));
        for (std::size_t i = 1ULL; i < sphere_end; ++i) {
            push_instance(
                demo_rt_transforms_[i],
                blas_sphere_,
                0U,
                VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR);
        }
        if (terrain_visible) {
            push_instance(
                TransformRows{},
                blas_terrain_,
                0U,
                VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR |
                    VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
        }
    }

    const auto instance_count = static_cast<std::uint32_t>(rt_tlas_instances_.size());
    ensure_frame_tlas_capacity(frame, std::max(instance_count, 1U));
    if (!rt_tlas_instances_.empty()) {
        frame.tlas_instance_buffer.write(std::as_bytes(std::span{rt_tlas_instances_}));
    }
    frame.tlas_instance_count = instance_count;
    frame.tlas_scene_revision = scene_revision;
    frame.tlas_runtime_revision = runtime_revision;
    frame.tlas_terrain_visible = terrain_visible;
    rt_tlas_build_due_ = true;
    lighting_statistics_.tlas_instances = instance_count;
}

void Renderer::record_ray_tracing_tlas(const VkCommandBuffer command, FrameContext& frame) {
    frame.tlas_was_updated =
        rt_provisioned_ && rt_tlas_build_due_ && mode_ != RunMode::instancing;
    if (!frame.tlas_was_updated) {
        return;
    }

    rt_memory_barrier(
        command,
        VK_PIPELINE_STAGE_2_HOST_BIT,
        VK_ACCESS_2_HOST_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);

    const VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = VkAccelerationStructureGeometryDataKHR{
            .instances = VkAccelerationStructureGeometryInstancesDataKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                .arrayOfPointers = VK_FALSE,
                .data = {.deviceAddress = frame.tlas_instance_buffer.device_address()},
            },
        },
    };
    const VkAccelerationStructureBuildGeometryInfoKHR build_info{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .dstAccelerationStructure = frame.tlas.handle(),
        .geometryCount = 1U,
        .pGeometries = &geometry,
        .scratchData = {.deviceAddress = frame.tlas_scratch_buffer.device_address()},
    };
    const VkAccelerationStructureBuildRangeInfoKHR range{
        .primitiveCount = frame.tlas_instance_count,
    };
    const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
    vkCmdBuildAccelerationStructuresKHR(command, 1U, &build_info, &range_ptr);

    rt_memory_barrier(
        command,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    frame.tlas_built_once = true;
    rt_tlas_build_due_ = false;
    ++lighting_statistics_.tlas_builds;
}

void Renderer::refresh_ray_tracing_statistics() noexcept {
    if (!rt_provisioned_) {
        lighting_statistics_.blas_count = 0U;
        lighting_statistics_.tlas_instances = 0U;
        lighting_statistics_.acceleration_structure_bytes = 0ULL;
        lighting_statistics_.opacity_micromap_bytes = 0ULL;
        return;
    }
    std::uint32_t blas_count = 0U;
    std::uint64_t as_bytes = 0ULL;
    for (const auto* const blas : {&blas_cube_, &blas_plane_, &blas_sphere_, &blas_terrain_}) {
        if (*blas) {
            ++blas_count;
            as_bytes += blas->size_bytes();
        }
    }
#if VCR_EDITOR_ENABLED
    for (const auto& asset : editor_model_assets_) {
        for (const auto& submesh : asset.submeshes) {
            if (submesh.blas) {
                ++blas_count;
                as_bytes += submesh.blas.size_bytes();
            }
        }
    }
#endif
    for (const auto& frame : frames_) {
        if (frame.tlas) {
            as_bytes += frame.tlas.size_bytes();
        }
    }
    lighting_statistics_.blas_count = blas_count;
    lighting_statistics_.acceleration_structure_bytes = as_bytes;
    lighting_statistics_.opacity_micromap_bytes =
        static_cast<std::uint64_t>(rt_opacity_triangle_count_) * sizeof(RtMaskedTriangle) +
        static_cast<std::uint64_t>(rt_alpha_texel_words_used_) * sizeof(std::uint32_t);
}

#if VCR_EDITOR_ENABLED
std::uint32_t Renderer::find_or_upload_rt_alpha_map(
    const std::string_view path,
    const std::vector<std::uint32_t>* const words_if_new) {
    if (path.empty()) {
        return 0U;
    }
    auto found = std::ranges::find_if(
        rt_alpha_maps_, [&](const RtAlphaMapEntry& e) { return e.relative_path == path; });
    if (found == rt_alpha_maps_.end()) {
        if (words_if_new == nullptr || words_if_new->empty()) {
            return 0U;
        }
        rt_alpha_maps_.push_back(RtAlphaMapEntry{
            .relative_path = std::string(path),
            .gpu_word_offset = 0U,
            .cpu_words = *words_if_new,
        });
        found = rt_alpha_maps_.end() - 1;
    }
    if (!rt_provisioned_ || found->cpu_words.empty()) {
        return 0U;
    }
    if (found->gpu_word_offset != 0U) {
        return found->gpu_word_offset;
    }
    if (found->cpu_words.size() > rt_alpha_max_texel_words - rt_alpha_texel_words_used_) {
        std::fprintf(stderr, "warning: RT R8 alpha map cache reached 32-texture limit\n");
        return 0U;
    }
    const std::uint32_t word_offset = rt_alpha_texel_base_word + rt_alpha_texel_words_used_;
    if (asset_upload_.recording()) {
        rt_memory_barrier(
            asset_upload_.command_buffer(),
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    upload_to_device_buffer(
        rt_opacity_micromaps_,
        std::as_bytes(std::span{found->cpu_words}),
        static_cast<VkDeviceSize>(word_offset) * sizeof(std::uint32_t));
    found->gpu_word_offset = word_offset;
    rt_alpha_texel_words_used_ += static_cast<std::uint32_t>(found->cpu_words.size());
    refresh_ray_tracing_statistics();
    return word_offset;
}

std::uint32_t Renderer::append_rt_masked_triangles(
    const std::span<const RtMaskedTriangle> triangles) {
    if (!rt_provisioned_ || triangles.empty()) {
        return 0U;
    }
    if (triangles.size() > rt_opacity_max_triangles - rt_opacity_triangle_count_) {
        fatal("RT masked triangle cache exceeds 524,288 triangles; use --rt off");
    }
    const std::uint32_t first = rt_opacity_triangle_count_;
    if (asset_upload_.recording()) {
        rt_memory_barrier(
            asset_upload_.command_buffer(),
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    upload_to_device_buffer(
        rt_opacity_micromaps_,
        std::as_bytes(triangles),
        static_cast<VkDeviceSize>(first) * sizeof(RtMaskedTriangle));
    rt_opacity_triangle_count_ += static_cast<std::uint32_t>(triangles.size());
    refresh_ray_tracing_statistics();
    return first;
}

void Renderer::build_model_blas_on_upload(EditorModelAsset& asset) {
    if (!rt_provisioned_ || !asset.vertex_buffer || !asset.index_buffer ||
        asset.submeshes.empty()) {
        return;
    }
    const std::uint32_t vertex_count = static_cast<std::uint32_t>(
        asset.vertex_buffer.size() / sizeof(CookedModelVertex));
    if (vertex_count == 0U) {
        return;
    }
    const std::uint32_t max_vertex = vertex_count - 1U;
    const VkDeviceAddress vertex_address = asset.vertex_buffer.device_address();
    const VkDeviceAddress index_address = asset.index_buffer.device_address();
    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        context_->acceleration_structure_properties().minAccelerationStructureScratchOffsetAlignment,
        256ULL);

    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> build_infos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<VkDeviceSize> scratch_offsets;
    geometries.reserve(asset.submeshes.size());
    build_infos.reserve(asset.submeshes.size());
    ranges.reserve(asset.submeshes.size());
    scratch_offsets.reserve(asset.submeshes.size());

    VkDeviceSize total_scratch_bytes = 0ULL;
    for (auto& submesh : asset.submeshes) {
        const std::uint32_t triangle_count = submesh.index_count / 3U;
        if (triangle_count == 0U) {
            continue;
        }
        const VkGeometryFlagsKHR geom_flags =
            (submesh.coverage & 1U) == 0U ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0U;
        geometries.push_back(VkAccelerationStructureGeometryKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry = VkAccelerationStructureGeometryDataKHR{
                .triangles = VkAccelerationStructureGeometryTrianglesDataKHR{
                    .sType =
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                    .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                    .vertexData = {.deviceAddress = vertex_address},
                    .vertexStride = sizeof(CookedModelVertex),
                    .maxVertex = max_vertex,
                    .indexType = VK_INDEX_TYPE_UINT32,
                    .indexData = {.deviceAddress = index_address},
                },
            },
            .flags = geom_flags,
        });
        VkAccelerationStructureBuildGeometryInfoKHR build_info{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1U,
            .pGeometries = &geometries.back(),
        };
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
        };
        vkGetAccelerationStructureBuildSizesKHR(
            context_->device(),
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &build_info,
            &triangle_count,
            &sizes);

        submesh.blas.create(
            *context_,
            sizes.accelerationStructureSize,
            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            "imported model submesh BLAS",
            triangle_count);
        build_info.dstAccelerationStructure = submesh.blas.handle();
        build_infos.push_back(build_info);
        ranges.push_back(VkAccelerationStructureBuildRangeInfoKHR{
            .primitiveCount = triangle_count,
            .primitiveOffset = submesh.first_index * static_cast<std::uint32_t>(sizeof(std::uint32_t)),
            .firstVertex = 0U,
            .transformOffset = 0U,
        });
        scratch_offsets.push_back(total_scratch_bytes);
        total_scratch_bytes = align_up_device_size(
            total_scratch_bytes + std::max<VkDeviceSize>(sizes.buildScratchSize, 256ULL),
            scratch_alignment);
    }

    if (build_infos.empty()) {
        return;
    }

    if (rt_upload_scratch_.size() < total_scratch_bytes) {
        rt_upload_scratch_.create(
            *context_,
            std::max<VkDeviceSize>(total_scratch_bytes, 65'536ULL),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            0,
            "model BLAS upload scratch",
            scratch_alignment);
    }

    const VkDeviceAddress scratch_base = rt_upload_scratch_.device_address();
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> range_ptrs(build_infos.size());
    for (std::size_t i = 0ULL; i < build_infos.size(); ++i) {
        build_infos[i].pGeometries = &geometries[i];
        build_infos[i].scratchData.deviceAddress = scratch_base + scratch_offsets[i];
        range_ptrs[i] = &ranges[i];
    }

    std::optional<RtImmediateCommands> immediate;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (asset_upload_.recording()) {
        cmd = asset_upload_.command_buffer();
        rt_memory_barrier(
            cmd,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);
    } else {
        immediate.emplace(*context_);
        cmd = immediate->command_buffer();
    }

    vkCmdBuildAccelerationStructuresKHR(
        cmd,
        static_cast<std::uint32_t>(build_infos.size()),
        build_infos.data(),
        range_ptrs.data());
    rt_memory_barrier(
        cmd,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    if (immediate.has_value()) {
        immediate->submit_and_wait();
    }
    refresh_ray_tracing_statistics();
}

void Renderer::compact_model_blas(EditorModelAsset& asset) {
    if (!rt_provisioned_ || asset.submeshes.empty()) {
        return;
    }
    std::vector<std::size_t> submesh_indices;
    std::vector<VkAccelerationStructureKHR> handles;
    submesh_indices.reserve(asset.submeshes.size());
    handles.reserve(asset.submeshes.size());
    for (std::size_t i = 0ULL; i < asset.submeshes.size(); ++i) {
        if (asset.submeshes[i].blas && asset.submeshes[i].blas.primitive_count() > 0U) {
            submesh_indices.push_back(i);
            handles.push_back(asset.submeshes[i].blas.handle());
        }
    }
    if (handles.empty()) {
        return;
    }

    DeviceObject<VkQueryPool> query_pool{};
    const VkQueryPoolCreateInfo query_info{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
        .queryCount = static_cast<std::uint32_t>(handles.size()),
    };
    VK_CHECK(vkCreateQueryPool(
        context_->device(),
        &query_info,
        nullptr,
        query_pool.put(context_->device(), vkDestroyQueryPool)));

    {
        RtImmediateCommands commands(*context_);
        const VkCommandBuffer cmd = commands.command_buffer();
        vkCmdResetQueryPool(cmd, query_pool.get(), 0U, static_cast<std::uint32_t>(handles.size()));
        vkCmdWriteAccelerationStructuresPropertiesKHR(
            cmd,
            static_cast<std::uint32_t>(handles.size()),
            handles.data(),
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
            query_pool.get(),
            0U);
        commands.submit_and_wait();
    }

    std::vector<VkDeviceSize> compacted_sizes(handles.size(), 0ULL);
    VK_CHECK(vkGetQueryPoolResults(
        context_->device(),
        query_pool.get(),
        0U,
        static_cast<std::uint32_t>(handles.size()),
        compacted_sizes.size() * sizeof(VkDeviceSize),
        compacted_sizes.data(),
        sizeof(VkDeviceSize),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));

    std::vector<AccelerationStructure> compacted(handles.size());
    bool any_compacted = false;
    for (std::size_t i = 0ULL; i < handles.size(); ++i) {
        const auto& src = asset.submeshes[submesh_indices[i]].blas;
        if (compacted_sizes[i] > 0ULL && compacted_sizes[i] < src.size_bytes()) {
            compacted[i].create(
                *context_,
                compacted_sizes[i],
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                "compacted model submesh BLAS",
                src.primitive_count());
            any_compacted = true;
        }
    }
    if (!any_compacted) {
        return;
    }

    {
        RtImmediateCommands commands(*context_);
        const VkCommandBuffer cmd = commands.command_buffer();
        for (std::size_t i = 0ULL; i < handles.size(); ++i) {
            if (!compacted[i]) {
                continue;
            }
            const VkCopyAccelerationStructureInfoKHR copy_info{
                .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                .src = handles[i],
                .dst = compacted[i].handle(),
                .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR,
            };
            vkCmdCopyAccelerationStructureKHR(cmd, &copy_info);
        }
        rt_memory_barrier(
            cmd,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        commands.submit_and_wait();
    }

    for (std::size_t i = 0ULL; i < handles.size(); ++i) {
        if (compacted[i]) {
            asset.submeshes[submesh_indices[i]].blas = std::move(compacted[i]);
        }
    }
    refresh_ray_tracing_statistics();
}
#endif

} // namespace vcr
