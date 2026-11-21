#include "asset_pak.hpp"
#include "native_asset_path.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

constexpr std::array<std::uint8_t, 8U> pak_magic{
    static_cast<std::uint8_t>('V'),
    static_cast<std::uint8_t>('C'),
    static_cast<std::uint8_t>('R'),
    static_cast<std::uint8_t>('P'),
    static_cast<std::uint8_t>('A'),
    static_cast<std::uint8_t>('K'),
    static_cast<std::uint8_t>('0'),
    static_cast<std::uint8_t>('1'),
};

constexpr std::size_t max_entry_path_bytes = 65'535U;
constexpr std::uint64_t min_index_entry_bytes = 4ULL + 1ULL + 8ULL + 8ULL;
constexpr std::uint64_t max_index_table_bytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t write_chunk_bytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::size_t index_chunk_capacity = 128ULL * 1024ULL;

void set_error(std::string* const out_error, std::string message) {
    if (out_error != nullptr) {
        *out_error = std::move(message);
    }
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

[[nodiscard]] bool checked_add(
    const std::uint64_t lhs,
    const std::uint64_t rhs,
    std::uint64_t& output) noexcept {
    if (rhs > (std::numeric_limits<std::uint64_t>::max)() - lhs) {
        return false;
    }
    output = lhs + rhs;
    return true;
}

[[nodiscard]] bool checked_align_up(
    const std::uint64_t value,
    const std::uint64_t alignment,
    std::uint64_t& output) noexcept {
    if (alignment == 0U) {
        return false;
    }
    const std::uint64_t remainder = value % alignment;
    if (remainder == 0U) {
        output = value;
        return true;
    }
    return checked_add(value, alignment - remainder, output);
}

[[nodiscard]] std::uint32_t read_u32_le(const std::uint8_t* const ptr) noexcept {
    return static_cast<std::uint32_t>(ptr[0]) |
        (static_cast<std::uint32_t>(ptr[1]) << 8U) |
        (static_cast<std::uint32_t>(ptr[2]) << 16U) |
        (static_cast<std::uint32_t>(ptr[3]) << 24U);
}

[[nodiscard]] std::uint64_t read_u64_le(const std::uint8_t* const ptr) noexcept {
    return static_cast<std::uint64_t>(ptr[0]) |
        (static_cast<std::uint64_t>(ptr[1]) << 8U) |
        (static_cast<std::uint64_t>(ptr[2]) << 16U) |
        (static_cast<std::uint64_t>(ptr[3]) << 24U) |
        (static_cast<std::uint64_t>(ptr[4]) << 32U) |
        (static_cast<std::uint64_t>(ptr[5]) << 40U) |
        (static_cast<std::uint64_t>(ptr[6]) << 48U) |
        (static_cast<std::uint64_t>(ptr[7]) << 56U);
}

void append_u32_le(
    std::vector<std::uint8_t>& out,
    const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void append_u64_le(
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

[[nodiscard]] bool is_valid_pak_entry_path(
    const std::string_view path) noexcept {
    if (path.empty() || path.size() > max_entry_path_bytes) {
        return false;
    }
    if (path.front() == '/' || path.back() == '/') {
        return false;
    }
    std::size_t segment_start = 0U;
    for (std::size_t i = 0U; i <= path.size(); ++i) {
        const bool at_end = (i == path.size());
        const char ch = at_end ? '/' : path[i];
        if (!at_end && (ch == '\0' || ch == '\\')) {
            return false;
        }
        if (ch == '/') {
            const std::string_view segment =
                path.substr(segment_start, i - segment_start);
            if (segment.empty() || segment == "." || segment == "..") {
                return false;
            }
            segment_start = i + 1U;
        }
    }
    return true;
}

[[nodiscard]] std::string normalize_root_key(
    const std::filesystem::path& root) {
    if (root.empty()) {
        return {};
    }
    std::string key = path_to_utf8(root.lexically_normal());
#if defined(_WIN32)
    for (char& ch : key) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    // Normal and extended Windows spellings share one archive and one
    // invalidation key. File IO still uses the caller's canonical path.
    if (key.starts_with("//?/unc/")) key.replace(0, 8, "//");
    else if (key.starts_with("//?/") && key.size() >= 6 && key[5] == ':') key.erase(0, 4);
#endif
    while (key.size() > 1U && key.back() == '/' &&
           !(key.size() == 3U && key[1] == ':')) {
        key.pop_back();
    }
    return key;
}

class TempFileGuard final {
public:
    explicit TempFileGuard(std::filesystem::path path) noexcept
        : path_(std::move(path)), armed_(true) {}

    ~TempFileGuard() noexcept {
        if (armed_ && !path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;

    void disarm() noexcept {
        armed_ = false;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
    bool armed_ = false;
};

[[nodiscard]] std::filesystem::path make_temp_pak_path(
    const std::filesystem::path& target_pak_path) {
    static std::atomic<std::uint64_t> counter{1U};
    const std::uint64_t id = counter.fetch_add(1U, std::memory_order_relaxed);
    std::filesystem::path temp_path = target_pak_path;
    temp_path += ".tmp." + std::to_string(id);
    return temp_path;
}

[[nodiscard]] bool write_stream_bytes(
    std::ofstream& stream,
    const std::uint8_t* const data,
    const std::size_t size) {
    if (size == 0U) {
        return true;
    }
    const std::size_t max_chunk = (std::min)(
        write_chunk_bytes,
        static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)()));
    std::size_t written = 0U;
    while (written < size) {
        const std::size_t chunk = (std::min)(size - written, max_chunk);
        stream.write(
            reinterpret_cast<const char*>(data + written),
            static_cast<std::streamsize>(chunk));
        if (!stream) {
            return false;
        }
        written += chunk;
    }
    return true;
}

#if defined(_WIN32)
class PakOsFile final {
public:
    PakOsFile() noexcept = default;
    ~PakOsFile() noexcept {
        close();
    }

    PakOsFile(const PakOsFile&) = delete;
    PakOsFile& operator=(const PakOsFile&) = delete;

    PakOsFile(PakOsFile&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    PakOsFile& operator=(PakOsFile&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] bool open(
        const std::filesystem::path& path,
        std::uint64_t& out_file_size,
        std::string* const out_error) {
        close();
        out_file_size = 0U;
        const HANDLE opened = ::CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (opened == INVALID_HANDLE_VALUE) {
            const DWORD err = ::GetLastError();
            set_error(
                out_error,
                "Unable to open asset pak '" + path_to_utf8(path) +
                    "' (Win32 error " + std::to_string(err) + ").");
            return false;
        }
        handle_ = opened;
        LARGE_INTEGER size_li{};
        if (::GetFileSizeEx(handle_, &size_li) == FALSE ||
            size_li.QuadPart < 0) {
            const DWORD err = ::GetLastError();
            close();
            set_error(
                out_error,
                "Unable to query asset pak size '" + path_to_utf8(path) +
                    "' (Win32 error " + std::to_string(err) + ").");
            return false;
        }
        out_file_size = static_cast<std::uint64_t>(size_li.QuadPart);
        return true;
    }

    // Safe for lock-free concurrent reads from any number of threads: every
    // ReadFile call passes an explicit stack-local OVERLAPPED offset.
    [[nodiscard]] bool read_at(
        const std::uint64_t offset,
        const std::size_t size,
        std::uint8_t* const destination,
        std::string* const out_error) const {
        if (size == 0U) {
            return true;
        }
        if (handle_ == INVALID_HANDLE_VALUE || handle_ == nullptr ||
            destination == nullptr) {
            set_error(out_error, "Asset pak file handle is not open.");
            return false;
        }
        constexpr std::size_t max_chunk_bytes = 64ULL * 1024ULL * 1024ULL;
        std::uint64_t current_offset = offset;
        std::size_t bytes_done = 0U;
        while (bytes_done < size) {
            const std::size_t remaining = size - bytes_done;
            const DWORD to_read = static_cast<DWORD>(
                (std::min)(remaining, max_chunk_bytes));
            OVERLAPPED overlapped{};
            overlapped.Offset =
                static_cast<DWORD>(current_offset & 0xFFFFFFFFULL);
            overlapped.OffsetHigh =
                static_cast<DWORD>((current_offset >> 32U) & 0xFFFFFFFFULL);
            DWORD bytes_read = 0U;
            if (::ReadFile(
                    handle_,
                    destination + bytes_done,
                    to_read,
                    &bytes_read,
                    &overlapped) == FALSE ||
                bytes_read == 0U) {
                const DWORD err = ::GetLastError();
                set_error(
                    out_error,
                    "Failed reading asset pak at offset " +
                        std::to_string(current_offset) +
                        " (Win32 error " + std::to_string(err) + ").");
                return false;
            }
            const std::size_t read_size = static_cast<std::size_t>(bytes_read);
            if (read_size > remaining) {
                set_error(
                    out_error,
                    "Asset pak read returned unexpected byte count.");
                return false;
            }
            bytes_done += read_size;
            if (!checked_add(
                    current_offset,
                    static_cast<std::uint64_t>(read_size),
                    current_offset)) {
                set_error(out_error, "Asset pak read offset overflow.");
                return false;
            }
        }
        return true;
    }

    void close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#else
class PakOsFile final {
public:
    PakOsFile() noexcept = default;
    ~PakOsFile() noexcept = default;

    PakOsFile(const PakOsFile&) = delete;
    PakOsFile& operator=(const PakOsFile&) = delete;

    PakOsFile(PakOsFile&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mutex_);
        stream_ = std::move(other.stream_);
    }

    PakOsFile& operator=(PakOsFile&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(mutex_, other.mutex_);
            stream_ = std::move(other.stream_);
        }
        return *this;
    }

    [[nodiscard]] bool open(
        const std::filesystem::path& path,
        std::uint64_t& out_file_size,
        std::string* const out_error) {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_.close();
        stream_.clear();
        out_file_size = 0U;
        stream_.open(path, std::ios::binary | std::ios::ate);
        if (!stream_) {
            set_error(
                out_error,
                "Unable to open asset pak '" + path_to_utf8(path) + "'.");
            return false;
        }
        const std::ifstream::pos_type end_pos = stream_.tellg();
        if (end_pos < std::ifstream::pos_type(0)) {
            stream_.close();
            set_error(
                out_error,
                "Unable to query asset pak size '" + path_to_utf8(path) + "'.");
            return false;
        }
        out_file_size = static_cast<std::uint64_t>(end_pos);
        return true;
    }

    [[nodiscard]] bool read_at(
        const std::uint64_t offset,
        const std::size_t size,
        std::uint8_t* const destination,
        std::string* const out_error) const {
        if (size == 0U) {
            return true;
        }
        if (destination == nullptr) {
            set_error(out_error, "Asset pak destination buffer is null.");
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stream_.is_open()) {
            set_error(out_error, "Asset pak file stream is not open.");
            return false;
        }
        if (offset > static_cast<std::uint64_t>(
                (std::numeric_limits<std::streamoff>::max)())) {
            set_error(out_error, "Asset pak offset exceeds stream limits.");
            return false;
        }
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream_) {
            set_error(out_error, "Failed seeking in asset pak.");
            return false;
        }
        constexpr std::size_t max_chunk_bytes = 64ULL * 1024ULL * 1024ULL;
        const std::size_t capped_chunk = (std::min)(
            max_chunk_bytes,
            static_cast<std::size_t>(
                (std::numeric_limits<std::streamsize>::max)()));
        std::size_t bytes_done = 0U;
        while (bytes_done < size) {
            const std::size_t chunk = (std::min)(size - bytes_done, capped_chunk);
            stream_.read(
                reinterpret_cast<char*>(destination + bytes_done),
                static_cast<std::streamsize>(chunk));
            if (!stream_ ||
                static_cast<std::size_t>(stream_.gcount()) != chunk) {
                set_error(out_error, "Failed reading bytes from asset pak.");
                return false;
            }
            bytes_done += chunk;
        }
        return true;
    }

    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_.close();
        stream_.clear();
    }

private:
    mutable std::mutex mutex_{};
    mutable std::ifstream stream_{};
};
#endif

struct PakEntry {
    std::string path{};
    std::uint64_t offset = 0U;
    std::uint64_t size = 0U;
};

struct PakLocation {
    std::uint64_t offset = 0U;
    std::uint64_t size = 0U;
};

struct PakArchive {
    PakOsFile file{};
    std::uint64_t file_size = 0U;
    std::vector<PakEntry> entries{};

    [[nodiscard]] std::optional<PakLocation> find(
        const std::string_view normalized_relative_path) const noexcept {
        const auto it = std::lower_bound(
            entries.begin(),
            entries.end(),
            normalized_relative_path,
            [](const PakEntry& entry, const std::string_view target) noexcept {
                return entry.path < target;
            });
        if (it != entries.end() && it->path == normalized_relative_path) {
            return PakLocation{
                .offset = it->offset,
                .size = it->size,
            };
        }
        return std::nullopt;
    }
};

[[nodiscard]] std::shared_ptr<const PakArchive> open_and_validate_archive(
    const std::filesystem::path& pak_path,
    const std::uintmax_t expected_file_size,
    AssetPakReadStatus& out_status,
    std::string* const out_error) {
    PakOsFile file;
    std::uint64_t file_size = 0U;
    if (!file.open(pak_path, file_size, out_error)) {
        out_status = AssetPakReadStatus::io_error;
        return nullptr;
    }
    if (file_size != expected_file_size) {
        out_status = AssetPakReadStatus::io_error;
        set_error(out_error, "Asset pak changed concurrently while opening.");
        return nullptr;
    }
    if (file_size < asset_pak_header_bytes) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(out_error, "Asset pak is smaller than the 32-byte header.");
        return nullptr;
    }

    std::array<std::uint8_t, asset_pak_header_bytes> header{};
    if (!file.read_at(0U, header.size(), header.data(), out_error)) {
        out_status = AssetPakReadStatus::io_error;
        return nullptr;
    }
    if (std::memcmp(header.data(), pak_magic.data(), pak_magic.size()) != 0) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(out_error, "Asset pak magic header is invalid.");
        return nullptr;
    }

    const std::uint32_t version = read_u32_le(header.data() + 8U);
    if (version != asset_pak_format_version) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(out_error, "Asset pak version is unsupported.");
        return nullptr;
    }

    const std::uint32_t entry_count = read_u32_le(header.data() + 12U);
    if (entry_count > asset_pak_max_entries) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(out_error, "Asset pak entry count exceeds maximum limit.");
        return nullptr;
    }

    const std::uint64_t index_offset = read_u64_le(header.data() + 16U);
    const std::uint64_t index_byte_size = read_u64_le(header.data() + 24U);
    if (index_offset < asset_pak_header_bytes ||
        index_offset > file_size ||
        index_byte_size != file_size - index_offset) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(out_error, "Asset pak index region bounds are invalid.");
        return nullptr;
    }
    if (entry_count == 0U && index_byte_size != 0U) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(out_error, "Empty asset pak must have a zero-byte index.");
        return nullptr;
    }
    if (entry_count > 0U &&
        index_byte_size <
            static_cast<std::uint64_t>(entry_count) * min_index_entry_bytes) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(
            out_error,
            "Asset pak index table is smaller than minimum entry size.");
        return nullptr;
    }
    if (index_byte_size > max_index_table_bytes ||
        index_byte_size > (std::numeric_limits<std::size_t>::max)()) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(out_error, "Asset pak index table exceeds size limits.");
        return nullptr;
    }

    std::vector<std::uint8_t> index_bytes(
        static_cast<std::size_t>(index_byte_size));
    if (!index_bytes.empty() &&
        !file.read_at(
            index_offset,
            index_bytes.size(),
            index_bytes.data(),
            out_error)) {
        out_status = AssetPakReadStatus::io_error;
        return nullptr;
    }

    std::vector<PakEntry> entries;
    entries.reserve(entry_count);
    std::size_t cursor = 0U;
    for (std::uint32_t i = 0U; i < entry_count; ++i) {
        const std::size_t remaining = index_bytes.size() - cursor;
        if (remaining < 20U) {
            out_status = AssetPakReadStatus::invalid_archive;
            set_error(out_error, "Asset pak index entry is truncated.");
            return nullptr;
        }
        const std::uint32_t path_length =
            read_u32_le(index_bytes.data() + cursor);
        cursor += 4U;
        if (path_length == 0U ||
            path_length > max_entry_path_bytes ||
            static_cast<std::size_t>(path_length) > remaining - 20U) {
            out_status = AssetPakReadStatus::invalid_archive;
            set_error(out_error, "Asset pak index path length is invalid.");
            return nullptr;
        }
        const std::string_view path_view(
            reinterpret_cast<const char*>(index_bytes.data() + cursor),
            static_cast<std::size_t>(path_length));
        if (!is_valid_pak_entry_path(path_view)) {
            out_status = AssetPakReadStatus::invalid_archive;
            set_error(out_error, "Asset pak contains an invalid entry path.");
            return nullptr;
        }
        if (i > 0U && !(entries.back().path < path_view)) {
            out_status = AssetPakReadStatus::invalid_archive;
            set_error(
                out_error,
                "Asset pak index entries are unsorted or duplicated.");
            return nullptr;
        }
        cursor += static_cast<std::size_t>(path_length);

        const std::uint64_t offset = read_u64_le(index_bytes.data() + cursor);
        const std::uint64_t size =
            read_u64_le(index_bytes.data() + cursor + 8U);
        cursor += 16U;

        if (offset < asset_pak_header_bytes ||
            offset > index_offset ||
            size > index_offset - offset) {
            out_status = AssetPakReadStatus::invalid_archive;
            set_error(
                out_error,
                "Asset pak entry payload exceeds payload region.");
            return nullptr;
        }
        entries.push_back(PakEntry{
            .path = std::string(path_view),
            .offset = offset,
            .size = size,
        });
    }
    if (cursor != index_bytes.size()) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(
            out_error,
            "Asset pak index table contains trailing bytes.");
        return nullptr;
    }

    auto archive = std::make_shared<PakArchive>();
    archive->file = std::move(file);
    archive->file_size = file_size;
    archive->entries = std::move(entries);
    out_status = AssetPakReadStatus::ok;
    return archive;
}

