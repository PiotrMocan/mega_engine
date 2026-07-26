#include "cooked_texture_asset.hpp"
#include "png_encoder.hpp"

#include <ImfHeader.h>
#include <ImfRgbaFile.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace Imf = OPENEXR_IMF_NAMESPACE;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::error_code error;
        const std::filesystem::path base =
            std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        const auto token =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        for (std::size_t attempt = 0U; attempt < 128U; ++attempt) {
            const std::filesystem::path candidate =
                base /
                ("vcr_cooked_texture_tests_" +
                 std::to_string(token) + "_" +
                 std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error) {
                return;
            }
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() noexcept {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] bool write_bytes(
    const std::filesystem::path& path,
    const std::span<const std::uint8_t> bytes) {
    std::ofstream output(
        path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] bool write_json(
    const std::filesystem::path& path,
    const Json& json) {
    const std::string text = json.dump(2);
    std::ofstream output(
        path, std::ios::binary | std::ios::trunc);
    output.write(
        text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] Json texture_metadata(
    const std::string_view name,
    const std::string_view source,
    const std::string_view usage,
    const std::string_view color_space) {
    return Json{
        {"schema", "vcr.asset"},
        {"version", 1U},
        {"kind", "texture"},
        {"name", name},
        {"source", source},
        {"origin", "embedded"},
        {"usage", usage},
        {"color_space", color_space},
        {"decode", "deferred"},
    };
}

[[nodiscard]] bool close_byte(
    const std::uint8_t actual,
    const std::uint8_t expected,
    const std::uint8_t tolerance = 8U) {
    return std::abs(
               static_cast<int>(actual) -
               static_cast<int>(expected)) <=
        static_cast<int>(tolerance);
}

constexpr std::array<std::uint8_t, 286U> red_jpeg{{
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
    0x00, 0x03, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x02, 0x02, 0x02, 0x03,
    0x03, 0x03, 0x03, 0x04, 0x06, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x06,
    0x06, 0x05, 0x06, 0x09, 0x08, 0x0a, 0x0a, 0x09, 0x08, 0x09, 0x09, 0x0a,
    0x0c, 0x0f, 0x0c, 0x0a, 0x0b, 0x0e, 0x0b, 0x09, 0x09, 0x0d, 0x11, 0x0d,
    0x0e, 0x0f, 0x10, 0x10, 0x11, 0x10, 0x0a, 0x0c, 0x12, 0x13, 0x12, 0x10,
    0x13, 0x0f, 0x10, 0x10, 0x10, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x03, 0x03,
    0x03, 0x04, 0x03, 0x04, 0x08, 0x04, 0x04, 0x08, 0x10, 0x0b, 0x09, 0x0b,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0xff, 0xc0,
    0x00, 0x11, 0x08, 0x00, 0x01, 0x00, 0x02, 0x03, 0x01, 0x11, 0x00, 0x02,
    0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x08, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0xc4, 0x00, 0x15, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x09,
    0xff, 0xc4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00,
    0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x3a,
    0x03, 0x15, 0x4d, 0xff, 0xd9,
}};

} // namespace

int main() {
    TemporaryDirectory temporary;
    expect(!temporary.path().empty(), "temporary directory is created");
    if (temporary.path().empty()) {
        return 1;
    }
    const std::filesystem::path asset_root =
        temporary.path() / "assets";
    const std::filesystem::path package =
        asset_root / "painted";
    const std::filesystem::path textures =
        package / "textures";
    std::error_code error;
    std::filesystem::create_directories(textures, error);
    expect(!error, "texture fixture directories are created");

    expect(
        write_bytes(textures / "base.jpg", red_jpeg),
        "JPEG fixture is written");
    expect(
        write_json(
            textures / "base.vcrtexture.json",
            texture_metadata(
                "base", "textures/base.jpg", "base_color", "srgb")),
        "JPEG metadata is written");
    const auto jpeg = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/base.vcrtexture.json");
    if (!jpeg) {
        std::cerr << "JPEG load error: " << jpeg.error().message
                  << " [" << jpeg.error().path.string() << "]\n";
    }
    expect(static_cast<bool>(jpeg), "JPEG texture decodes");
    if (jpeg) {
        expect(
            jpeg->width == 2U && jpeg->height == 1U &&
                jpeg->row_stride_bytes == 8U &&
                jpeg->pixels.size() == 8U,
            "JPEG dimensions and owned RGBA8 storage are correct");
        expect(
            jpeg->semantic ==
                    vcr::CookedTextureSemantic::base_color &&
                jpeg->color_space ==
                    vcr::CookedTextureColorSpace::srgb &&
                jpeg->source_relative_path ==
                    "painted/textures/base.jpg",
            "JPEG semantic, color space, and source path are preserved");
    }

    constexpr std::array<std::uint8_t, 8U> png_pixels{
        0x12U, 0x34U, 0x56U, 0x78U,
        0x90U, 0xabU, 0xcdU, 0xefU,
    };
    const vcr::PngEncodeResult encoded_png =
        vcr::encode_png_rgba(png_pixels, 2U, 1U);
    expect(static_cast<bool>(encoded_png), "PNG fixture is encoded");
    expect(
        write_bytes(textures / "color.png", encoded_png.bytes),
        "PNG fixture is written");
    expect(
        write_json(
            textures / "color.vcrtexture.json",
            texture_metadata(
                "color", "textures/color.png", "emissive", "srgb")),
        "PNG metadata is written");
    const auto png = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/color.vcrtexture.json");
    expect(static_cast<bool>(png), "PNG texture decodes");
    if (png) {
        expect(
            png->width == 2U && png->height == 1U &&
                png->pixels ==
                    std::vector<std::uint8_t>(
                        png_pixels.begin(), png_pixels.end()),
            "PNG RGBA8 pixels round-trip exactly");
    }

    constexpr std::array<std::uint8_t, 8U> raw_pixels{
        1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    expect(
        write_bytes(textures / "embedded.rgba8", raw_pixels),
        "raw RGBA8 fixture is written");
    Json raw_metadata = texture_metadata(
        "embedded", "textures/embedded.rgba8", "normal", "linear");
    raw_metadata.erase("decode");
    raw_metadata["storage"] = "rgba8";
    raw_metadata["dimensions"] = {2U, 1U};
    expect(
        write_json(
            textures / "embedded.vcrtexture.json", raw_metadata),
        "raw RGBA8 metadata is written");
    const auto raw = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/embedded.vcrtexture.json");
    expect(static_cast<bool>(raw), "embedded raw RGBA8 texture loads");
    if (raw) {
        expect(
            raw->pixels ==
                std::vector<std::uint8_t>(
                    raw_pixels.begin(), raw_pixels.end()),
            "embedded raw RGBA8 bytes are preserved exactly");
    }

    const std::filesystem::path exr_path = textures / "normal.exr";
    try {
        Imf::Header header(2, 1);
        header.compression() = Imf::DWAA_COMPRESSION;
        const std::string exr_name = exr_path.string();
        Imf::RgbaOutputFile output(
            exr_name.c_str(), header, Imf::WRITE_RGBA, 1);
        std::array<Imf::Rgba, 2U> pixels{
            Imf::Rgba(0.25F, 0.5F, 1.0F, 1.0F),
            Imf::Rgba(0.0F, 1.0F, 0.5F, 1.0F),
        };
        output.setFrameBuffer(pixels.data(), 1U, 2U);
        output.writePixels(1);
    } catch (const std::exception& exception) {
        std::cerr << "EXR fixture error: " << exception.what() << '\n';
        ++failures;
    }
    expect(
        write_json(
            textures / "normal.vcrtexture.json",
            texture_metadata(
                "normal", "textures/normal.exr", "normal", "linear")),
        "OpenEXR metadata is written");
    const auto exr = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/normal.vcrtexture.json");
    if (!exr) {
        std::cerr << "EXR load error: " << exr.error().message
                  << " [" << exr.error().path.string() << "]\n";
    }
    expect(static_cast<bool>(exr), "DWA-compressed OpenEXR texture decodes");
    if (exr) {
        expect(
            exr->width == 2U && exr->height == 1U &&
                exr->pixels.size() == 8U &&
                exr->semantic ==
                    vcr::CookedTextureSemantic::normal &&
                exr->color_space ==
                    vcr::CookedTextureColorSpace::linear,
            "OpenEXR dimensions and linear normal semantic are correct");
        expect(
            close_byte(exr->pixels[0U], 64U) &&
                close_byte(exr->pixels[1U], 128U) &&
                close_byte(exr->pixels[2U], 255U) &&
                close_byte(exr->pixels[4U], 0U) &&
                close_byte(exr->pixels[5U], 255U) &&
                close_byte(exr->pixels[6U], 128U),
            "OpenEXR channels convert to RGBA8 UNORM");
    }

    vcr::CookedTextureLoadOptions one_pixel{};
    one_pixel.maximum_pixels = 1U;
    const auto limited = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/base.vcrtexture.json",
        one_pixel);
    expect(
        !limited &&
            limited.error().code ==
                vcr::CookedTextureLoadErrorCode::limit_exceeded,
        "decoded pixel limit is enforced before allocation");

    vcr::CookedTextureLoadOptions tiny_file{};
    tiny_file.maximum_source_file_bytes = 1U;
    const auto file_limited = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/base.vcrtexture.json",
        tiny_file);
    expect(
        !file_limited &&
            file_limited.error().code ==
                vcr::CookedTextureLoadErrorCode::file_too_large,
        "source file byte limit is enforced");

    Json traversal = texture_metadata(
        "escape", "../outside.jpg", "base_color", "srgb");
    expect(
        write_json(
            textures / "traversal.vcrtexture.json", traversal),
        "traversal metadata is written");
    const auto escaped = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/traversal.vcrtexture.json");
    expect(
        !escaped &&
            escaped.error().code ==
                vcr::CookedTextureLoadErrorCode::invalid_path,
        "source traversal is rejected");

    expect(
        write_bytes(
            textures / "broken.jpg",
            std::array<std::uint8_t, 4U>{1U, 2U, 3U, 4U}),
        "corrupt image fixture is written");
    expect(
        write_json(
            textures / "broken.vcrtexture.json",
            texture_metadata(
                "broken",
                "textures/broken.jpg",
                "base_color",
                "srgb")),
        "corrupt image metadata is written");
    const auto corrupt = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/broken.vcrtexture.json");
    expect(
        !corrupt &&
            corrupt.error().code ==
                vcr::CookedTextureLoadErrorCode::decode_failed,
        "corrupt encoded image is rejected");

    Json wrong_raw_size = raw_metadata;
    wrong_raw_size["dimensions"] = {3U, 1U};
    expect(
        write_json(
            textures / "wrong_raw_size.vcrtexture.json",
            wrong_raw_size),
        "wrong raw byte-count metadata is written");
    const auto wrong_raw = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/wrong_raw_size.vcrtexture.json");
    expect(
        !wrong_raw &&
            wrong_raw.error().code ==
                vcr::CookedTextureLoadErrorCode::decode_failed,
        "raw RGBA8 exact byte count is enforced");

    Json negative_dimension = raw_metadata;
    negative_dimension["dimensions"] = {-1, 1};
    expect(
        write_json(
            textures / "negative_dimension.vcrtexture.json",
            negative_dimension),
        "negative raw dimension metadata is written");
    const auto negative = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/negative_dimension.vcrtexture.json");
    expect(
        !negative &&
            negative.error().code ==
                vcr::CookedTextureLoadErrorCode::invalid_json,
        "negative raw RGBA8 dimensions are rejected");

    {
        std::ofstream malformed(
            textures / "malformed.vcrtexture.json",
            std::ios::binary | std::ios::trunc);
        malformed << "{not json";
    }
    const auto malformed = vcr::load_cooked_texture_asset(
        asset_root,
        "painted/textures/malformed.vcrtexture.json");
    expect(
        !malformed &&
            malformed.error().code ==
                vcr::CookedTextureLoadErrorCode::invalid_json,
        "malformed texture metadata is rejected");

    const std::filesystem::path linked_source =
        textures / "linked.jpg";
    std::filesystem::create_symlink(
        textures / "base.jpg", linked_source, error);
    if (!error) {
        expect(
            write_json(
                textures / "linked.vcrtexture.json",
                texture_metadata(
                    "linked",
                    "textures/linked.jpg",
                    "base_color",
                    "srgb")),
            "symlink texture metadata is written");
        const auto symlinked = vcr::load_cooked_texture_asset(
            asset_root,
            "painted/textures/linked.vcrtexture.json");
        expect(
            !symlinked &&
                symlinked.error().code ==
                    vcr::CookedTextureLoadErrorCode::symbolic_link,
            "texture source symlink is rejected");
    }

    if (failures != 0) {
        std::cerr << failures << " cooked texture test(s) failed\n";
        return 1;
    }
    std::cout << "All cooked texture asset tests passed\n";
    return 0;
}
