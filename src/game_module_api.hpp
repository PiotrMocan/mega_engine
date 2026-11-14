#pragma once

#include <cstddef>
#include <cstdint>
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

inline constexpr std::uint32_t vcr_game_module_api_version = 1U;
inline constexpr const char* vcr_game_module_entry_symbol =
    "vcr_get_game_module_api";

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
    const char* module_name = "";
    GameModuleInstanceId (*create_instance)() noexcept = nullptr;
    void (*destroy_instance)(GameModuleInstanceId instance) noexcept = nullptr;
    bool (*configure_session)(
        GameModuleInstanceId instance,
        const GameModuleHostContext& host,
        std::string& out_error) = nullptr;
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
                                     std::string& out_error) -> bool {        \
                GameClass* const instance =                                   \
                    vcr_game_module_registry().find(id);                      \
                if (instance == nullptr) {                                    \
                    out_error = "Invalid project game module instance.";      \
                    return false;                                             \
                }                                                             \
                try {                                                         \
                    return instance->configure_module(host, out_error);       \
                } catch (const std::exception& ex) {                          \
                    out_error = ex.what();                                    \
                    return false;                                             \
                } catch (...) {                                               \
                    out_error = "Unknown exception while configuring game module."; \
                    return false;                                             \
                }                                                             \
            },                                                                \
        };                                                                    \
        return &api;                                                          \
    }
