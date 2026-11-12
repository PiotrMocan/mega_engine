#include "cooked_audio_asset.hpp"

#include "scene_components.hpp"

#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#include <miniaudio.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::filesystem::path path_from_utf8(
    const std::string_view text) {
    std::u8string encoded(text.size(), u8'\0');
    std::transform(
        text.begin(),
        text.end(),
        encoded.begin(),
        [](const char byte) {
            return static_cast<char8_t>(static_cast<unsigned char>(byte));
        });
    return std::filesystem::path(encoded);
}

[[nodiscard]] bool path_escapes_root(const std::filesystem::path& relative) {
    if (relative.is_absolute()) {
        return true;
    }
    for (const auto& component : relative.lexically_normal()) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool verify_no_symlinks_within_root(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& relative,
    std::filesystem::path& out_resolved,
    std::string& error) {
    if (path_escapes_root(relative)) {
        error = "Audio asset path escapes the asset root.";
        return false;
    }
    std::error_code fs_err;
    std::filesystem::path walk = canonical_root;
    for (const auto& part : relative.lexically_normal()) {
        if (part == ".") {
            continue;
        }
        walk /= part;
        const auto status = std::filesystem::symlink_status(walk, fs_err);
        if (fs_err) {
            error = "Audio asset file does not exist or is inaccessible.";
            return false;
        }
        if (std::filesystem::is_symlink(status)) {
            error = "Symbolic links inside asset root are not followed.";
            return false;
        }
    }
    out_resolved = std::filesystem::weakly_canonical(walk, fs_err);
    if (fs_err ||
        !std::filesystem::is_regular_file(out_resolved, fs_err) ||
        fs_err) {
        error = "Audio asset does not resolve to a regular file.";
        return false;
    }
    const std::filesystem::path rel_check =
        std::filesystem::relative(out_resolved, canonical_root, fs_err);
    if (fs_err || path_escapes_root(rel_check)) {
        error = "Audio asset resolves outside the asset root.";
        return false;
    }
    return true;
}

[[nodiscard]] ma_result init_decoder_from_path(
    const std::filesystem::path& file_path,
    const ma_decoder_config* const config,
    ma_decoder* const decoder) noexcept {
#if defined(_WIN32)
    return ma_decoder_init_file_w(file_path.c_str(), config, decoder);
#else
    const std::string narrow = file_path.string();
    return ma_decoder_init_file(narrow.c_str(), config, decoder);
#endif
}

class ScopedMaDecoder final {
public:
    ScopedMaDecoder() = default;
    ~ScopedMaDecoder() noexcept { reset(); }

    ScopedMaDecoder(const ScopedMaDecoder&) = delete;
    ScopedMaDecoder& operator=(const ScopedMaDecoder&) = delete;

    [[nodiscard]] bool open(
        const std::filesystem::path& path,
        const ma_decoder_config* const config) noexcept {
        reset();
        if (init_decoder_from_path(path, config, &decoder_) != MA_SUCCESS) {
            return false;
        }
        initialized_ = true;
        return true;
    }

    void reset() noexcept {
        if (initialized_) {
            ma_decoder_uninit(&decoder_);
            initialized_ = false;
        }
    }

    [[nodiscard]] ma_decoder* get() noexcept { return &decoder_; }

private:
    ma_decoder decoder_{};
    bool initialized_ = false;
};

} // namespace

