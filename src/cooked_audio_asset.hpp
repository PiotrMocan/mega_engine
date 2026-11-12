#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t default_audio_sample_rate = 48'000U;
inline constexpr std::uintmax_t maximum_audio_metadata_bytes =
    1ULL * 1024ULL * 1024ULL;
inline constexpr std::uintmax_t maximum_audio_source_bytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximum_resident_audio_frames =
    48'000ULL * 180ULL; // Up to 3 minutes resident; longer clips stream.

struct AudioSourceFormatInfo {
    std::uint32_t sample_rate = 0U;
    std::uint32_t channels = 0U;
    std::uint64_t total_pcm_frames = 0U;
    double duration_seconds = 0.0;
    bool stream_recommended = false;
};

struct CookedAudioAsset {
    std::filesystem::path requested_relative_path{};
    std::filesystem::path resolved_source_relative_path{};
    std::string name{};
    std::uint32_t sample_rate = default_audio_sample_rate;
    std::uint32_t channels = 2U; // 1 (mono) or 2 (interleaved stereo)
    std::uint64_t frame_count = 0U;
    double duration_seconds = 0.0;
    bool stream_recommended = false;
    std::vector<float> samples{};
};

// Inspects a raw .wav, .ogg, or .flac file header/stream without decoding the
// full PCM payload into memory.
[[nodiscard]] bool inspect_audio_source_file(
    const std::filesystem::path& source_file,
    AudioSourceFormatInfo& out_info,
    std::string& error);

// Resolves either a cooked .vcraudio.json manifest or a direct .wav/.ogg/.flac
// relative path strictly within asset_root (rejecting '..' and symbolic links).
[[nodiscard]] bool resolve_sandboxed_audio_file(
    const std::filesystem::path& asset_root,
    std::string_view relative_asset_path,
    std::filesystem::path& out_resolved_audio_file,
    std::filesystem::path& out_source_relative_path,
    std::string& out_display_name,
    bool& out_stream_recommended,
    std::string& error);

// Decodes a sandboxed audio asset into interleaved float32 PCM resampled to
// target_sample_rate (1 channel for mono sources, 2 channels for >=2 sources).
[[nodiscard]] bool load_cooked_audio_asset(
    const std::filesystem::path& asset_root,
    std::string_view relative_asset_path,
    CookedAudioAsset& out_asset,
    std::string& error,
    std::uint32_t target_sample_rate = default_audio_sample_rate);

struct AudioStreamReaderImpl;

// Move-only RAII frame-by-frame stereo float32 streaming decoder for long
// music/ambience/voice tracks. Never used directly inside the real-time audio
// callback; driven by the background streaming worker into lock-free rings.
class AudioStreamReader final {
public:
    AudioStreamReader() noexcept;
    ~AudioStreamReader() noexcept;

    AudioStreamReader(const AudioStreamReader&) = delete;
    AudioStreamReader& operator=(const AudioStreamReader&) = delete;
    AudioStreamReader(AudioStreamReader&&) noexcept;
    AudioStreamReader& operator=(AudioStreamReader&&) noexcept;

    [[nodiscard]] bool open(
        const std::filesystem::path& asset_root,
        std::string_view relative_asset_path,
        std::uint32_t target_sample_rate,
        std::string& error);
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] double duration_seconds() const noexcept;

    // Reads up to interleaved_stereo.size() / 2 stereo frames. When loop is
    // true and EOF is reached, seamlessly wraps back to frame 0.
    // Returns the number of stereo frames written; sets reached_end on non-loop
    // EOF.
    [[nodiscard]] std::size_t read_stereo_frames(
        std::span<float> interleaved_stereo,
        bool loop,
        bool& reached_end) noexcept;

    [[nodiscard]] bool seek_to_start() noexcept;

private:
    std::unique_ptr<AudioStreamReaderImpl> impl_;
};

} // namespace vcr
