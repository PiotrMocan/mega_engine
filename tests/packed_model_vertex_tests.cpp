#include "packed_model_vertex.hpp"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void expect(bool result, const char *message) {
    if (!result) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
}
float decode(std::uint32_t bits, unsigned shift) {
    const std::uint32_t channel = (bits >> shift) & 1023U;
    const auto signed_value = channel >= 512U ? static_cast<int>(channel) - 1024 : static_cast<int>(channel);
    return std::max(-1.0F, static_cast<float>(signed_value) / 511.0F);
}
}

int main() {
    expect(vcr::parse_model_vertex_format("auto") == vcr::ModelVertexFormat::automatic &&
           vcr::parse_model_vertex_format("float") == vcr::ModelVertexFormat::float32 &&
           vcr::parse_model_vertex_format("packed") == vcr::ModelVertexFormat::packed &&
           !vcr::parse_model_vertex_format("half"), "vertex format tokens are exact");
    vcr::CookedModelVertex source{};
    source.position = {123456.789F, -0.0F, 1.0e-10F};
    source.texcoord = {-10.125F, 2.0e6F};
    source.normal = {0, 0, 1};
    source.tangent = {1, 0, 0, -1};
    const auto packed = vcr::pack_model_vertex(source);
    expect(std::bit_cast<std::array<std::uint32_t, 3>>(source.position) ==
           std::bit_cast<std::array<std::uint32_t, 3>>(packed.position) &&
           std::bit_cast<std::array<std::uint32_t, 2>>(source.texcoord) ==
           std::bit_cast<std::array<std::uint32_t, 2>>(packed.texcoord),
           "positions and UVs retain every binary32 bit, including signed zero");
    expect(decode(packed.normal, 20) == 1 && decode(packed.normal, 0) == 0 &&
           decode(packed.tangent, 0) == 1 && packed.tangent >> 30U == 3,
           "SNORM axes and negative tangent handedness decode exactly");
    expect((vcr::pack_model_direction(0, 0, 0, 1) & 0x3fffffffU) == 0 &&
           vcr::pack_model_direction(0, 0, 0, 1) >> 30U == 1,
           "zero direction and positive handedness are defined");
    std::uint32_t random = 42;
    for (unsigned i = 0; i < 4096; ++i) {
        std::array<double, 3> direction{};
        for (auto &value : direction) {
            random = random * 1664525U + 1013904223U;
            value = double(random) / double(UINT32_MAX) * 2.0 - 1.0;
        }
        const double length = std::hypot(direction[0], direction[1], direction[2]);
        for (auto &value : direction) value /= length;
        const auto bits = vcr::pack_model_direction(float(direction[0]), float(direction[1]), float(direction[2]), 1);
        const std::array<double, 3> decoded{decode(bits, 0), decode(bits, 10), decode(bits, 20)};
        const double decoded_length = std::hypot(decoded[0], decoded[1], decoded[2]);
        const double cosine = (direction[0] * decoded[0] + direction[1] * decoded[1] +
                               direction[2] * decoded[2]) / decoded_length;
        expect(cosine > std::cos(0.11 * 3.141592653589793 / 180.0),
               "direction quantization stays below 0.11 degrees across all octants");
    }
    const auto huge = vcr::pack_model_direction(std::numeric_limits<float>::max(), 0, 0, 1);
    expect(decode(huge, 0) == 1, "finite large directions cannot overflow normalization");
    std::puts("Packed model vertex precision and encoding tests passed.");
}
