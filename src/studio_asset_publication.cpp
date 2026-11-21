#include "studio_asset_publication.hpp"
#include "native_asset_path.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

namespace vcr {
namespace fs = std::filesystem;
namespace {

constexpr auto marker_name = "studio-export-v1";
constexpr auto journal_name = "pending.json";

std::string display_path(const fs::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

bool file_error(const char* operation, const fs::path& path, const std::error_code& ec, std::string& error) {
    error = std::string(operation) + " '" + display_path(path) + "': " + ec.message();
    return false;
}

bool check_location(const fs::path& root, const fs::path& relative, std::string& error) {
    if (!StudioAssetPublication::valid_relative_path(relative)) {
        error = "Invalid relative studio export path: " + display_path(relative);
        return false;
    }
    auto current = root;
    for (const auto& part : relative) {
        current /= part;
        const auto flags = GetFileAttributesW(current.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) continue;
            return file_error("Cannot inspect", current, {static_cast<int>(code), std::system_category()}, error);
        }
        if ((flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            error = "Studio export destination contains a reparse point: " + display_path(current);
            return false;
        }
    }
    return true;
}

bool make_parent(const fs::path& path, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return !ec || file_error("Cannot create parent of", path, ec, error);
}

bool move_entry(const fs::path& from, const fs::path& to, std::string& error) {
    // Both paths are on the same volume. The kernel completes the rename
    // before returning. The journal handles writer termination between moves.
    if (MoveFileExW(from.c_str(), to.c_str(), 0)) return true;
    const auto code = GetLastError();
    return file_error("Cannot move", from, {static_cast<int>(code), std::system_category()}, error);
}

// Keep the ownership marker until everything else is gone. A process may
// terminate during cleanup, and the next writer must recognize its own files.
void discard_workspace(const fs::path& workspace, std::error_code& ec) {
    for (const auto name : {"assets", "previous", "pending.tmp"}) {
        fs::remove_all(workspace / name, ec);
        if (ec) return;
    }
    fs::remove(workspace / marker_name, ec);
    if (!ec) fs::remove(workspace, ec);
}

} // namespace

bool StudioAssetPublication::valid_relative_path(const fs::path& path) {
    if (path.empty() || path.has_root_path() || path.lexically_normal() == ".") return false;
    for (const auto& part : path) {
        const auto& s = part.native();
        if (s == L".") continue;
        if (s.empty() || s.back() == L'.' || s.back() == L' ' ||
            s.find_first_of(L"<>:\"|?*") != s.npos) return false;
    }
    return true;
}

void StudioAssetPublication::CloseLock::operator()(void* handle) const noexcept { CloseHandle(handle); }

StudioAssetPublication::~StudioAssetPublication() noexcept {
    try {
        std::error_code ec;
        if (discard_workspace_) discard_workspace(workspace_, ec);
        if (lock_) {
            lock_.reset();
            auto parent = workspace_.parent_path();
            while (RemoveDirectoryW(parent.c_str()) && parent != private_root_)
                parent = parent.parent_path();
        }
    } catch (const std::exception&) {
        // Path construction during cleanup can exhaust memory. The handle
        // still closes and the next publisher resumes from the on-disk state.
    }
}

bool StudioAssetPublication::begin(const fs::path& asset_root, const fs::path& package, std::string& error) {
    error.clear();
    std::error_code ec;
    root_ = fs::absolute(asset_root, ec);
    if (!ec) root_ = weakly_canonical_asset_path(extended_windows_path(root_.lexically_normal()), ec);
    if (ec) return file_error("Cannot resolve asset root", asset_root, ec, error);
    if (root_.filename().empty() && root_.has_relative_path()) root_ = root_.parent_path();
    if (!check_location(root_, package, error)) return false;
    private_root_ = root_.parent_path() / (L"." + root_.filename().wstring() + L".vcr-studio-exports");
    const auto work_relative = private_root_.filename() / package / ".work";
    if (!check_location(root_.parent_path(), work_relative, error)) return false;
    workspace_ = (private_root_ / package / ".work").make_preferred();
    staging_ = workspace_ / "assets";
    const auto lock_path = workspace_.parent_path() / ".publish.lock";
    if (!make_parent(lock_path, error)) return false;
    const auto handle = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        return file_error("Cannot lock studio export", lock_path, {static_cast<int>(code), std::system_category()}, error);
    }
    lock_.reset(handle);
    const auto marker = workspace_ / marker_name;
    const auto journal = workspace_ / journal_name;
    if (!fs::create_directory(workspace_, ec)) {
        if (ec) return file_error("Cannot create export workspace", workspace_, ec, error);
        if (!fs::is_regular_file(marker, ec) && !fs::is_empty(workspace_, ec)) {
            error = "Unrecognized studio export workspace: " + display_path(workspace_);
            return false;
        }
        const bool pending = fs::exists(journal, ec);
        if (ec) return file_error("Cannot inspect export journal", journal, ec, error);
        if (pending) {
            std::ifstream in(journal, std::ios::binary);
            const auto plan = nlohmann::json::parse(in, nullptr, false);
            if (!plan.is_array() || plan.empty()) {
                error = "Cannot recover the studio export journal: " + display_path(journal);
                return false;
            }
            std::vector<fs::path> entries;
            for (const auto& item : plan) {
                if (!item.is_string()) { error = "Invalid studio export journal entry."; return false; }
                entries.push_back(item.get<fs::path>());
                if (!check_location(root_, entries.back(), error)) return false;
            }
            in.close();
            if (!rollback(entries, error)) return false;
        }
        discard_workspace(workspace_, ec);
        if (ec) return file_error("Cannot clean previous export", workspace_, ec, error);
        fs::create_directory(workspace_, ec);
        if (ec) return file_error("Cannot recreate export workspace", workspace_, ec, error);
    }
    discard_workspace_ = true;
    std::ofstream owner(marker, std::ios::binary);
    owner.close();
    if (!owner) { error = "Cannot mark studio export workspace: " + display_path(marker); return false; }
    fs::create_directory(staging_, ec);
    return !ec || file_error("Cannot create staging root", staging_, ec, error);
}

bool StudioAssetPublication::rollback(std::span<const fs::path> entries, std::string& error) {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const auto rel = fs::path(*it).make_preferred();
        const auto dest = root_ / rel, fresh = staging_ / rel, old = workspace_ / "previous" / rel;
        std::error_code ec;
        const bool staged = fs::exists(fresh, ec);
        if (ec) return file_error("Cannot inspect staged output", fresh, ec, error);
        if (!staged && !move_entry(dest, fresh, error)) return false;
        const bool backed_up = fs::exists(old, ec);
        if (ec) return file_error("Cannot inspect previous output", old, ec, error);
        if (backed_up && !move_entry(old, dest, error)) return false;
    }
    // Recovery is idempotent until the journal is removed.
    std::error_code ec;
    fs::remove(workspace_ / journal_name, ec);
    return !ec || file_error("Cannot retire export journal", workspace_, ec, error);
}

