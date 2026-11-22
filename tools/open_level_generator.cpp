#include "open_level.hpp"

#include <charconv>
#include <iostream>
#include <string_view>

namespace {
template <class T> bool parse_number(const std::filesystem::path& argument, T& value) {
    const auto bytes = argument.u8string();
    const std::string text(bytes.begin(), bytes.end());
    const auto end = text.data() + text.size();
    auto result = std::from_chars(text.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}
constexpr auto usage =
    "vcr_open_level --assets <project/assets> --name <name> [--heightmap gray.png]\n"
    "  [--overview color.png] [--detail tile.png] [--seed N] [--samples N]\n"
    "  [--size metres] [--relief metres] [--tile-size metres] [--fade-start metres] [--fade-end metres]\n";
} // namespace

int wmain(int argc, wchar_t** argv) {
    vcr::OpenLevelSettings s;
    std::filesystem::path root, scene;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view key = argv[i];
        if (key == L"--help") {
            std::cout << usage;
            return 0;
        }
        if (++i == argc) {
            std::cerr << usage;
            return 2;
        }
        std::filesystem::path value = argv[i];
        if (key == L"--assets")
            root = value;
        else if (key == L"--name") {
            const auto name = value.u8string();
            s.name.assign(name.begin(), name.end());
        } else if (key == L"--heightmap")
            s.heightmap = value;
        else if (key == L"--overview")
            s.overview = value;
        else if (key == L"--detail")
            s.detail = value;
        else if (!((key == L"--seed" && parse_number(value, s.seed)) ||
                   (key == L"--samples" && parse_number(value, s.samples)) ||
                   (key == L"--size" && parse_number(value, s.width_m)) ||
                   (key == L"--relief" && parse_number(value, s.relief_m)) ||
                   (key == L"--tile-size" && parse_number(value, s.detail_period_m)) ||
                   (key == L"--fade-start" && parse_number(value, s.fade_start_m)) ||
                   (key == L"--fade-end" && parse_number(value, s.fade_end_m)))) {
            std::wcerr << L"Invalid argument: " << key << L" " << argv[i] << L'\n';
            return 2;
        }
    }
    if (root.empty()) {
        std::cerr << usage;
        return 2;
    }
    std::string error;
    if (!vcr::generate_open_level(root, s, scene, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::wcout << (root / scene).wstring() << L'\n';
    return 0;
}
