#include "fog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace shader {
using std::exp;
using std::max;
using std::min;
#include "../shaders/fog_density.glsl"
} // namespace shader

namespace {
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// Independent midpoint integration of local density along the ray.
double reference_mean(const double start, const double end,
                      const double base, const double falloff) {
    constexpr int steps = 16384;
    double sum = 0.0;
    for (int step = 0; step < steps; ++step) {
        const double t = (static_cast<double>(step) + 0.5) / steps;
        const double height = start + (end - start) * t;
        sum += std::exp(-std::max(height - base, 0.0) * falloff);
    }
    return sum / steps;
}
} // namespace

int main() {
    for (const float start : {-100.0F, -1.0F, 0.0F, 0.001F, 1.0F, 100.0F}) {
        for (const float end : {-100.0F, -1.0F, 0.0F, 0.001F, 1.0F, 100.0F}) {
            for (const float base : {-2.0F, 0.0F, 2.0F}) {
                for (const float falloff : {0.0F, 0.000001F, 0.001F, 0.05F, 10.0F}) {
                    const float actual = shader::fogMeanDensity(start, end, base, falloff);
                    const double expected = reference_mean(start, end, base, falloff);
                    expect(std::isfinite(actual) && actual >= 0.0F && actual <= 1.0F,
                           "horizontal, ascending and descending rays have bounded density");
                    expect(std::abs(actual - expected) < 0.00006,
                           "shader height integral agrees with numerical quadrature");
                }
            }
        }
    }
    expect(shader::fogMeanDensity(-100000.0F, -100000.0F, 0.0F, 10.0F) == 1.0F,
           "density below the base stays bounded without exponential overflow");
    expect(shader::fogMeanDensity(100000.0F, 100000.0F, 0.0F, 10.0F) == 0.0F,
           "very high altitude safely converges to clear air");
    expect(!vcr::FogSettings{}.enabled && vcr::valid_fog_settings({}),
           "fog defaults are valid and opt-in");
    constexpr std::array members{
        &vcr::FogSettings::density, &vcr::FogSettings::height_falloff,
        &vcr::FogSettings::base_height, &vcr::FogSettings::start_distance,
        &vcr::FogSettings::maximum_distance, &vcr::FogSettings::maximum_opacity,
    };
    for (const auto member : members) {
        for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity()}) {
            vcr::FogSettings settings;
            settings.*member = invalid;
            expect(!vcr::valid_fog_settings(settings), "non-finite fog parameters are rejected");
        }
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
        for (const float invalid : {-0.1F, 16.1F, std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity()}) {
            vcr::FogSettings settings;
            settings.color[channel] = invalid;
            expect(!vcr::valid_fog_settings(settings), "invalid HDR fog color is rejected");
        }
    }
    vcr::FogSettings settings;
    settings.start_distance = settings.maximum_distance;
    expect(!vcr::valid_fog_settings(settings), "empty fog interval is rejected");
    settings.start_distance = 0.0F;
    settings.base_height = -1000.0F;
    settings.density = 0.0F;
    settings.height_falloff = 0.0F;
    settings.maximum_opacity = 0.0F;
    expect(vcr::valid_fog_settings(settings), "zero-density and negative-height settings are valid");
    if (failures == 0) std::cout << "Fog tests passed\n";
    return failures == 0 ? 0 : 1;
}
