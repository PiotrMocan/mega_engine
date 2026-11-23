#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vcr {
// The mono FLAC bank contains three independent takes, each with a full-length decay.
inline constexpr std::uint32_t thunder_take_count = 3;
inline constexpr std::size_t thunder_take_seconds = 16;
inline constexpr std::uint32_t thunder_recording_rate = 24'000;
std::span<const unsigned char> embedded_thunder_recordings() noexcept;
std::span<const unsigned char> embedded_rain_recordings() noexcept;
inline constexpr std::size_t rain_loop_seconds = 16;
} // namespace vcr
