#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vcr {

// World Y is altitude. Wind headings turn from +X toward +Z.
struct CloudSettings {
    bool enabled = false;
    float coverage = 0.55F;
    float density = 1.0F;
    float base_height = 600.0F;
    float thickness = 350.0F;
    float wind_direction = 20.0F;
    float wind_speed = 12.0F;
    float shadow_strength = 1.0F;
    float variation_chance = 0.0F;
    float variation_amount = 0.45F;
    friend bool operator==(const CloudSettings&, const CloudSettings&) = default;
};

struct StormSettings {
    float rain_buildup = 0.0F;
    bool enabled = false;
    float interval_seconds = 12.0F;
    float flash_strength = 0.70F;
    float thunder_volume = 0.80F;
    float thunder_duration = 4.0F;
    friend bool operator==(const StormSettings&, const StormSettings&) = default;
};

struct RainSettings {
    bool enabled = false;
    float intensity = 0.65F;
    float fall_speed = 9.0F;
    float splashes = 1.0F;
    float volume = 0.8F;
    friend bool operator==(const RainSettings&, const RainSettings&) = default;
};

struct WeatherSettings {
    CloudSettings clouds{};
    float transition_seconds = 0.0F;
    StormSettings storm{};
    RainSettings rain{};
    friend bool operator==(const WeatherSettings&, const WeatherSettings&) = default;
};

inline bool valid_weather(const WeatherSettings& weather) noexcept {
    const auto& c = weather.clouds;
    const auto within = [](float x, float lo, float hi) { return x >= lo && x <= hi; };
    const auto& s = weather.storm;
    const auto& r = weather.rain;
    return within(r.intensity, 0, 1) && within(r.fall_speed, 1, 20) &&
        within(r.splashes, 0, 1) && within(r.volume, 0, 1) &&
        within(s.rain_buildup, 0, 1) && within(s.interval_seconds, 2, 120) &&
        within(s.flash_strength, 0, 1) && within(s.thunder_volume, 0, 1) &&
        within(s.thunder_duration, 0.5F, 15) && within(c.coverage, 0, 1) && within(c.density, 0, 4) &&
        within(c.base_height, -10000, 20000) && within(c.thickness, 10, 10000) &&
        within(c.wind_direction, 0, 360) && within(c.wind_speed, 0, 200) &&
        within(c.shadow_strength, 0, 1) && within(c.variation_chance, 0, 1) &&
        within(c.variation_amount, 0, 1) && within(weather.transition_seconds, 0, 86400);
}

class WeatherSystem {
public:
    [[nodiscard]] const CloudSettings& clouds() const noexcept { return current_; }
    [[nodiscard]] const StormSettings& storm() const noexcept { return storm_; }
    [[nodiscard]] const RainSettings& rain() const noexcept { return rain_; }
    [[nodiscard]] float rainfall() const noexcept { return rain_.enabled ? rain_.intensity : 0; }
    [[nodiscard]] float wetness() const noexcept { return wetness_; }
    [[nodiscard]] double rain_x() const noexcept { return rain_x_; }
    [[nodiscard]] double rain_z() const noexcept { return rain_z_; }
    [[nodiscard]] double rain_fall() const noexcept { return rain_fall_; }
    [[nodiscard]] double rain_clock() const noexcept { return rain_clock_; }
    [[nodiscard]] double offset_x() const noexcept { return offset_x_; }
    [[nodiscard]] double offset_z() const noexcept { return offset_z_; }

    void reset(const WeatherSettings& settings) noexcept {
        if (!valid_weather(settings)) return;
        target_ = settings;
        current_ = start_ = settings.clouds;
        storm_ = start_storm_ = settings.storm;
        rain_ = start_rain_ = settings.rain;
        wetness_ = rainfall();
        rain_x_ = rain_z_ = rain_fall_ = rain_clock_ = 0;
        remaining_ = offset_x_ = offset_z_ = 0;
    }

    void set(const WeatherSettings& settings) noexcept {
        if (settings == target_ || !valid_weather(settings)) return;
        start_ = current_;
        start_storm_ = storm_;
        start_rain_ = rain_;
        storm_.enabled = settings.storm.enabled;
        target_ = settings;
        remaining_ = settings.transition_seconds;
        if (remaining_ == 0) { current_ = settings.clouds; storm_ = settings.storm; rain_ = settings.rain; }
    }