struct CachedPakRecord {
    std::string root_key{};
    std::uintmax_t file_size = 0U;
    std::filesystem::file_time_type last_write_time{};
    std::uint64_t last_used_seq = 0U;
    // Shared ownership keeps PakOsFile open during concurrent lock-free reads
    // even if another thread invalidates or LRU-evicts the registry slot.
    std::shared_ptr<const PakArchive> archive{};
};

struct PakCacheState {
    // Lock order when acquiring both: open_mutex, then cache_mutex.
    std::mutex open_mutex{};
    std::mutex cache_mutex{};
    std::vector<CachedPakRecord> entries{};
    std::uint64_t access_seq = 0U;
    std::atomic<std::size_t> cached_count{0U};
    std::atomic<std::size_t> high_water{0U};
};

[[nodiscard]] PakCacheState& pak_cache_state() noexcept {
    static PakCacheState state;
    return state;
}

void update_cache_counters_locked(PakCacheState& state) noexcept {
    const std::size_t count = state.entries.size();
    state.cached_count.store(count, std::memory_order_seq_cst);
    const std::size_t prev_hw =
        state.high_water.load(std::memory_order_relaxed);
    if (count > prev_hw) {
        state.high_water.store(count, std::memory_order_release);
    }
}

void evict_matching_key(
    PakCacheState& state,
    const std::string_view root_key) {
    if (root_key.empty() ||
        state.cached_count.load(std::memory_order_seq_cst) == 0U) {
        return;
    }
    std::vector<std::shared_ptr<const PakArchive>> dropped;
    {
        std::scoped_lock lock(state.open_mutex, state.cache_mutex);
        for (auto it = state.entries.begin(); it != state.entries.end();) {
            if (it->root_key == root_key) {
                dropped.push_back(std::move(it->archive));
                it = state.entries.erase(it);
            } else {
                ++it;
            }
        }
        update_cache_counters_locked(state);
    }
}

