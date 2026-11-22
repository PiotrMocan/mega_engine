#include "foliage_proxy_compute.hpp"
#include "foliage_proxy_bake.hpp"
#include "upload_batch.hpp"
#include <algorithm>
#include <fstream>
#include <vector>

namespace vcr::foliage {
ComputeBindings::ComputeBindings(VulkanContext& context, std::span<const VkDescriptorBufferInfo> buffers,
                                 std::uint32_t push_bytes)
    : context_(context), push_bytes_(push_bytes) {
    const auto& caps = context.properties().limits;
    const auto count = static_cast<std::uint32_t>(buffers.size());
    if (!count || count > caps.maxPerStageDescriptorStorageBuffers ||
        count > caps.maxDescriptorSetStorageBuffers || push_bytes > caps.maxPushConstantsSize ||
        caps.maxComputeWorkGroupSize[0] < 64 || caps.maxComputeWorkGroupInvocations < 64)
        throw std::runtime_error("Foliage compute exceeds device limits");
    const auto device = context.device();
    std::vector<VkDescriptorSetLayoutBinding> bindings(count);
    for (unsigned i = 0; i < count; ++i) {
        if (!buffers[i].range || buffers[i].range > caps.maxStorageBufferRange)
            throw std::length_error("Foliage SSBO range");
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    const VkDescriptorSetLayoutCreateInfo set_info{.sType =
                                                       VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .bindingCount = count,
                                                   .pBindings = bindings.data()};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &set_info, nullptr,
                                         set_layout_.put(device, vkDestroyDescriptorSetLayout)));
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count};
    const VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                               .maxSets = 1,
                                               .poolSizeCount = 1,
                                               .pPoolSizes = &size};
    VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr, pool_.put(device, vkDestroyDescriptorPool)));
    const auto set_layout = set_layout_.get();
    const VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = pool_.get(),
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &set_layout};
    VK_CHECK(vkAllocateDescriptorSets(device, &alloc, &set_));
    std::vector<VkWriteDescriptorSet> updates(count);
    for (unsigned i = 0; i < count; ++i)
        updates[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                      .dstSet = set_,
                      .dstBinding = i,
                      .descriptorCount = 1,
                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(device, count, updates.data(), 0, nullptr);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    const VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                 .setLayoutCount = 1,
                                                 .pSetLayouts = &set_layout,
                                                 .pushConstantRangeCount = 1,
                                                 .pPushConstantRanges = &range};
    VK_CHECK(
        vkCreatePipelineLayout(device, &layout_info, nullptr, layout_.put(device, vkDestroyPipelineLayout)));
}
DeviceObject<VkPipeline> ComputeBindings::pipeline(const std::filesystem::path& path) const {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    const auto bytes = static_cast<std::streamoff>(f.tellg());
    if (bytes < 20 || bytes > 4 * 1024 * 1024 || bytes % 4)
        throw std::runtime_error("Invalid foliage shader: " + path.string());
    std::vector<std::uint32_t> code(static_cast<std::size_t>(bytes) / 4);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(code.data()), bytes))
        throw std::runtime_error("Unable to read foliage shader");
    const auto device = context_.device();
    ShaderModule shader;
    const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = code.size() * 4,
                                        .pCode = code.data()};
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, shader.put(device, vkDestroyShaderModule)));
    const VkComputePipelineCreateInfo compute{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader.get(),
                  .pName = "main"},
        .layout = layout_.get()};
    DeviceObject<VkPipeline> result;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr,
                                      result.put(device, vkDestroyPipeline)));
    context_.set_debug_name(VK_OBJECT_TYPE_PIPELINE, handle_value(result.get()),
                            path.filename().string().c_str());
    return result;
}
void ComputeBindings::bind(VkCommandBuffer command, VkPipeline pipeline) const {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout_.get(), 0, 1, &set_, 0, nullptr);
}
void ComputeBindings::push(VkCommandBuffer cmd, std::span<const std::byte> constants) const {
    if (constants.size() != push_bytes_)
        throw std::invalid_argument("Foliage push constant layout mismatch");
    vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes_, constants.data());
}
void compute_barrier(VkCommandBuffer command, VkPipelineStageFlags2 from, VkAccessFlags2 writes,
                     VkPipelineStageFlags2 to, VkAccessFlags2 reads) {
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = from,
                                   .srcAccessMask = writes,
                                   .dstStageMask = to,
                                   .dstAccessMask = reads};
    const VkDependencyInfo dep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(command, &dep);
}

