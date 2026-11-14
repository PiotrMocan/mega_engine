#include "audio_engine.hpp"

#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#include <miniaudio.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <numbers>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vcr {
namespace {

constexpr float speed_of_sound_mps = 343.0F;
constexpr float limiter_ceiling = 0.985F;
constexpr std::size_t command_queue_capacity = 512U;
constexpr std::size_t stream_ring_frames = 131'072U; // Power-of-two (~2.73s @ 48 kHz)
constexpr std::size_t stream_ring_mask = stream_ring_frames - 1U;
constexpr std::size_t stream_prefill_frames = 4'096U;
constexpr std::size_t stream_refill_chunk_frames = 4'096U;
constexpr std::size_t mixer_scratch_frames = 512U;

static_assert((command_queue_capacity & (command_queue_capacity - 1U)) == 0U);
static_assert((stream_ring_frames & (stream_ring_frames - 1U)) == 0U);

[[nodiscard]] float sanitize_finite(
    const float value,
    const float fallback = 0.0F) noexcept {
    return std::isfinite(value) ? value : fallback;
}

[[nodiscard]] glm::vec3 safe_normalize(
    const glm::vec3& v,
    const glm::vec3& fallback) noexcept {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        return fallback;
    }
    const float len_sq = glm::dot(v, v);
    if (len_sq <= 1.0e-8F) {
        return fallback;
    }
    return v * (1.0F / std::sqrt(len_sq));
}

[[nodiscard]] bool is_gameplay_bus(const AudioBusId bus) noexcept {
    return bus == AudioBusId::music ||
           bus == AudioBusId::sfx ||
           bus == AudioBusId::voice;
}

#ifdef _MSC_VER
#pragma warning(push)
// Intentional cache-line isolation (`alignas(64)`) to prevent false sharing
// between producer and real-time audio mixer threads (AGENTS.md §5.1).
#pragma warning(disable : 4324)
#endif