[[nodiscard]] bool stat_pak_file(
    const std::filesystem::path& pak_path,
    std::uintmax_t& out_file_size,
    std::filesystem::file_time_type& out_mtime,
    AssetPakReadStatus& out_status,
    std::string* const out_error) {
    std::error_code ec;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(pak_path, ec);
    if (!ec && std::filesystem::is_symlink(status)) {
        out_status = AssetPakReadStatus::symbolic_link;
        set_error(
            out_error,
            "Asset pak cannot be a symbolic link: '" +
                path_to_utf8(pak_path) + "'.");
        return false;
    }
    if (status.type() == std::filesystem::file_type::not_found ||
        ec == std::errc::no_such_file_or_directory) {
        out_status = AssetPakReadStatus::not_found;
        return false;
    }
    if (ec) {
        out_status = AssetPakReadStatus::io_error;
        set_error(
            out_error,
            "Unable to query asset pak '" + path_to_utf8(pak_path) +
                "': " + ec.message());
        return false;
    }
    if (!std::filesystem::is_regular_file(status)) {
        out_status = AssetPakReadStatus::invalid_archive;
        set_error(
            out_error,
            "Asset pak path is not a regular file: '" +
                path_to_utf8(pak_path) + "'.");
        return false;
    }

    std::error_code size_ec;
    out_file_size = std::filesystem::file_size(pak_path, size_ec);
    std::error_code time_ec;
    out_mtime = std::filesystem::last_write_time(pak_path, time_ec);
    if (size_ec || time_ec) {
        out_status = AssetPakReadStatus::io_error;
        set_error(
            out_error,
            "Unable to query asset pak metadata for '" +
                path_to_utf8(pak_path) + "'.");
        return false;
    }
    return true;
}

