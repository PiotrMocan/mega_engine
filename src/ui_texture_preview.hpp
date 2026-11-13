#pragma once

#include "gpu_texture.hpp"
#include "icon_system.hpp"
#include "vulkan_context.hpp"

#include <imgui.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace vcr {

/// Move-only RAII GPU texture and Dear ImGui descriptor registration for UI previews.
class UiTexturePreview final {
public:
    UiTexturePreview() = default;
    ~UiTexturePreview();

    UiTexturePreview(const UiTexturePreview&) = delete;
    UiTexturePreview& operator=(const UiTexturePreview&) = delete;
    UiTexturePreview(UiTexturePreview&& other) noexcept;
    UiTexturePreview& operator=(UiTexturePreview&& other) noexcept;

    /// Uploads an RGBA8 image into a device-local sampled Vulkan texture and registers it with ImGui.
    [[nodiscard]] bool upload(
        VulkanContext& context,
        VkCommandPool upload_command_pool,
        const IconRgbaImage& image,
        std::string_view debug_name,
        std::string& error,
        bool imgui_backend_alive = true);

    /// Releases the ImGui descriptor set (if backend is still alive) and destroys the GPU texture.
    void destroy(bool imgui_backend_alive = true) noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return descriptor_set_ != VK_NULL_HANDLE &&
               texture_.image() != VK_NULL_HANDLE &&
               texture_.view() != VK_NULL_HANDLE;
    }

    [[nodiscard]] ImTextureID imgui_id() const noexcept {
        return static_cast<ImTextureID>(
            reinterpret_cast<std::uintptr_t>(descriptor_set_));
    }

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    GpuTexture texture_{};
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
};

} // namespace vcr