template <typename T, std::size_t Capacity>
class SpscRingQueue final {
public:
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t write = write_index_.load(std::memory_order_relaxed);
        const std::size_t read = read_index_.load(std::memory_order_acquire);
        if (write - read >= Capacity) {
            return false;
        }
        storage_[write & (Capacity - 1U)] = item;
        write_index_.store(write + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out_item) noexcept {
        const std::size_t read = read_index_.load(std::memory_order_relaxed);
        const std::size_t write = write_index_.load(std::memory_order_acquire);
        if (read == write) {
            return false;
        }
        out_item = storage_[read & (Capacity - 1U)];
        read_index_.store(read + 1U, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> storage_{};
    alignas(64) std::atomic<std::size_t> write_index_{0U};
    alignas(64) std::atomic<std::size_t> read_index_{0U};
};

struct StereoStreamRing final {
    std::vector<float> samples{};
    alignas(64) std::atomic<std::uint64_t> write_frames{0U};
    alignas(64) std::atomic<std::uint64_t> read_frames{0U};
    std::atomic<bool> reached_eof{false};

    void allocate_once() {
        if (samples.empty()) {
            samples.resize(stream_ring_frames * 2U, 0.0F);
        }
    }

    void reset() noexcept {
        write_frames.store(0U, std::memory_order_relaxed);
        read_frames.store(0U, std::memory_order_relaxed);
        reached_eof.store(false, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t available_to_read() const noexcept {
        const std::uint64_t w = write_frames.load(std::memory_order_acquire);
        const std::uint64_t r = read_frames.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(w - r);
    }

    [[nodiscard]] std::size_t available_to_write() const noexcept {
        const std::uint64_t w = write_frames.load(std::memory_order_relaxed);
        const std::uint64_t r = read_frames.load(std::memory_order_acquire);
        const std::uint64_t used = w - r;
        return used >= stream_ring_frames
            ? 0U
            : static_cast<std::size_t>(stream_ring_frames - used);
    }

    void push_frames(const float* const interleaved, const std::size_t frames) noexcept {
        std::uint64_t w = write_frames.load(std::memory_order_relaxed);
        for (std::size_t i = 0U; i < frames; ++i) {
            const std::size_t idx = static_cast<std::size_t>((w + i) & stream_ring_mask) * 2U;
            samples[idx] = interleaved[i * 2U];
            samples[idx + 1U] = interleaved[i * 2U + 1U];
        }
        write_frames.store(w + frames, std::memory_order_release);
    }

    [[nodiscard]] bool pop_frame(float& out_l, float& out_r) noexcept {
        const std::uint64_t r = read_frames.load(std::memory_order_relaxed);
        const std::uint64_t w = write_frames.load(std::memory_order_acquire);
        if (r == w) {
            return false;
        }
        const std::size_t idx = static_cast<std::size_t>(r & stream_ring_mask) * 2U;
        out_l = samples[idx];
        out_r = samples[idx + 1U];
        read_frames.store(r + 1U, std::memory_order_release);
        return true;
    }
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

struct ResidentClipSlot final {
    std::string key{};
    std::vector<float> samples{};
    std::uint32_t channels = 2U;
    std::uint64_t frame_count = 0U;
    std::atomic<bool> ready{false};
    std::atomic<std::uint32_t> active_voices{0U};
};

struct StreamSlot final {
    StereoStreamRing ring{};
    AudioStreamReader reader{};
    bool loop = false;
    std::atomic<bool> active_for_worker{false};
    std::atomic<std::uint32_t> active_voices{0U};
};

class CombFilter final {
public:
    void configure(const std::size_t delay_samples) noexcept {
        length_ = std::clamp<std::size_t>(delay_samples, 16U, buffer_.size());
        index_ = 0U;
        filter_store_ = 0.0F;
        buffer_.fill(0.0F);
    }

    [[nodiscard]] float process(
        const float input,
        const float feedback,
        const float damp) noexcept {
        const float output = buffer_[index_];
        filter_store_ = output * (1.0F - damp) + filter_store_ * damp;
        buffer_[index_] = input + filter_store_ * feedback;
        if (++index_ >= length_) {
            index_ = 0U;
        }
        return output;
    }

    void clear() noexcept {
        filter_store_ = 0.0F;
        buffer_.fill(0.0F);
    }

private:
    std::array<float, 2048U> buffer_{};
    std::size_t length_ = 1024U;
    std::size_t index_ = 0U;
    float filter_store_ = 0.0F;
};

class AllpassFilter final {
public:
    void configure(const std::size_t delay_samples) noexcept {
        length_ = std::clamp<std::size_t>(delay_samples, 16U, buffer_.size());
        index_ = 0U;
        buffer_.fill(0.0F);
    }

    [[nodiscard]] float process(const float input) noexcept {
        constexpr float feedback = 0.5F;
        const float buf_out = buffer_[index_];
        const float output = -input + buf_out;
        buffer_[index_] = input + buf_out * feedback;
        if (++index_ >= length_) {
            index_ = 0U;
        }
        return output;
    }

    void clear() noexcept { buffer_.fill(0.0F); }

private:
    std::array<float, 1024U> buffer_{};
    std::size_t length_ = 512U;
    std::size_t index_ = 0U;
};

class StereoReverbProcessor final {
public:
    void configure(const std::uint32_t sample_rate) noexcept {
        const double scale = static_cast<double>(sample_rate) / 44100.0;
        constexpr std::array<std::size_t, 4U> comb_tunings{1116U, 1188U, 1277U, 1356U};
        constexpr std::array<std::size_t, 2U> allpass_tunings{556U, 441U};
        constexpr std::size_t stereo_spread = 23U;
        for (std::size_t i = 0U; i < 4U; ++i) {
            combs_l_[i].configure(
                static_cast<std::size_t>(static_cast<double>(comb_tunings[i]) * scale));
            combs_r_[i].configure(static_cast<std::size_t>(
                static_cast<double>(comb_tunings[i] + stereo_spread) * scale));
        }
        for (std::size_t i = 0U; i < 2U; ++i) {
            allpass_l_[i].configure(static_cast<std::size_t>(
                static_cast<double>(allpass_tunings[i]) * scale));
            allpass_r_[i].configure(static_cast<std::size_t>(
                static_cast<double>(allpass_tunings[i] + stereo_spread) * scale));
        }
    }

    void clear() noexcept {
        for (auto& c : combs_l_) c.clear();
        for (auto& c : combs_r_) c.clear();
        for (auto& a : allpass_l_) a.clear();
        for (auto& a : allpass_r_) a.clear();
    }

    void process_frame(
        const float send_l,
        const float send_r,
        const AudioReverbState& state,
        float& out_wet_l,
        float& out_wet_r) noexcept {
        if (!state.enabled || state.wet_level <= 1.0e-4F) {
            out_wet_l = 0.0F;
            out_wet_r = 0.0F;
            return;
        }
        const float decay_norm =
            std::clamp((state.decay_seconds - 0.1F) / 8.0F, 0.0F, 1.0F);
        const float feedback = 0.50F + 0.42F * decay_norm;
        const float damp = std::clamp(state.damping * 0.65F, 0.02F, 0.65F);
        const float mono_in = (send_l + send_r) * 0.015F;

        float acc_l = 0.0F;
        float acc_r = 0.0F;
        for (std::size_t i = 0U; i < 4U; ++i) {
            acc_l += combs_l_[i].process(mono_in, feedback, damp);
            acc_r += combs_r_[i].process(mono_in, feedback, damp);
        }
        for (std::size_t i = 0U; i < 2U; ++i) {
            acc_l = allpass_l_[i].process(acc_l);
            acc_r = allpass_r_[i].process(acc_r);
        }
        out_wet_l = acc_l * state.wet_level;
        out_wet_r = acc_r * state.wet_level;
    }

private:
    std::array<CombFilter, 4U> combs_l_{};
    std::array<CombFilter, 4U> combs_r_{};
    std::array<AllpassFilter, 2U> allpass_l_{};
    std::array<AllpassFilter, 2U> allpass_r_{};
};

enum class MixerCommandKind : std::uint8_t {
    set_bus_volume,
    set_bus_muted,
    set_gameplay_paused,
    stop_gameplay_voices,
    stop_all_voices,
    start_voice,
    stop_voice,
    fade_voice,
    update_voice,
    set_listener,
    set_reverb,
};

struct MixerCommand final {
    std::uint64_t sequence = 0U;
    MixerCommandKind kind = MixerCommandKind::set_bus_volume;
    AudioVoiceHandle handle{};
    AudioBusId bus = AudioBusId::sfx;
    std::uint32_t resource_index = 0U;
    float value_a = 0.0F;
    float value_b = 0.0F;
    float fade_seconds = 0.0F;
    bool flag_a = false;
    bool flag_b = false;
    bool flag_c = false;
    bool flag_d = false;
    AudioListenerState listener{};
    AudioSpatialParams spatial{};
    AudioReverbState reverb{};
};

struct MixerVoice final {
    std::uint32_t generation = 0U;
    bool active = false;
    bool is_streaming = false;
    bool is_preview = false;
    bool loop = false;
    bool spatial = false;
    bool stop_at_zero = false;
    AudioBusId bus = AudioBusId::sfx;
    std::uint32_t resource_index = 0U;
    double cursor = 0.0;
    float stream_prev_l = 0.0F;
    float stream_prev_r = 0.0F;
    float stream_curr_l = 0.0F;
    float stream_curr_r = 0.0F;
    bool stream_primed = false;
    float base_volume = 1.0F;
    float base_pitch = 1.0F;
    float envelope_gain = 1.0F;
    float envelope_target = 1.0F;
    float envelope_step = 0.0F;
    float smooth_gain_l = 0.0F;
    float smooth_gain_r = 0.0F;
    bool gain_initialized = false;
    float lp_state_l = 0.0F;
    float lp_state_r = 0.0F;
    AudioSpatialParams spatial_params{};
};

} // namespace

float compute_distance_attenuation(
    const AudioAttenuationModel model,
    const float distance,
    const float min_distance,
    const float max_distance,
    const float rolloff) noexcept {
    const float min_d = std::max(sanitize_finite(min_distance, 1.0F), 0.05F);
    const float max_d = std::max(sanitize_finite(max_distance, 35.0F), min_d);
    const float dist = std::max(sanitize_finite(distance, 0.0F), 0.0F);
    const float roll = std::clamp(sanitize_finite(rolloff, 1.0F), 0.01F, 16.0F);

    if (dist <= min_d) {
        return 1.0F;
    }
    if (dist >= max_d) {
        return 0.0F;
    }
    const float span = std::max(max_d - min_d, 1.0e-4F);
    const float u = std::clamp((dist - min_d) / span, 0.0F, 1.0F);
    const float boundary_taper = 1.0F - (u * u);

    switch (model) {
    case AudioAttenuationModel::linear:
        return std::clamp(1.0F - roll * u, 0.0F, 1.0F);
    case AudioAttenuationModel::exponential: {
        const float ratio = dist / min_d;
        return std::clamp(std::pow(ratio, -roll) * boundary_taper, 0.0F, 1.0F);
    }
    case AudioAttenuationModel::inverse_clamped:
    default: {
        const float inv = min_d / (min_d + roll * (dist - min_d));
        return std::clamp(inv * boundary_taper, 0.0F, 1.0F);
    }
    }
}

float compute_cone_attenuation(
    const glm::vec3& source_position,
    const glm::vec3& source_forward,
    const glm::vec3& listener_position,
    const float inner_cone_degrees,
    const float outer_cone_degrees,
    const float outer_cone_gain) noexcept {
    const float inner_deg =
        std::clamp(sanitize_finite(inner_cone_degrees, 360.0F), 0.0F, 360.0F);
    const float outer_deg = std::clamp(
        sanitize_finite(outer_cone_degrees, 360.0F), inner_deg, 360.0F);
    const float outer_gain =
        std::clamp(sanitize_finite(outer_cone_gain, 0.25F), 0.0F, 1.0F);
    if (inner_deg >= 359.9F) {
        return 1.0F;
    }
    const glm::vec3 to_listener = listener_position - source_position;
    const float len_sq = glm::dot(to_listener, to_listener);
    if (!std::isfinite(len_sq) || len_sq <= 1.0e-8F) {
        return 1.0F;
    }
    const glm::vec3 dir = to_listener * (1.0F / std::sqrt(len_sq));
    const glm::vec3 fwd =
        safe_normalize(source_forward, glm::vec3{0.0F, 0.0F, -1.0F});
    const float cos_theta = std::clamp(glm::dot(fwd, dir), -1.0F, 1.0F);
    const float angle_deg =
        std::acos(cos_theta) * (180.0F / std::numbers::pi_v<float>);
    const float half_inner = inner_deg * 0.5F;
    const float half_outer = outer_deg * 0.5F;
    if (angle_deg <= half_inner) {
        return 1.0F;
    }
    if (angle_deg >= half_outer) {
        return outer_gain;
    }
    const float t =
        (angle_deg - half_inner) / std::max(half_outer - half_inner, 1.0e-4F);
    return 1.0F + (outer_gain - 1.0F) * t;
}

float compute_doppler_pitch(
    const glm::vec3& source_position,
    const glm::vec3& source_velocity,
    const glm::vec3& listener_position,
    const glm::vec3& listener_velocity,
    const float doppler_scale) noexcept {
    const float scale =
        std::clamp(sanitize_finite(doppler_scale, 1.0F), 0.0F, 5.0F);
    if (scale <= 1.0e-4F) {
        return 1.0F;
    }
    const glm::vec3 src_to_listener = listener_position - source_position;
    const float dist_sq = glm::dot(src_to_listener, src_to_listener);
    if (!std::isfinite(dist_sq) || dist_sq <= 1.0e-6F) {
        return 1.0F;
    }
    const glm::vec3 axis = src_to_listener * (1.0F / std::sqrt(dist_sq));
    const glm::vec3 s_vel{
        sanitize_finite(source_velocity.x),
        sanitize_finite(source_velocity.y),
        sanitize_finite(source_velocity.z)};
    const glm::vec3 l_vel{
        sanitize_finite(listener_velocity.x),
        sanitize_finite(listener_velocity.y),
        sanitize_finite(listener_velocity.z)};

    const float max_radial = speed_of_sound_mps * 0.48F;
    const float v_l = std::clamp(glm::dot(l_vel, axis) * scale, -max_radial, max_radial);
    const float v_s = std::clamp(glm::dot(s_vel, axis) * scale, -max_radial, max_radial);
    const float shifted =
        (speed_of_sound_mps - v_l) / (speed_of_sound_mps - v_s);
    return std::clamp(shifted, 0.5F, 2.0F);
}

AudioReverbState evaluate_reverb_zones_at_listener(
    const glm::vec3& listener_position,
    const std::span<const std::pair<glm::vec3, AudioReverbZoneComponent>> zones) noexcept {
    AudioReverbState blended{};
    float total_weight = 0.0F;
    float weighted_decay = 0.0F;
    float weighted_damping = 0.0F;
    float max_wet = 0.0F;

    for (const auto& [center, raw_zone] : zones) {
        const AudioReverbZoneComponent zone =
            sanitize_audio_reverb_zone_component(raw_zone);
        if (!zone.enabled || zone.wet_level <= 0.0F) {
            continue;
        }
        const glm::vec3 delta = listener_position - center;
        const float dist_sq = glm::dot(delta, delta);
        if (!std::isfinite(dist_sq)) {
            continue;
        }
        const float dist = std::sqrt(std::max(dist_sq, 0.0F));
        if (dist >= zone.outer_radius) {
            continue;
        }
        float weight = 1.0F;
        if (dist > zone.inner_radius) {
            const float span =
                std::max(zone.outer_radius - zone.inner_radius, 1.0e-4F);
            const float t = std::clamp((dist - zone.inner_radius) / span, 0.0F, 1.0F);
            const float smooth = t * t * (3.0F - 2.0F * t);
            weight = 1.0F - smooth;
        }
        const float effective_wet = zone.wet_level * weight;
        if (effective_wet <= 1.0e-4F) {
            continue;
        }
        max_wet = std::max(max_wet, effective_wet);
        total_weight += effective_wet;
        weighted_decay += zone.decay_seconds * effective_wet;
        weighted_damping += zone.damping * effective_wet;
    }

    if (total_weight > 1.0e-4F) {
        blended.enabled = true;
        blended.wet_level = std::clamp(max_wet, 0.0F, 1.0F);
        blended.decay_seconds =
            std::clamp(weighted_decay / total_weight, 0.1F, 20.0F);
        blended.damping =
            std::clamp(weighted_damping / total_weight, 0.0F, 1.0F);
    }
    return blended;
}

#ifdef _MSC_VER
#pragma warning(push)
// Intentional cache-line isolation (`alignas(64)`) on `mixer_processed_seq`
// and embedded SPSC ring queues (AGENTS.md §5.1).
#pragma warning(disable : 4324)
#endif

struct AudioEngineState {
    static constexpr std::size_t max_voices = AudioEngine::max_voices;
    static constexpr std::size_t max_resident_clips =
        AudioEngine::max_resident_clips;
    static constexpr std::size_t max_stream_slots =
        AudioEngine::max_stream_slots;

    AudioEngineOptions options{};
    std::filesystem::path canonical_root{};
    std::uint32_t sample_rate = default_audio_sample_rate;

    // Resident immutable clip pool & main-thread lookup table.
    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(
            const std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
    };

    std::array<ResidentClipSlot, max_resident_clips> resident_clips{};
    std::unordered_map<std::string, std::uint32_t, TransparentStringHash, std::equal_to<>> clip_index_by_key{};
    std::size_t resident_pcm_bytes = 0U;
    std::size_t resident_pcm_high_water = 0U;

    // Sound cue cache & variation state (main thread only).
    struct CueCacheItem {
        SoundCueAsset cue{};
        double next_allowed_time = 0.0;
        std::optional<std::size_t> previous_index{};
    };
    std::unordered_map<std::string, CueCacheItem, TransparentStringHash, std::equal_to<>> cue_cache{};
    std::uint64_t rng_state = 0x517CC1B727220A95ULL;
    double engine_time_seconds = 0.0;

    // Streaming slots & background refill thread.
    std::array<StreamSlot, max_stream_slots> stream_slots{};
    std::mutex stream_mutex{};
    std::condition_variable stream_cv{};
    std::thread stream_thread{};
    std::atomic<bool> stop_stream_worker{false};

    // Main-thread mirror of bus state and voice slot reservation.
    std::array<float, audio_bus_count> host_bus_volumes{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    std::array<bool, audio_bus_count> host_bus_muted{false, false, false, false, false};
    bool host_gameplay_paused = false;

    struct VoiceSlotReservation {
        std::uint32_t next_generation = 1U;
        std::uint64_t start_command_seq = 0U;
    };
    std::array<VoiceSlotReservation, max_voices> host_voice_slots{};
    std::array<std::atomic<std::uint32_t>, max_voices> mixer_live_generations{};

    // Music dual-deck crossfade & Asset Browser preview state (main thread).
    AudioVoiceHandle active_music_voice{};
    AudioVoiceHandle fading_music_voice{};
    std::string active_music_path{};
    AudioVoiceHandle preview_voice{};
    std::string preview_path{};

    // Lock-free command channel to real-time mixer.
    SpscRingQueue<MixerCommand, command_queue_capacity> command_queue{};
    std::uint64_t next_command_seq = 1U;
    alignas(64) std::atomic<std::uint64_t> mixer_processed_seq{0U};

    // Real-time mixer state (owned exclusively by audio callback — or test runner
    // when hardware_device_initialized == false).
    std::array<MixerVoice, max_voices> mixer_voices{};
    std::array<float, audio_bus_count> mixer_bus_target_gain{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, audio_bus_count> mixer_bus_smooth_gain{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    std::array<bool, audio_bus_count> mixer_bus_muted{false, false, false, false, false};
    bool mixer_gameplay_paused = false;
    AudioListenerState mixer_listener{};
    AudioReverbState mixer_reverb{};
    StereoReverbProcessor reverb_dsp{};
    float limiter_gain = 1.0F;

    // Lock-free telemetry atomics published by mixer.
    struct BusTelemetryAtomics {
        std::atomic<float> peak_left{0.0F};
        std::atomic<float> peak_right{0.0F};
        std::atomic<std::uint32_t> active_voices{0U};
    };
    std::array<BusTelemetryAtomics, audio_bus_count> bus_telemetry{};
    std::atomic<float> telemetry_limiter_gain{1.0F};
    std::atomic<std::uint32_t> telemetry_active_voices{0U};

    // Optional WASAPI playback device.
    ma_device device{};
    bool hardware_device_initialized = false;

    void push_command(MixerCommand cmd) noexcept {
        cmd.sequence = next_command_seq++;
        if (!command_queue.try_push(cmd)) {
            // If the host bursts >512 commands before a single audio callback
            // runs in headless mode, drain on headless mixer or drop safely.
            if (!hardware_device_initialized) {
                drain_commands_on_mixer();
                (void)command_queue.try_push(cmd);
            }
        }
    }

    void release_voice_resource_on_mixer(const MixerVoice& voice) noexcept {
        if (voice.is_streaming) {
            if (voice.resource_index < max_stream_slots) {
                stream_slots[voice.resource_index].active_voices.fetch_sub(
                    1U, std::memory_order_acq_rel);
            }
        } else if (voice.resource_index < max_resident_clips) {
            resident_clips[voice.resource_index].active_voices.fetch_sub(
                1U, std::memory_order_acq_rel);
        }
    }

    void deactivate_mixer_voice(const std::size_t slot) noexcept {
        MixerVoice& v = mixer_voices[slot];
        if (!v.active) {
            return;
        }
        release_voice_resource_on_mixer(v);
        v.active = false;
        v.generation = 0U;
        mixer_live_generations[slot].store(0U, std::memory_order_release);
    }

    void drain_commands_on_mixer() noexcept {
        const float sr = static_cast<float>(sample_rate);
        MixerCommand cmd{};
        while (command_queue.try_pop(cmd)) {
            switch (cmd.kind) {
            case MixerCommandKind::set_bus_volume: {
                const auto idx = static_cast<std::size_t>(cmd.bus);
                if (idx < audio_bus_count) {
                    mixer_bus_target_gain[idx] =
                        std::clamp(sanitize_finite(cmd.value_a, 1.0F), 0.0F, 4.0F);
                }
                break;
            }
            case MixerCommandKind::set_bus_muted: {
                const auto idx = static_cast<std::size_t>(cmd.bus);
                if (idx < audio_bus_count) {
                    mixer_bus_muted[idx] = cmd.flag_a;
                }
                break;
            }
            case MixerCommandKind::set_gameplay_paused:
                mixer_gameplay_paused = cmd.flag_a;
                break;
            case MixerCommandKind::stop_gameplay_voices: {
                const float fade_frames = std::max(cmd.fade_seconds * sr, 0.0F);
                for (std::size_t i = 0U; i < max_voices; ++i) {
                    MixerVoice& v = mixer_voices[i];
                    if (!v.active || v.is_preview || !is_gameplay_bus(v.bus)) {
                        continue;
                    }
                    if (fade_frames <= 1.0F) {
                        deactivate_mixer_voice(i);
                    } else {
                        v.envelope_target = 0.0F;
                        v.envelope_step =
                            -std::max(v.envelope_gain, 0.01F) / fade_frames;
                        v.stop_at_zero = true;
                    }
                }
                if (fade_frames <= 1.0F) {
                    reverb_dsp.clear();
                }
                break;
            }
            case MixerCommandKind::stop_all_voices:
                for (std::size_t i = 0U; i < max_voices; ++i) {
                    if (mixer_voices[i].active) {
                        deactivate_mixer_voice(i);
                    }
                }
                reverb_dsp.clear();
                break;
            case MixerCommandKind::start_voice: {
                if (cmd.handle.slot >= max_voices) {
                    break;
                }
                MixerVoice& v = mixer_voices[cmd.handle.slot];
                if (v.active) {
                    deactivate_mixer_voice(cmd.handle.slot);
                }
                v = MixerVoice{};
                v.generation = cmd.handle.generation;
                v.active = true;
                v.bus = cmd.bus;
                v.resource_index = cmd.resource_index;
                v.base_volume = std::clamp(sanitize_finite(cmd.value_a, 1.0F), 0.0F, 4.0F);
                v.base_pitch = std::clamp(sanitize_finite(cmd.value_b, 1.0F), 0.25F, 4.0F);
                v.loop = cmd.flag_a;
                v.spatial = cmd.flag_b;
                v.is_streaming = cmd.flag_c;
                v.is_preview = cmd.flag_d;
                v.spatial_params = cmd.spatial;
                if (cmd.fade_seconds > 1.0e-4F) {
                    v.envelope_gain = 0.0F;
                    v.envelope_target = 1.0F;
                    v.envelope_step = 1.0F / std::max(cmd.fade_seconds * sr, 1.0F);
                } else {
                    v.envelope_gain = 1.0F;
                    v.envelope_target = 1.0F;
                    v.envelope_step = 0.0F;
                }
                mixer_live_generations[cmd.handle.slot].store(
                    v.generation, std::memory_order_release);
                break;
            }
            case MixerCommandKind::stop_voice: {
                if (cmd.handle.slot >= max_voices) {
                    break;
                }
                MixerVoice& v = mixer_voices[cmd.handle.slot];
                if (!v.active || v.generation != cmd.handle.generation) {
                    break;
                }
                const float fade_frames = cmd.fade_seconds * sr;
                if (fade_frames <= 1.0F) {
                    deactivate_mixer_voice(cmd.handle.slot);
                } else {
                    v.envelope_target = 0.0F;
                    v.envelope_step =
                        -std::max(v.envelope_gain, 0.001F) / fade_frames;
                    v.stop_at_zero = true;
                }
                break;
            }
            case MixerCommandKind::fade_voice: {
                if (cmd.handle.slot >= max_voices) {
                    break;
                }
                MixerVoice& v = mixer_voices[cmd.handle.slot];
                if (!v.active || v.generation != cmd.handle.generation) {
                    break;
                }
                const float target =
                    std::clamp(sanitize_finite(cmd.value_a, 0.0F), 0.0F, 4.0F);
                const float fade_frames = std::max(cmd.fade_seconds * sr, 1.0F);
                v.envelope_target = target;
                v.envelope_step = (target - v.envelope_gain) / fade_frames;
                v.stop_at_zero = cmd.flag_a;
                break;
            }
            case MixerCommandKind::update_voice: {
                if (cmd.handle.slot >= max_voices) {
                    break;
                }
                MixerVoice& v = mixer_voices[cmd.handle.slot];
                if (!v.active || v.generation != cmd.handle.generation) {
                    break;
                }
                v.base_volume = std::clamp(sanitize_finite(cmd.value_a, 1.0F), 0.0F, 4.0F);
                v.base_pitch = std::clamp(sanitize_finite(cmd.value_b, 1.0F), 0.25F, 4.0F);
                v.spatial_params = cmd.spatial;
                break;
            }
            case MixerCommandKind::set_listener:
                mixer_listener = cmd.listener;
                break;
            case MixerCommandKind::set_reverb:
                mixer_reverb = cmd.reverb;
                break;
            }
            mixer_processed_seq.store(cmd.sequence, std::memory_order_release);
        }
    }

    void mix_frames(std::span<float> output_stereo) noexcept {
        std::fill(output_stereo.begin(), output_stereo.end(), 0.0F);
        if (output_stereo.size() < 2U) {
            return;
        }
        drain_commands_on_mixer();

        const std::size_t total_frames = output_stereo.size() / 2U;
        std::size_t frame_offset = 0U;
        while (frame_offset < total_frames) {
            const std::size_t block_frames =
                std::min(total_frames - frame_offset, mixer_scratch_frames);
            mix_block(
                output_stereo.subspan(frame_offset * 2U, block_frames * 2U),
                block_frames);
            frame_offset += block_frames;
        }
    }

    void mix_block(
        const std::span<float> out_stereo,
        const std::size_t frames) noexcept {
        std::array<std::array<float, mixer_scratch_frames * 2U>, audio_bus_count>
            bus_buffers{};
        std::array<float, mixer_scratch_frames * 2U> reverb_send_buf{};
        std::array<std::uint32_t, audio_bus_count> bus_voice_counts{};

        // Build orthonormal listener basis once per block.
        const glm::vec3 listener_fwd = safe_normalize(
            mixer_listener.forward, glm::vec3{0.0F, 0.0F, -1.0F});
        const glm::vec3 listener_up_hint =
            safe_normalize(mixer_listener.up, glm::vec3{0.0F, 1.0F, 0.0F});
        glm::vec3 listener_right = glm::cross(listener_fwd, listener_up_hint);
        if (glm::dot(listener_right, listener_right) <= 1.0e-6F) {
            listener_right = glm::vec3{1.0F, 0.0F, 0.0F};
        } else {
            listener_right = glm::normalize(listener_right);
        }
        const float listener_vol =
            std::clamp(sanitize_finite(mixer_listener.volume_scale, 1.0F), 0.0F, 4.0F);
        const float sr = static_cast<float>(sample_rate);
        const float gain_smooth_alpha =
            1.0F - std::exp(-1.0F / std::max(0.004F * sr, 1.0F));

        std::uint32_t total_active_voices = 0U;

        for (std::size_t slot = 0U; slot < max_voices; ++slot) {
            MixerVoice& v = mixer_voices[slot];
            if (!v.active) {
                continue;
            }
            const auto bus_idx = static_cast<std::size_t>(v.bus);
            if (bus_idx >= audio_bus_count) {
                deactivate_mixer_voice(slot);
                continue;
            }

            // Pause freezes gameplay voices cleanly without dropping their cursor.
            if (mixer_gameplay_paused && !v.is_preview && is_gameplay_bus(v.bus)) {
                ++bus_voice_counts[bus_idx];
                ++total_active_voices;
                continue;
            }

            float target_l = v.base_volume;
            float target_r = v.base_volume;
            float effective_pitch = v.base_pitch;
            float lp_alpha = 1.0F; // 1.0 = bypass
            float reverb_send_gain = 0.0F;

            if (v.spatial) {
                const glm::vec3 delta =
                    v.spatial_params.position - mixer_listener.position;
                const float dist_sq = glm::dot(delta, delta);
                const float dist =
                    std::isfinite(dist_sq) ? std::sqrt(std::max(dist_sq, 0.0F)) : 0.0F;
                const float dist_gain = compute_distance_attenuation(
                    v.spatial_params.attenuation,
                    dist,
                    v.spatial_params.min_distance,
                    v.spatial_params.max_distance,
                    v.spatial_params.rolloff);
                const float cone_gain = compute_cone_attenuation(
                    v.spatial_params.position,
                    v.spatial_params.forward,
                    mixer_listener.position,
                    v.spatial_params.inner_cone_degrees,
                    v.spatial_params.outer_cone_degrees,
                    v.spatial_params.outer_cone_gain);
                const float occ =
                    std::clamp(sanitize_finite(v.spatial_params.occlusion, 0.0F), 0.0F, 1.0F);
                const float occ_gain = 1.0F - 0.68F * occ;

                float pan = 0.0F;
                if (dist > 1.0e-4F) {
                    const glm::vec3 dir_to_src = delta * (1.0F / dist);
                    pan = std::clamp(glm::dot(dir_to_src, listener_right), -1.0F, 1.0F);
                }
                const float pan_angle =
                    (pan + 1.0F) * (0.25F * std::numbers::pi_v<float>);
                const float pan_l = std::cos(pan_angle);
                const float pan_r = std::sin(pan_angle);

                const float combined =
                    v.base_volume * listener_vol * dist_gain * cone_gain * occ_gain;
                target_l = combined * pan_l;
                target_r = combined * pan_r;

                const float doppler = compute_doppler_pitch(
                    v.spatial_params.position,
                    v.spatial_params.velocity,
                    mixer_listener.position,
                    mixer_listener.velocity,
                    v.spatial_params.doppler_factor * mixer_listener.doppler_scale);
                effective_pitch = std::clamp(v.base_pitch * doppler, 0.25F, 4.0F);

                if (occ > 1.0e-3F) {
                    const float cutoff_hz = 18'000.0F * std::pow(0.05F, occ);
                    const float rc = 1.0F / (2.0F * std::numbers::pi_v<float> * cutoff_hz);
                    const float dt = 1.0F / sr;
                    lp_alpha = std::clamp(dt / (rc + dt), 0.01F, 1.0F);
                }
                reverb_send_gain = std::clamp(
                    v.base_volume * listener_vol * dist_gain * cone_gain *
                        sanitize_finite(v.spatial_params.reverb_send, 1.0F),
                    0.0F,
                    2.0F);
            } else if (!v.is_preview && is_gameplay_bus(v.bus)) {
                target_l *= listener_vol * 0.70710678F;
                target_r *= listener_vol * 0.70710678F;
            } else {
                target_l *= 0.70710678F;
                target_r *= 0.70710678F;
            }

            if (!v.gain_initialized) {
                v.smooth_gain_l = target_l;
                v.smooth_gain_r = target_r;
                v.gain_initialized = true;
            }

            auto& bus_out = bus_buffers[bus_idx];
            bool finished = false;

            if (!v.is_streaming) {
                if (v.resource_index >= max_resident_clips ||
                    !resident_clips[v.resource_index].ready.load(
                        std::memory_order_acquire)) {
                    deactivate_mixer_voice(slot);
                    continue;
                }
                const ResidentClipSlot& clip = resident_clips[v.resource_index];
                const float* const pcm = clip.samples.data();
                const std::uint64_t total_clip_frames = clip.frame_count;
                const std::uint32_t ch = clip.channels;
                if (total_clip_frames == 0U || pcm == nullptr) {
                    deactivate_mixer_voice(slot);
                    continue;
                }

                for (std::size_t f = 0U; f < frames; ++f) {
                    if (v.cursor >= static_cast<double>(total_clip_frames)) {
                        if (v.loop) {
                            v.cursor = std::fmod(
                                v.cursor, static_cast<double>(total_clip_frames));
                        } else {
                            finished = true;
                            break;
                        }
                    }

                    const auto i0 = static_cast<std::uint64_t>(v.cursor);
                    const float frac = static_cast<float>(v.cursor - static_cast<double>(i0));
                    std::uint64_t i1 = i0 + 1U;
                    if (i1 >= total_clip_frames) {
                        i1 = v.loop ? 0U : (total_clip_frames - 1U);
                    }

                    float s_l = 0.0F;
                    float s_r = 0.0F;
                    if (ch == 1U) {
                        const float mono =
                            pcm[i0] + (pcm[i1] - pcm[i0]) * frac;
                        s_l = mono;
                        s_r = mono;
                    } else {
                        const float l0 = pcm[i0 * 2U];
                        const float r0 = pcm[i0 * 2U + 1U];
                        const float l1 = pcm[i1 * 2U];
                        const float r1 = pcm[i1 * 2U + 1U];
                        s_l = l0 + (l1 - l0) * frac;
                        s_r = r0 + (r1 - r0) * frac;
                    }
                    if (v.spatial) {
                        const float mono_src = 0.5F * (s_l + s_r);
                        s_l = mono_src;
                        s_r = mono_src;
                    }

                    // Advance envelope.
                    if (v.envelope_step != 0.0F) {
                        v.envelope_gain += v.envelope_step;
                        if ((v.envelope_step > 0.0F &&
                             v.envelope_gain >= v.envelope_target) ||
                            (v.envelope_step < 0.0F &&
                             v.envelope_gain <= v.envelope_target)) {
                            v.envelope_gain = v.envelope_target;
                            v.envelope_step = 0.0F;
                            if (v.stop_at_zero && v.envelope_gain <= 1.0e-5F) {
                                finished = true;
                                break;
                            }
                        }
                    }

                    v.smooth_gain_l += gain_smooth_alpha * (target_l - v.smooth_gain_l);
                    v.smooth_gain_r += gain_smooth_alpha * (target_r - v.smooth_gain_r);

                    v.lp_state_l += lp_alpha * (s_l - v.lp_state_l);
                    v.lp_state_r += lp_alpha * (s_r - v.lp_state_r);

                    const float env = v.envelope_gain;
                    bus_out[f * 2U] += v.lp_state_l * v.smooth_gain_l * env;
                    bus_out[f * 2U + 1U] += v.lp_state_r * v.smooth_gain_r * env;

                    if (reverb_send_gain > 1.0e-4F) {
                        const float send = 0.5F * (v.lp_state_l + v.lp_state_r) *
                            reverb_send_gain * env;
                        reverb_send_buf[f * 2U] += send;
                        reverb_send_buf[f * 2U + 1U] += send;
                    }

                    v.cursor += static_cast<double>(effective_pitch);
                }
            } else {
                if (v.resource_index >= max_stream_slots) {
                    deactivate_mixer_voice(slot);
                    continue;
                }
                StereoStreamRing& ring = stream_slots[v.resource_index].ring;
                if (!v.stream_primed) {
                    float l0 = 0.0F, r0 = 0.0F, l1 = 0.0F, r1 = 0.0F;
                    if (ring.pop_frame(l0, r0)) {
                        if (!ring.pop_frame(l1, r1)) {
                            l1 = l0;
                            r1 = r0;
                        }
                        v.stream_prev_l = l0;
                        v.stream_prev_r = r0;
                        v.stream_curr_l = l1;
                        v.stream_curr_r = r1;
                        v.cursor = 0.0;
                        v.stream_primed = true;
                    } else if (ring.reached_eof.load(std::memory_order_acquire)) {
                        finished = true;
                    }
                }

                if (v.stream_primed && !finished) {
                    for (std::size_t f = 0U; f < frames; ++f) {
                        while (v.cursor >= 1.0) {
                            float next_l = 0.0F;
                            float next_r = 0.0F;
                            if (ring.pop_frame(next_l, next_r)) {
                                v.stream_prev_l = v.stream_curr_l;
                                v.stream_prev_r = v.stream_curr_r;
                                v.stream_curr_l = next_l;
                                v.stream_curr_r = next_r;
                                v.cursor -= 1.0;
                            } else {
                                if (ring.reached_eof.load(std::memory_order_acquire)) {
                                    finished = true;
                                } else {
                                    // Temporary starvation: hold silence cleanly.
                                    v.stream_prev_l = 0.0F;
                                    v.stream_prev_r = 0.0F;
                                    v.stream_curr_l = 0.0F;
                                    v.stream_curr_r = 0.0F;
                                    v.cursor = 0.0;
                                }
                                break;
                            }
                        }
                        if (finished) {
                            break;
                        }

                        const float frac = static_cast<float>(v.cursor);
                        float s_l =
                            v.stream_prev_l + (v.stream_curr_l - v.stream_prev_l) * frac;
                        float s_r =
                            v.stream_prev_r + (v.stream_curr_r - v.stream_prev_r) * frac;
                        if (v.spatial) {
                            const float mono_src = 0.5F * (s_l + s_r);
                            s_l = mono_src;
                            s_r = mono_src;
                        }

                        if (v.envelope_step != 0.0F) {
                            v.envelope_gain += v.envelope_step;
                            if ((v.envelope_step > 0.0F &&
                                 v.envelope_gain >= v.envelope_target) ||
                                (v.envelope_step < 0.0F &&
                                 v.envelope_gain <= v.envelope_target)) {
                                v.envelope_gain = v.envelope_target;
                                v.envelope_step = 0.0F;
                                if (v.stop_at_zero && v.envelope_gain <= 1.0e-5F) {
                                    finished = true;
                                    break;
                                }
                            }
                        }

                        v.smooth_gain_l +=
                            gain_smooth_alpha * (target_l - v.smooth_gain_l);
                        v.smooth_gain_r +=
                            gain_smooth_alpha * (target_r - v.smooth_gain_r);

                        v.lp_state_l += lp_alpha * (s_l - v.lp_state_l);
                        v.lp_state_r += lp_alpha * (s_r - v.lp_state_r);

                        const float env = v.envelope_gain;
                        bus_out[f * 2U] += v.lp_state_l * v.smooth_gain_l * env;
                        bus_out[f * 2U + 1U] += v.lp_state_r * v.smooth_gain_r * env;

                        if (reverb_send_gain > 1.0e-4F) {
                            const float send =
                                0.5F * (v.lp_state_l + v.lp_state_r) *
                                reverb_send_gain * env;
                            reverb_send_buf[f * 2U] += send;
                            reverb_send_buf[f * 2U + 1U] += send;
                        }

                        v.cursor += static_cast<double>(effective_pitch);
                    }
                }
            }

            if (finished) {
                deactivate_mixer_voice(slot);
            } else {
                ++bus_voice_counts[bus_idx];
                ++total_active_voices;
            }
        }

        // Mix stereo reverb return into the SFX bus when gameplay is unpaused.
        if (!mixer_gameplay_paused && mixer_reverb.enabled &&
            mixer_reverb.wet_level > 1.0e-4F) {
            auto& sfx_buf =
                bus_buffers[static_cast<std::size_t>(AudioBusId::sfx)];
            for (std::size_t f = 0U; f < frames; ++f) {
                float wet_l = 0.0F;
                float wet_r = 0.0F;
                reverb_dsp.process_frame(
                    reverb_send_buf[f * 2U],
                    reverb_send_buf[f * 2U + 1U],
                    mixer_reverb,
                    wet_l,
                    wet_r);
                sfx_buf[f * 2U] += wet_l;
                sfx_buf[f * 2U + 1U] += wet_r;
            }
        }

        // Sum sub-buses (Music, SFX, Voice, UI + direct Master voices) into Master.
        std::array<float, audio_bus_count> block_peaks_l{};
        std::array<float, audio_bus_count> block_peaks_r{};
        auto& master_buf =
            bus_buffers[static_cast<std::size_t>(AudioBusId::master)];

        for (std::size_t b = 1U; b < audio_bus_count; ++b) {
            const float target_bus_gain =
                mixer_bus_muted[b] ? 0.0F : mixer_bus_target_gain[b];
            auto& sub_buf = bus_buffers[b];
            float peak_l = 0.0F;
            float peak_r = 0.0F;
            for (std::size_t f = 0U; f < frames; ++f) {
                mixer_bus_smooth_gain[b] +=
                    gain_smooth_alpha *
                    (target_bus_gain - mixer_bus_smooth_gain[b]);
                const float l = sub_buf[f * 2U] * mixer_bus_smooth_gain[b];
                const float r = sub_buf[f * 2U + 1U] * mixer_bus_smooth_gain[b];
                master_buf[f * 2U] += l;
                master_buf[f * 2U + 1U] += r;
                peak_l = std::max(peak_l, std::abs(l));
                peak_r = std::max(peak_r, std::abs(r));
            }
            block_peaks_l[b] = peak_l;
            block_peaks_r[b] = peak_r;
        }

        // Apply Master bus fader + brickwall peak limiter.
        const float master_target =
            mixer_bus_muted[0] ? 0.0F : mixer_bus_target_gain[0];
        const float limiter_release =
            1.0F - std::exp(-1.0F / std::max(0.080F * sr, 1.0F));
        float master_peak_l = 0.0F;
        float master_peak_r = 0.0F;

        for (std::size_t f = 0U; f < frames; ++f) {
            mixer_bus_smooth_gain[0] +=
                gain_smooth_alpha * (master_target - mixer_bus_smooth_gain[0]);
            float l = master_buf[f * 2U] * mixer_bus_smooth_gain[0];
            float r = master_buf[f * 2U + 1U] * mixer_bus_smooth_gain[0];
            if (!std::isfinite(l)) l = 0.0F;
            if (!std::isfinite(r)) r = 0.0F;

            const float abs_max = std::max(std::abs(l), std::abs(r));
            const float needed_gain =
                abs_max > limiter_ceiling ? (limiter_ceiling / abs_max) : 1.0F;
            if (needed_gain < limiter_gain) {
                limiter_gain = needed_gain; // Instant brickwall attack
            } else {
                limiter_gain += limiter_release * (1.0F - limiter_gain);
            }

            l = std::clamp(l * limiter_gain, -1.0F, 1.0F);
            r = std::clamp(r * limiter_gain, -1.0F, 1.0F);
            out_stereo[f * 2U] = l;
            out_stereo[f * 2U + 1U] = r;
            master_peak_l = std::max(master_peak_l, std::abs(l));
            master_peak_r = std::max(master_peak_r, std::abs(r));
        }
        block_peaks_l[0] = master_peak_l;
        block_peaks_r[0] = master_peak_r;

        // Publish decayed meter telemetry atomically (relaxed — UI visualization).
        const float meter_decay =
            std::exp(-static_cast<float>(frames) / std::max(0.15F * sr, 1.0F));
        for (std::size_t b = 0U; b < audio_bus_count; ++b) {
            const float prev_l =
                bus_telemetry[b].peak_left.load(std::memory_order_relaxed);
            const float prev_r =
                bus_telemetry[b].peak_right.load(std::memory_order_relaxed);
            bus_telemetry[b].peak_left.store(
                std::max(block_peaks_l[b], prev_l * meter_decay),
                std::memory_order_relaxed);
            bus_telemetry[b].peak_right.store(
                std::max(block_peaks_r[b], prev_r * meter_decay),
                std::memory_order_relaxed);
            bus_telemetry[b].active_voices.store(
                b == 0U ? total_active_voices : bus_voice_counts[b],
                std::memory_order_relaxed);
        }
        telemetry_limiter_gain.store(limiter_gain, std::memory_order_relaxed);
        telemetry_active_voices.store(total_active_voices, std::memory_order_relaxed);
    }

    void refill_active_streams_once() noexcept {
        std::array<float, stream_refill_chunk_frames * 2U> temp{};
        for (auto& slot : stream_slots) {
            if (!slot.active_for_worker.load(std::memory_order_acquire)) {
                continue;
            }
            if (slot.ring.reached_eof.load(std::memory_order_relaxed)) {
                continue;
            }
            while (slot.ring.available_to_write() >= stream_refill_chunk_frames) {
                bool reached_end = false;
                const std::size_t got = slot.reader.read_stereo_frames(
                    temp, slot.loop, reached_end);
                if (got > 0U) {
                    slot.ring.push_frames(temp.data(), got);
                }
                if (reached_end) {
                    slot.ring.reached_eof.store(true, std::memory_order_release);
                    break;
                }
                if (got == 0U) {
                    break;
                }
            }
        }
    }

    [[nodiscard]] bool any_stream_active() const noexcept {
        for (const auto& slot : stream_slots) {
            if (slot.active_for_worker.load(std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void stream_worker_loop() noexcept {
        std::unique_lock<std::mutex> lock(stream_mutex);
        while (!stop_stream_worker.load(std::memory_order_acquire)) {
            if (!any_stream_active()) {
                stream_cv.wait(lock, [this] {
                    return stop_stream_worker.load(std::memory_order_acquire) ||
                           any_stream_active();
                });
                continue;
            }
            refill_active_streams_once();
            stream_cv.wait_for(
                lock,
                std::chrono::milliseconds(15),
                [this] {
                    return stop_stream_worker.load(std::memory_order_acquire);
                });
        }
    }

    void reclaim_finished_streams_on_host() noexcept {
        const std::uint64_t ack =
            mixer_processed_seq.load(std::memory_order_acquire);
        bool any_pending_start = false;
        for (std::size_t i = 0U; i < max_voices; ++i) {
            if (host_voice_slots[i].start_command_seq > ack) {
                any_pending_start = true;
                break;
            }
        }
        if (any_pending_start) {
            return;
        }
        std::unique_lock<std::mutex> lock(stream_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        for (auto& slot : stream_slots) {
            if (slot.active_for_worker.load(std::memory_order_acquire) &&
                slot.active_voices.load(std::memory_order_acquire) == 0U) {
                slot.active_for_worker.store(false, std::memory_order_release);
                slot.reader.close();
                slot.ring.reset();
            }
        }
    }

    [[nodiscard]] std::optional<AudioVoiceHandle> allocate_voice_handle() noexcept {
        const std::uint64_t ack =
            mixer_processed_seq.load(std::memory_order_acquire);
        for (std::uint32_t i = 0U; i < max_voices; ++i) {
            if (host_voice_slots[i].start_command_seq <= ack &&
                mixer_live_generations[i].load(std::memory_order_acquire) == 0U) {
                std::uint32_t gen = host_voice_slots[i].next_generation++;
                if (gen == 0U) {
                    gen = host_voice_slots[i].next_generation++;
                }
                return AudioVoiceHandle{.slot = i, .generation = gen};
            }
        }
        return std::nullopt;
    }

    void evict_unused_resident_clips_if_needed(const std::size_t incoming_bytes) {
        if (resident_pcm_bytes + incoming_bytes <=
                options.maximum_resident_pcm_bytes &&
            clip_index_by_key.size() < max_resident_clips) {
            return;
        }
        const std::uint64_t ack =
            mixer_processed_seq.load(std::memory_order_acquire);
        for (const auto& s : host_voice_slots) {
            if (s.start_command_seq > ack) {
                return;
            }
        }
        for (std::uint32_t i = 0U; i < max_resident_clips; ++i) {
            ResidentClipSlot& slot = resident_clips[i];
            if (!slot.ready.load(std::memory_order_acquire) ||
                slot.active_voices.load(std::memory_order_acquire) != 0U) {
                continue;
            }
            const std::size_t freed = slot.samples.size() * sizeof(float);
            slot.ready.store(false, std::memory_order_release);
            clip_index_by_key.erase(slot.key);
            slot.key.clear();
            slot.samples.clear();
            slot.samples.shrink_to_fit();
            slot.frame_count = 0U;
            resident_pcm_bytes =
                freed <= resident_pcm_bytes ? (resident_pcm_bytes - freed) : 0U;
            if (resident_pcm_bytes + incoming_bytes <=
                    options.maximum_resident_pcm_bytes &&
                clip_index_by_key.size() < max_resident_clips) {
                break;
            }
        }
    }

    [[nodiscard]] bool acquire_resident_clip(
        const std::string_view relative_path,
        std::uint32_t& out_index,
        bool& out_prefer_stream,
        std::string& error) {
        out_prefer_stream = false;
        if (const auto it = clip_index_by_key.find(relative_path);
            it != clip_index_by_key.end()) {
            out_index = it->second;
            return true;
        }
        const std::string key(relative_path);

        CookedAudioAsset decoded{};
        if (!load_cooked_audio_asset(
                canonical_root, relative_path, decoded, error, sample_rate)) {
            if (error.find("enable streaming") != std::string::npos) {
                out_prefer_stream = true;
            }
            return false;
        }
        const std::size_t pcm_bytes = decoded.samples.size() * sizeof(float);
        if (pcm_bytes > options.maximum_resident_pcm_bytes) {
            out_prefer_stream = true;
            error = "Decoded clip exceeds resident audio cache capacity; streaming required.";
            return false;
        }

        evict_unused_resident_clips_if_needed(pcm_bytes);

        std::uint32_t chosen = UINT32_MAX;
        for (std::uint32_t i = 0U; i < max_resident_clips; ++i) {
            if (!resident_clips[i].ready.load(std::memory_order_acquire) &&
                resident_clips[i].active_voices.load(std::memory_order_acquire) == 0U) {
                chosen = i;
                break;
            }
        }
        if (chosen == UINT32_MAX ||
            resident_pcm_bytes + pcm_bytes > options.maximum_resident_pcm_bytes) {
            out_prefer_stream = true;
            error = "Resident audio clip pool is full.";
            return false;
        }

        ResidentClipSlot& slot = resident_clips[chosen];
        slot.key = key;
        slot.channels = decoded.channels;
        slot.frame_count = decoded.frame_count;
        slot.samples = std::move(decoded.samples);
        slot.ready.store(true, std::memory_order_release);

        clip_index_by_key.emplace(key, chosen);
        resident_pcm_bytes += pcm_bytes;
        resident_pcm_high_water =
            std::max(resident_pcm_high_water, resident_pcm_bytes);
        out_index = chosen;
        return true;
    }

    [[nodiscard]] bool open_stream_slot(
        const std::string_view relative_path,
        const bool loop,
        std::uint32_t& out_slot_index,
        std::string& error) {
        reclaim_finished_streams_on_host();
        std::lock_guard<std::mutex> lock(stream_mutex);
        std::uint32_t chosen = UINT32_MAX;
        for (std::uint32_t i = 0U; i < max_stream_slots; ++i) {
            if (!stream_slots[i].active_for_worker.load(std::memory_order_acquire) &&
                stream_slots[i].active_voices.load(std::memory_order_acquire) == 0U) {
                chosen = i;
                break;
            }
        }
        if (chosen == UINT32_MAX) {
            error = "All 8 streaming audio channels are currently in use.";
            return false;
        }

        StreamSlot& slot = stream_slots[chosen];
        if (!slot.reader.open(canonical_root, relative_path, sample_rate, error)) {
            return false;
        }
        slot.ring.allocate_once();
        slot.ring.reset();
        slot.loop = loop;

        // Synchronous prefill of ~85 ms eliminates initial callback underrun.
        std::array<float, stream_prefill_frames * 2U> prefill{};
        bool reached_end = false;
        const std::size_t got =
            slot.reader.read_stereo_frames(prefill, loop, reached_end);
        if (got > 0U) {
            slot.ring.push_frames(prefill.data(), got);
        }
        if (reached_end) {
            slot.ring.reached_eof.store(true, std::memory_order_release);
        }

        slot.active_voices.store(1U, std::memory_order_release);
        slot.active_for_worker.store(true, std::memory_order_release);
        out_slot_index = chosen;
        stream_cv.notify_one();
        return true;
    }

    static void ma_audio_callback(
        ma_device* const p_device,
        void* const p_output,
        const void* const /*p_input*/,
        const ma_uint32 frame_count) noexcept {
        auto* const self = static_cast<AudioEngineState*>(p_device->pUserData);
        if (self == nullptr || p_output == nullptr || frame_count == 0U) {
            return;
        }
        auto* const out_floats = static_cast<float*>(p_output);
        self->mix_frames(
            std::span<float>(out_floats, static_cast<std::size_t>(frame_count) * 2U));
    }
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

static_assert(sizeof(AudioEngine) == sizeof(void*));

AudioEngine::AudioEngine() noexcept {}

AudioEngine::~AudioEngine() noexcept {
    shutdown();
}

AudioEngine::AudioEngine(AudioEngine&&) noexcept = default;
AudioEngine& AudioEngine::operator=(AudioEngine&&) noexcept = default;

bool AudioEngine::initialize(
    const AudioEngineOptions& options,
    std::string& error) {
    shutdown();
    error.clear();

    if (options.sample_rate < 8'000U || options.sample_rate > 192'000U) {
        error = "AudioEngine sample_rate must be within [8000, 192000].";
        return false;
    }

    std::filesystem::path canonical_root{};
    if (!options.asset_root.empty()) {
        std::error_code fs_err;
        canonical_root =
            std::filesystem::weakly_canonical(options.asset_root, fs_err);
        if (fs_err ||
            !std::filesystem::is_directory(canonical_root, fs_err) ||
            fs_err) {
            error = "AudioEngine asset_root is not an accessible directory.";
            return false;
        }
    }

    auto st = std::make_unique<AudioEngineState>();
    st->options = options;
    st->canonical_root = std::move(canonical_root);
    st->sample_rate = options.sample_rate;
    st->reverb_dsp.configure(st->sample_rate);

    if (options.enable_hardware_output) {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2U;
        cfg.sampleRate = st->sample_rate;
        cfg.dataCallback = &AudioEngineState::ma_audio_callback;
        cfg.pUserData = st.get();
        cfg.noPreSilencedOutputBuffer = MA_TRUE;
        cfg.noClip = MA_TRUE; // Our Master brickwall limiter handles clipping.

        if (ma_device_init(nullptr, &cfg, &st->device) == MA_SUCCESS) {
            if (ma_device_start(&st->device) == MA_SUCCESS) {
                st->hardware_device_initialized = true;
            } else {
                ma_device_uninit(&st->device);
            }
        }
        // Note: If no audio output endpoint exists (e.g. headless CI VM), we
        // fall back cleanly to null/headless output per AGENTS.md Rule 8.
    }

    st->stream_thread = std::thread([raw = st.get()] {
        raw->stream_worker_loop();
    });

    state_ = std::move(st);
    return true;
}

void AudioEngine::shutdown() noexcept {
    if (!state_) {
        return;
    }
    if (state_->hardware_device_initialized) {
        ma_device_uninit(&state_->device);
        state_->hardware_device_initialized = false;
    }
    state_->stop_stream_worker.store(true, std::memory_order_release);
    state_->stream_cv.notify_all();
    if (state_->stream_thread.joinable()) {
        state_->stream_thread.join();
    }
    for (auto& slot : state_->stream_slots) {
        slot.reader.close();
    }
    state_.reset();
}

bool AudioEngine::initialized() const noexcept {
    return state_ != nullptr;
}

bool AudioEngine::hardware_output_active() const noexcept {
    return state_ != nullptr && state_->hardware_device_initialized;
}

std::uint32_t AudioEngine::sample_rate() const noexcept {
    return state_ ? state_->sample_rate : default_audio_sample_rate;
}

const std::filesystem::path& AudioEngine::asset_root() const noexcept {
    static const std::filesystem::path empty_path{};
    return state_ ? state_->canonical_root : empty_path;
}

void AudioEngine::set_bus_volume(
    const AudioBusId bus,
    const float volume) noexcept {
    if (!state_) {
        return;
    }
    const auto idx = static_cast<std::size_t>(bus);
    if (idx >= audio_bus_count) {
        return;
    }
    const float clamped = std::clamp(sanitize_finite(volume, 1.0F), 0.0F, 4.0F);
    state_->host_bus_volumes[idx] = clamped;
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::set_bus_volume,
        .bus = bus,
        .value_a = clamped,
    });
}

float AudioEngine::bus_volume(const AudioBusId bus) const noexcept {
    const auto idx = static_cast<std::size_t>(bus);
    if (!state_ || idx >= audio_bus_count) {
        return 1.0F;
    }
    return state_->host_bus_volumes[idx];
}

void AudioEngine::set_bus_muted(
    const AudioBusId bus,
    const bool muted) noexcept {
    if (!state_) {
        return;
    }
    const auto idx = static_cast<std::size_t>(bus);
    if (idx >= audio_bus_count) {
        return;
    }
    state_->host_bus_muted[idx] = muted;
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::set_bus_muted,
        .bus = bus,
        .flag_a = muted,
    });
}

bool AudioEngine::bus_muted(const AudioBusId bus) const noexcept {
    const auto idx = static_cast<std::size_t>(bus);
    if (!state_ || idx >= audio_bus_count) {
        return false;
    }
    return state_->host_bus_muted[idx];
}

void AudioEngine::set_gameplay_paused(const bool paused) noexcept {
    if (!state_ || state_->host_gameplay_paused == paused) {
        return;
    }
    state_->host_gameplay_paused = paused;
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::set_gameplay_paused,
        .flag_a = paused,
    });
}

bool AudioEngine::gameplay_paused() const noexcept {
    return state_ != nullptr && state_->host_gameplay_paused;
}

void AudioEngine::stop_gameplay_audio(const float fade_out_seconds) noexcept {
    if (!state_) {
        return;
    }
    state_->host_gameplay_paused = false;
    state_->active_music_voice = {};
    state_->fading_music_voice = {};
    state_->active_music_path.clear();
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::set_gameplay_paused,
        .flag_a = false,
    });
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::stop_gameplay_voices,
        .fade_seconds = std::clamp(sanitize_finite(fade_out_seconds, 0.03F), 0.0F, 5.0F),
    });
}

void AudioEngine::stop_all_audio() noexcept {
    if (!state_) {
        return;
    }
    state_->host_gameplay_paused = false;
    state_->active_music_voice = {};
    state_->fading_music_voice = {};
    state_->active_music_path.clear();
    state_->preview_voice = {};
    state_->preview_path.clear();
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::stop_all_voices,
    });
}

AudioVoiceHandle AudioEngine::play_asset(
    const std::string_view relative_asset_path,
    const AudioPlayParams& params,
    std::string& error) {
    error.clear();
    if (!state_) {
        error = "AudioEngine is not initialized.";
        return {};
    }
    if (state_->canonical_root.empty()) {
        error = "AudioEngine has no configured asset_root.";
        return {};
    }
    if (relative_asset_path.empty() ||
        !valid_audio_asset_reference(relative_asset_path)) {
        error = "Invalid relative audio asset path.";
        return {};
    }

    std::string_view resolved_clip_path = relative_asset_path;
    AudioPlayParams effective = params;

    // Resolve Sound Cue (.vcrcue.json) into a randomized clip entry + parameters.
    if (relative_asset_path.ends_with(".vcrcue.json")) {
        auto it = state_->cue_cache.find(relative_asset_path);
        if (it == state_->cue_cache.end()) {
            SoundCueAsset loaded{};
            if (!load_sound_cue_file(
                    state_->canonical_root, relative_asset_path, loaded, error)) {
                return {};
            }
            it = state_->cue_cache.emplace(
                std::string(relative_asset_path),
                AudioEngineState::CueCacheItem{.cue = std::move(loaded)}).first;
        }
        AudioEngineState::CueCacheItem& cue_item = it->second;
        if (!effective.is_preview &&
            state_->engine_time_seconds < cue_item.next_allowed_time) {
            return {}; // Cooldown suppressed cleanly
        }
        const auto picked = pick_sound_cue_variation(
            cue_item.cue, state_->rng_state, cue_item.previous_index);
        if (!picked) {
            error = "Sound cue contains zero playable clip entries.";
            return {};
        }
        cue_item.previous_index = picked->entry_index;
        if (!effective.is_preview && cue_item.cue.cooldown_seconds > 0.0F) {
            cue_item.next_allowed_time =
                state_->engine_time_seconds +
                static_cast<double>(cue_item.cue.cooldown_seconds);
        }
        resolved_clip_path = cue_item.cue.entries[picked->entry_index].asset_path;
        effective.volume = std::clamp(effective.volume * picked->volume, 0.0F, 4.0F);
        effective.pitch = std::clamp(effective.pitch * picked->pitch, 0.25F, 4.0F);
        if (!effective.is_preview) {
            effective.bus = cue_item.cue.bus;
        }
    }

    const auto handle_opt = state_->allocate_voice_handle();
    if (!handle_opt) {
        error = "Maximum simultaneous voices (64) reached.";
        return {};
    }

    bool use_streaming = effective.streaming;
    std::uint32_t resource_index = 0U;
    if (!use_streaming) {
        bool fallback_to_stream = false;
        if (!state_->acquire_resident_clip(
                resolved_clip_path,
                resource_index,
                fallback_to_stream,
                error)) {
            if (fallback_to_stream) {
                error.clear();
                use_streaming = true;
            } else {
                return {};
            }
        } else {
            state_->resident_clips[resource_index].active_voices.fetch_add(
                1U, std::memory_order_acq_rel);
        }
    }
    if (use_streaming) {
        if (!state_->open_stream_slot(
                resolved_clip_path, effective.loop, resource_index, error)) {
            return {};
        }
    }

    const AudioVoiceHandle handle = *handle_opt;
    state_->host_voice_slots[handle.slot].start_command_seq =
        state_->next_command_seq;
    state_->mixer_live_generations[handle.slot].store(
        handle.generation, std::memory_order_release);

    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::start_voice,
        .handle = handle,
        .bus = effective.bus,
        .resource_index = resource_index,
        .value_a = effective.volume,
        .value_b = effective.pitch,
        .fade_seconds = std::max(sanitize_finite(effective.fade_in_seconds), 0.0F),
        .flag_a = effective.loop,
        .flag_b = effective.spatial,
        .flag_c = use_streaming,
        .flag_d = effective.is_preview,
        .spatial = effective.spatial_params,
    });
    return handle;
}

