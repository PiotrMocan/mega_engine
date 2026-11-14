#pragma once

#include "game_module_api.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace vcr {

class GameplaySession;

class LoadedGameModule final {
public:
    LoadedGameModule() noexcept = default;
    ~LoadedGameModule() noexcept;

    LoadedGameModule(const LoadedGameModule&) = delete;
    LoadedGameModule& operator=(const LoadedGameModule&) = delete;
    LoadedGameModule(LoadedGameModule&& other) noexcept;
    LoadedGameModule& operator=(LoadedGameModule&& other) noexcept;

    [[nodiscard]] bool load(
        const std::filesystem::path& dll_path,
        const GameModuleHostContext& host,
        bool shadow_copy_dll,
        std::string& error);

    void unload(GameplaySession* session = nullptr) noexcept;

    [[nodiscard]] bool is_loaded() const noexcept {
        return library_handle_ != nullptr && instance_id_ != 0U;
    }

    [[nodiscard]] std::string_view module_name() const noexcept {
        return module_name_;
    }

    [[nodiscard]] const std::filesystem::path& source_path() const noexcept {
        return source_path_;
    }

private:
    void* library_handle_ = nullptr;
    const VcrGameModuleApi* api_ = nullptr;
    GameModuleInstanceId instance_id_ = 0U;
    GameplaySession* bound_session_ = nullptr; // non-owning observer
    std::string module_name_{};
    std::filesystem::path source_path_{};
    std::filesystem::path shadow_copy_path_{};
};

} // namespace vcr
