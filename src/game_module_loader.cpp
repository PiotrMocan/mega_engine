#include "game_module_loader.hpp"

#include "gameplay_session.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

std::atomic<std::uint64_t> g_shadow_copy_serial{0U};

[[nodiscard]] std::string path_to_utf8_local(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

void close_native_library(void* const handle) noexcept {
#if defined(_WIN32)
    if (handle != nullptr) {
        static_cast<void>(FreeLibrary(static_cast<HMODULE>(handle)));
    }
#else
    static_cast<void>(handle);
#endif
}

#if defined(_WIN32)
class ScopedNativeLibrary final {
public:
    ScopedNativeLibrary() noexcept = default;
    explicit ScopedNativeLibrary(const HMODULE handle) noexcept
        : handle_(handle) {}
    ~ScopedNativeLibrary() noexcept {
        reset();
    }

    ScopedNativeLibrary(const ScopedNativeLibrary&) = delete;
    ScopedNativeLibrary& operator=(const ScopedNativeLibrary&) = delete;

    ScopedNativeLibrary(ScopedNativeLibrary&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    ScopedNativeLibrary& operator=(ScopedNativeLibrary&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HMODULE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] void* release() noexcept {
        return static_cast<void*>(std::exchange(handle_, nullptr));
    }

    void reset() noexcept {
        if (handle_ != nullptr) {
            static_cast<void>(FreeLibrary(handle_));
            handle_ = nullptr;
        }
    }

private:
    HMODULE handle_ = nullptr;
};

class ScopedDllDirectoryCookie final {
public:
    ScopedDllDirectoryCookie() noexcept = default;
    explicit ScopedDllDirectoryCookie(const wchar_t* const directory) noexcept
        : cookie_(directory != nullptr ? AddDllDirectory(directory) : nullptr) {}
    ~ScopedDllDirectoryCookie() noexcept {
        reset();
    }

    ScopedDllDirectoryCookie(const ScopedDllDirectoryCookie&) = delete;
    ScopedDllDirectoryCookie& operator=(const ScopedDllDirectoryCookie&) = delete;

    void reset() noexcept {
        if (cookie_ != nullptr) {
            static_cast<void>(RemoveDllDirectory(cookie_));
            cookie_ = nullptr;
        }
    }

private:
    DLL_DIRECTORY_COOKIE cookie_ = nullptr;
};
#endif

} // namespace

LoadedGameModule::~LoadedGameModule() noexcept {
    unload(nullptr);
}

LoadedGameModule::LoadedGameModule(LoadedGameModule&& other) noexcept
    : library_handle_(std::exchange(other.library_handle_, nullptr)),
      api_(std::exchange(other.api_, nullptr)),
      instance_id_(std::exchange(other.instance_id_, 0U)),
      bound_session_(std::exchange(other.bound_session_, nullptr)),
      module_name_(std::move(other.module_name_)),
      source_path_(std::move(other.source_path_)),
      shadow_copy_path_(std::move(other.shadow_copy_path_)) {}

LoadedGameModule& LoadedGameModule::operator=(
    LoadedGameModule&& other) noexcept {
    if (this != &other) {
        unload(nullptr);
        library_handle_ = std::exchange(other.library_handle_, nullptr);
        api_ = std::exchange(other.api_, nullptr);
        instance_id_ = std::exchange(other.instance_id_, 0U);
        bound_session_ = std::exchange(other.bound_session_, nullptr);
        module_name_ = std::move(other.module_name_);
        source_path_ = std::move(other.source_path_);
        shadow_copy_path_ = std::move(other.shadow_copy_path_);
    }
    return *this;
}

bool LoadedGameModule::load(
    const std::filesystem::path& dll_path,
    const GameModuleHostContext& host,
    const bool shadow_copy_dll,
    std::string& error) {
    error.clear();
    unload(host.session);

#if !defined(_WIN32)
    static_cast<void>(dll_path);
    static_cast<void>(shadow_copy_dll);
    error = "Dynamic project game modules are currently supported only on Windows.";
    return false;
#else
    if (dll_path.empty()) {
        error = "Project game module path is empty.";
        return false;
    }
    if (host.session == nullptr || host.scene == nullptr ||
        host.asset_root == nullptr) {
        error = "Game module host context is incomplete.";
        return false;
    }

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(dll_path, ec);
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        error = "Project game module DLL does not exist or is a symbolic link: " +
                path_to_utf8_local(dll_path);
        return false;
    }

    std::filesystem::path resolved_source =
        std::filesystem::weakly_canonical(dll_path, ec);
    if (ec) {
        resolved_source = dll_path;
    }

    std::filesystem::path load_target = resolved_source;
    std::filesystem::path created_shadow{};
    if (shadow_copy_dll) {
        ec.clear();
        const std::filesystem::path temp_root =
            std::filesystem::temp_directory_path(ec) / "vcr_live_modules";
        if (ec) {
            error = "Unable to resolve temporary directory for module shadow copy: " +
                    ec.message();
            return false;
        }
        const auto root_status = std::filesystem::symlink_status(temp_root, ec);
        if (!ec && std::filesystem::is_symlink(root_status)) {
            error = "Refusing shadow copy through symbolic link directory: " +
                    path_to_utf8_local(temp_root);
            return false;
        }
        ec.clear();
        std::filesystem::create_directories(temp_root, ec);
        if (ec) {
            error = "Unable to create module shadow-copy directory: " +
                    ec.message();
            return false;
        }
        const auto post_status = std::filesystem::symlink_status(temp_root, ec);
        if (ec || !std::filesystem::is_directory(post_status) ||
            std::filesystem::is_symlink(post_status)) {
            error = "Shadow-copy directory is invalid or is a symbolic link: " +
                    path_to_utf8_local(temp_root);
            return false;
        }
        const std::uint64_t serial =
            g_shadow_copy_serial.fetch_add(1U, std::memory_order_relaxed) + 1U;
        const std::string stem = resolved_source.stem().empty()
            ? std::string("game_module")
            : path_to_utf8_local(resolved_source.stem());
        const std::string filename =
            stem + "_" + std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(serial) + ".dll";
        created_shadow = temp_root / filename;
        if (!std::filesystem::copy_file(
                resolved_source,
                created_shadow,
                std::filesystem::copy_options::overwrite_existing,
                ec) ||
            ec) {
            error = "Unable to create shadow copy of game module DLL: " +
                    ec.message();
            return false;
        }
        load_target = created_shadow;
    }

    const std::filesystem::path source_dir = resolved_source.parent_path();
    ScopedDllDirectoryCookie dll_dir_cookie(
        (shadow_copy_dll && !source_dir.empty()) ? source_dir.c_str() : nullptr);
    const DWORD flags = shadow_copy_dll
        ? LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
        : (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ScopedNativeLibrary lib(
        LoadLibraryExW(load_target.c_str(), nullptr, flags));
    dll_dir_cookie.reset();
    if (lib.get() == nullptr) {
        const DWORD last_err = GetLastError();
        if (!created_shadow.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove(created_shadow, rm_ec);
        }
        error = "Failed to load project game module '" +
                path_to_utf8_local(resolved_source) + "' (Win32 error " +
                std::to_string(last_err) + ").";
        return false;
    }

    const FARPROC raw_proc =
        GetProcAddress(lib.get(), vcr_game_module_entry_symbol);
    if (raw_proc == nullptr) {
        lib.reset();
        if (!created_shadow.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove(created_shadow, rm_ec);
        }
        error = "Project game module '" + path_to_utf8_local(resolved_source) +
                "' does not export '" + vcr_game_module_entry_symbol + "'.";
        return false;
    }

    const auto get_api = reinterpret_cast<VcrGetGameModuleApiFn>(raw_proc);
    const VcrGameModuleApi* const api = get_api();
    if (api == nullptr || api->api_version != vcr_game_module_api_version ||
        api->create_instance == nullptr || api->destroy_instance == nullptr ||
        api->configure_session == nullptr) {
        lib.reset();
        if (!created_shadow.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove(created_shadow, rm_ec);
        }
        error = "Project game module API version mismatch in '" +
                path_to_utf8_local(resolved_source) +
                "'. Rebuild the project's game module against the current engine SDK.";
        return false;
    }
    if (api->abi_stamp != vcr_current_module_abi_stamp()) {
        lib.reset();
        if (!created_shadow.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove(created_shadow, rm_ec);
        }
        error = "Project game module CRT/ABI mismatch in '" +
                path_to_utf8_local(resolved_source) +
                "' (Debug/Release CRT or STL layout mismatch with engine).";
        return false;
    }

    const GameModuleInstanceId id = api->create_instance();
    if (id == 0U) {
        lib.reset();
        if (!created_shadow.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove(created_shadow, rm_ec);
        }
        error = "Project game module failed to allocate a game instance.";
        return false;
    }

    std::array<char, 512> cfg_error_buf{};
    if (!api->configure_session(id, host, cfg_error_buf.data(), cfg_error_buf.size())) {
        cfg_error_buf.back() = '\0';
        error = cfg_error_buf[0] != '\0'
            ? std::string(cfg_error_buf.data())
            : std::string("Project game module configure_session returned false.");
        host.session->set_extension_callbacks({});
        api->destroy_instance(id);
        lib.reset();
        if (!created_shadow.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove(created_shadow, rm_ec);
        }
        return false;
    }

    library_handle_ = lib.release();
    api_ = api;
    instance_id_ = id;
    bound_session_ = host.session;
    module_name_ = api->module_name != nullptr ? api->module_name : "";
    source_path_ = std::move(resolved_source);
    shadow_copy_path_ = std::move(created_shadow);
    return true;
#endif
}

void LoadedGameModule::unload(GameplaySession* const session) noexcept {
    GameplaySession* const target_session =
        session != nullptr ? session : bound_session_;
    if (target_session != nullptr) {
        target_session->set_extension_callbacks({});
    }
    bound_session_ = nullptr;
    if (api_ != nullptr && instance_id_ != 0U && api_->destroy_instance != nullptr) {
        api_->destroy_instance(instance_id_);
    }
    instance_id_ = 0U;
    api_ = nullptr;
    if (library_handle_ != nullptr) {
        close_native_library(library_handle_);
        library_handle_ = nullptr;
    }
    if (!shadow_copy_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(shadow_copy_path_, ec);
        shadow_copy_path_.clear();
    }
    module_name_.clear();
    source_path_.clear();
}

} // namespace vcr