AudioVoiceHandle AudioEngine::play_pcm_clip(
    const std::string_view debug_name,
    const std::span<const float> samples,
    const std::uint32_t channels,
    const AudioPlayParams& params,
    std::string& error) {
    error.clear();
    if (!state_) {
        error = "AudioEngine is not initialized.";
        return {};
    }
    if ((channels != 1U && channels != 2U) || samples.size() < channels ||
        (samples.size() % channels) != 0U) {
        error = "PCM clip must be non-empty mono or interleaved stereo float32.";
        return {};
    }

    const std::string key =
        "__mem__:" + std::string(debug_name.empty() ? "clip" : debug_name);
    std::uint32_t clip_idx = UINT32_MAX;
    if (const auto it = state_->clip_index_by_key.find(key);
        it != state_->clip_index_by_key.end()) {
        clip_idx = it->second;
    } else {
        const std::size_t pcm_bytes = samples.size() * sizeof(float);
        state_->evict_unused_resident_clips_if_needed(pcm_bytes);
        for (std::uint32_t i = 0U; i < max_resident_clips; ++i) {
            if (!state_->resident_clips[i].ready.load(std::memory_order_acquire) &&
                state_->resident_clips[i].active_voices.load(
                    std::memory_order_acquire) == 0U) {
                clip_idx = i;
                break;
            }
        }
        if (clip_idx == UINT32_MAX ||
            state_->resident_pcm_bytes + pcm_bytes >
                state_->options.maximum_resident_pcm_bytes) {
            error = "Resident audio clip cache is full.";
            return {};
        }
        ResidentClipSlot& slot = state_->resident_clips[clip_idx];
        slot.key = key;
        slot.channels = channels;
        slot.frame_count = samples.size() / channels;
        slot.samples.assign(samples.begin(), samples.end());
        slot.ready.store(true, std::memory_order_release);
        state_->clip_index_by_key.emplace(key, clip_idx);
        state_->resident_pcm_bytes += pcm_bytes;
        state_->resident_pcm_high_water = std::max(
            state_->resident_pcm_high_water, state_->resident_pcm_bytes);
    }

    const auto handle_opt = state_->allocate_voice_handle();
    if (!handle_opt) {
        error = "Maximum simultaneous voices (64) reached.";
        return {};
    }
    state_->resident_clips[clip_idx].active_voices.fetch_add(
        1U, std::memory_order_acq_rel);

    const AudioVoiceHandle handle = *handle_opt;
    state_->host_voice_slots[handle.slot].start_command_seq =
        state_->next_command_seq;
    state_->mixer_live_generations[handle.slot].store(
        handle.generation, std::memory_order_release);

    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::start_voice,
        .handle = handle,
        .bus = params.bus,
        .resource_index = clip_idx,
        .value_a = params.volume,
        .value_b = params.pitch,
        .fade_seconds = std::max(sanitize_finite(params.fade_in_seconds), 0.0F),
        .flag_a = params.loop,
        .flag_b = params.spatial,
        .flag_c = false,
        .flag_d = params.is_preview,
        .spatial = params.spatial_params,
    });
    return handle;
}

