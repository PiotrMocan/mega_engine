#pragma once

#include "icon_system.hpp"

#include <filesystem>

struct SDL_Window;

namespace vcr {

void apply_window_icon_rgba(
    SDL_Window* window,
    const IconRgbaImage& image) noexcept;

void apply_window_builtin_icon(
    SDL_Window* window,
    BuiltinIconKind kind) noexcept;

void apply_window_icon_from_file_or_builtin(
    SDL_Window* window,
    const std::filesystem::path& icon_path,
    BuiltinIconKind fallback) noexcept;

} // namespace vcr
