#include "fsr3_upscaler.hpp"

#include "vulkan_common.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>

namespace vcr {
namespace {

#if VCR_HAS_FSR3
constexpr std::string_view bundled_fsr_version = "FSR 3.1.4";

void fsr_message(const std::uint32_t type, const wchar_t* const message) {
    if (message == nullptr) {
        return;
    }
    std::wcerr
        << (type == FFX_API_MESSAGE_TYPE_ERROR
                ? L"FidelityFX error: "
                : L"FidelityFX warning: ")
        << message << L'\n';
}

template <typename Function>
[[nodiscard]] Function load_function(
    SDL_SharedObject* const library,
    const char* const name) noexcept {
    return reinterpret_cast<Function>(SDL_LoadFunction(library, name));
}
#endif

} // namespace

Fsr3Upscaler::~Fsr3Upscaler() {
    shutdown();
}

void Fsr3Upscaler::initialize(
    VulkanContext& context,
    const VkExtent2D display_extent) {
    shutdown();
    reset_history_ = true;

#if VCR_HAS_FSR3
    if (!context.fsr3_device_compatible()) {
        unavailable_reason_ =
            "the device lacks required FidelityFX Vulkan features or extensions";
        return;
    }
    if (display_extent.width == 0U || display_extent.height == 0U) {
        unavailable_reason_ = "the display extent is empty";
        return;
    }

    const char* const base_path = SDL_GetBasePath();
    if (base_path == nullptr) {
        unavailable_reason_ = SDL_GetError();
        return;
    }
    const std::filesystem::path library_path =
        std::filesystem::path(base_path) / "amd_fidelityfx_vk.dll";
    const std::string library_path_string = library_path.string();
    library_ = SDL_LoadObject(library_path_string.c_str());
    if (library_ == nullptr) {
        unavailable_reason_ =
            "unable to load amd_fidelityfx_vk.dll: ";
        unavailable_reason_.append(SDL_GetError());
        return;
    }
    create_context_ =
        load_function<PfnFfxCreateContext>(library_, "ffxCreateContext");
    destroy_context_ =
        load_function<PfnFfxDestroyContext>(library_, "ffxDestroyContext");
    query_ = load_function<PfnFfxQuery>(library_, "ffxQuery");
    dispatch_ = load_function<PfnFfxDispatch>(library_, "ffxDispatch");
    if (create_context_ == nullptr ||
        destroy_context_ == nullptr ||
        query_ == nullptr ||
        dispatch_ == nullptr) {
        unavailable_reason_ =
            "the FidelityFX DLL is missing required ffx-api exports";
        shutdown();
        return;
    }
    backend_description_ = {};
    backend_description_.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend_description_.vkDevice = context.device();
    backend_description_.vkPhysicalDevice = context.physical_device();
    backend_description_.vkDeviceProcAddr = vkGetDeviceProcAddr;

    context_description_ = {};
    context_description_.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    context_description_.header.pNext = &backend_description_.header;
    context_description_.flags =
        FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
        FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
        FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    if (context.validation_enabled()) {
        context_description_.flags |=
            FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        context_description_.fpMessage = fsr_message;
    }
    context_description_.maxRenderSize = {
        display_extent.width,
        display_extent.height,
    };
    context_description_.maxUpscaleSize = {
        display_extent.width,
        display_extent.height,
    };

    const ffxReturnCode_t create_result = create_context_(
        &api_context_,
        &context_description_.header,
        nullptr);
    if (create_result != FFX_API_RETURN_OK) {
        api_context_ = nullptr;
        unavailable_reason_ =
            "ffxCreateContext failed with code " +
            std::to_string(create_result);
        shutdown();
        return;
    }

    ffxQueryGetProviderVersion provider_version{};
    provider_version.header.type =
        FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const ffxReturnCode_t query_result =
        query_(&api_context_, &provider_version.header);
    version_ = std::string(bundled_fsr_version);
    const std::string_view provider_name =
        query_result == FFX_API_RETURN_OK &&
                provider_version.versionName != nullptr
            ? std::string_view{provider_version.versionName}
            : std::string_view{};
    if (!provider_name.empty() &&
        provider_name != "3.1.4" &&
        provider_name != bundled_fsr_version) {
        version_.append(" (");
        version_.append(provider_name);
        version_.push_back(')');
    }
    unavailable_reason_.clear();
    std::cout << "FidelityFX upscaler: " << version_ << '\n';
#else
    static_cast<void>(context);
    static_cast<void>(display_extent);
#endif
}

void Fsr3Upscaler::shutdown() noexcept {
#if VCR_HAS_FSR3
    if (api_context_ != nullptr && destroy_context_ != nullptr) {
        const ffxReturnCode_t result =
            destroy_context_(&api_context_, nullptr);
        if (result != FFX_API_RETURN_OK) {
            std::fprintf(stderr, "warning: ffxDestroyContext failed with code %u\n",
                         static_cast<unsigned>(result));
        }
        api_context_ = nullptr;
    }
    if (library_ != nullptr) {
        SDL_UnloadObject(library_);
        library_ = nullptr;
    }
    create_context_ = nullptr;
    destroy_context_ = nullptr;
    query_ = nullptr;
    dispatch_ = nullptr;
    backend_description_ = {};
    context_description_ = {};
#endif
    // Accessors supply literal fallback text; shutdown never allocates it.
    version_.clear();
    reset_history_ = true;
}

VkExtent2D Fsr3Upscaler::render_extent(
    const Fsr3Quality quality,
    const VkExtent2D display_extent) const noexcept {
    if (display_extent.width == 0U || display_extent.height == 0U) {
        return {};
    }
#if VCR_HAS_FSR3
    if (available() && query_ != nullptr) {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query{};
        query.header.type =
            FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        query.displayWidth = display_extent.width;
        query.displayHeight = display_extent.height;
        query.qualityMode = quality_mode(quality);
        query.pOutRenderWidth = &width;
        query.pOutRenderHeight = &height;
        if (query_(
                const_cast<ffxContext*>(&api_context_),
                &query.header) == FFX_API_RETURN_OK &&
            width > 0U && height > 0U) {
            return VkExtent2D{width, height};
        }
    }
#endif
    const float scale = fsr3_scale(quality);
    return VkExtent2D{
        std::max(
            1U,
            static_cast<std::uint32_t>(
                std::ceil(static_cast<float>(display_extent.width) / scale))),
        std::max(
            1U,
            static_cast<std::uint32_t>(
                std::ceil(static_cast<float>(display_extent.height) / scale))),
    };
}

Fsr3Jitter Fsr3Upscaler::jitter(
    const std::uint32_t frame_index,
    const std::uint32_t render_width,
    const std::uint32_t display_width) const noexcept {
#if VCR_HAS_FSR3
    if (!available() || query_ == nullptr ||
        render_width == 0U || display_width == 0U) {
        return {};
    }
    std::int32_t phase_count = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount phase_query{};
    phase_query.header.type =
        FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    phase_query.renderWidth = render_width;
    phase_query.displayWidth = display_width;
    phase_query.pOutPhaseCount = &phase_count;
    if (query_(
            const_cast<ffxContext*>(&api_context_),
            &phase_query.header) != FFX_API_RETURN_OK ||
        phase_count <= 0) {
        return {};
    }

    Fsr3Jitter result{};
    ffxQueryDescUpscaleGetJitterOffset jitter_query{};
    jitter_query.header.type =
        FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    jitter_query.index = static_cast<std::int32_t>(
        frame_index % static_cast<std::uint32_t>(phase_count));
    jitter_query.phaseCount = phase_count;
    jitter_query.pOutX = &result.x;
    jitter_query.pOutY = &result.y;
    if (query_(
            const_cast<ffxContext*>(&api_context_),
            &jitter_query.header) != FFX_API_RETURN_OK) {
        return {};
    }
    return result;
#else
    static_cast<void>(frame_index);
    static_cast<void>(render_width);
    static_cast<void>(display_width);
    return {};
#endif
}

void Fsr3Upscaler::dispatch(const Fsr3Dispatch& description) {
#if VCR_HAS_FSR3
    if (!available() || dispatch_ == nullptr) {
        fatal("FSR 3 dispatch requested without an available context.");
    }
    if (description.command_buffer == VK_NULL_HANDLE ||
        description.color == nullptr ||
        description.depth == nullptr ||
        description.motion_vectors == nullptr ||
        description.output == nullptr ||
        description.render_extent.width == 0U ||
        description.render_extent.height == 0U ||
        description.display_extent.width == 0U ||
        description.display_extent.height == 0U) {
        fatal("FSR 3 dispatch description is incomplete.");
    }

    ffxDispatchDescUpscale dispatch_description{};
    dispatch_description.header.type =
        FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch_description.commandList =
        reinterpret_cast<void*>(description.command_buffer);
    dispatch_description.color = wrap_resource(
        *description.color,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch_description.depth = wrap_resource(
        *description.depth,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch_description.motionVectors = wrap_resource(
        *description.motion_vectors,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch_description.output = wrap_resource(
        *description.output,
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto pixel_jitter = vulkan_pixel_jitter(description.jitter);
    dispatch_description.jitterOffset = {pixel_jitter.x, pixel_jitter.y};
    const auto motion_scale = ndc_motion_to_pixel_scale(
        description.render_extent.width, description.render_extent.height);
    dispatch_description.motionVectorScale = {
        motion_scale.x, motion_scale.y,
    };
    dispatch_description.renderSize = {
        description.render_extent.width,
        description.render_extent.height,
    };
    dispatch_description.upscaleSize = {
        description.display_extent.width,
        description.display_extent.height,
    };
    dispatch_description.enableSharpening = true;
    dispatch_description.sharpness = 0.2F;
    dispatch_description.frameTimeDelta =
        std::clamp(description.frame_time_ms, 0.001F, 1000.0F);
    dispatch_description.preExposure = 1.0F;
    dispatch_description.reset = reset_history_;
    dispatch_description.cameraNear = 0.1F;
    dispatch_description.cameraFar = 500.0F;
    dispatch_description.cameraFovAngleVertical =
        65.0F * 3.14159265358979323846F / 180.0F;
    dispatch_description.viewSpaceToMetersFactor = 1.0F;

    const ffxReturnCode_t result = dispatch_(
        &api_context_,
        &dispatch_description.header);
    if (result != FFX_API_RETURN_OK) {
        std::cerr
            << "FidelityFX FSR 3 dispatch failed with code "
            << result << '\n';
        fatal("FSR 3 upscaling dispatch failed.");
    }
    reset_history_ = false;
#else
    static_cast<void>(description);
    fatal("FSR 3 is unavailable in this build.");
#endif
}

#if VCR_HAS_FSR3
std::uint32_t Fsr3Upscaler::quality_mode(
    const Fsr3Quality quality) noexcept {
    switch (quality) {
    case Fsr3Quality::native_aa:
        return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    case Fsr3Quality::quality:
        return FFX_UPSCALE_QUALITY_MODE_QUALITY;
    case Fsr3Quality::balanced:
        return FFX_UPSCALE_QUALITY_MODE_BALANCED;
    case Fsr3Quality::performance:
        return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    case Fsr3Quality::ultra_performance:
        return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
    }
    return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
}

FfxApiResource Fsr3Upscaler::wrap_resource(
    const GpuTexture& texture,
    const std::uint32_t state) const noexcept {
    return ffxApiGetResourceVK(
        reinterpret_cast<void*>(texture.image()),
        ffxApiGetImageResourceDescriptionVK(
            texture.image(),
            texture.image_create_info(),
            0),
        state);
}
#endif

} // namespace vcr