void AudioEngine::stop_voice(
    const AudioVoiceHandle handle,
    const float fade_out_seconds) noexcept {
    if (!state_ || !handle || handle.slot >= max_voices) {
        return;
    }
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::stop_voice,
        .handle = handle,
        .fade_seconds = std::clamp(sanitize_finite(fade_out_seconds, 0.01F), 0.0F, 10.0F),
    });
}

bool AudioEngine::is_voice_active(const AudioVoiceHandle handle) const noexcept {
    if (!state_ || !handle || handle.slot >= max_voices) {
        return false;
    }
    return state_->mixer_live_generations[handle.slot].load(
               std::memory_order_acquire) == handle.generation;
}

void AudioEngine::update_voice(
    const AudioVoiceHandle handle,
    const float volume,
    const float pitch,
    const AudioSpatialParams& spatial_params) noexcept {
    if (!state_ || !handle || handle.slot >= max_voices) {
        return;
    }
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::update_voice,
        .handle = handle,
        .value_a = volume,
        .value_b = pitch,
        .spatial = spatial_params,
    });
}

bool AudioEngine::crossfade_music(
    const std::string_view relative_asset_path,
    const float fade_seconds,
    const bool loop,
    std::string& error) {
    error.clear();
    if (!state_) {
        error = "AudioEngine is not initialized.";
        return false;
    }
    const float clamped_fade =
        std::clamp(sanitize_finite(fade_seconds, 1.0F), 0.0F, 30.0F);
    if (state_->active_music_path == relative_asset_path &&
        is_voice_active(state_->active_music_voice)) {
        return true;
    }

    AudioPlayParams params{};
    params.bus = AudioBusId::music;
    params.volume = 1.0F;
    params.pitch = 1.0F;
    params.fade_in_seconds = clamped_fade;
    params.loop = loop;
    params.spatial = false;
    params.streaming = true;

    const AudioVoiceHandle next_voice =
        play_asset(relative_asset_path, params, error);
    if (!next_voice) {
        return false;
    }

    if (is_voice_active(state_->fading_music_voice)) {
        stop_voice(state_->fading_music_voice, 0.05F);
    }
    if (is_voice_active(state_->active_music_voice)) {
        state_->fading_music_voice = state_->active_music_voice;
        state_->push_command(MixerCommand{
            .kind = MixerCommandKind::fade_voice,
            .handle = state_->fading_music_voice,
            .value_a = 0.0F,
            .fade_seconds = clamped_fade,
            .flag_a = true, // Stop voice once crossfade reaches 0
        });
    }
    state_->active_music_voice = next_voice;
    state_->active_music_path.assign(relative_asset_path);
    return true;
}