    void advance(double dt) noexcept {
        if (!(dt > 0) || !std::isfinite(dt)) return;
        if (remaining_ > 0) {
            remaining_ = std::max(0.0, remaining_ - dt);
            const auto& end = target_.clouds;
            const float t = 1.0F - static_cast<float>(remaining_ / target_.transition_seconds);
            const float u = t * t * (3.0F - 2.0F * t);
            current_ = end;
            const auto& s = target_.storm;
            storm_ = s;
            rain_ = target_.rain;
            if (remaining_ > 0) {
                rain_.enabled = start_rain_.enabled || target_.rain.enabled;
                rain_.intensity = std::lerp(start_rain_.enabled ? start_rain_.intensity : 0.0F,
                    target_.rain.enabled ? target_.rain.intensity : 0.0F, u);
                rain_.volume = std::lerp(start_rain_.volume, target_.rain.volume, u);
                rain_.fall_speed = std::lerp(start_rain_.fall_speed, target_.rain.fall_speed, u);
                rain_.splashes = std::lerp(start_rain_.splashes, target_.rain.splashes, u);
                storm_.rain_buildup = std::lerp(start_storm_.rain_buildup, s.rain_buildup, u);
                storm_.flash_strength = std::lerp(start_storm_.flash_strength, s.flash_strength, u);
                storm_.interval_seconds = std::lerp(start_storm_.interval_seconds, s.interval_seconds, u);
                storm_.thunder_volume = std::lerp(start_storm_.thunder_volume, s.thunder_volume, u);
                storm_.thunder_duration = std::lerp(start_storm_.thunder_duration, s.thunder_duration, u);
                current_.enabled = start_.enabled || end.enabled;
                current_.coverage = std::lerp(start_.enabled ? start_.coverage : 0.0F,
                                              end.enabled ? end.coverage : 0.0F, u);
                current_.density = std::lerp(start_.density, end.density, u);
                current_.base_height = std::lerp(start_.base_height, end.base_height, u);
                current_.thickness = std::lerp(start_.thickness, end.thickness, u);
                current_.wind_direction = std::fmod(start_.wind_direction + 360.0F +
                    std::remainder(end.wind_direction - start_.wind_direction, 360.0F) * u, 360.0F);
                current_.wind_speed = std::lerp(start_.wind_speed, end.wind_speed, u);
                current_.shadow_strength = std::lerp(start_.shadow_strength, end.shadow_strength, u);
                current_.variation_chance = std::lerp(start_.variation_chance, end.variation_chance, u);
                current_.variation_amount = std::lerp(start_.variation_amount, end.variation_amount, u);
            }
        }
        const double angle = current_.wind_direction * (std::numbers::pi / 180.0);
        const float strength = rainfall();
        if (strength > 0 || wetness_ > 0) {
            const double tau = strength > wetness_ ? 5.0 : 28.0;
            wetness_ = std::lerp(wetness_, strength, static_cast<float>(-std::expm1(-dt / tau)));
            if (strength == 0 && wetness_ < 0.001F) wetness_ = 0;
        }
        if (strength > 0) {
            const double drift = std::min(16.0, current_.wind_speed * 0.32) * dt;
            rain_x_ = std::remainder(rain_x_ + std::cos(angle) * drift, 48.0);
            rain_z_ = std::remainder(rain_z_ + std::sin(angle) * drift, 48.0);
            rain_fall_ = std::remainder(rain_fall_ - rain_.fall_speed * dt, 24.0);
            rain_clock_ = std::fmod(rain_clock_ + dt, 4096.0);
        }
        if (!current_.enabled || current_.wind_speed == 0) return;
        const double distance = current_.wind_speed * dt;
        // 1024 weather cells of 1024 m. Both the regional warp and the cell hash
        // repeat at this displacement, so wind can wrap without changing the sky.
        constexpr double weather_period = 1048576.0;
        offset_x_ = std::remainder(offset_x_ + std::cos(angle) * distance, weather_period);
        offset_z_ = std::remainder(offset_z_ + std::sin(angle) * distance, weather_period);
    }

private:
    WeatherSettings target_{};
    StormSettings storm_{}, start_storm_{};
    RainSettings rain_{}, start_rain_{};
    float wetness_ = 0;
    double rain_x_ = 0, rain_z_ = 0, rain_fall_ = 0, rain_clock_ = 0;
    CloudSettings start_{}, current_{};
    double remaining_ = 0;
    double offset_x_ = 0, offset_z_ = 0;
};
} // namespace vcr
