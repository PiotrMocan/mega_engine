#include "foliage_proxy_tile.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* why) {
    if (!condition)
        throw std::runtime_error(why);
}
struct TestDirectory {
    std::filesystem::path path;
    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
} // namespace

int main() {
    using namespace vcr::foliage;
    namespace fs = std::filesystem;
    try {
        require(!valid_resolution(0) && !valid_resolution(65) && valid_resolution(128), "resolution limits");
        constexpr unsigned n = 16;
        std::vector<Cell> chain(4096 + 512 + 64 + 8 + 1);
        chain[8] = {1, 2, 3};
        chain.back() = {10, 20, 30};
        const Tile tile = pack_tile(chain, n, {1, 2, 3}, .5F);
        const auto directory = directory_words(n);
        require(directory == 12 && tile.words.size() == directory + 2 * brick_words,
                "sparse brick allocation");
        require(tile.words[0] == 0 && tile.words[1] == directory && tile.words[directory] == 1 &&
                    tile.words[directory + 18] == 3,
                "base cell addressing");
        require(tile.words[directory - 1] == directory + brick_words && valid_tile(tile), "last mip");
        auto invalid = tile;
        invalid.words[1]++;
        require(!valid_tile(invalid), "misaligned brick");
        bool rejected = false;
        try {
            (void)pack_tile(std::span(chain).first(4096), n, {}, 1);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "truncated mip chain");

        TestDirectory dir{fs::temp_directory_path() /
                          ("vcr-foliage-test-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
        require(fs::create_directory(dir.path), "test directory");
        const auto bytes = 48 + tile.words.size() * 4;
        Tile loaded;
        require(!load_tile(dir.path, 1, loaded), "cold miss");
        require(store_tile(dir.path, 1, tile, 2 * bytes), "first publication");
        require(load_tile(dir.path, 1, loaded) && loaded.words == tile.words &&
                    loaded.origin == tile.origin && loaded.cell_size == .5F,
                "round trip");
        require(store_tile(dir.path, 2, tile, 2 * bytes), "second publication");
        const auto oldest = dir.path / "foliage-0000000000000002.vpt";
        fs::last_write_time(oldest, fs::file_time_type::clock::now() - std::chrono::hours(1));
        const auto unrelated = dir.path / "other.vpt";
        std::ofstream(unrelated).put('a');
        require(store_tile(dir.path, 3, tile, 2 * bytes) && !fs::exists(oldest) && fs::exists(unrelated) &&
                    load_tile(dir.path, 1, loaded),
                "bounded LRU");
        auto first = dir.path / "foliage-0000000000000001.vpt";
        std::fstream damage(first, std::ios::in | std::ios::out | std::ios::binary);
        damage.seekp(-1, std::ios::end);
        damage.put('X');
        damage.close();
        require(!load_tile(dir.path, 1, loaded) && loaded.words == tile.words, "damaged payload");
        fs::resize_file(first, 12);
        require(!load_tile(dir.path, 1, loaded) && !store_tile(dir.path, 1, tile, bytes - 1),
                "truncation and budget");
        bool left = false, right = false;
        std::thread writer([&] { left = store_tile(dir.path, 1, tile, 2 * bytes); });
        right = store_tile(dir.path, 1, tile, 2 * bytes);
        writer.join();
        require((left || right) && load_tile(dir.path, 1, loaded) && loaded.words == tile.words,
                "concurrent publication");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