[[nodiscard]] std::shared_ptr<const PakArchive> lookup_cached_locked(
    PakCacheState& state,
    const std::string_view root_key,
    const std::uintmax_t file_size,
    const std::filesystem::file_time_type mtime) noexcept {
    for (auto& entry : state.entries) {
        if (entry.root_key == root_key &&
            entry.file_size == file_size &&
            entry.last_write_time == mtime &&
            entry.archive != nullptr) {
            entry.last_used_seq = ++state.access_seq;
            return entry.archive;
        }
    }
    return nullptr;
}

[[nodiscard]] std::shared_ptr<const PakArchive> acquire_cached_archive(
    const std::filesystem::path& canonical_asset_root,
    AssetPakReadStatus& out_status,
    std::string* const out_error) {
    if (canonical_asset_root.empty()) {
        out_status = AssetPakReadStatus::not_found;
        return nullptr;
    }

    const std::filesystem::path pak_path =
        canonical_asset_root / default_asset_pak_filename;
    const std::string root_key = normalize_root_key(canonical_asset_root);
    PakCacheState& state = pak_cache_state();

    std::uintmax_t disk_size = 0U;
    std::filesystem::file_time_type disk_mtime{};
    if (!stat_pak_file(pak_path, disk_size, disk_mtime, out_status, out_error)) {
        evict_matching_key(state, root_key);
        return nullptr;
    }

    // Fast path: cache hit only holds cache_mutex long enough to copy the
    // shared_ptr; actual index search and file reads happen lock-free.
    {
        std::lock_guard<std::mutex> cache_lock(state.cache_mutex);
        if (auto hit = lookup_cached_locked(
                state, root_key, disk_size, disk_mtime)) {
            out_status = AssetPakReadStatus::ok;
            return hit;
        }
    }

    std::lock_guard<std::mutex> open_lock(state.open_mutex);
    if (!stat_pak_file(pak_path, disk_size, disk_mtime, out_status, out_error)) {
        std::vector<std::shared_ptr<const PakArchive>> dropped;
        {
            std::lock_guard<std::mutex> cache_lock(state.cache_mutex);
            for (auto it = state.entries.begin(); it != state.entries.end();) {
                if (it->root_key == root_key) {
                    dropped.push_back(std::move(it->archive));
                    it = state.entries.erase(it);
                } else {
                    ++it;
                }
            }
            update_cache_counters_locked(state);
        }
        return nullptr;
    }

    std::shared_ptr<const PakArchive> stale_drop;
    {
        std::lock_guard<std::mutex> cache_lock(state.cache_mutex);
        if (auto hit = lookup_cached_locked(
                state, root_key, disk_size, disk_mtime)) {
            out_status = AssetPakReadStatus::ok;
            return hit;
        }
        for (auto it = state.entries.begin(); it != state.entries.end(); ++it) {
            if (it->root_key == root_key) {
                stale_drop = std::move(it->archive);
                state.entries.erase(it);
                update_cache_counters_locked(state);
                break;
            }
        }
    }
    stale_drop.reset();

    auto opened = open_and_validate_archive(
        pak_path, disk_size, out_status, out_error);
    if (!opened) {
        return nullptr;
    }

    std::shared_ptr<const PakArchive> evicted_drop;
    {
        std::lock_guard<std::mutex> cache_lock(state.cache_mutex);
        if (state.entries.size() >= asset_pak_max_cached_archives) {
            auto victim = std::min_element(
                state.entries.begin(),
                state.entries.end(),
                [](const CachedPakRecord& a, const CachedPakRecord& b) noexcept {
                    return a.last_used_seq < b.last_used_seq;
                });
            if (victim != state.entries.end()) {
                evicted_drop = std::move(victim->archive);
                state.entries.erase(victim);
            }
        }
        state.entries.push_back(CachedPakRecord{
            .root_key = root_key,
            .file_size = disk_size,
            .last_write_time = disk_mtime,
            .last_used_seq = ++state.access_seq,
            .archive = opened,
        });
        update_cache_counters_locked(state);
    }
    evicted_drop.reset();

    out_status = AssetPakReadStatus::ok;
    return opened;
}

} // namespace