bool StudioAssetPublication::commit(std::span<const fs::path> entries, std::string& error) {
    error.clear();
    std::error_code ec;
    for (auto rel : entries) {
        rel.make_preferred();
        if (!check_location(root_, rel, error)) return false;
        const auto src = staging_ / rel, dst = root_ / rel;
        const auto source = fs::status(src, ec);
        if (ec || (!fs::is_regular_file(source) && !fs::is_directory(source))) {
            error = "Missing staged studio output: " + display_path(src);
            return false;
        }
        const bool exists = fs::exists(dst, ec);
        if (ec) return file_error("Cannot inspect studio output", dst, ec, error);
        if (exists && fs::status(dst, ec).type() != source.type()) {
            error = "Incompatible destination for studio output: " + display_path(dst);
            return false;
        }
        if (!make_parent(dst, error) || !make_parent(workspace_ / "previous" / rel, error)) return false;
    }
    const auto journal = workspace_ / journal_name;
    const auto temporary = workspace_ / "pending.tmp";
    std::ofstream out(temporary, std::ios::binary);
    out << nlohmann::json(entries).dump();
    out.close();
    if (!out) { error = "Cannot write studio export journal."; return false; }
    if (!move_entry(temporary, journal, error)) return false;
    discard_workspace_ = false;
    for (auto rel : entries) {
        rel.make_preferred();
        const auto dst = root_ / rel;
        const bool exists = fs::exists(dst, ec);
        if (ec) { file_error("Cannot inspect destination", dst, ec, error); break; }
        if (exists && !move_entry(dst, workspace_ / "previous" / rel, error)) break;
        if (!move_entry(staging_ / rel, dst, error)) break;
    }
    if (error.empty()) {
        fs::remove(journal, ec);
        if (ec) file_error("Cannot retire export journal", journal, ec, error);
    }
    if (!error.empty()) {
        std::string recovery_error;
        discard_workspace_ = rollback(entries, recovery_error);
        if (!discard_workspace_) error += " Recovery pending: " + recovery_error;
        return false;
    }
    discard_workspace_ = true;
    return true;
}
} // namespace vcr
