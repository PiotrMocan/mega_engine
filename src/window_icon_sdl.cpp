#include "window_icon_sdl.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>

namespace vcr {
namespace {

struct SdlSurfaceDeleter {
    void operator()(SDL_Surface* const surface) const noexcept {
        if (surface != nullptr) {
            SDL_DestroySurface(surface);
        }
    }
};

using ScopedSdlSurface = std::unique_ptr<SDL_Surface, SdlSurfaceDeleter>;

} // namespace

void apply_window_icon_rgba(
    SDL_Window* const window,
    const IconRgbaImage& image) noexcept {
    if (window == nullptr || !image.valid() ||
        image.width > 4096U || image.height > 4096U) {
        return;
    }
    const std::size_t expected_bytes =
        static_cast<std::size_t>(image.width) *
        static_cast<std::size_t>(image.height) * 4U;
    if (image.pixels.size() < expected_bytes) {
        return;
    }
    const int pitch = static_cast<int>(image.width * 4U);
    ScopedSdlSurface surface(SDL_CreateSurfaceFrom(
        static_cast<int>(image.width),
        static_cast<int>(image.height),
        SDL_PIXELFORMAT_RGBA32,
        const_cast<std::uint8_t*>(image.pixels.data()),
        pitch));
    if (surface) {
        static_cast<void>(SDL_SetWindowIcon(window, surface.get()));
    }
}

void apply_window_builtin_icon(
    SDL_Window* const window,
    const BuiltinIconKind kind) noexcept {
    if (window == nullptr) {
        return;
    }
    try {
        const IconRgbaImage icon = generate_builtin_icon_rgba(kind, 64U);
        apply_window_icon_rgba(window, icon);
    } catch (...) {
    }
}

void apply_window_icon_from_file_or_builtin(
    SDL_Window* const window,
    const std::filesystem::path& icon_path,
    const BuiltinIconKind fallback) noexcept {
    if (window == nullptr) {
        return;
    }
    try {
        if (!icon_path.empty()) {
            IconRgbaImage loaded;
            std::string ignore_error;
            if (load_icon_source_image(icon_path, loaded, ignore_error)) {
                const IconRgbaImage resized =
                    resize_rgba_square_icon(loaded, 64U);
                if (resized.valid()) {
                    apply_window_icon_rgba(window, resized);
                    return;
                }
            }
        }
    } catch (...) {
    }
    apply_window_builtin_icon(window, fallback);
}

} // namespace vcr
