#pragma once
#include "device_object.hpp"
#include "gpu_buffer.hpp"
#include <optional>
#include <functional>

namespace vcr {
// Submission-thread owned, reusable command pool/fence and mapped arena.
// Arena reset and resource publication occur only after this batch's fence.
// Destination resources are owned by the renderer cache, which outlives stop().
class UploadBatch final {
  public:
    ~UploadBatch() noexcept {
        stop();
    }
    UploadBatch() = default;
    UploadBatch(const UploadBatch &) = delete;
    UploadBatch &operator=(const UploadBatch &) = delete;
    void begin(VulkanContext &context, std::size_t capacity);
    [[nodiscard]] VkDeviceSize stage(std::span<const std::byte> bytes);
    void submit();
    [[nodiscard]] bool complete();
    void wait();
    void stop() noexcept;
    [[nodiscard]] bool recording() const noexcept {
        return recording_;
    }
    [[nodiscard]] bool pending() const noexcept {
        return pending_;
    }
    [[nodiscard]] VkCommandBuffer command_buffer() const noexcept {
        return command_;
    }
    [[nodiscard]] VkBuffer buffer() const noexcept {
        return staging_.handle();
    }
    [[nodiscard]] std::size_t high_water() const noexcept {
        return high_water_;
    }
    [[nodiscard]] std::uint64_t submissions() const noexcept {
        return submissions_;
    }

  private:
    std::optional<std::reference_wrapper<VulkanContext>> context_;
    DeviceObject<VkCommandPool> pool_;
    DeviceObject<VkFence> fence_;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    GpuBuffer staging_;
    std::size_t offset_ = 0, high_water_ = 0;
    std::uint64_t submissions_ = 0;
    bool recording_ = false, pending_ = false;
};
} // namespace vcr
