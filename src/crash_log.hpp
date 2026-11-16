#pragma once

#include <filesystem>
#include <string_view>

namespace vcr {

void install_crash_handlers(
    std::string_view app_name,
    int argc = 0,
    char** argv = nullptr) noexcept;

void set_crash_stage(const char* stage) noexcept;

void record_crash_diagnostic(std::string_view message) noexcept;

[[nodiscard]] std::filesystem::path crash_log_path();

std::filesystem::path write_crash_log(
    std::string_view category,
    std::string_view message) noexcept;

} // namespace vcr