bool AssetPakWriter::add_entry(
    std::string normalized_relative_path,
    std::vector<std::uint8_t> bytes,
    std::string& error) {
    error.clear();
    if (!is_valid_pak_entry_path(normalized_relative_path)) {
        error = "Invalid asset pak entry path.";
        return false;
    }
    if (entries_.size() >= asset_pak_max_entries) {
        error = "Asset pak entry count exceeds maximum limit.";
        return false;
    }
    const auto [it, inserted] = entries_.try_emplace(
        std::move(normalized_relative_path), std::move(bytes));
    if (!inserted) {
        error = "Duplicate asset pak entry path: '" + it->first + "'.";
        return false;
    }
    return true;
}

bool AssetPakWriter::write_to_file(
    const std::filesystem::path& target_pak_path,
    std::string& error) {
    error.clear();
    if (target_pak_path.empty()) {
        error = "Target asset pak path is empty.";
        return false;
    }
    if (entries_.size() > asset_pak_max_entries) {
        error = "Asset pak entry count exceeds maximum limit.";
        return false;
    }

    std::error_code ec;
    const std::filesystem::file_status target_status =
        std::filesystem::symlink_status(target_pak_path, ec);
    if (!ec && std::filesystem::is_symlink(target_status)) {
        error = "Target asset pak path cannot be a symbolic link.";
        return false;
    }

    const std::filesystem::path parent = target_pak_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "Unable to create directory for asset pak '" +
                path_to_utf8(parent) + "': " + ec.message();
            return false;
        }
    }

    std::vector<std::uint64_t> offsets;
    offsets.reserve(entries_.size());
    std::uint64_t cursor = asset_pak_header_bytes;
    std::uint64_t index_byte_size = 0U;
    for (const auto& [path, bytes] : entries_) {
        std::uint64_t aligned_offset = 0U;
        if (!checked_align_up(
                cursor, asset_pak_payload_alignment, aligned_offset)) {
            error = "Asset pak payload offset overflow.";
            return false;
        }
        offsets.push_back(aligned_offset);
        if (!checked_add(
                aligned_offset,
                static_cast<std::uint64_t>(bytes.size()),
                cursor)) {
            error = "Asset pak payload size overflow.";
            return false;
        }
        std::uint64_t entry_index_size = 0U;
        if (!checked_add(
                20ULL,
                static_cast<std::uint64_t>(path.size()),
                entry_index_size) ||
            !checked_add(index_byte_size, entry_index_size, index_byte_size)) {
            error = "Asset pak index size overflow.";
            return false;
        }
    }

    std::uint64_t index_offset = 0U;
    std::uint64_t total_file_size = 0U;
    if (!checked_align_up(
            cursor, asset_pak_payload_alignment, index_offset) ||
        !checked_add(index_offset, index_byte_size, total_file_size)) {
        error = "Asset pak total archive size overflow.";
        return false;
    }

    TempFileGuard temp_guard(make_temp_pak_path(target_pak_path));
    {
        std::ofstream stream(
            temp_guard.path(), std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Unable to open temporary asset pak file '" +
                path_to_utf8(temp_guard.path()) + "'.";
            return false;
        }

        std::vector<std::uint8_t> header;
        header.reserve(asset_pak_header_bytes);
        header.insert(header.end(), pak_magic.begin(), pak_magic.end());
        append_u32_le(header, asset_pak_format_version);
        append_u32_le(header, static_cast<std::uint32_t>(entries_.size()));
        append_u64_le(header, index_offset);
        append_u64_le(header, index_byte_size);
        if (!write_stream_bytes(stream, header.data(), header.size())) {
            error = "Failed writing asset pak header.";
            return false;
        }

        constexpr std::array<std::uint8_t, asset_pak_payload_alignment>
            zero_pad{};
        std::uint64_t written_pos = asset_pak_header_bytes;
        std::size_t entry_index = 0U;
        for (const auto& [path, bytes] : entries_) {
            (void)path;
            const std::uint64_t entry_offset = offsets[entry_index++];
            const std::uint64_t pad_bytes = entry_offset - written_pos;
            if (pad_bytes > 0U &&
                !write_stream_bytes(
                    stream,
                    zero_pad.data(),
                    static_cast<std::size_t>(pad_bytes))) {
                error = "Failed writing asset pak alignment padding.";
                return false;
            }
            if (!write_stream_bytes(stream, bytes.data(), bytes.size())) {
                error = "Failed writing asset pak payload.";
                return false;
            }
            written_pos = entry_offset + static_cast<std::uint64_t>(bytes.size());
        }

        const std::uint64_t tail_pad = index_offset - written_pos;
        if (tail_pad > 0U &&
            !write_stream_bytes(
                stream,
                zero_pad.data(),
                static_cast<std::size_t>(tail_pad))) {
            error = "Failed writing asset pak pre-index padding.";
            return false;
        }

        std::vector<std::uint8_t> index_chunk;
        index_chunk.reserve(index_chunk_capacity);
        entry_index = 0U;
        for (const auto& [path, bytes] : entries_) {
            const std::size_t required = 20U + path.size();
            if (!index_chunk.empty() &&
                index_chunk.size() + required > index_chunk_capacity) {
                if (!write_stream_bytes(
                        stream, index_chunk.data(), index_chunk.size())) {
                    error = "Failed writing asset pak index table.";
                    return false;
                }
                index_chunk.clear();
            }
            append_u32_le(index_chunk, static_cast<std::uint32_t>(path.size()));
            index_chunk.insert(
                index_chunk.end(),
                reinterpret_cast<const std::uint8_t*>(path.data()),
                reinterpret_cast<const std::uint8_t*>(path.data()) +
                    path.size());
            append_u64_le(index_chunk, offsets[entry_index++]);
            append_u64_le(index_chunk, static_cast<std::uint64_t>(bytes.size()));
        }
        if (!index_chunk.empty() &&
            !write_stream_bytes(
                stream, index_chunk.data(), index_chunk.size())) {
            error = "Failed writing final asset pak index chunk.";
            return false;
        }

        stream.flush();
        stream.close();
        if (!stream) {
            error = "Failed flushing temporary asset pak file.";
            return false;
        }
    }

