#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::string_view default_asset_pak_filename = "content.vcrpak";
inline constexpr std::uint32_t asset_pak_format_version = 1U;
inline constexpr std::size_t asset_pak_header_bytes = 32U;
inline constexpr std::size_t asset_pak_payload_alignment = 16U;
inline constexpr std::uint32_t asset_pak_max_entries = 1'000'000U;
inline constexpr std::size_t asset_pak_max_cached_archives = 16U;

enum class AssetPakReadStatus : std::uint8_t {
    not_found,
    ok,
    symbolic_link,
    file_too_large,
    io_error,
    invalid_archive,
};

// Move-only streaming writer for `content.vcrpak` archives.
// Entries are stored sorted strictly ascending by normalized generic UTF-8
// relative path and written with 16-byte aligned payloads.
class AssetPakWriter final {
public:
    AssetPakWriter() = default;
    ~AssetPakWriter() noexcept = default;

    AssetPakWriter(const AssetPakWriter&) = delete;
    AssetPakWriter& operator=(const AssetPakWriter&) = delete;
    AssetPakWriter(AssetPakWriter&&) = default;
    AssetPakWriter& operator=(AssetPakWriter&&) = default;

    [[nodiscard]] bool add_entry(
        std::string normalized_relative_path,
        std::vector<std::uint8_t> bytes,
        std::string& error);

    [[nodiscard]] bool write_to_file(
        const std::filesystem::path& target_pak_path,
        std::string& error);

private:
    std::map<std::string, std::vector<std::uint8_t>> entries_{};
};

[[nodiscard]] bool asset_pak_contains(
    const std::filesystem::path& canonical_asset_root,
    std::string_view normalized_relative_path);

[[nodiscard]] AssetPakReadStatus try_read_asset_pak_file(
    const std::filesystem::path& canonical_asset_root,
    std::string_view normalized_relative_path,
    std::uintmax_t maximum_bytes,
    std::vector<std::uint8_t>& out_bytes,
    std::string* out_error = nullptr);

void invalidate_asset_pak_cache(const std::filesystem::path& asset_root = {});

[[nodiscard]] std::size_t asset_pak_cache_high_water() noexcept;

} // namespace vcr