Tile bake_tile(VulkanContext& context, const BakeInput& input, const std::filesystem::path& shaders) {
    const auto n = input.resolution;
    if (!valid_resolution(n) || input.draws.empty())
        throw std::invalid_argument("Invalid foliage bake");
    std::uint32_t cell_count = 0;
    for (auto m = n; m; m /= 2)
        cell_count += m * m * m;
    const auto& limits = context.properties().limits;
    if ((n * n * n + 63) / 64 > limits.maxComputeWorkGroupCount[0] ||
        std::uint64_t(n) * n * n * accumulator_bytes > limits.maxStorageBufferRange)
        throw std::length_error("Foliage grid exceeds device limits");
    std::array<GpuBuffer, 6> buffers;
    const auto upload = [&](unsigned binding, auto span, const char* label) {
        if (span.empty() || span.size_bytes() > limits.maxStorageBufferRange)
            throw std::length_error("Foliage input buffer exceeds device limits");
        buffers[binding].create(
            context, span.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, label);
        buffers[binding].write(std::as_bytes(span));
    };
    upload(0, std::span(input.vertices), "foliage source vertices");
    upload(1, std::span(input.indices), "foliage source indices");
    upload(2, std::span(input.pixels), "foliage material texels");
    upload(3, std::span(input.draws), "foliage tile draws");
    buffers[4].create(context, std::uint64_t(n) * n * n * accumulator_bytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "foliage optical accumulators");
    buffers[5].create(context, cell_count * sizeof(Cell), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                      "foliage resolved mip chain");
    std::array<VkDescriptorBufferInfo, 6> desc{};
    for (unsigned i = 0; i < desc.size(); ++i)
        desc[i] = {buffers[i].handle(), 0, buffers[i].size()};
    struct Constants {
        glm::vec4 origin_cell;
        glm::uvec4 grid;
    };
    Constants pc{{input.origin[0], input.origin[1], input.origin[2], input.cell_size}, {}};
    ComputeBindings bindings(context, desc, sizeof(pc));
    auto bake = bindings.pipeline(shaders / "foliage_proxy_bake.comp.spv");
    auto mip = bindings.pipeline(shaders / "foliage_proxy_mip.comp.spv");
    UploadBatch submission;
    submission.begin(context, 256);
    auto cmd = submission.command_buffer();
    vkCmdFillBuffer(cmd, buffers[4].handle(), 0, VK_WHOLE_SIZE, 0);
    constexpr auto compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto rw = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    compute_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, compute, rw);
    bindings.bind(cmd, bake.get());
    for (unsigned i = 0; i < input.draws.size(); ++i) {
        const auto triangles = input.draws[i].range.y;
        const auto batch = std::min(limits.maxComputeWorkGroupCount[0], 4096U) * 64U;
        for (unsigned first = 0; first < triangles; first += batch) {
            if (i || first)
                compute_barrier(cmd, compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, compute, rw);
            pc.grid = {n, i, first, 0};
            bindings.push(cmd, std::as_bytes(std::span{&pc, 1}));
            vkCmdDispatch(cmd, (std::min(batch, triangles - first) + 63) / 64, 1, 1);
        }
    }
    bindings.bind(cmd, mip.get());
    unsigned offset = 0, previous = 0;
    for (auto m = n; m; m /= 2) {
        compute_barrier(cmd, compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, compute, rw);
        pc.grid = {m, offset, previous, m == n ? 0U : 1U};
        bindings.push(cmd, std::as_bytes(std::span{&pc, 1}));
        vkCmdDispatch(cmd, (m * m * m + 63) / 64, 1, 1);
        previous = offset;
        offset += m * m * m;
    }
    compute_barrier(cmd, compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                    VK_ACCESS_2_HOST_READ_BIT);
    submission.submit();
    submission.wait();
    const auto bytes = buffers[5].read_completed_bytes(VK_WHOLE_SIZE);
    std::vector<Cell> cells(cell_count);
    std::memcpy(cells.data(), bytes.data(), bytes.size());
    return pack_tile(cells, n, input.origin, input.cell_size);
}
} // namespace vcr::foliage
