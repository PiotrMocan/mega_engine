#include "foliage_proxy_tile.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace vcr::foliage {
namespace {
constexpr std::uint32_t signature = 0x31545056; // VPT1
struct Header {
    std::uint32_t magic{}, version{};
    std::uint64_t key{};
    std::uint32_t resolution{}, word_count{};
    std::array<float, 3> origin{};
    float cell_size{};
    std::uint64_t checksum{};
};
static_assert(sizeof(Header) == 48 && offsetof(Header, checksum) == 40);
static_assert(std::endian::native == std::endian::little);

std::uint32_t cube(std::uint32_t n) noexcept { return n * n * n; }
std::uint32_t bricks(std::uint32_t n) noexcept { return (n + 7) / 8; }
std::filesystem::path filename(std::uint64_t key) { return std::format("foliage-{:016x}.vpt", key); }
bool owned_filename(const std::filesystem::path& path) {
    const auto name = path.filename().wstring();
    return name.size() == 28 && name.starts_with(L"foliage-") && name.ends_with(L".vpt") &&
           name.find_first_not_of(L"0123456789abcdef", 8) == 24;
}
bool valid_bounds(const std::array<float, 3>& origin, float size, std::uint32_t n) noexcept {
    return size > 0 && std::isfinite(size) && std::ranges::all_of(origin, [&](float v) {
               return std::isfinite(v) && std::isfinite(v + size * n) && v + size * n > v;
           });
}
std::uint64_t checksum(const Header& h, std::span<const std::uint32_t> words) {
    const auto prefix = std::as_bytes(std::span{&h, 1}).first(offsetof(Header, checksum));
    return content_hash(std::as_bytes(words), content_hash(prefix));
}
class WriterLock {
  public:
    explicit WriterLock(const std::filesystem::path& dir)
        : handle_(CreateFileW((dir / L"foliage-cache.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~WriterLock() {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE handle_;
};
} // namespace

std::uint64_t content_hash(std::span<const std::byte> bytes, std::uint64_t seed) noexcept {
    for (const auto b : bytes)
        seed = (seed ^ std::to_integer<unsigned>(b)) * 1099511628211ULL;
    return seed;
}
bool valid_resolution(std::uint32_t n) noexcept {
    return n >= 8 && n <= max_resolution && std::has_single_bit(n);
}
std::uint32_t directory_words(std::uint32_t n) noexcept {
    if (!valid_resolution(n))
        return 0;
    std::uint32_t count = 0;
    for (; n; n /= 2)
        count += cube(bricks(n));
    return count;
}
bool valid_tile(const Tile& t) noexcept {
    const auto entries = directory_words(t.resolution);
    if (!entries || t.words.size() < entries || !valid_bounds(t.origin, t.cell_size, t.resolution))
        return false;
    std::size_t next = entries;
    for (auto offset : std::span(t.words).first(entries)) {
        if (!offset)
            continue;
        if (offset != next || t.words.size() - next < brick_words)
            return false;
        next += brick_words;
    }
    return next == t.words.size();
}
CellBounds occupied_bounds(const Tile& tile) {
    if (!valid_tile(tile))
        throw std::invalid_argument("Invalid foliage tile");
    const auto n = tile.resolution;
    const auto side = bricks(n);
    CellBounds bounds{{n, n, n}, {}};
    for (unsigned index = 0; index < cube(side); ++index) {
        const auto offset = tile.words[index];
        if (!offset)
            continue;
        for (unsigned word = 0; word < 16; ++word) {
            auto mask = tile.words[offset + word];
            while (mask) {
                const auto cell = word * 32U + static_cast<unsigned>(std::countr_zero(mask));
                mask &= mask - 1;
                const std::array p{8 * (index % side) + cell % 8, 8 * (index / side % side) + cell / 8 % 8,
                                   8 * (index / (side * side)) + cell / 64};
                for (unsigned axis = 0; axis < 3; ++axis) {
                    bounds.low[axis] = std::min(bounds.low[axis], p[axis]);
                    bounds.high[axis] = std::max(bounds.high[axis], p[axis] + 1);
                }
            }
        }
    }
    return bounds;
}
Tile pack_tile(std::span<const Cell> chain, std::uint32_t n, std::array<float, 3> origin, float size) {
    if (!valid_resolution(n) || !valid_bounds(origin, size, n))
        throw std::invalid_argument("Invalid foliage tile grid");
    Tile tile{n, origin, size, std::vector<std::uint32_t>(directory_words(n))};
    std::size_t entry = 0;
    for (; n; n /= 2) {
        const auto count = cube(n);
        if (chain.size() < count)
            throw std::invalid_argument("Incomplete foliage mip chain");
        const auto side = bricks(n);
        for (unsigned bz = 0; bz < side; ++bz)
            for (unsigned by = 0; by < side; ++by)
                for (unsigned bx = 0; bx < side; ++bx, ++entry) {
                    std::uint32_t offset = 0;
                    for (unsigned z = 0; z < std::min(n, 8U); ++z)
                        for (unsigned y = 0; y < std::min(n, 8U); ++y)
                            for (unsigned x = 0; x < std::min(n, 8U); ++x) {
                                const auto& cell = chain[bx * 8 + x + n * (by * 8 + y + n * (bz * 8 + z))];
                                if (!cell[2])
                                    continue;
                                if (!offset) {
                                    offset = static_cast<std::uint32_t>(tile.words.size());
                                    tile.words[entry] = offset;
                                    tile.words.resize(tile.words.size() + brick_words);
                                }
                                const auto local = x + 8 * (y + 8 * z);
                                tile.words[offset + local / 32] |= 1U << (local % 32);
                                std::ranges::copy(cell, tile.words.begin() + offset + 16 + local * 3);
                            }
                }
        chain = chain.subspan(count);
    }
    if (!chain.empty())
        throw std::invalid_argument("Unexpected foliage mip data");
    return tile;
}

bool load_tile(const std::filesystem::path& dir, std::uint64_t key, Tile& out) {
    const auto path = dir / filename(key);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const auto size = file.tellg();
    Header h{};
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(&h), sizeof(h)) || h.magic != signature ||
        h.version != bake_version || h.key != key || !valid_resolution(h.resolution))
        return false;
    const auto entries = directory_words(h.resolution);
    if (h.word_count < entries || h.word_count > entries * (brick_words + 1) ||
        size != static_cast<std::streamoff>(sizeof(h) + std::uint64_t(h.word_count) * 4))
        return false;
    Tile t{h.resolution, h.origin, h.cell_size, std::vector<std::uint32_t>(h.word_count)};
    if (!file.read(reinterpret_cast<char*>(t.words.data()), h.word_count * 4) ||
        h.checksum != checksum(h, t.words) || !valid_tile(t))
        return false;
    out = std::move(t);
    file.close();
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
    return true;
}

bool store_tile(const std::filesystem::path& dir, std::uint64_t key, const Tile& tile,
                std::uintmax_t budget) {
    if (!valid_tile(tile))
        return false;
    const std::uintmax_t bytes = sizeof(Header) + tile.words.size() * 4;
    if (bytes > budget)
        return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return false;
    const WriterLock lock(dir);
    if (!lock)
        return false;
    const auto target = dir / filename(key);
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
        std::uintmax_t bytes;
    };
    std::vector<Entry> files;
    std::uintmax_t other = 0;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path();
        if (path == target || !owned_filename(path) ||
            !std::filesystem::is_regular_file(it->symlink_status(ec)))
            continue;
        const auto size = it->file_size(ec);
        if (ec || size > UINTMAX_MAX - other || files.size() >= 65536)
            return false;
        other += size;
        const auto time = it->last_write_time(ec);
        if (ec)
            return false;
        files.push_back({path, time, size});
    }
    if (ec)
        return false;
    std::ranges::sort(files, {}, &Entry::time);
    for (const auto& f : files) {
        if (other <= budget - bytes)
            break;
        if (std::filesystem::remove(f.path, ec))
            other -= f.bytes;
    }
    if (other > budget - bytes)
        return false;
    Header h{signature,   bake_version,  key, tile.resolution, static_cast<std::uint32_t>(tile.words.size()),
             tile.origin, tile.cell_size};
    h.checksum = checksum(h, tile.words);
    const auto temp = dir / L"foliage-cache.tmp";
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    f.write(reinterpret_cast<const char*>(tile.words.data()), tile.words.size() * 4);
    f.close();
    if (f && MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    std::filesystem::remove(temp, ec);
    return false;
}
} // namespace vcr::foliage
