#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {
enum class ModelVertexFormat : std::uint8_t { automatic, float32, packed };
[[nodiscard]] constexpr std::string_view to_token(ModelVertexFormat format) noexcept {
    switch (format) {
    case ModelVertexFormat::automatic: return "auto";
    case ModelVertexFormat::float32: return "float";
    case ModelVertexFormat::packed: return "packed";
    }
    return "auto";
}
[[nodiscard]] constexpr std::optional<ModelVertexFormat> parse_model_vertex_format(std::string_view token) noexcept {
    if (token == "auto") return ModelVertexFormat::automatic;
    if (token == "float") return ModelVertexFormat::float32;
    if (token == "packed") return ModelVertexFormat::packed;
    return std::nullopt;
}
} // namespace vcr
