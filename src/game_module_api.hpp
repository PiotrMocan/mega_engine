#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vcr {

class AudioEngine;
class GameplaySession;
class InputActionSystem;
class SceneDocument;

inline constexpr std::uint32_t vcr_game_module_api_version = 2U;
inline constexpr const char* vcr_game_module_entry_symbol =
    "vcr_get_game_module_api";

[[nodiscard]] constexpr std::uint32_t vcr_current_module_abi_stamp() noexcept {
    std::uint32_t stamp = 0U;
#if defined(_DEBUG)
    stamp |= 1U << 0U;
#endif
#if defined(_DLL)
    stamp |= 1U << 1U;
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
    stamp |= (static_cast<std::uint32_t>(_ITERATOR_DEBUG_LEVEL) & 0xFU) << 4U;
#endif
    stamp |= (static_cast<std::uint32_t>(sizeof(std::string)) & 0xFFU) << 8U;
    stamp |= (static_cast<std::uint32_t>(sizeof(std::filesystem::path)) & 0xFFU) << 16U;
    return stamp;
}

using GameModuleInstanceId = std::uint64_t;

struct GameModuleHostContext {
    const std::filesystem::path* project_root = nullptr;
    const std::filesystem::path* asset_root = nullptr;
    SceneDocument* scene = nullptr;
    GameplaySession* session = nullptr;
    AudioEngine* audio_engine = nullptr;
    InputActionSystem* input_actions = nullptr;
};

struct VcrGameModuleApi {
    std::uint32_t api_version = vcr_game_module_api_version;
    std::uint32_t abi_stamp = vcr_current_module_abi_stamp();
    const char* module_name = "";
    GameModuleInstanceId (*create_instance)() noexcept = nullptr;
    void (*destroy_instance)(GameModuleInstanceId instance) noexcept = nullptr;
    bool (*configure_session)(
        GameModuleInstanceId instance,
        const GameModuleHostContext& host,
        char* error_buf,
        std::size_t error_buf_cap) noexcept = nullptr;
};

using VcrGetGameModuleApiFn = const VcrGameModuleApi* (*)() noexcept;

template <typename GameClass>
class GameModuleRegistry final {
public:
    [[nodiscard]] GameModuleInstanceId create() noexcept {
        try {
            auto instance = std::make_unique<GameClass>();
            for (std::size_t i = 0U; i < slots_.size(); ++i) {
                if (!slots_[i]) {
                    slots_[i] = std::move(instance);
                    return static_cast<GameModuleInstanceId>(i + 1U);
                }
            }
            slots_.push_back(std::move(instance));
            return static_cast<GameModuleInstanceId>(slots_.size());
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] GameClass* find(
        const GameModuleInstanceId instance_id) noexcept {
        if (instance_id == 0U) {
            return nullptr;
        }
        const std::size_t index = static_cast<std::size_t>(instance_id - 1U);
        if (index >= slots_.size()) {
            return nullptr;
        }
        return slots_[index].get();
    }

    void destroy(const GameModuleInstanceId instance_id) noexcept {
        if (instance_id == 0U) {
            return;
        }
        const std::size_t index = static_cast<std::size_t>(instance_id - 1U);
        if (index < slots_.size()) {
            slots_[index].reset();
        }
    }

private:
    std::vector<std::unique_ptr<GameClass>> slots_{};
};

} // namespace vcr

#if defined(_WIN32)
#define VCR_GAME_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define VCR_GAME_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define VCR_DEFINE_GAME_MODULE(GameClass, ModuleNameLiteral)                  \
    namespace {                                                               \
    inline ::vcr::GameModuleRegistry<GameClass>& vcr_game_module_registry() { \
        static ::vcr::GameModuleRegistry<GameClass> registry;                 \
        return registry;                                                      \
    }                                                                         \
    } /* namespace */                                                         \
    VCR_GAME_MODULE_EXPORT const ::vcr::VcrGameModuleApi*                    \
    vcr_get_game_module_api() noexcept {                                      \
        static const ::vcr::VcrGameModuleApi api{                             \
            .api_version = ::vcr::vcr_game_module_api_version,                \
            .abi_stamp = ::vcr::vcr_current_module_abi_stamp(),               \
            .module_name = (ModuleNameLiteral),                               \
            .create_instance = +[]() noexcept -> ::vcr::GameModuleInstanceId { \
                return vcr_game_module_registry().create();                   \
            },                                                                \
            .destroy_instance = +[](const ::vcr::GameModuleInstanceId id)     \
                                    noexcept {                                \
                vcr_game_module_registry().destroy(id);                       \
            },                                                                \
            .configure_session = +[](const ::vcr::GameModuleInstanceId id,    \
                                     const ::vcr::GameModuleHostContext& host, \
                                     char* const error_buf,                   \
                                     const ::std::size_t error_buf_cap)       \
                                     noexcept -> bool {                       \
                const auto write_err =                                        \
                    [error_buf, error_buf_cap](const char* const msg) noexcept { \
                        if (error_buf == nullptr || error_buf_cap == 0U) {    \
                            return;                                           \
                        }                                                     \
                        const ::std::size_t len =                             \
                            msg != nullptr ? ::std::strlen(msg) : 0U;         \
                        const ::std::size_t copy_len =                        \
                            ::std::min(len, error_buf_cap - 1U);              \
                        if (copy_len > 0U) {                                  \
                            ::std::memcpy(error_buf, msg, copy_len);          \
                        }                                                     \
                        error_buf[copy_len] = '\0';                           \
                    };                                                        \
                write_err("");                                                \
                GameClass* const instance =                                   \
                    vcr_game_module_registry().find(id);                      \
                if (instance == nullptr) {                                    \
                    write_err("Invalid project game module instance.");       \
                    return false;                                             \
                }                                                             \
                try {                                                         \
                    ::std::string local_err;                                  \
                    if (!instance->configure_module(host, local_err)) {       \
                        write_err(local_err.c_str());                         \
                        return false;                                         \
                    }                                                         \
                    return true;                                              \
                } catch (const ::std::exception& ex) {                        \
                    write_err(ex.what());                                     \
                    return false;                                             \
                } catch (...) {                                               \
                    write_err(                                                \
                        "Unknown exception while configuring game module.");  \
                    return false;                                             \
                }                                                             \
            },                                                                \
        };                                                                    \
        return &api;                                                          \
    }
