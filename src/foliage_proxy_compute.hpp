#pragma once
#include "gpu_buffer.hpp"
#include "device_object.hpp"
#include <filesystem>
#include <span>

namespace vcr::foliage {
// Owns one immutable descriptor set for an entire bake or preview submission.
class ComputeBindings {
  public:
    ComputeBindings(VulkanContext& context, std::span<const VkDescriptorBufferInfo> buffers,
                    std::uint32_t push_bytes);
    [[nodiscard]] DeviceObject<VkPipeline> pipeline(const std::filesystem::path& shader) const;
    void bind(VkCommandBuffer command, VkPipeline pipeline) const;
    void push(VkCommandBuffer command, std::span<const std::byte> constants) const;

  private:
    VulkanContext& context_;
    DeviceObject<VkDescriptorSetLayout> set_layout_;
    DeviceObject<VkDescriptorPool> pool_;
    DeviceObject<VkPipelineLayout> layout_;
    VkDescriptorSet set_{};
    std::uint32_t push_bytes_{};
};
void compute_barrier(VkCommandBuffer command, VkPipelineStageFlags2 from, VkAccessFlags2 writes,
                     VkPipelineStageFlags2 to, VkAccessFlags2 reads);
} // namespace vcr::foliage
