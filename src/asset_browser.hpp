#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vcr {

enum class AssetEntryKind : std::uint8_t {
    directory,
    image,
    shader,
    scene,
    binary,
    other,
};

struct AssetEntry {
    std::filesystem::path relative_path{};
    std::string relative_display{};
    std::string display_name{};
    AssetEntryKind kind = AssetEntryKind::other;
    std::uintmax_t size = 0U;
};

class AssetBrowser final {
public:
    static constexpr std::size_t maximum_entries = 100'000U;

    [[nodiscard]] bool initialize(
        const std::filesystem::path& asset_root,
        std::string& error);
    [[nodiscard]] bool navigate_to(
        const std::filesystem::path& relative_directory,
        std::string& error);
    [[nodiscard]] bool navigate_up(std::string& error);
    [[nodiscard]] bool refresh(std::string& error);

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }
    [[nodiscard]] const std::filesystem::path&
        current_directory() const noexcept {
        return current_directory_;
    }
    [[nodiscard]] std::string_view
        current_directory_display() const noexcept {
        return current_directory_display_;
    }
    [[nodiscard]] std::span<const AssetEntry> entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] std::size_t high_water_mark() const noexcept {
        return high_water_mark_;
    }
    [[nodiscard]] bool truncated() const noexcept {
        return truncated_;
    }

private:
    [[nodiscard]] bool resolve_directory(
        const std::filesystem::path& relative_directory,
        std::filesystem::path& resolved,
        std::string& error) const;
    [[nodiscard]] bool enumerate_directory(
        const std::filesystem::path& directory,
        std::vector<AssetEntry>& output,
        bool& truncated,
        std::string& error) const;
    [[nodiscard]] static AssetEntryKind classify(
        const std::filesystem::directory_entry& entry,
        std::error_code& error) noexcept;

    std::filesystem::path root_{};
    std::filesystem::path current_directory_{};
    std::string current_directory_display_{"/"};
    std::vector<AssetEntry> entries_{};
    std::size_t high_water_mark_ = 0U;
    bool truncated_ = false;
};

[[nodiscard]] const char* asset_kind_name(
    AssetEntryKind kind) noexcept;

} // namespace vcr
