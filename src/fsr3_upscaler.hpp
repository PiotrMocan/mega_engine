#pragma once

#include "anti_aliasing.hpp"
#include "temporal_coordinates.hpp"
#include "gpu_texture.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL_loadso.h>

#include <cstdint>
#include <string>
#include <string_view>

#ifndef VCR_HAS_FSR3
#define VCR_HAS_FSR3 0
#endif

#if VCR_HAS_FSR3
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#endif

namespace vcr {

using Fsr3Jitter = TemporalOffset;

struct Fsr3Dispatch {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    const GpuTexture* color = nullptr;
    const GpuTexture* depth = nullptr;
    const GpuTexture* motion_vectors = nullptr;
    const GpuTexture* output = nullptr;
    VkExtent2D render_extent{};
    VkExtent2D display_extent{};
    Fsr3Jitter jitter{};
    float frame_time_ms = 16.6667F;
};

class Fsr3Upscaler final {
public:
    Fsr3Upscaler() = default;
    ~Fsr3Upscaler();

    Fsr3Upscaler(const Fsr3Upscaler&) = delete;
    Fsr3Upscaler& operator=(const Fsr3Upscaler&) = delete;
    Fsr3Upscaler(Fsr3Upscaler&&) = delete;
    Fsr3Upscaler& operator=(Fsr3Upscaler&&) = delete;

    void initialize(
        VulkanContext& context,
        VkExtent2D display_extent);
    void shutdown() noexcept;

    [[nodiscard]] bool available() const noexcept {
#if VCR_HAS_FSR3
        return api_context_ != nullptr;
#else
        return false;
#endif
    }
    [[nodiscard]] std::string_view version() const noexcept {
        return available() ? std::string_view(version_) : "FSR 3 unavailable";
    }
    [[nodiscard]] std::string_view unavailable_reason() const noexcept {
        return unavailable_reason_.empty() && !available()
            ? "the FidelityFX Vulkan context is not initialized"
            : std::string_view(unavailable_reason_);
    }
    [[nodiscard]] VkExtent2D render_extent(
        Fsr3Quality quality,
        VkExtent2D display_extent) const noexcept;
    [[nodiscard]] Fsr3Jitter jitter(
        std::uint32_t frame_index,
        std::uint32_t render_width,
        std::uint32_t display_width) const noexcept;

    void dispatch(const Fsr3Dispatch& description);
    void request_reset() noexcept { reset_history_ = true; }

private:
#if VCR_HAS_FSR3
    [[nodiscard]] static std::uint32_t quality_mode(
        Fsr3Quality quality) noexcept;
    [[nodiscard]] FfxApiResource wrap_resource(
        const GpuTexture& texture,
        std::uint32_t state) const noexcept;

    // Both are opaque C handles whose ownership is contained by this RAII
    // object. Descriptor storage remains alive for the entire FFX context,
    // as required by the ffxCreateContext contract.
    SDL_SharedObject* library_ = nullptr;
    ffxContext api_context_ = nullptr;
    PfnFfxCreateContext create_context_ = nullptr;
    PfnFfxDestroyContext destroy_context_ = nullptr;
    PfnFfxQuery query_ = nullptr;
    PfnFfxDispatch dispatch_ = nullptr;
    ffxCreateBackendVKDesc backend_description_{};
    ffxCreateContextDescUpscale context_description_{};
#endif
    std::string version_{"FSR 3 unavailable"};
    std::string unavailable_reason_{
        "this build does not include the FidelityFX Vulkan backend"};
    bool reset_history_ = true;
};

} // namespace vcr
