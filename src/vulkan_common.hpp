#pragma once

#include <volk.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <cstdint>

namespace vcr {

[[noreturn]] inline void fatal(const std::string_view message) {
    // Failure is outside the successful hot path. Both executables catch at
    // main(), allowing application/device resources to unwind first.
    throw std::runtime_error(std::string(message));
}

inline void check_vk(
    const VkResult result,
    const char* expression,
    const char* file,
    const int line) {
    if (result != VK_SUCCESS) {
        std::cerr << "Vulkan call failed: " << expression << " -> "
                  << static_cast<int>(result) << " at " << file << ':' << line << '\n';
        throw std::runtime_error(
            std::string(expression) + " failed with VkResult " +
            std::to_string(static_cast<int>(result)));
    }
}

template <typename Handle>
[[nodiscard]] std::uint64_t handle_value(const Handle handle) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

} // namespace vcr

#define VK_CHECK(expression) ::vcr::check_vk((expression), #expression, __FILE__, __LINE__)