bool inspect_audio_source_file(
    const std::filesystem::path& source_file,
    AudioSourceFormatInfo& out_info,
    std::string& error) {
    error.clear();
    out_info = {};
    std::error_code fs_err;
    if (!std::filesystem::is_regular_file(source_file, fs_err) || fs_err) {
        error = "Audio source file does not exist.";
        return false;
    }
    const std::uintmax_t file_bytes =
        std::filesystem::file_size(source_file, fs_err);
    if (fs_err || file_bytes == 0U || file_bytes > maximum_audio_source_bytes) {
        error = "Audio source file is empty or exceeds the 512 MiB limit.";
        return false;
    }

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0U, 0U);
    ScopedMaDecoder decoder;
    if (!decoder.open(source_file, &cfg)) {
        error = "Audio file header or stream is invalid or unsupported.";
        return false;
    }
    const std::uint32_t sample_rate = decoder.get()->outputSampleRate;
    const std::uint32_t channels = decoder.get()->outputChannels;
    if (sample_rate < 4'000U || sample_rate > 384'000U ||
        channels == 0U || channels > 8U) {
        error = "Audio sample rate or channel count is outside supported bounds.";
        return false;
    }

    ma_uint64 length_frames = 0U;
    if (ma_decoder_get_length_in_pcm_frames(decoder.get(), &length_frames) !=
            MA_SUCCESS ||
        length_frames == 0U) {
        // Some Ogg streams compute exact frame count by scanning packets; read
        // bounded chunks if header length was 0.
        std::array<float, 4096U> scratch{};
        const std::size_t chunk_frames = scratch.size() / channels;
        std::uint64_t counted = 0U;
        constexpr std::uint64_t max_scan_frames = 384'000ULL * 3600ULL;
        while (counted < max_scan_frames) {
            ma_uint64 read_now = 0U;
            const ma_result res = ma_decoder_read_pcm_frames(
                decoder.get(), scratch.data(), chunk_frames, &read_now);
            counted += read_now;
            if (read_now == 0U || res != MA_SUCCESS) {
                break;
            }
        }
        length_frames = counted;
    }
    if (length_frames == 0U) {
        error = "Audio stream contains zero PCM frames.";
        return false;
    }

    out_info.sample_rate = sample_rate;
    out_info.channels = channels;
    out_info.total_pcm_frames = length_frames;
    out_info.duration_seconds =
        static_cast<double>(length_frames) / static_cast<double>(sample_rate);
    out_info.stream_recommended =
        out_info.duration_seconds >= 10.0 || file_bytes >= 2ULL * 1024ULL * 1024ULL;
    return true;
}

bool resolve_sandboxed_audio_file(
    const std::filesystem::path& asset_root,
    const std::string_view relative_asset_path,
    std::filesystem::path& out_resolved_audio_file,
    std::filesystem::path& out_source_relative_path,
    std::string& out_display_name,
    bool& out_stream_recommended,
    std::string& error) {
    error.clear();
    out_resolved_audio_file.clear();
    out_source_relative_path.clear();
    out_display_name.clear();
    out_stream_recommended = false;

    if (relative_asset_path.empty() ||
        !valid_audio_asset_reference(relative_asset_path) ||
        relative_asset_path.ends_with(".vcrcue.json")) {
        error = "Path is not a valid relative audio clip or .vcraudio.json manifest.";
        return false;
    }

    std::error_code fs_err;
    const auto root_symlink =
        std::filesystem::symlink_status(asset_root, fs_err);
    if (fs_err || std::filesystem::is_symlink(root_symlink)) {
        error = "Asset root is inaccessible or is a symbolic link.";
        return false;
    }
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(asset_root, fs_err);
    if (fs_err ||
        !std::filesystem::is_directory(canonical_root, fs_err) ||
        fs_err) {
        error = "Asset root is not an accessible directory.";
        return false;
    }

    const std::filesystem::path requested_rel =
        path_from_utf8(relative_asset_path).lexically_normal();
    std::filesystem::path resolved_primary;
    if (!verify_no_symlinks_within_root(
            canonical_root, requested_rel, resolved_primary, error)) {
        return false;
    }

    if (!relative_asset_path.ends_with(".vcraudio.json")) {
        const std::uintmax_t raw_bytes =
            std::filesystem::file_size(resolved_primary, fs_err);
        if (fs_err || raw_bytes == 0U ||
            raw_bytes > maximum_audio_source_bytes) {
            error = "Audio file is empty or exceeds the 512 MiB limit.";
            return false;
        }
        out_resolved_audio_file = std::move(resolved_primary);
        out_source_relative_path = requested_rel;
        out_display_name = requested_rel.stem().string();
        out_stream_recommended = raw_bytes >= 2ULL * 1024ULL * 1024ULL;
        return true;
    }

    const std::uintmax_t meta_bytes =
        std::filesystem::file_size(resolved_primary, fs_err);
    if (fs_err || meta_bytes == 0U ||
        meta_bytes > maximum_audio_metadata_bytes) {
        error = "Audio manifest is empty or exceeds the 1 MiB limit.";
        return false;
    }
    std::ifstream in(resolved_primary, std::ios::binary);
    if (!in) {
        error = "Unable to open .vcraudio.json manifest.";
        return false;
    }
    const Json doc = Json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() ||
        doc.value("schema", std::string{}) != "vcr.asset" ||
        doc.value("version", 0U) != 1U ||
        doc.value("kind", std::string{}) != "audio" ||
        !doc.contains("source") || !doc["source"].is_string()) {
        error = "Invalid .vcraudio.json schema or version.";
        return false;
    }
    const std::string& src_field = doc["source"].get_ref<const std::string&>();
    if (src_field.empty() || src_field.size() > 4096U) {
        error = "Audio manifest source path is invalid.";
        return false;
    }
    const std::filesystem::path manifest_dir_rel = requested_rel.parent_path();
    const std::filesystem::path combined_source_rel =
        (manifest_dir_rel / path_from_utf8(src_field)).lexically_normal();
    std::filesystem::path resolved_source;
    if (!verify_no_symlinks_within_root(
            canonical_root, combined_source_rel, resolved_source, error)) {
        return false;
    }
    const std::uintmax_t src_bytes =
        std::filesystem::file_size(resolved_source, fs_err);
    if (fs_err || src_bytes == 0U || src_bytes > maximum_audio_source_bytes) {
        error = "Referenced audio source is empty or exceeds 512 MiB.";
        return false;
    }

    out_resolved_audio_file = std::move(resolved_source);
    out_source_relative_path = combined_source_rel;
    out_display_name = doc.value("name", requested_rel.stem().string());
    out_stream_recommended = doc.value(
        "stream_recommended", src_bytes >= 2ULL * 1024ULL * 1024ULL);
    return true;
}

