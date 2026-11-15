#pragma once

#include "character_package.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

struct OllamaBridgeConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port = 11434U;
    std::string model{};
    bool enable_network = true;
    std::uint32_t timeout_ms = 20000U;
};

struct OllamaStatus {
    bool reachable = false;
    std::string active_model{};
    std::vector<std::string> available_models{};
    std::string diagnostic{};
};

struct CharacterAiGenerationResult {
    bool used_ollama = false;
    bool used_fallback = false;
    std::string model_used{};
    std::string summary{};
    std::string diagnostic{};
};

[[nodiscard]] OllamaStatus probe_ollama_status(
    const OllamaBridgeConfig& config = {});

[[nodiscard]] bool ai_generate_body_spec(
    std::string_view prompt,
    std::uint64_t seed,
    const OllamaBridgeConfig& config,
    CharacterPackage& in_out_package,
    CharacterAiGenerationResult& out_result);

[[nodiscard]] bool ai_generate_outfit_spec(
    std::string_view prompt,
    std::uint64_t seed,
    const OllamaBridgeConfig& config,
    CharacterPackage& in_out_package,
    CharacterAiGenerationResult& out_result);

[[nodiscard]] bool ai_generate_acting_spec(
    std::string_view prompt,
    std::uint64_t seed,
    const OllamaBridgeConfig& config,
    CharacterPackage& in_out_package,
    CharacterAiGenerationResult& out_result);

} // namespace vcr
