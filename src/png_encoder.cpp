#include "png_encoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string_view>

namespace vcr {
namespace {

constexpr std::array<std::uint8_t, 8> png_signature{
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
};
constexpr std::size_t maximum_raw_bytes = 256U * 1024U * 1024U;
constexpr std::uint32_t adler_modulus = 65'521U;

void append_u32_be(
    std::vector<std::uint8_t>& output,
    const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

[[nodiscard]] std::uint32_t crc32(
    const std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xffff'ffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (std::uint32_t bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask =
                0U - static_cast<std::uint32_t>(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb8'8320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] std::uint32_t adler32(
    const std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t first = 1U;
    std::uint32_t second = 0U;
    for (const std::uint8_t byte : bytes) {
        first += byte;
        if (first >= adler_modulus) {
            first -= adler_modulus;
        }
        second += first;
        if (second >= adler_modulus) {
            second %= adler_modulus;
        }
    }
    return (second << 16U) | first;
}

void append_chunk(
    std::vector<std::uint8_t>& output,
    const std::string_view type,
    const std::span<const std::uint8_t> payload) {
    append_u32_be(output, static_cast<std::uint32_t>(payload.size()));
    const std::size_t crc_begin = output.size();
    output.insert(output.end(), type.begin(), type.end());
    output.insert(output.end(), payload.begin(), payload.end());
    append_u32_be(
        output,
        crc32(std::span<const std::uint8_t>{
            output.data() + crc_begin,
            output.size() - crc_begin}));
}

[[nodiscard]] bool checked_raw_size(
    const std::uint32_t width,
    const std::uint32_t height,
    std::size_t& row_bytes,
    std::size_t& filtered_bytes) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    constexpr std::size_t channels = 4U;
    if (static_cast<std::uint64_t>(width) >
        std::numeric_limits<std::size_t>::max() / channels) {
        return false;
    }
    row_bytes = static_cast<std::size_t>(width) * channels;
    if (row_bytes > std::numeric_limits<std::size_t>::max() - 1U) {
        return false;
    }
    const std::size_t filtered_row_bytes = row_bytes + 1U;
    if (static_cast<std::uint64_t>(height) >
        std::numeric_limits<std::size_t>::max() / filtered_row_bytes) {
        return false;
    }
    filtered_bytes =
        filtered_row_bytes * static_cast<std::size_t>(height);
    if (static_cast<std::uint64_t>(height) >
        std::numeric_limits<std::size_t>::max() / row_bytes) {
        return false;
    }
    const std::size_t raw_bytes =
        row_bytes * static_cast<std::size_t>(height);
    return raw_bytes <= maximum_raw_bytes;
}

} // namespace

PngEncodeResult encode_png_rgba(
    const std::span<const std::uint8_t> rgba,
    const std::uint32_t width,
    const std::uint32_t height) {
    PngEncodeResult result{};
    std::size_t row_bytes = 0;
    std::size_t filtered_size = 0;
    if (!checked_raw_size(width, height, row_bytes, filtered_size)) {
        result.error = "invalid or excessively large PNG dimensions";
        return result;
    }
    const std::size_t expected_pixels =
        row_bytes * static_cast<std::size_t>(height);
    if (rgba.size() != expected_pixels) {
        result.error = "RGBA byte count does not match PNG dimensions";
        return result;
    }

    std::vector<std::uint8_t> filtered;
    filtered.resize(filtered_size);
    const std::size_t filtered_row_bytes = row_bytes + 1U;
    for (std::size_t row = 0; row < height; ++row) {
        const std::size_t destination = row * filtered_row_bytes;
        filtered[destination] = 0U;
        std::copy_n(
            rgba.data() + row * row_bytes,
            row_bytes,
            filtered.data() + destination + 1U);
    }

    const std::size_t block_count =
        (filtered.size() + 65'534U) / 65'535U;
    if (block_count >
        (std::numeric_limits<std::size_t>::max() - filtered.size() - 6U) /
            5U) {
        result.error = "PNG zlib stream size overflow";
        return result;
    }
    std::vector<std::uint8_t> zlib;
    zlib.reserve(2U + filtered.size() + block_count * 5U + 4U);
    zlib.push_back(0x78U);
    zlib.push_back(0x01U);
    std::size_t offset = 0;
    while (offset < filtered.size()) {
        const std::size_t remaining = filtered.size() - offset;
        const auto block_size = static_cast<std::uint16_t>(
            std::min<std::size_t>(remaining, 65'535U));
        const bool final_block =
            offset + static_cast<std::size_t>(block_size) == filtered.size();
        zlib.push_back(final_block ? 0x01U : 0x00U);
        zlib.push_back(static_cast<std::uint8_t>(block_size));
        zlib.push_back(static_cast<std::uint8_t>(block_size >> 8U));
        const std::uint16_t inverse =
            static_cast<std::uint16_t>(~block_size);
        zlib.push_back(static_cast<std::uint8_t>(inverse));
        zlib.push_back(static_cast<std::uint8_t>(inverse >> 8U));
        zlib.insert(
            zlib.end(),
            filtered.begin() + static_cast<std::ptrdiff_t>(offset),
            filtered.begin() + static_cast<std::ptrdiff_t>(
                offset + block_size));
        offset += block_size;
    }
    append_u32_be(zlib, adler32(filtered));

    std::array<std::uint8_t, 13> ihdr{};
    ihdr[0] = static_cast<std::uint8_t>(width >> 24U);
    ihdr[1] = static_cast<std::uint8_t>(width >> 16U);
    ihdr[2] = static_cast<std::uint8_t>(width >> 8U);
    ihdr[3] = static_cast<std::uint8_t>(width);
    ihdr[4] = static_cast<std::uint8_t>(height >> 24U);
    ihdr[5] = static_cast<std::uint8_t>(height >> 16U);
    ihdr[6] = static_cast<std::uint8_t>(height >> 8U);
    ihdr[7] = static_cast<std::uint8_t>(height);
    ihdr[8] = 8U;
    ihdr[9] = 6U;

    result.bytes.reserve(
        png_signature.size() + 12U + ihdr.size() + 12U + zlib.size() + 12U);
    result.bytes.insert(
        result.bytes.end(), png_signature.begin(), png_signature.end());
    append_chunk(result.bytes, "IHDR", ihdr);
    append_chunk(result.bytes, "IDAT", zlib);
    append_chunk(result.bytes, "IEND", {});
    return result;
}

} // namespace vcr