void AudioEngine::stop_music(const float fade_seconds) noexcept {
    if (!state_) {
        return;
    }
    if (is_voice_active(state_->fading_music_voice)) {
        stop_voice(state_->fading_music_voice, fade_seconds);
    }
    if (is_voice_active(state_->active_music_voice)) {
        stop_voice(state_->active_music_voice, fade_seconds);
    }
    state_->fading_music_voice = {};
    state_->active_music_voice = {};
    state_->active_music_path.clear();
}

std::string_view AudioEngine::current_music_path() const noexcept {
    return state_ ? std::string_view(state_->active_music_path) : std::string_view{};
}

bool AudioEngine::play_preview(
    const std::string_view relative_asset_path,
    std::string& error) {
    stop_preview();
    if (!state_) {
        error = "AudioEngine is not initialized.";
        return false;
    }
    AudioPlayParams params{};
    params.bus = AudioBusId::ui;
    params.volume = 1.0F;
    params.pitch = 1.0F;
    params.loop = false;
    params.spatial = false;
    params.is_preview = true;
    const AudioVoiceHandle voice = play_asset(relative_asset_path, params, error);
    if (!voice) {
        return false;
    }
    state_->preview_voice = voice;
    state_->preview_path.assign(relative_asset_path);
    return true;
}