bool load_cooked_audio_asset(
    const std::filesystem::path& asset_root,
    const std::string_view relative_asset_path,
    CookedAudioAsset& out_asset,
    std::string& error,
    const std::uint32_t target_sample_rate) {
    error.clear();
    out_asset = {};
    if (target_sample_rate < 8'000U || target_sample_rate > 192'000U) {
        error = "Target audio sample rate must be in [8000, 192000].";
        return false;
    }

    std::filesystem::path resolved_file;
    std::filesystem::path source_rel;
    std::string display_name;
    bool stream_recommended = false;
    if (!resolve_sandboxed_audio_file(
            asset_root,
            relative_asset_path,
            resolved_file,
            source_rel,
            display_name,
            stream_recommended,
            error)) {
        return false;
    }

    // Probe native channel count so mono 3D SFX stay mono (half memory) while
    // stereo/multichannel assets fold cleanly to 2 channels.
    ma_decoder_config probe_cfg = ma_decoder_config_init(ma_format_f32, 0U, 0U);
    std::uint32_t out_channels = 2U;
    {
        ScopedMaDecoder probe;
        if (!probe.open(resolved_file, &probe_cfg)) {
            error = "Unable to initialize audio decoder for asset.";
            return false;
        }
        out_channels = probe.get()->outputChannels == 1U ? 1U : 2U;
    }

    ma_decoder_config decode_cfg = ma_decoder_config_init(
        ma_format_f32, out_channels, target_sample_rate);
    ScopedMaDecoder decoder;
    if (!decoder.open(resolved_file, &decode_cfg)) {
        error = "Unable to configure audio resampler/decoder.";
        return false;
    }

    std::vector<float> pcm;
    ma_uint64 estimated_frames = 0U;
    if (ma_decoder_get_length_in_pcm_frames(decoder.get(), &estimated_frames) ==
            MA_SUCCESS &&
        estimated_frames > 0U) {
        if (estimated_frames > maximum_resident_audio_frames) {
            error =
                "Audio asset exceeds the 3-minute resident PCM cap; enable streaming.";
            return false;
        }
        const std::uint64_t reserve_samples =
            estimated_frames * static_cast<std::uint64_t>(out_channels);
        if (reserve_samples < std::numeric_limits<std::size_t>::max() / sizeof(float)) {
            pcm.reserve(static_cast<std::size_t>(reserve_samples));
        }
    }

    constexpr std::size_t chunk_frames = 4'096U;
    std::array<float, chunk_frames * 2U> chunk{};
    std::uint64_t total_frames = 0U;
    for (;;) {
        ma_uint64 frames_read = 0U;
        const ma_result res = ma_decoder_read_pcm_frames(
            decoder.get(), chunk.data(), chunk_frames, &frames_read);
        if (frames_read > 0U) {
            if (total_frames + frames_read > maximum_resident_audio_frames) {
                error =
                    "Decoded audio exceeds the 3-minute resident PCM budget; enable streaming.";
                return false;
            }
            const std::size_t sample_count =
                static_cast<std::size_t>(frames_read) * out_channels;
            pcm.insert(
                pcm.end(),
                chunk.begin(),
                chunk.begin() + static_cast<std::ptrdiff_t>(sample_count));
            total_frames += frames_read;
        }
        if (frames_read < chunk_frames || res != MA_SUCCESS) {
            break;
        }
    }
    if (total_frames == 0U) {
        error = "Decoded audio asset contains zero PCM frames.";
        return false;
    }

    out_asset.requested_relative_path = path_from_utf8(relative_asset_path);
    out_asset.resolved_source_relative_path = std::move(source_rel);
    out_asset.name = std::move(display_name);
    out_asset.sample_rate = target_sample_rate;
    out_asset.channels = out_channels;
    out_asset.frame_count = total_frames;
    out_asset.duration_seconds =
        static_cast<double>(total_frames) /
        static_cast<double>(target_sample_rate);
    out_asset.stream_recommended =
        stream_recommended || out_asset.duration_seconds >= 10.0;
    out_asset.samples = std::move(pcm);
    return true;
}

