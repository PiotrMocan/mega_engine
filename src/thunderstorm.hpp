#pragma once

#include "weather.hpp"
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <array>
#include <cstdint>
#include <optional>

namespace vcr {

struct ThunderEvent {
    glm::vec3 position{};
    float volume = 0.0F;
    float duration = 0.0F;
    std::uint32_t seed = 0;
};

// Owned by the weather update thread. Only due sound events cross into the mixer.
class Thunderstorm {
public:
    void set_override(std::optional<bool> enabled) noexcept {
        if (override_ != enabled) { stop(); override_ = enabled; }
    }
    void reset() noexcept {
        stop();
        override_.reset();
        sequence_ = 0;
        random_ = 0x8c41db27U;
    }
    [[nodiscard]] bool requested(const StormSettings& s) const noexcept { return override_.value_or(s.enabled); }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::uint32_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] glm::vec3 center() const noexcept { return center_; }
    [[nodiscard]] float light() const noexcept { return light_; }

    void stop() noexcept {
        cancel_audio_ |= running_;
        running_ = false;
        light_ = 0;
        count_ = 0;
        clock_ = 0;
    }
    bool take_cancel() noexcept { const bool result = cancel_audio_; cancel_audio_ = false; return result; }

    std::optional<ThunderEvent> take_sound() noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (queue_[i].arrival > clock_) continue;
            const auto sound = queue_[i].event;
            queue_[i] = queue_[--count_];
            return sound;
        }
        return std::nullopt;
    }

    void advance(double dt, const StormSettings& s, glm::vec3 camera, glm::vec3 forward,
                 float base, float thickness) noexcept {
        // Phase is normalized to the configured interval so changing frequency
        // midway through a long interval takes effect immediately.
        if (!running_) {
            running_ = true;
            phase_ = 1.0;
            interval_scale_ = 1.0;
        }
        clock_ += dt;
        phase_ += dt / (static_cast<double>(s.interval_seconds) * interval_scale_);
        if (phase_ >= 1.0) {
            const bool first = sequence_ == 0 || clock_ == dt;
            ++sequence_;
            phase_ = 0;
            interval_scale_ = 0.80F + 0.40F * uniform();
            const float azimuth = std::atan2(forward.z, forward.x) +
                (uniform() - 0.5F) * (first ? 0.25F : 3.5F);
            const float height = base + thickness * (0.43F + 0.27F * uniform());
            const float range = std::clamp(std::abs(height - camera.y) * (1.35F + uniform()), 400.0F, 10000.0F);
            center_ = {camera.x + range * std::cos(azimuth), height, camera.z + range * std::sin(azimuth)};
            flash_start_ = clock_;
            second_pulse_ = 0.14F + 0.13F * uniform();
            const float distance = glm::length(center_ - camera);
            // 12 km / 343 m/s < 35 s. At the minimum interval (1.6 s)
            // at most 22 audible discharges can be in transit.
            if (distance < 12000.0F && s.thunder_volume > 0 && count_ < queue_.size())
                queue_[count_++] = {clock_ + distance / 343.0, {center_, s.thunder_volume, s.thunder_duration, random_}};
        }
        const float age = static_cast<float>(clock_ - flash_start_);
        light_ = 0;
        if (age < 0.85F) {
            const float primary = std::exp(-8.0F * age);
            const float echo = std::max(0.0F, 1.0F - std::abs(age - second_pulse_) / 0.09F);
            light_ = s.flash_strength * (primary + 0.76F * echo * echo);
        }
    }

private:
    struct Pending { double arrival{}; ThunderEvent event{}; };
    float uniform() noexcept {
        random_ ^= random_ << 13; random_ ^= random_ >> 17; random_ ^= random_ << 5;
        return static_cast<float>(random_ >> 8) / 16777216.0F;
    }
    std::optional<bool> override_{};
    std::array<Pending, 24> queue_{};
    std::size_t count_ = 0;
    glm::vec3 center_{};
    double clock_ = 0, phase_ = 0, flash_start_ = 0;
    float interval_scale_ = 1, second_pulse_ = 0, light_ = 0;
    std::uint32_t sequence_ = 0, random_ = 0x8c41db27U;
    bool running_ = false, cancel_audio_ = false;
};
} // namespace vcr