void AudioEngine::stop_preview() noexcept {
    if (!state_) {
        return;
    }
    if (is_voice_active(state_->preview_voice)) {
        stop_voice(state_->preview_voice, 0.015F);
    }
    state_->preview_voice = {};
    state_->preview_path.clear();
}

bool AudioEngine::is_preview_playing() const noexcept {
    return state_ != nullptr && is_voice_active(state_->preview_voice);
}

std::string_view AudioEngine::preview_asset_path() const noexcept {
    if (!is_preview_playing()) {
        return {};
    }
    return state_->preview_path;
}

void AudioEngine::set_listener(const AudioListenerState& listener) noexcept {
    if (!state_) {
        return;
    }
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::set_listener,
        .listener = listener,
    });
}

void AudioEngine::set_reverb_state(const AudioReverbState& reverb) noexcept {
    if (!state_) {
        return;
    }
    state_->push_command(MixerCommand{
        .kind = MixerCommandKind::set_reverb,
        .reverb = reverb,
    });
}

void AudioEngine::update(const float dt_seconds) noexcept {
    if (!state_) {
        return;
    }
    const float safe_dt = std::clamp(sanitize_finite(dt_seconds, 0.0F), 0.0F, 1.0F);
    if (!state_->host_gameplay_paused) {
        state_->engine_time_seconds += static_cast<double>(safe_dt);
    }
    if (state_->preview_voice && !is_voice_active(state_->preview_voice)) {
        state_->preview_voice = {};
        state_->preview_path.clear();
    }
    if (state_->active_music_voice && !is_voice_active(state_->active_music_voice)) {
        state_->active_music_voice = {};
        state_->active_music_path.clear();
    }
    state_->reclaim_finished_streams_on_host();
}

