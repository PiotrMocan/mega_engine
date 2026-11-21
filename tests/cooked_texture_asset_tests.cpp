#include "cooked_texture_asset.hpp"
#include "asset_pak.hpp"
#include "png_encoder.hpp"
#include "native_asset_path.hpp"
#include "texture_compression.hpp"

#include <ImfHeader.h>
#include <ImfRgbaFile.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
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
#include <thread>
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

[[nodiscard]] std::vector<std::uint8_t> json_to_bytes(const Json& json) {
    const std::string text = json.dump(2);
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

void append_u32_le_test(
    std::vector<std::uint8_t>& out,
    const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void append_u64_le_test(
    std::vector<std::uint8_t>& out,
    const std::uint64_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 32U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 40U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 48U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 56U) & 0xFFU));
}

void test_asset_pak(const std::filesystem::path& temp_base) {
    const std::filesystem::path pak_root = temp_base / "pak_only_assets";
    std::error_code fs_error;
    std::filesystem::create_directories(pak_root, fs_error);
    expect(!fs_error, "pak test root directory is created");
    if (fs_error) {
        return;
    }
    const std::filesystem::path canonical_pak_root =
        std::filesystem::weakly_canonical(pak_root, fs_error);
    expect(!fs_error && !canonical_pak_root.empty(),
           "pak test root canonicalizes");

    vcr::CookedTextureAsset src_bc7{};
    src_bc7.semantic = vcr::CookedTextureSemantic::base_color;
    src_bc7.color_space = vcr::CookedTextureColorSpace::srgb;
    src_bc7.pixel_format = vcr::CookedTexturePixelFormat::rgba8_unorm;
    src_bc7.width = 6U;
    src_bc7.height = 5U;
    src_bc7.row_stride_bytes = 24U;
    src_bc7.pixels.resize(6U * 5U * 4U);
    for (std::size_t i = 0U; i < 6U * 5U; ++i) {
        src_bc7.pixels[i * 4U + 0U] = 40U;
        src_bc7.pixels[i * 4U + 1U] = 180U;
        src_bc7.pixels[i * 4U + 2U] = 90U;
        src_bc7.pixels[i * 4U + 3U] = (i % 2U == 0U) ? 230U : 30U;
    }
    std::vector<std::uint8_t> vcrtex_bc7;
    std::uint32_t mips = 0U;
    std::string cook_error;
    expect(
        vcr::cook_texture_to_vcrtex(
            src_bc7,
            0.5F,
            vcr::CookedTexturePixelFormat::bc7_unorm,
            vcrtex_bc7,
            mips,
            cook_error),
        "pak BC7 texture cooks cleanly: " + cook_error);

    Json bc7_meta = texture_metadata(
        "pak_albedo", "textures/albedo.vcrtex", "base_color", "srgb");
    bc7_meta.erase("decode");
    bc7_meta["storage"] = "bc7";

    constexpr std::array<std::uint8_t, 8U> png_pixels{
        0x12U, 0x34U, 0x56U, 0x78U,
        0x90U, 0xabU, 0xcdU, 0xefU,
    };
    const vcr::PngEncodeResult encoded_png =
        vcr::encode_png_rgba(png_pixels, 2U, 1U);
    expect(static_cast<bool>(encoded_png), "pak PNG fixture encodes");

    constexpr std::array<std::uint8_t, 8U> raw_pixels{
        1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
    };
    Json raw_meta = texture_metadata(
        "pak_raw", "textures/embedded.rgba8", "normal", "linear");
    raw_meta.erase("decode");
    raw_meta["storage"] = "rgba8";
    raw_meta["dimensions"] = {2U, 1U};

    vcr::AssetPakWriter writer;
    std::string pak_error;
    expect(
        !writer.add_entry("", {}, pak_error) &&
            !writer.add_entry("../escape.vcrtex", {}, pak_error) &&
            !writer.add_entry("/leading.vcrtex", {}, pak_error) &&
            !writer.add_entry("painted\\backslash.vcrtex", {}, pak_error) &&
            !writer.add_entry("painted/./dot.vcrtex", {}, pak_error) &&
            !writer.add_entry("painted/trail/", {}, pak_error) &&
            !writer.add_entry(std::string({'a', '/', 'b', '\0', 'c'}), {}, pak_error),
        "AssetPakWriter rejects invalid entry paths");

    expect(
        writer.add_entry(
            "painted/textures/albedo.vcrtex", vcrtex_bc7, pak_error),
        "pak BC7 payload entry added: " + pak_error);
    expect(
        !writer.add_entry(
            "painted/textures/albedo.vcrtex", vcrtex_bc7, pak_error),
        "AssetPakWriter rejects duplicate entry paths");
    expect(
        writer.add_entry(
            "painted/textures/albedo_bc7.vcrtexture.json",
            json_to_bytes(bc7_meta),
            pak_error),
        "pak BC7 metadata entry added: " + pak_error);
    expect(
        writer.add_entry(
            "painted/textures/color.png", encoded_png.bytes, pak_error),
        "pak PNG payload entry added: " + pak_error);
    expect(
        writer.add_entry(
            "painted/textures/color.vcrtexture.json",
            json_to_bytes(texture_metadata(
                "pak_color", "textures/color.png", "emissive", "srgb")),
            pak_error),
        "pak PNG metadata entry added: " + pak_error);
    expect(
        writer.add_entry(
            "painted/textures/embedded.rgba8",
            std::vector<std::uint8_t>(raw_pixels.begin(), raw_pixels.end()),
            pak_error),
        "pak raw RGBA8 payload entry added: " + pak_error);
    expect(
        writer.add_entry(
            "painted/textures/embedded.vcrtexture.json",
            json_to_bytes(raw_meta),
            pak_error),
        "pak raw RGBA8 metadata entry added: " + pak_error);

    const std::filesystem::path pak_path =
        pak_root / vcr::default_asset_pak_filename;
    expect(
        writer.write_to_file(pak_path, pak_error),
        "content.vcrpak writes cleanly: " + pak_error);
    expect(
        !std::filesystem::exists(pak_root / "painted"),
        "no loose asset files exist alongside content.vcrpak");

    expect(
        vcr::asset_pak_contains(
            canonical_pak_root, "painted/textures/albedo_bc7.vcrtexture.json") &&
            vcr::asset_pak_contains(
                canonical_pak_root, "painted/textures/albedo.vcrtex") &&
            !vcr::asset_pak_contains(
                canonical_pak_root, "painted/textures/does_not_exist.vcrtex"),
        "asset_pak_contains reports packed and missing entries accurately");

    const auto pak_bc7 = vcr::load_cooked_texture_asset(
        pak_root, "painted/textures/albedo_bc7.vcrtexture.json");
    expect(
        static_cast<bool>(pak_bc7),
        "BC7 .vcrtex loads from content.vcrpak without loose disk files");
    if (pak_bc7) {
        expect(
            pak_bc7->name == "pak_albedo" &&
                pak_bc7->pixel_format == vcr::CookedTexturePixelFormat::bc7_unorm &&
                pak_bc7->width == 6U && pak_bc7->height == 5U &&
                pak_bc7->mip_levels == 3U,
            "packed BC7 metadata and mip count match");
        std::vector<std::uint8_t> unpacked;
        std::string unpack_err;
        expect(
            vcr::decode_bc_mip0_to_rgba8(*pak_bc7, unpacked, unpack_err) &&
                unpacked.size() == 6U * 5U * 4U &&
                close_byte(unpacked[0U], 40U, 16U) &&
                close_byte(unpacked[1U], 180U, 16U) &&
                close_byte(unpacked[3U], 230U, 20U),
            "packed BC7 mip0 unpacks to expected RGBA values");
    }

    const auto pak_png = vcr::load_cooked_texture_asset(
        pak_root, "painted/textures/color.vcrtexture.json");
    expect(
        pak_png && pak_png->width == 2U && pak_png->height == 1U &&
            pak_png->pixels ==
                std::vector<std::uint8_t>(
                    png_pixels.begin(), png_pixels.end()),
        "packed PNG texture decodes from memory identically");

    const auto pak_raw = vcr::load_cooked_texture_asset(
        pak_root, "painted/textures/embedded.vcrtexture.json");
    expect(
        pak_raw && pak_raw->width == 2U && pak_raw->height == 1U &&
            pak_raw->pixels ==
                std::vector<std::uint8_t>(
                    raw_pixels.begin(), raw_pixels.end()),
        "packed raw RGBA8 texture loads identically");

    expect(
        vcr::asset_pak_cache_high_water() >= 1U &&
            vcr::asset_pak_cache_high_water() <= vcr::asset_pak_max_cached_archives,
        "asset pak cache high-water mark tracks bounded usage");

    {
        std::atomic<int> concurrent_failures{0};
        std::vector<std::thread> workers;
        workers.reserve(4U);
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([&]() {
                for (int iter = 0; iter < 8; ++iter) {
                    std::vector<std::uint8_t> bytes;
                    if (vcr::try_read_asset_pak_file(
                            canonical_pak_root,
                            "painted/textures/albedo.vcrtex",
                            1024ULL * 1024ULL,
                            bytes) != vcr::AssetPakReadStatus::ok ||
                        bytes != vcrtex_bc7) {
                        concurrent_failures.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    const auto tex = vcr::load_cooked_texture_asset(
                        pak_root, "painted/textures/albedo_bc7.vcrtexture.json");
                    if (!tex || tex->width != 6U || tex->height != 5U) {
                        concurrent_failures.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        expect(
            concurrent_failures.load(std::memory_order_relaxed) == 0,
            "concurrent pak texture reads succeed without data races");
    }

    vcr::CookedTextureLoadOptions tiny_source{};
    tiny_source.maximum_source_file_bytes = 1U;
    const auto pak_source_too_large = vcr::load_cooked_texture_asset(
        pak_root, "painted/textures/albedo_bc7.vcrtexture.json", tiny_source);
    expect(
        !pak_source_too_large &&
            pak_source_too_large.error().code ==
                vcr::CookedTextureLoadErrorCode::file_too_large,
        "packed texture source byte limit returns file_too_large");

    vcr::CookedTextureLoadOptions tiny_meta{};
    tiny_meta.maximum_metadata_bytes = 1U;
    const auto pak_meta_too_large = vcr::load_cooked_texture_asset(
        pak_root, "painted/textures/albedo_bc7.vcrtexture.json", tiny_meta);
    expect(
        !pak_meta_too_large &&
            pak_meta_too_large.error().code ==
                vcr::CookedTextureLoadErrorCode::file_too_large,
        "packed texture metadata byte limit returns file_too_large");

    std::vector<std::uint8_t> read_buf;
    expect(
        vcr::try_read_asset_pak_file(
            canonical_pak_root,
            "painted/textures/albedo.vcrtex",
            1U,
            read_buf) == vcr::AssetPakReadStatus::file_too_large,
        "try_read_asset_pak_file enforces maximum_bytes");

    // Invalidating through the extended Windows spelling must close the
    // same handle that was opened through the normal path.
    vcr::invalidate_asset_pak_cache(vcr::extended_windows_path(canonical_pak_root));
    {
        vcr::AssetPakWriter writer_v2;
        Json bc7_meta_v2 = bc7_meta;
        bc7_meta_v2["name"] = "pak_albedo_v2";
        expect(
            writer_v2.add_entry(
                "painted/textures/albedo.vcrtex", vcrtex_bc7, pak_error) &&
                writer_v2.add_entry(
                    "painted/textures/albedo_bc7.vcrtexture.json",
                    json_to_bytes(bc7_meta_v2),
                    pak_error) &&
                writer_v2.write_to_file(pak_path, pak_error),
            "updated content.vcrpak writes cleanly after cache invalidation");
        const auto reloaded = vcr::load_cooked_texture_asset(
            pak_root, "painted/textures/albedo_bc7.vcrtexture.json");
        expect(
            reloaded && reloaded->name == "pak_albedo_v2",
            "reloaded texture observes updated archive after cache invalidation");
    }

    // Corrupt archive header / truncated file rejection.
    vcr::invalidate_asset_pak_cache(pak_root);
    expect(
        write_bytes(
            pak_path,
            std::array<std::uint8_t, 12U>{
                'B', 'A', 'D', 'P', 'A', 'K', 0U, 1U, 2U, 3U, 4U, 5U}),
        "corrupt short pak fixture is written");
    expect(
        vcr::try_read_asset_pak_file(
            canonical_pak_root,
            "painted/textures/albedo_bc7.vcrtexture.json",
            1024ULL * 1024ULL,
            read_buf) == vcr::AssetPakReadStatus::invalid_archive,
        "corrupt pak header returns invalid_archive");
    expect(
        !vcr::load_cooked_texture_asset(
            pak_root, "painted/textures/albedo_bc7.vcrtexture.json"),
        "load_cooked_texture_asset rejects corrupt pak");

    // Corrupt index entry whose payload range exceeds index_offset.
    {
        std::vector<std::uint8_t> bad_pak;
        const std::string bad_entry_path = "a.vcrtex";
        const std::uint64_t index_offset = 32ULL;
        const std::uint64_t index_byte_size = 20ULL + bad_entry_path.size();
        bad_pak.insert(
            bad_pak.end(),
            {'V', 'C', 'R', 'P', 'A', 'K', '0', '1'});
        append_u32_le_test(bad_pak, 1U);
        append_u32_le_test(bad_pak, 1U);
        append_u64_le_test(bad_pak, index_offset);
        append_u64_le_test(bad_pak, index_byte_size);
        append_u32_le_test(
            bad_pak, static_cast<std::uint32_t>(bad_entry_path.size()));
        bad_pak.insert(
            bad_pak.end(), bad_entry_path.begin(), bad_entry_path.end());
        append_u64_le_test(bad_pak, 32ULL);
        append_u64_le_test(bad_pak, 100ULL);
        vcr::invalidate_asset_pak_cache(pak_root);
        expect(
            write_bytes(pak_path, bad_pak),
            "out-of-bounds payload pak fixture is written");
        expect(
            vcr::try_read_asset_pak_file(
                canonical_pak_root,
                "a.vcrtex",
                1024ULL * 1024ULL,
                read_buf) == vcr::AssetPakReadStatus::invalid_archive,
            "pak entry exceeding index_offset is rejected");
    }

    // Unsorted index entries must be rejected.
    {
        std::vector<std::uint8_t> unsorted_pak;
        const std::string p1 = "z.vcrtex";
        const std::string p2 = "a.vcrtex";
        const std::uint64_t index_offset = 32ULL;
        const std::uint64_t index_byte_size =
            (20ULL + p1.size()) + (20ULL + p2.size());
        unsorted_pak.insert(
            unsorted_pak.end(),
            {'V', 'C', 'R', 'P', 'A', 'K', '0', '1'});
        append_u32_le_test(unsorted_pak, 1U);
        append_u32_le_test(unsorted_pak, 2U);
        append_u64_le_test(unsorted_pak, index_offset);
        append_u64_le_test(unsorted_pak, index_byte_size);
        append_u32_le_test(unsorted_pak, static_cast<std::uint32_t>(p1.size()));
        unsorted_pak.insert(unsorted_pak.end(), p1.begin(), p1.end());
        append_u64_le_test(unsorted_pak, 32ULL);
        append_u64_le_test(unsorted_pak, 0ULL);
        append_u32_le_test(unsorted_pak, static_cast<std::uint32_t>(p2.size()));
        unsorted_pak.insert(unsorted_pak.end(), p2.begin(), p2.end());
        append_u64_le_test(unsorted_pak, 32ULL);
        append_u64_le_test(unsorted_pak, 0ULL);
        vcr::invalidate_asset_pak_cache(pak_root);
        expect(
            write_bytes(pak_path, unsorted_pak),
            "unsorted pak fixture is written");
        expect(
            vcr::try_read_asset_pak_file(
                canonical_pak_root,
                "a.vcrtex",
                1024ULL * 1024ULL,
                read_buf) == vcr::AssetPakReadStatus::invalid_archive,
            "unsorted pak index entries are rejected");
    }

    // Symlink content.vcrpak rejection (when OS permissions allow symlinks).
    {
        const std::filesystem::path sym_root = temp_base / "pak_sym_assets";
        std::filesystem::create_directories(sym_root, fs_error);
        if (!fs_error) {
            const std::filesystem::path target_pak =
                temp_base / "sym_target.vcrpak";
            vcr::AssetPakWriter sym_writer;
            expect(
                sym_writer.add_entry(
                    "a.vcrtex",
                    std::vector<std::uint8_t>{1U, 2U, 3U},
                    pak_error) &&
                    sym_writer.write_to_file(target_pak, pak_error),
                "symlink target pak writes cleanly");
            std::error_code sym_ec;
            std::filesystem::create_symlink(
                target_pak,
                sym_root / vcr::default_asset_pak_filename,
                sym_ec);
            if (!sym_ec) {
                const std::filesystem::path canonical_sym_root =
                    std::filesystem::weakly_canonical(sym_root, sym_ec);
                expect(
                    !sym_ec &&
                        vcr::try_read_asset_pak_file(
                            canonical_sym_root,
                            "a.vcrtex",
                            1024ULL,
                            read_buf) == vcr::AssetPakReadStatus::symbolic_link,
                    "symbolic link content.vcrpak is rejected");
            }
        }
    }

    // Clear all cached archive handles before TemporaryDirectory cleanup.
    vcr::invalidate_asset_pak_cache();
}

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

    Json packed_metadata = raw_metadata;
    packed_metadata["usage"] = "metallic_roughness";
    expect(write_json(textures / "packed.vcrtexture.json", packed_metadata),
           "packed metallic-roughness metadata written");
    const auto packed = vcr::load_cooked_texture_asset(
        asset_root, "painted/textures/packed.vcrtexture.json");
    expect(packed && packed->semantic == vcr::CookedTextureSemantic::metallic_roughness &&
               packed->color_space == vcr::CookedTextureColorSpace::linear &&
               packed->pixels == std::vector<std::uint8_t>(raw_pixels.begin(), raw_pixels.end()),
           "packed G/B channels remain linear and are not swizzled or gamma converted");

    Json displacement_metadata = raw_metadata;
    displacement_metadata["usage"] = "displacement";
    expect(write_json(textures / "displacement.vcrtexture.json", displacement_metadata),
           "displacement metadata written");
    const auto displacement = vcr::load_cooked_texture_asset(
        asset_root, "painted/textures/displacement.vcrtexture.json");
    expect(displacement &&
               displacement->semantic == vcr::CookedTextureSemantic::displacement &&
               displacement->color_space == vcr::CookedTextureColorSpace::linear,
           "displacement texture remains linear scalar data");

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

    try {
        const std::string name = (textures / "roughness-y.exr").string();
        Imf::Header header(2, 1);
        Imf::RgbaOutputFile output(name.c_str(), header, Imf::WRITE_Y, 1);
        std::array<Imf::Rgba, 2U> pixels{
            Imf::Rgba(0.25F, 0.25F, 0.25F, 1.0F),
            Imf::Rgba(0.75F, 0.75F, 0.75F, 1.0F)};
        output.setFrameBuffer(pixels.data(), 1U, 2U);
        output.writePixels(1);
    } catch (const std::exception& exception) {
        std::cerr << "Scalar EXR fixture error: " << exception.what() << '\n';
        ++failures;
    }
    expect(write_json(textures / "roughness-y.vcrtexture.json", texture_metadata(
               "roughness", "textures/roughness-y.exr", "metallic_roughness", "linear")),
           "scalar roughness EXR metadata written");
    const auto scalar_exr = vcr::load_cooked_texture_asset(
        asset_root, "painted/textures/roughness-y.vcrtexture.json");
    expect(scalar_exr && scalar_exr->pixels.size() == 8U &&
               scalar_exr->pixels[0] == 255U && close_byte(scalar_exr->pixels[1], 64U) &&
               scalar_exr->pixels[2] == 0U && close_byte(scalar_exr->pixels[5], 191U) &&
               scalar_exr->pixels[6] == 0U,
           "luminance roughness EXR becomes G roughness and B zero metalness");
    expect(write_json(textures / "invalid-normal-y.vcrtexture.json", texture_metadata(
               "normal", "textures/roughness-y.exr", "normal", "linear")),
           "scalar normal rejection fixture written");
    expect(!vcr::load_cooked_texture_asset(
               asset_root, "painted/textures/invalid-normal-y.vcrtexture.json"),
           "normal maps still require three independent channels");

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

    // Compressed .vcrtex BC7 (sRGB with partial alpha) and BC4 (linear displacement) tests.
    {
        vcr::CookedTextureAsset src_bc7{};
        src_bc7.semantic = vcr::CookedTextureSemantic::base_color;
        src_bc7.color_space = vcr::CookedTextureColorSpace::srgb;
        src_bc7.pixel_format = vcr::CookedTexturePixelFormat::rgba8_unorm;
        src_bc7.width = 6U;
        src_bc7.height = 5U;
        src_bc7.row_stride_bytes = 24U;
        src_bc7.pixels.resize(6U * 5U * 4U);
        for (std::size_t i = 0U; i < 6U * 5U; ++i) {
            src_bc7.pixels[i * 4U + 0U] = 40U;
            src_bc7.pixels[i * 4U + 1U] = 180U;
            src_bc7.pixels[i * 4U + 2U] = 90U;
            src_bc7.pixels[i * 4U + 3U] = (i % 2U == 0U) ? 230U : 30U;
        }
        std::vector<std::uint8_t> vcrtex_bc7;
        std::uint32_t mips = 0U;
        std::string cook_error;
        expect(
            vcr::cook_texture_to_vcrtex(
                src_bc7,
                0.5F,
                vcr::CookedTexturePixelFormat::bc7_unorm,
                vcrtex_bc7,
                mips,
                cook_error),
            "NPOT BC7 texture cooks cleanly: " + cook_error);
        expect(mips == 3U, "6x5 texture produces 3 mip levels");
        expect(write_bytes(textures / "albedo.vcrtex", vcrtex_bc7), "BC7 .vcrtex written");
        Json bc7_meta = texture_metadata("albedo", "textures/albedo.vcrtex", "base_color", "srgb");
        bc7_meta.erase("decode");
        bc7_meta["storage"] = "bc7";
        expect(write_json(textures / "albedo_bc7.vcrtexture.json", bc7_meta), "BC7 meta written");
        const auto loaded_bc7 = vcr::load_cooked_texture_asset(
            asset_root, "painted/textures/albedo_bc7.vcrtexture.json");
        expect(static_cast<bool>(loaded_bc7), "BC7 .vcrtex loads cleanly");
        if (loaded_bc7) {
            expect(
                loaded_bc7->pixel_format == vcr::CookedTexturePixelFormat::bc7_unorm &&
                    loaded_bc7->width == 6U && loaded_bc7->height == 5U &&
                    loaded_bc7->mip_levels == 3U,
                "BC7 metadata & mip count match");
            std::vector<std::uint8_t> unpacked;
            std::string unpack_err;
            expect(
                vcr::decode_bc_mip0_to_rgba8(*loaded_bc7, unpacked, unpack_err) &&
                    unpacked.size() == 6U * 5U * 4U &&
                    close_byte(unpacked[0], 40U, 16U) &&
                    close_byte(unpacked[1], 180U, 16U) &&
                    close_byte(unpacked[3], 230U, 20U),
                "BC7 mip0 unpacks back to expected RGBA values");
        }

        auto truncated = vcrtex_bc7;
        truncated.pop_back();
        expect(write_bytes(textures / "truncated.vcrtex", truncated), "truncated .vcrtex written");
        Json trunc_meta = bc7_meta;
        trunc_meta["source"] = "textures/truncated.vcrtex";
        expect(write_json(textures / "truncated_bc7.vcrtexture.json", trunc_meta), "trunc meta written");
        const auto loaded_trunc = vcr::load_cooked_texture_asset(
            asset_root, "painted/textures/truncated_bc7.vcrtexture.json");
        expect(!loaded_trunc, "truncated .vcrtex is rejected");

        vcr::CookedTextureAsset src_bc4{};
        src_bc4.semantic = vcr::CookedTextureSemantic::displacement;
        src_bc4.color_space = vcr::CookedTextureColorSpace::linear;
        src_bc4.pixel_format = vcr::CookedTexturePixelFormat::rgba8_unorm;
        src_bc4.width = 8U;
        src_bc4.height = 8U;
        src_bc4.row_stride_bytes = 32U;
        src_bc4.pixels.resize(8U * 8U * 4U);
        for (std::size_t i = 0U; i < 64U; ++i) {
            src_bc4.pixels[i * 4U + 0U] = static_cast<std::uint8_t>(i * 4U);
            src_bc4.pixels[i * 4U + 1U] = 0U;
            src_bc4.pixels[i * 4U + 2U] = 0U;
            src_bc4.pixels[i * 4U + 3U] = 255U;
        }
        std::vector<std::uint8_t> vcrtex_bc4;
        std::uint32_t bc4_mips = 0U;
        expect(
            vcr::cook_texture_to_vcrtex(
                src_bc4,
                0.0F,
                vcr::CookedTexturePixelFormat::bc4_unorm,
                vcrtex_bc4,
                bc4_mips,
                cook_error),
            "BC4 displacement texture cooks cleanly: " + cook_error);
        expect(bc4_mips == 4U, "8x8 texture produces 4 mip levels");
        expect(write_bytes(textures / "disp.vcrtex", vcrtex_bc4), "BC4 .vcrtex written");
        Json bc4_meta = texture_metadata("disp", "textures/disp.vcrtex", "displacement", "linear");
        bc4_meta.erase("decode");
        bc4_meta["storage"] = "bc4";
        expect(write_json(textures / "disp_bc4.vcrtexture.json", bc4_meta), "BC4 meta written");
        const auto loaded_bc4 = vcr::load_cooked_texture_asset(
            asset_root, "painted/textures/disp_bc4.vcrtexture.json");
        expect(
            static_cast<bool>(loaded_bc4) &&
                loaded_bc4->pixel_format == vcr::CookedTexturePixelFormat::bc4_unorm &&
                loaded_bc4->mip_levels == 4U,
            "BC4 .vcrtex loads cleanly");
    }

    {
        vcr::CookedTextureAsset base;
        base.width = 5;
        base.height = 3;
        base.color_space = vcr::CookedTextureColorSpace::linear;
        base.semantic = vcr::CookedTextureSemantic::data;
        for (unsigned y = 0; y < 3; ++y) {
            for (unsigned x = 0; x < 5; ++x) {
                base.pixels.insert(base.pixels.end(), {static_cast<std::uint8_t>(x == 4 ? 255 : 0),
                    static_cast<std::uint8_t>(y == 2 ? 255 : 0), 0, 255});
            }
        }
        unsigned visits = 0;
        vcr::visit_filtered_texture_mips(base, 0.F, [&](auto level, auto, auto, auto texels) {
            ++visits;
            if (level == 2) expect(texels[0] == 51 && texels[1] == 85,
                "Both odd image borders must contribute their exact area to the final mip");
        });
        expect(visits == 2, "5x3 produces two lower mips");
        base.width = 2;
        base.height = 1;
        base.semantic = vcr::CookedTextureSemantic::normal;
        // Two unit normals with opposite X and Z=0.6. Their unnormalized mean has length 0.6.
        base.pixels = {230,128,204,0, 25,128,204,255};
        std::vector<std::uint8_t> container;
        std::uint32_t levels{};
        std::string normal_error;
        vcr::CookedTextureAsset cooked;
        expect(vcr::cook_texture_to_vcrtex(base, 0.5F, vcr::CookedTexturePixelFormat::bc7_unorm,
                container, levels, normal_error) &&
            vcr::decode_vcrtex_bytes(container, vcr::CookedTexturePixelFormat::bc7_unorm, {}, cooked, normal_error),
            "Normal-variance fixture must pass through the Shipping codec");
        if (cooked.mip_byte_offsets.size() == 2) {
            cooked.pixels.erase(cooked.pixels.begin(), cooked.pixels.begin() + cooked.mip_byte_offsets[1]);
            cooked.width = cooked.height = cooked.mip_levels = 1;
            cooked.mip_byte_offsets = {0};
            std::vector<std::uint8_t> pixel;
            expect(vcr::decode_bc_mip0_to_rgba8(cooked, pixel, normal_error) && pixel.size() == 4 &&
                std::abs(int(pixel[0]) - 128) < 4 && std::abs(int(pixel[2]) - 204) < 4 &&
                std::abs(int(pixel[3]) - 128) < 4, "BC7 normal mips must retain variance and independent alpha");
        } else expect(false, "Missing filtered normal mip");
    }

    {
        for (const bool signed_xy : {false, true}) {
            vcr::CookedTextureAsset bc5;
            bc5.semantic = vcr::CookedTextureSemantic::normal;
            bc5.pixel_format = signed_xy ? vcr::CookedTexturePixelFormat::bc5_snorm
                                         : vcr::CookedTexturePixelFormat::bc5;
            bc5.width = bc5.height = 4;
            bc5.mip_levels = 1;
            bc5.mip_byte_offsets = {0};
            bc5.pixels.resize(16);
            // BC4 endpoints selected by zero indices. X=-0.6, Y=0.
            bc5.pixels[0] = signed_xy ? static_cast<std::uint8_t>(-76) : 51;
            bc5.pixels[8] = signed_xy ? 0 : 128;
            std::vector<std::uint8_t> pixels;
            std::string bc5_error;
            expect(vcr::decode_bc_mip0_to_rgba8(bc5, pixels, bc5_error) && pixels.size() == 64 &&
                close_byte(pixels[0], 51, 1) && close_byte(pixels[1], 128, 1) &&
                close_byte(pixels[2], 230, 1) && pixels[3] == 255,
                "BC5 tangent normals preserve negative X and reconstruct positive Z");
        }
    }
    {
        // BC1, two mip levels in KTX2's smallest-first physical order.
        const std::array<std::uint8_t,12> magic{0xab,'K','T','X',' ','2','0',0xbb,13,10,26,10};
        std::vector<std::uint8_t> bytes(216);
        std::copy(magic.begin(), magic.end(), bytes.begin());
        auto field = [&](unsigned offset, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) bytes[offset+i] = static_cast<std::uint8_t>(value >> (8*i));
        };
        field(12,131); field(16,1); field(20,8); field(24,8); field(36,1); field(40,2);
        field(48,128); field(52,44); field(128,44);
        field(136,(40U<<16)|2U); field(140,128U | (1U<<8) | (1U<<16));
        field(144,0x0303); field(148,8); field(156,63U<<16); field(168,0xffffffffU);
        field(80,184); field(88,32); field(96,32); field(104,176); field(112,8); field(120,8);
        bytes[176] = 22; bytes[184] = 11;
        auto meta = texture_metadata("ktx", "textures/test.ktx2", "normal", "linear");
        meta.erase("decode"); meta["storage"] = "ktx2";
        expect(write_json(textures / "ktx.vcrtexture.json", meta) && write_bytes(textures / "test.ktx2", bytes),
            "write KTX2 texture fixture");
        const auto loaded = vcr::load_cooked_texture_asset(asset_root,"painted/textures/ktx.vcrtexture.json");
        expect(loaded && loaded->pixel_format == vcr::CookedTexturePixelFormat::bc1_rgb &&
            loaded->mip_byte_offsets == std::vector<std::size_t>{0,32} && loaded->pixels[0] == 11 &&
            loaded->pixels[32] == 22, "KTX2 preserves native BC blocks and reorders mip payload");
        vcr::CookedTextureAsset target;
        target.color_space = vcr::CookedTextureColorSpace::linear;
        std::string ktx_error;
        field(104,184);
        expect(!vcr::decode_ktx2_bytes(bytes,{},target,ktx_error), "reject overlapping KTX2 mip levels");
        field(104,176); field(84,0xffffffff);
        expect(!vcr::decode_ktx2_bytes(bytes,{},target,ktx_error), "reject out-of-range 64-bit KTX2 mip offset");
    }

    test_asset_pak(temporary.path());

    if (failures != 0) {
        std::cerr << failures << " cooked texture test(s) failed\n";
        return 1;
    }
    std::cout << "All cooked texture asset tests passed\n";
    return 0;
}
