#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vcr {

struct PngEncodeResult {
    std::vector<std::uint8_t> bytes;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

// Encodes tightly packed, top-to-bottom RGBA8 pixels. The encoder uses
// standards-compliant uncompressed DEFLATE blocks: screenshots are encoded on
// the MCP I/O thread, so renderer latency and steady-state allocations remain
// unaffected.
[[nodiscard]] PngEncodeResult encode_png_rgba(
    std::span<const std::uint8_t> rgba,
    std::uint32_t width,
    std::uint32_t height);

} // namespace vcr