struct AudioStreamReaderImpl {
    ma_decoder decoder{};
    std::uint32_t sample_rate = default_audio_sample_rate;
    double duration_seconds = 0.0;
    bool initialized = false;

    ~AudioStreamReaderImpl() noexcept {
        if (initialized) {
            ma_decoder_uninit(&decoder);
            initialized = false;
        }
    }
};

AudioStreamReader::AudioStreamReader() noexcept {}
AudioStreamReader::~AudioStreamReader() noexcept = default;
AudioStreamReader::AudioStreamReader(AudioStreamReader&&) noexcept = default;
AudioStreamReader& AudioStreamReader::operator=(AudioStreamReader&&) noexcept =
    default;

bool AudioStreamReader::open(
    const std::filesystem::path& asset_root,
    const std::string_view relative_asset_path,
    const std::uint32_t target_sample_rate,
    std::string& error) {
    close();
    if (target_sample_rate < 8'000U || target_sample_rate > 192'000U) {
        error = "Invalid streaming target sample rate.";
        return false;
    }

    std::filesystem::path resolved_file;
    std::filesystem::path source_rel;
    std::string display_name;
    bool stream_rec = false;
    if (!resolve_sandboxed_audio_file(
            asset_root,
            relative_asset_path,
            resolved_file,
            source_rel,
            display_name,
            stream_rec,
            error)) {
        return false;
    }

    auto state = std::make_unique<AudioStreamReaderImpl>();
    state->sample_rate = target_sample_rate;
    ma_decoder_config cfg =
        ma_decoder_config_init(ma_format_f32, 2U, target_sample_rate);
    if (init_decoder_from_path(resolved_file, &cfg, &state->decoder) !=
        MA_SUCCESS) {
        error = "Unable to open streaming audio decoder.";
        return false;
    }
    state->initialized = true;
    ma_uint64 length_frames = 0U;
    if (ma_decoder_get_length_in_pcm_frames(&state->decoder, &length_frames) ==
            MA_SUCCESS &&
        length_frames > 0U) {
        state->duration_seconds =
            static_cast<double>(length_frames) /
            static_cast<double>(target_sample_rate);
    }
    impl_ = std::move(state);
    return true;
}

void AudioStreamReader::close() noexcept {
    impl_.reset();
}

bool AudioStreamReader::is_open() const noexcept {
    return impl_ != nullptr && impl_->initialized;
}

std::uint32_t AudioStreamReader::sample_rate() const noexcept {
    return impl_ ? impl_->sample_rate : 0U;
}

double AudioStreamReader::duration_seconds() const noexcept {
    return impl_ ? impl_->duration_seconds : 0.0;
}

std::size_t AudioStreamReader::read_stereo_frames(
    const std::span<float> interleaved_stereo,
    const bool loop,
    bool& reached_end) noexcept {
    reached_end = false;
    if (!is_open() || interleaved_stereo.size() < 2U) {
        reached_end = true;
        return 0U;
    }
    const std::size_t requested_frames = interleaved_stereo.size() / 2U;
    std::size_t total_written = 0U;
    std::uint32_t wrap_guard = 0U;
    while (total_written < requested_frames) {
        const std::size_t remaining = requested_frames - total_written;
        ma_uint64 read_now = 0U;
        const ma_result res = ma_decoder_read_pcm_frames(
            &impl_->decoder,
            interleaved_stereo.data() + total_written * 2U,
            remaining,
            &read_now);
        total_written += static_cast<std::size_t>(read_now);
        if (total_written >= requested_frames) {
            break;
        }
        if (read_now == 0U || res == MA_AT_END || res != MA_SUCCESS) {
            if (loop && wrap_guard < 2U &&
                ma_decoder_seek_to_pcm_frame(&impl_->decoder, 0U) ==
                    MA_SUCCESS) {
                ++wrap_guard;
                continue;
            }
            reached_end = true;
            break;
        }
    }
    return total_written;
}

bool AudioStreamReader::seek_to_start() noexcept {
    if (!is_open()) {
        return false;
    }
    return ma_decoder_seek_to_pcm_frame(&impl_->decoder, 0U) == MA_SUCCESS;
}

} // namespace vcr
