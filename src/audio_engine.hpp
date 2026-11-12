#pragma once

#include "cooked_audio_asset.hpp"
#include "scene_components.hpp"
#include "sound_cue_io.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace vcr {

struct AudioVoiceHandle {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return slot != UINT32_MAX && generation != 0U;
    }

    friend constexpr bool operator==(
        const AudioVoiceHandle&,
        const AudioVoiceHandle&) = default;
};

struct AudioListenerState {
    glm::vec3 position{0.0F, 0.0F, 0.0F};
    glm::vec3 forward{0.0F, 0.0F, -1.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    glm::vec3 velocity{0.0F, 0.0F, 0.0F};
    float volume_scale = 1.0F;
    float doppler_scale = 1.0F;
};

struct AudioSpatialParams {
    glm::vec3 position{0.0F, 0.0F, 0.0F};
    glm::vec3 forward{0.0F, 0.0F, -1.0F};
    glm::vec3 velocity{0.0F, 0.0F, 0.0F};
    float min_distance = 1.5F;
    float max_distance = 35.0F;
    float rolloff = 1.0F;
    float doppler_factor = 1.0F;
    float inner_cone_degrees = 360.0F;
    float outer_cone_degrees = 360.0F;
    float outer_cone_gain = 0.25F;
    float occlusion = 0.0F; // 0 = unobstructed, 1 = occluded behind geometry
    float reverb_send = 1.0F;
    AudioAttenuationModel attenuation =
        AudioAttenuationModel::inverse_clamped;
};

struct AudioReverbState {
    float wet_level = 0.0F;
    float decay_seconds = 1.8F;
    float damping = 0.45F;
    bool enabled = false;
};

struct AudioPlayParams {
    AudioBusId bus = AudioBusId::sfx;
    float volume = 1.0F;
    float pitch = 1.0F;
    float fade_in_seconds = 0.0F;
    bool loop = false;
    bool spatial = false;
    bool streaming = false;
    bool is_preview = false;
    AudioSpatialParams spatial_params{};
};

struct AudioBusSnapshot {
    float volume = 1.0F;
    bool muted = false;
    float peak_left = 0.0F;
    float peak_right = 0.0F;
    std::uint32_t active_voices = 0U;
};

struct AudioEngineTelemetry {
    std::array<AudioBusSnapshot, audio_bus_count> buses{};
    float master_limiter_gain = 1.0F;
    std::uint32_t active_voice_count = 0U;
    std::uint32_t active_stream_count = 0U;
    std::size_t resident_clip_count = 0U;
    std::size_t resident_pcm_bytes = 0U;
    std::size_t resident_pcm_high_water_bytes = 0U;
    bool hardware_output_active = false;
    bool gameplay_paused = false;
};

struct AudioEngineOptions {
    std::filesystem::path asset_root{};
    std::uint32_t sample_rate = default_audio_sample_rate;
    std::size_t maximum_resident_pcm_bytes = 64ULL * 1024ULL * 1024ULL;
    bool enable_hardware_output = true;
};

// Pure math helpers exposed for deterministic unit verification.
[[nodiscard]] float compute_distance_attenuation(
    AudioAttenuationModel model,
    float distance,
    float min_distance,
    float max_distance,
    float rolloff) noexcept;

[[nodiscard]] float compute_cone_attenuation(
    const glm::vec3& source_position,
    const glm::vec3& source_forward,
    const glm::vec3& listener_position,
    float inner_cone_degrees,
    float outer_cone_degrees,
    float outer_cone_gain) noexcept;

[[nodiscard]] float compute_doppler_pitch(
    const glm::vec3& source_position,
    const glm::vec3& source_velocity,
    const glm::vec3& listener_position,
    const glm::vec3& listener_velocity,
    float doppler_scale) noexcept;

[[nodiscard]] AudioReverbState evaluate_reverb_zones_at_listener(
    const glm::vec3& listener_position,
    std::span<const std::pair<glm::vec3, AudioReverbZoneComponent>> zones) noexcept;

struct AudioEngineState;

class AudioEngine final {
public:
    static constexpr std::size_t max_voices = 64U;
    static constexpr std::size_t max_resident_clips = 256U;
    static constexpr std::size_t max_stream_slots = 8U;

    AudioEngine() noexcept;
    ~AudioEngine() noexcept;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) noexcept;
    AudioEngine& operator=(AudioEngine&&) noexcept;

    [[nodiscard]] bool initialize(
        const AudioEngineOptions& options,
        std::string& error);
    void shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool hardware_output_active() const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] const std::filesystem::path& asset_root() const noexcept;

    void set_bus_volume(AudioBusId bus, float volume) noexcept;
    [[nodiscard]] float bus_volume(AudioBusId bus) const noexcept;

    void set_bus_muted(AudioBusId bus, bool muted) noexcept;
    [[nodiscard]] bool bus_muted(AudioBusId bus) const noexcept;

    // Synchronizes gameplay audio (Music, SFX, Voice) with Play / Pause / Stop
    // while keeping UI and Asset Browser preview playback responsive.
    void set_gameplay_paused(bool paused) noexcept;
    [[nodiscard]] bool gameplay_paused() const noexcept;
    void stop_gameplay_audio(float fade_out_seconds = 0.03F) noexcept;
    void stop_all_audio() noexcept;

    // Plays a cooked .vcraudio.json, raw .wav/.ogg/.flac, or .vcrcue.json inside
    // asset_root.
    [[nodiscard]] AudioVoiceHandle play_asset(
        std::string_view relative_asset_path,
        const AudioPlayParams& params,
        std::string& error);

    // Plays an in-memory clip directly (used by procedural preview or tests).
    [[nodiscard]] AudioVoiceHandle play_pcm_clip(
        std::string_view debug_name,
        std::span<const float> samples,
        std::uint32_t channels,
        const AudioPlayParams& params,
        std::string& error);

    void stop_voice(
        AudioVoiceHandle handle,
        float fade_out_seconds = 0.01F) noexcept;
    [[nodiscard]] bool is_voice_active(AudioVoiceHandle handle) const noexcept;

    void update_voice(
        AudioVoiceHandle handle,
        float volume,
        float pitch,
        const AudioSpatialParams& spatial_params) noexcept;

    // Smooth dual-deck Music bus crossfader.
    [[nodiscard]] bool crossfade_music(
        std::string_view relative_asset_path,
        float fade_seconds,
        bool loop,
        std::string& error);
    void stop_music(float fade_seconds = 0.25F) noexcept;
    [[nodiscard]] std::string_view current_music_path() const noexcept;

    // Asset Browser one-click preview on the UI bus.
    [[nodiscard]] bool play_preview(
        std::string_view relative_asset_path,
        std::string& error);
    void stop_preview() noexcept;
    [[nodiscard]] bool is_preview_playing() const noexcept;
    [[nodiscard]] std::string_view preview_asset_path() const noexcept;

    void set_listener(const AudioListenerState& listener) noexcept;
    void set_reverb_state(const AudioReverbState& reverb) noexcept;

    // Periodic main-thread maintenance (advances cue cooldown clock, reclaims
    // finished streaming slots). Zero allocations in steady state.
    void update(float dt_seconds) noexcept;

    [[nodiscard]] AudioEngineTelemetry telemetry() const noexcept;

    // Deterministic offline rendering & stream pump for headless unit tests
    // (only valid when enable_hardware_output == false).
    void pump_streams_sync_for_test() noexcept;
    void render_frames_for_test(std::span<float> interleaved_stereo) noexcept;

private:
    std::unique_ptr<AudioEngineState> state_;
};

} // namespace vcr
