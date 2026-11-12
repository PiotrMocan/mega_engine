#pragma once

#include "scene_components.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t sound_cue_file_version = 1U;
inline constexpr std::uintmax_t maximum_sound_cue_file_bytes =
    1ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t maximum_sound_cue_entries = 64U;

struct SoundCueClipEntry {
    std::string asset_path{};
    float weight = 1.0F;
    float volume_scale = 1.0F;
    float pitch_scale = 1.0F;

    friend bool operator==(
        const SoundCueClipEntry&,
        const SoundCueClipEntry&) = default;
};

struct SoundCueAsset {
    std::string name{"Sound Cue"};
    AudioBusId bus = AudioBusId::sfx;
    float volume_min = 0.92F;
    float volume_max = 1.0F;
    float pitch_min = 0.96F;
    float pitch_max = 1.04F;
    float min_distance = 1.5F;
    float max_distance = 35.0F;
    float cooldown_seconds = 0.0F;
    bool spatial = true;
    bool avoid_repeat = true;
    std::vector<SoundCueClipEntry> entries{};

    friend bool operator==(
        const SoundCueAsset&,
        const SoundCueAsset&) = default;
};

struct SoundCuePickResult {
    std::size_t entry_index = 0U;
    float volume = 1.0F;
    float pitch = 1.0F;
};

[[nodiscard]] bool valid_sound_cue_asset_path(
    std::string_view path) noexcept;

[[nodiscard]] bool valid_sound_cue_clip_path(
    std::string_view path) noexcept;

[[nodiscard]] std::string make_default_sound_cue_relative_path(
    std::string_view cue_name);

[[nodiscard]] bool save_sound_cue_file(
    const SoundCueAsset& cue,
    const std::filesystem::path& asset_root,
    std::string_view cue_relative_path,
    std::string& error);

[[nodiscard]] bool load_sound_cue_file(
    const std::filesystem::path& asset_root,
    std::string_view cue_relative_path,
    SoundCueAsset& out_cue,
    std::string& error);

// Deterministic, zero-allocation variation selector with non-repeating clip
// pick when entries.size() > 1 and cue.avoid_repeat is true.
[[nodiscard]] std::optional<SoundCuePickResult> pick_sound_cue_variation(
    const SoundCueAsset& cue,
    std::uint64_t& rng_state,
    std::optional<std::size_t> previous_index = std::nullopt) noexcept;

} // namespace vcr