#if defined(_WIN32)
    if (::MoveFileExW(
            temp_guard.path().c_str(),
            target_pak_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD win32_err = ::GetLastError();
        error = "Unable to atomically publish asset pak '" +
            path_to_utf8(target_pak_path) +
            "' (Win32 error " + std::to_string(win32_err) + ").";
        return false;
    }
#else
    std::filesystem::rename(temp_guard.path(), target_pak_path, ec);
    if (ec) {
        error = "Unable to publish asset pak '" +
            path_to_utf8(target_pak_path) + "': " + ec.message();
        return false;
    }
#endif

    temp_guard.disarm();
    return true;
}

bool asset_pak_contains(
    const std::filesystem::path& canonical_asset_root,
    const std::string_view normalized_relative_path) {
    if (canonical_asset_root.empty() ||
        !is_valid_pak_entry_path(normalized_relative_path)) {
        return false;
    }
    AssetPakReadStatus status = AssetPakReadStatus::not_found;
    const std::shared_ptr<const PakArchive> archive =
        acquire_cached_archive(canonical_asset_root, status, nullptr);
    if (!archive) {
        return false;
    }
    return archive->find(normalized_relative_path).has_value();
}

AssetPakReadStatus try_read_asset_pak_file(
    const std::filesystem::path& canonical_asset_root,
    const std::string_view normalized_relative_path,
    const std::uintmax_t maximum_bytes,
    std::vector<std::uint8_t>& out_bytes,
    std::string* const out_error) {
    out_bytes.clear();
    if (out_error != nullptr) {
        out_error->clear();
    }

    AssetPakReadStatus status = AssetPakReadStatus::not_found;
    const std::shared_ptr<const PakArchive> archive =
        acquire_cached_archive(canonical_asset_root, status, out_error);
    if (!archive) {
        return status;
    }

    if (!is_valid_pak_entry_path(normalized_relative_path)) {
        return AssetPakReadStatus::not_found;
    }
    const std::optional<PakLocation> location =
        archive->find(normalized_relative_path);
    if (!location.has_value()) {
        return AssetPakReadStatus::not_found;
    }

    if (location->size > maximum_bytes ||
        location->size > (std::numeric_limits<std::size_t>::max)()) {
        set_error(
            out_error,
            "Packed asset file exceeds its configured size limit.");
        return AssetPakReadStatus::file_too_large;
    }

    if (location->size == 0U) {
        return AssetPakReadStatus::ok;
    }

    out_bytes.resize(static_cast<std::size_t>(location->size));
    if (!archive->file.read_at(
            location->offset,
            out_bytes.size(),
            out_bytes.data(),
            out_error)) {
        out_bytes.clear();
        return AssetPakReadStatus::io_error;
    }
    return AssetPakReadStatus::ok;
}

