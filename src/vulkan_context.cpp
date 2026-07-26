#include "vulkan_context.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace vcr {
namespace {

constexpr std::array kValidationLayers{"VK_LAYER_KHRONOS_validation"};
constexpr std::array kRequiredDeviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    [[maybe_unused]] const VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    [[maybe_unused]] void* user_data) {
    const char* label = "INFO";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U) {
        label = "ERROR";
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0U) {
        label = "WARN";
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) != 0U) {
        label = "VERBOSE";
    }
    std::cerr << "[Vulkan " << label << "] "
              << (callback_data->pMessage != nullptr ? callback_data->pMessage : "<no message>")
              << '\n';
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debug_create_info() {
    VkDebugUtilsMessengerCreateInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback,
    };
    return info;
}

bool layer_available(const char* name) {
    std::uint32_t count = 0;
    VK_CHECK(vkEnumerateInstanceLayerProperties(&count, nullptr));
    std::vector<VkLayerProperties> layers(count);
    VK_CHECK(vkEnumerateInstanceLayerProperties(&count, layers.data()));
    return std::ranges::any_of(layers, [name](const auto& layer) {
        return std::strcmp(layer.layerName, name) == 0;
    });
}

const char* device_type_name(const VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

std::uint64_t device_local_memory(const VkPhysicalDevice device) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    std::uint64_t bytes = 0;
    for (std::uint32_t index = 0; index < memory.memoryHeapCount; ++index) {
        if ((memory.memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U) {
            bytes += memory.memoryHeaps[index].size;
        }
    }
    return bytes;
}

} // namespace

VulkanContext::~VulkanContext() {
    shutdown();
}

void VulkanContext::initialize(SDL_Window* window, const bool validation_requested) {
    if (volkInitialize() != VK_SUCCESS) {
        fatal("Unable to load the Vulkan loader.");
    }
    create_instance(window, validation_requested);
    volkLoadInstance(instance_);
    create_debug_messenger();
    create_surface(window);
    select_physical_device();
    create_device();
    volkLoadDevice(device_);
    create_allocator();
}

void VulkanContext::shutdown() noexcept {
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (debug_messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

void VulkanContext::create_instance(SDL_Window* window, const bool validation_requested) {
    std::uint32_t supported_api = VK_API_VERSION_1_0;
    VK_CHECK(vkEnumerateInstanceVersion(&supported_api));
    if (supported_api < VK_API_VERSION_1_3) {
        fatal("Vulkan 1.3 is required.");
    }

    std::uint32_t sdl_extension_count = 0;
    const char* const* sdl_extensions =
        SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);
    if (sdl_extensions == nullptr || sdl_extension_count == 0) {
        fatal(SDL_GetError());
    }

    std::vector<const char*> extensions(
        sdl_extensions,
        sdl_extensions + sdl_extension_count);

    validation_enabled_ = validation_requested && layer_available(kValidationLayers[0]);
    if (validation_requested && !validation_enabled_) {
        std::cerr << "warning: validation requested, but VK_LAYER_KHRONOS_validation "
                     "is unavailable\n";
    }
    if (validation_enabled_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    const VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Vulkan Cube Benchmark",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "vcr",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };

    auto debug_info = debug_create_info();
    const VkInstanceCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = validation_enabled_ ? &debug_info : nullptr,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = validation_enabled_
            ? static_cast<std::uint32_t>(kValidationLayers.size())
            : 0U,
        .ppEnabledLayerNames = validation_enabled_ ? kValidationLayers.data() : nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    VK_CHECK(vkCreateInstance(&create_info, nullptr, &instance_));
    (void)window;
}

void VulkanContext::create_debug_messenger() {
    if (!validation_enabled_) {
        return;
    }
    const auto info = debug_create_info();
    VK_CHECK(vkCreateDebugUtilsMessengerEXT(
        instance_,
        &info,
        nullptr,
        &debug_messenger_));
}

void VulkanContext::create_surface(SDL_Window* window) {
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        fatal(SDL_GetError());
    }
}

QueueFamilySelection VulkanContext::find_queue_families(
    const VkPhysicalDevice device) const {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());

    QueueFamilySelection result{};
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& family = properties[index];
        if (family.queueCount > 0 &&
            (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U &&
            result.graphics == VK_QUEUE_FAMILY_IGNORED) {
            result.graphics = index;
        }

        VkBool32 present_supported = VK_FALSE;
        VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(
            device,
            index,
            surface_,
            &present_supported));
        if (family.queueCount > 0 &&
            present_supported == VK_TRUE &&
            result.present == VK_QUEUE_FAMILY_IGNORED) {
            result.present = index;
        }

        if (result.complete() && result.graphics == result.present) {
            break;
        }
    }
    return result;
}

