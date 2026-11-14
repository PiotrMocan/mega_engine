#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

struct LocalizationEntry {
    std::string key{};
    std::string value{};
};

struct LocalizationArg {
    std::string_view name{};
    std::string_view value{};
};

struct LocalizationLocaleInfo {
    std::string locale{};
    std::string display_name{};
    std::size_t entry_count = 0U;
};

class LocalizationSystem {
public:
    LocalizationSystem();

    void reset_to_builtins();

    bool set_locale(std::string_view locale);
    bool set_fallback_locale(std::string_view locale);
    [[nodiscard]] std::string_view locale() const noexcept { return active_locale_; }
    [[nodiscard]] std::string_view fallback_locale() const noexcept { return fallback_locale_; }
    [[nodiscard]] std::string_view display_name(std::string_view locale) const noexcept;

    [[nodiscard]] std::vector<LocalizationLocaleInfo> available_locales() const;

    void set_entry(std::string_view locale, std::string_view key, std::string_view value, std::string_view display_name = {});
    bool load_file(const std::filesystem::path& path, std::string* error_message = nullptr);
    std::size_t load_directory(const std::filesystem::path& directory, std::string* error_message = nullptr);
    bool save_file(const std::filesystem::path& path, std::string_view locale, std::string* error_message = nullptr) const;

    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    [[nodiscard]] std::string_view try_get(std::string_view key) const noexcept;
    [[nodiscard]] std::string_view get(std::string_view key) const noexcept;
    [[nodiscard]] std::string_view resolve(std::string_view key_or_text) const noexcept;

    [[nodiscard]] std::string_view format_into(
        std::span<char> buffer,
        std::string_view key_or_text,
        std::span<const LocalizationArg> args = {}) const noexcept;

private:
    struct LocaleTable {
        std::string locale{};
        std::string display_name{};
        std::vector<LocalizationEntry> entries{};
        bool sorted = true;
    };

    std::vector<LocaleTable> tables_{};
    std::string active_locale_{"en"};
    std::string fallback_locale_{"en"};
    std::size_t active_table_index_ = 0U;
    std::size_t primary_fallback_table_index_ = 0U;
    std::size_t fallback_table_index_ = 0U;

    void refresh_indices() noexcept;
    [[nodiscard]] std::size_t find_table_index(std::string_view normalized_locale) const noexcept;
    [[nodiscard]] std::size_t ensure_table(std::string_view normalized_locale, std::string_view display_name);
    [[nodiscard]] static std::string_view find_in_table(const LocaleTable& table, std::string_view key) noexcept;
    [[nodiscard]] static std::string normalize_locale_code(std::string_view locale);
};

} // namespace vcr