void invalidate_asset_pak_cache(const std::filesystem::path& asset_root) {
    PakCacheState& state = pak_cache_state();
    std::string key_raw;
    std::string key_canon;
    if (!asset_root.empty()) {
        key_raw = normalize_root_key(asset_root);
        std::error_code ec;
        const std::filesystem::path canon =
            weakly_canonical_asset_path(asset_root, ec);
        if (!ec && !canon.empty()) {
            key_canon = normalize_root_key(canon);
        }
    }

    std::vector<std::shared_ptr<const PakArchive>> dropped;
    {
        std::scoped_lock lock(state.open_mutex, state.cache_mutex);
        if (asset_root.empty()) {
            dropped.reserve(state.entries.size());
            for (auto& entry : state.entries) {
                dropped.push_back(std::move(entry.archive));
            }
            state.entries.clear();
        } else {
            for (auto it = state.entries.begin(); it != state.entries.end();) {
                if (it->root_key == key_raw ||
                    (!key_canon.empty() && it->root_key == key_canon)) {
                    dropped.push_back(std::move(it->archive));
                    it = state.entries.erase(it);
                } else {
                    ++it;
                }
            }
        }
        update_cache_counters_locked(state);
    }
}

std::size_t asset_pak_cache_high_water() noexcept {
    return pak_cache_state().high_water.load(std::memory_order_acquire);
}

} // namespace vcr