bool VulkanContext::supports_required_device_features(
    const VkPhysicalDevice device,
    QueueFamilySelection& families) const {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        return false;
    }

    std::uint32_t extension_count = 0;
    VK_CHECK(vkEnumerateDeviceExtensionProperties(
        device,
        nullptr,
        &extension_count,
        nullptr));
    std::vector<VkExtensionProperties> available(extension_count);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(
        device,
        nullptr,
        &extension_count,
        available.data()));
    for (const char* required : kRequiredDeviceExtensions) {
        if (!std::ranges::any_of(available, [required](const auto& ext) {
                return std::strcmp(ext.extensionName, required) == 0;
            })) {
            return false;
        }
    }

    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceFeatures2 features2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features13,
    };
    vkGetPhysicalDeviceFeatures2(device, &features2);
    if (features13.dynamicRendering != VK_TRUE ||
        features13.synchronization2 != VK_TRUE) {
        return false;
    }

    families = find_queue_families(device);
    if (!families.complete()) {
        return false;
    }

    std::uint32_t format_count = 0;
    std::uint32_t present_mode_count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(
        device, surface_, &format_count, nullptr));
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
        device, surface_, &present_mode_count, nullptr));
    return format_count > 0 && present_mode_count > 0;
}

void VulkanContext::select_physical_device() {
    std::uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        fatal("No Vulkan physical devices found.");
    }
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));

    int best_score = std::numeric_limits<int>::min();
    for (const VkPhysicalDevice candidate : devices) {
        QueueFamilySelection families{};
        if (!supports_required_device_features(candidate, families)) {
            continue;
        }
        VkPhysicalDeviceProperties candidate_properties{};
        vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
        int score = static_cast<int>(
            candidate_properties.limits.maxImageDimension2D / 1024U);
        if (candidate_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 10'000;
        } else if (
            candidate_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 5'000;
        }
        if (score > best_score) {
            best_score = score;
            physical_device_ = candidate;
            queue_families_ = families;
            properties_ = candidate_properties;
        }
    }
    if (physical_device_ == VK_NULL_HANDLE) {
        fatal("No device supports Vulkan 1.3, swapchain, dynamic rendering, and sync2.");
    }

    std::cout << "Selected GPU: " << properties_.deviceName << '\n'
              << "  type: " << device_type_name(properties_.deviceType) << '\n'
              << "  Vulkan API: "
              << VK_API_VERSION_MAJOR(properties_.apiVersion) << '.'
              << VK_API_VERSION_MINOR(properties_.apiVersion) << '.'
              << VK_API_VERSION_PATCH(properties_.apiVersion) << '\n'
              << "  driver version: " << properties_.driverVersion << '\n'
              << "  device-local memory: "
              << std::fixed << std::setprecision(2)
              << static_cast<double>(device_local_memory(physical_device_)) /
                     (1024.0 * 1024.0 * 1024.0)
              << " GiB\n"
              << "  graphics queue family: " << queue_families_.graphics << '\n'
              << "  present queue family: " << queue_families_.present << '\n'
              << "  extensions: " << VK_KHR_SWAPCHAIN_EXTENSION_NAME << '\n'
              << "  features: dynamicRendering, synchronization2\n";

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device_, &family_count, families.data());
    for (std::uint32_t index = 0; index < family_count; ++index) {
        std::cout << "  queue[" << index << "]: count=" << families[index].queueCount
                  << " flags=0x" << std::hex << families[index].queueFlags
                  << std::dec << '\n';
    }
    timestamp_valid_bits_ =
        families[queue_families_.graphics].timestampValidBits;
    std::cout << "  timestamps: " << timestamp_valid_bits_
              << " valid bits, period " << properties_.limits.timestampPeriod
              << " ns\n";

    std::uint32_t mode_count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device_, surface_, &mode_count, nullptr));
    std::vector<VkPresentModeKHR> modes(mode_count);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device_, surface_, &mode_count, modes.data()));
    std::cout << "  present modes:";
    for (const auto mode : modes) {
        std::cout << ' ' << static_cast<int>(mode);
    }
    std::cout << '\n' << std::flush;
}

void VulkanContext::create_device() {
    const float priority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    const std::set unique_families{
        queue_families_.graphics,
        queue_families_.present,
    };
    queue_infos.reserve(unique_families.size());
    for (const std::uint32_t family : unique_families) {
        queue_infos.push_back(VkDeviceQueueCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        });
    }

    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    const VkPhysicalDeviceFeatures2 features2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features13,
    };
    const VkDeviceCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features2,
        .queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size()),
        .pQueueCreateInfos = queue_infos.data(),
        .enabledExtensionCount =
            static_cast<std::uint32_t>(kRequiredDeviceExtensions.size()),
        .ppEnabledExtensionNames = kRequiredDeviceExtensions.data(),
    };
    VK_CHECK(vkCreateDevice(physical_device_, &create_info, nullptr, &device_));
    vkGetDeviceQueue(device_, queue_families_.graphics, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, queue_families_.present, 0, &present_queue_);
}

void VulkanContext::create_allocator() {
    const VmaVulkanFunctions functions{
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
    };
    const VmaAllocatorCreateInfo info{
        .physicalDevice = physical_device_,
        .device = device_,
        .pVulkanFunctions = &functions,
        .instance = instance_,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    VK_CHECK(vmaCreateAllocator(&info, &allocator_));
}

void VulkanContext::set_debug_name(
    const VkObjectType type,
    const std::uint64_t handle,
    const char* name) const noexcept {
    if (!validation_enabled_ || device_ == VK_NULL_HANDLE ||
        vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }
    const VkDebugUtilsObjectNameInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = type,
        .objectHandle = handle,
        .pObjectName = name,
    };
    (void)vkSetDebugUtilsObjectNameEXT(device_, &info);
}

} // namespace vcr
