#pragma once
#include "device_object.hpp"
#include "gpu_buffer.hpp"
#include <glm/vec3.hpp>
#include <vector>

namespace vcr {
struct FoliageProxyRender {
    GpuBuffer volume;
    GpuBuffer parameters;
    DeviceObject<VkDescriptorSetLayout> set_layout;
    DeviceObject<VkDescriptorPool> pool;
    DeviceObject<VkPipelineLayout> layout;
    DeviceObject<VkPipeline> pipeline;
    VkDescriptorSet descriptors{};
    VkDescriptorSetLayout lighting_layout{}; // Observer of the active engine layout.
    VkFormat depth_format{};
    glm::vec3 origin{}, bounds_low{}, bounds_high{};
    std::vector<std::uint32_t> member_generations;
    float cell_size{}, max_cell_pixels{};
    std::uint32_t resolution{}, phase{}, source_records{};
    std::uint64_t scene_revision{};
    bool far{};
    VkRect2D scissor{};
};
} // namespace vcr
