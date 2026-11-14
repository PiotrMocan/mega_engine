#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vcr {

enum class BuiltinIconKind : std::uint8_t {
    engine,
    editor,
    project_default,
    project_manager,
    icon_tool,
};

struct IconRgbaImage {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> pixels{};

    [[nodiscard]] bool valid() const noexcept {
        return width > 0U && height > 0U &&
               pixels.size() ==
                   static_cast<std::size_t>(width) *
                       static_cast<std::size_t>(height) * 4U;
    }
};

[[nodiscard]] IconRgbaImage generate_builtin_icon_rgba(
    BuiltinIconKind kind,
    std::uint32_t size = 256U);

[[nodiscard]] IconRgbaImage resize_rgba_square_icon(
    const IconRgbaImage& source,
    std::uint32_t target_size);

[[nodiscard]] bool build_multi_size_ico_from_rgba(
    const IconRgbaImage& source,
    std::vector<std::uint8_t>& out_ico_bytes,
    std::string& error);

[[nodiscard]] bool encode_builtin_ico(
    BuiltinIconKind kind,
    std::vector<std::uint8_t>& out_ico_bytes,
    std::string& error);

[[nodiscard]] bool write_builtin_ico_file(
    BuiltinIconKind kind,
    const std::filesystem::path& output_path,
    std::string& error);

[[nodiscard]] bool load_icon_source_image(
    const std::filesystem::path& input_path,
    IconRgbaImage& out_rgba,
    std::string& error);

[[nodiscard]] bool load_or_convert_to_ico_bytes(
    const std::filesystem::path& source_path,
    std::vector<std::uint8_t>& out_ico_bytes,
    IconRgbaImage& out_preview_rgba,
    std::string& error);

[[nodiscard]] bool apply_ico_to_pe_executable(
    const std::filesystem::path& executable_path,
    std::span<const std::uint8_t> ico_bytes,
    std::string& error);

[[nodiscard]] bool read_pe_executable_icon_frame_count(
    const std::filesystem::path& executable_path,
    std::uint32_t& out_count,
    std::string& error);

} // namespace vcr