AudioEngineTelemetry AudioEngine::telemetry() const noexcept {
    AudioEngineTelemetry out{};
    if (!state_) {
        return out;
    }
    for (std::size_t b = 0U; b < audio_bus_count; ++b) {
        out.buses[b].volume = state_->host_bus_volumes[b];
        out.buses[b].muted = state_->host_bus_muted[b];
        out.buses[b].peak_left =
            state_->bus_telemetry[b].peak_left.load(std::memory_order_relaxed);
        out.buses[b].peak_right =
            state_->bus_telemetry[b].peak_right.load(std::memory_order_relaxed);
        out.buses[b].active_voices =
            state_->bus_telemetry[b].active_voices.load(std::memory_order_relaxed);
    }
    out.master_limiter_gain =
        state_->telemetry_limiter_gain.load(std::memory_order_relaxed);
    out.active_voice_count =
        state_->telemetry_active_voices.load(std::memory_order_relaxed);
    std::uint32_t active_streams = 0U;
    for (const auto& s : state_->stream_slots) {
        if (s.active_for_worker.load(std::memory_order_relaxed)) {
            ++active_streams;
        }
    }
    out.active_stream_count = active_streams;
    out.resident_clip_count = state_->clip_index_by_key.size();
    out.resident_pcm_bytes = state_->resident_pcm_bytes;
    out.resident_pcm_high_water_bytes = state_->resident_pcm_high_water;
    out.hardware_output_active = state_->hardware_device_initialized;
    out.gameplay_paused = state_->host_gameplay_paused;
    return out;
}

void AudioEngine::pump_streams_sync_for_test() noexcept {
    if (!state_ || state_->hardware_device_initialized) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->stream_mutex);
    state_->refill_active_streams_once();
}

void AudioEngine::render_frames_for_test(
    const std::span<float> interleaved_stereo) noexcept {
    if (!state_ || state_->hardware_device_initialized) {
        std::fill(interleaved_stereo.begin(), interleaved_stereo.end(), 0.0F);
        return;
    }
    state_->mix_frames(interleaved_stereo);
}

} // namespace vcr
