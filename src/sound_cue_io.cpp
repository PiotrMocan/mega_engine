#include "sound_cue_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr std::string_view sound_cue_schema = "vcr.sound_cue";

[[nodiscard]] std::filesystem::path utf8_to_path(const std::string_view text) {
    std::u8string encoded(text.size(), u8'\0');
    std::transform(
        text.begin(),
        text.end(),
        encoded.begin(),
        [](const char ch) {
            return static_cast<char8_t>(static_cast<unsigned char>(ch));
        });
    return std::filesystem::path(std::move(encoded));
}

[[nodiscard]] bool path_escapes_root(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute() ||
        relative.has_root_name() || relative.has_root_directory()) {
        return true;
    }
    for (const auto& part : relative.lexically_normal()) {
        if (part == "..") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_symlink_along_path(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& relative,
    const bool allow_missing_leaf,
    std::string& error) {
    std::error_code fs_err;
    std::filesystem::path cursor = canonical_root;
    const std::filesystem::path normalized = relative.lexically_normal();
    std::vector<std::filesystem::path> parts{};
    for (const auto& part : normalized) {
        if (part != ".") {
            parts.push_back(part);
        }
    }
    for (std::size_t idx = 0U; idx < parts.size(); ++idx) {
        cursor /= parts[idx];
        const bool is_leaf = (idx + 1U == parts.size());
        const auto status = std::filesystem::symlink_status(cursor, fs_err);
        if (fs_err) {
            if (allow_missing_leaf &&
                std::filesystem::status_known(status) &&
                !std::filesystem::exists(status)) {
                fs_err.clear();
                continue;
            }
            if (allow_missing_leaf) {
                fs_err.clear();
                if (!std::filesystem::exists(cursor, fs_err) && !fs_err) {
                    continue;
                }
            }
            error = is_leaf
                ? "Sound cue file does not exist or cannot be inspected."
                : "Sound cue directory cannot be inspected.";
            return true;
        }
        if (std::filesystem::is_symlink(status)) {
            error = "Symbolic links are not permitted for sound cue paths.";
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool replace_file_atomically(
    const std::filesystem::path& temp_path,
    const std::filesystem::path& destination_path,
    std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temp_path.c_str(),
            destination_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    error = "Unable to commit sound cue file atomically (Win32 error " +
        std::to_string(GetLastError()) + ").";
    return false;
#else
    std::error_code fs_err;
    std::filesystem::rename(temp_path, destination_path, fs_err);
    if (fs_err) {
        error = "Unable to commit sound cue file: " + fs_err.message();
        return false;
    }
    return true;
#endif
}

[[nodiscard]] std::uint64_t next_u64(std::uint64_t& state) noexcept {
    if (state == 0U) {
        state = 0x9E3779B97F4A7C15ULL;
    }
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] float next_unit_float(std::uint64_t& state) noexcept {
    const std::uint64_t bits = next_u64(state) >> 40U; // 24-bit mantissa
    return static_cast<float>(bits) * (1.0F / 16777216.0F);
}

[[nodiscard]] bool valid_cue_values(
    const SoundCueAsset& cue,
    std::string& error) {
    if (cue.name.empty() || cue.name.size() > 128U) {
        error = "Sound cue name must be 1..128 bytes.";
        return false;
    }
    if (to_token(cue.bus) == "unknown") {
        error = "Sound cue bus is invalid.";
        return false;
    }
    if (!std::isfinite(cue.volume_min) || !std::isfinite(cue.volume_max) ||
        cue.volume_min < 0.0F || cue.volume_max > 4.0F ||
        cue.volume_min > cue.volume_max) {
        error = "Sound cue volume_min/volume_max must satisfy 0 <= min <= max <= 4.";
        return false;
    }
    if (!std::isfinite(cue.pitch_min) || !std::isfinite(cue.pitch_max) ||
        cue.pitch_min < 0.25F || cue.pitch_max > 4.0F ||
        cue.pitch_min > cue.pitch_max) {
        error = "Sound cue pitch_min/pitch_max must satisfy 0.25 <= min <= max <= 4.";
        return false;
    }
    if (!std::isfinite(cue.min_distance) || !std::isfinite(cue.max_distance) ||
        cue.min_distance < 0.05F || cue.max_distance > 100'000.0F ||
        cue.min_distance > cue.max_distance) {
        error = "Sound cue distance bounds are invalid.";
        return false;
    }
    if (!std::isfinite(cue.cooldown_seconds) ||
        cue.cooldown_seconds < 0.0F ||
        cue.cooldown_seconds > 3600.0F) {
        error = "Sound cue cooldown_seconds must be within [0, 3600].";
        return false;
    }
    if (cue.entries.size() > maximum_sound_cue_entries) {
        error = "Sound cue exceeds maximum clip entry count.";
        return false;
    }
    for (const auto& entry : cue.entries) {
        if (!valid_sound_cue_clip_path(entry.asset_path)) {
            error = "Sound cue entry has an invalid clip path (nested cues are disallowed).";
            return false;
        }
        if (!std::isfinite(entry.weight) || entry.weight <= 0.0F ||
            entry.weight > 1000.0F) {
            error = "Sound cue entry weight must be within (0, 1000].";
            return false;
        }
        if (!std::isfinite(entry.volume_scale) || entry.volume_scale < 0.0F ||
            entry.volume_scale > 4.0F ||
            !std::isfinite(entry.pitch_scale) || entry.pitch_scale < 0.25F ||
            entry.pitch_scale > 4.0F) {
            error = "Sound cue entry volume_scale or pitch_scale is out of bounds.";
            return false;
        }
    }
    return true;
}

} // namespace

bool valid_sound_cue_asset_path(const std::string_view path) noexcept {
    return !path.empty() &&
           path.ends_with(".vcrcue.json") &&
           valid_audio_asset_reference(path);
}

bool valid_sound_cue_clip_path(const std::string_view path) noexcept {
    return !path.empty() &&
           !path.ends_with(".vcrcue.json") &&
           valid_audio_asset_reference(path);
}

std::string make_default_sound_cue_relative_path(
    const std::string_view cue_name) {
    std::string slug;
    slug.reserve(48U);
    bool prev_sep = false;
    for (const unsigned char ch : cue_name) {
        if (slug.size() >= 48U) {
            break;
        }
        if (std::isalnum(ch) != 0) {
            slug.push_back(static_cast<char>(std::tolower(ch)));
            prev_sep = false;
        } else if (!slug.empty() && !prev_sep) {
            slug.push_back('_');
            prev_sep = true;
        }
    }
    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "sound_cue";
    }
    return "audio/cues/" + slug + ".vcrcue.json";
}

bool save_sound_cue_file(
    const SoundCueAsset& cue,
    const std::filesystem::path& asset_root,
    const std::string_view cue_relative_path,
    std::string& error) {
    error.clear();
    if (!valid_sound_cue_asset_path(cue_relative_path)) {
        error = "Sound cue path must be a relative '*.vcrcue.json' path inside asset_root.";
        return false;
    }
    if (!valid_cue_values(cue, error)) {
        return false;
    }

    std::error_code fs_err;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(asset_root, fs_err);
    if (fs_err || !std::filesystem::is_directory(canonical_root, fs_err) || fs_err) {
        error = "Asset root is not an accessible directory.";
        return false;
    }

    const std::filesystem::path rel_path = utf8_to_path(cue_relative_path);
    if (path_escapes_root(rel_path) ||
        has_symlink_along_path(canonical_root, rel_path, true, error)) {
        if (error.empty()) {
            error = "Sound cue path escapes asset_root.";
        }
        return false;
    }

    const std::filesystem::path destination =
        (canonical_root / rel_path).lexically_normal();
    const std::filesystem::path parent_dir = destination.parent_path();
    std::filesystem::create_directories(parent_dir, fs_err);
    if (fs_err) {
        error = "Unable to create sound cue directory: " + fs_err.message();
        return false;
    }

    Json entries_json = Json::array();
    for (const auto& entry : cue.entries) {
        entries_json.push_back(Json{
            {"asset_path", entry.asset_path},
            {"weight", entry.weight},
            {"volume_scale", entry.volume_scale},
            {"pitch_scale", entry.pitch_scale},
        });
    }

    const Json doc{
        {"schema", sound_cue_schema},
        {"version", sound_cue_file_version},
        {"name", cue.name},
        {"bus", to_token(cue.bus)},
        {"volume_min", cue.volume_min},
        {"volume_max", cue.volume_max},
        {"pitch_min", cue.pitch_min},
        {"pitch_max", cue.pitch_max},
        {"min_distance", cue.min_distance},
        {"max_distance", cue.max_distance},
        {"cooldown_seconds", cue.cooldown_seconds},
        {"spatial", cue.spatial},
        {"avoid_repeat", cue.avoid_repeat},
        {"entries", std::move(entries_json)},
    };

    const std::filesystem::path temp_path =
        parent_dir / (".vcr-cue-tmp-" + destination.filename().string());
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Unable to create temporary sound cue file.";
            return false;
        }
        out << doc.dump(2) << '\n';
        out.flush();
        if (!out) {
            std::filesystem::remove(temp_path, fs_err);
            error = "Failed while writing temporary sound cue file.";
            return false;
        }
    }

    if (!replace_file_atomically(temp_path, destination, error)) {
        std::filesystem::remove(temp_path, fs_err);
        return false;
    }
    return true;
}

bool load_sound_cue_file(
    const std::filesystem::path& asset_root,
    const std::string_view cue_relative_path,
    SoundCueAsset& out_cue,
    std::string& error) {
    out_cue = {};
    error.clear();
    if (!valid_sound_cue_asset_path(cue_relative_path)) {
        error = "Sound cue path must be a relative '*.vcrcue.json' path.";
        return false;
    }

    std::error_code fs_err;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(asset_root, fs_err);
    if (fs_err || !std::filesystem::is_directory(canonical_root, fs_err) || fs_err) {
        error = "Asset root is not an accessible directory.";
        return false;
    }

    const std::filesystem::path rel_path = utf8_to_path(cue_relative_path);
    if (path_escapes_root(rel_path) ||
        has_symlink_along_path(canonical_root, rel_path, false, error)) {
        if (error.empty()) {
            error = "Sound cue path escapes asset_root.";
        }
        return false;
    }

    const std::filesystem::path full_path =
        std::filesystem::weakly_canonical(canonical_root / rel_path, fs_err);
    if (fs_err || !std::filesystem::is_regular_file(full_path, fs_err) || fs_err) {
        error = "Sound cue file does not exist.";
        return false;
    }
    const std::uintmax_t bytes = std::filesystem::file_size(full_path, fs_err);
    if (fs_err || bytes > maximum_sound_cue_file_bytes) {
        error = "Sound cue file exceeds maximum metadata size.";
        return false;
    }

    std::ifstream in(full_path, std::ios::binary);
    if (!in) {
        error = "Unable to open sound cue file.";
        return false;
    }

    Json doc;
    try {
        in >> doc;
    } catch (const std::exception& ex) {
        error = std::string("Invalid JSON in sound cue: ") + ex.what();
        return false;
    }
    if (!doc.is_object() ||
        doc.value("schema", std::string{}) != sound_cue_schema ||
        doc.value("version", 0U) != sound_cue_file_version) {
        error = "Unsupported sound cue schema or version.";
        return false;
    }

    SoundCueAsset parsed{};
    parsed.name = doc.value("name", std::string{"Sound Cue"});
    const std::string bus_str = doc.value("bus", std::string{"sfx"});
    const auto bus_opt = parse_audio_bus_id(bus_str);
    if (!bus_opt) {
        error = "Unknown audio bus in sound cue: '" + bus_str + "'.";
        return false;
    }
    parsed.bus = *bus_opt;
    parsed.volume_min = doc.value("volume_min", parsed.volume_min);
    parsed.volume_max = doc.value("volume_max", parsed.volume_max);
    parsed.pitch_min = doc.value("pitch_min", parsed.pitch_min);
    parsed.pitch_max = doc.value("pitch_max", parsed.pitch_max);
    parsed.min_distance = doc.value("min_distance", parsed.min_distance);
    parsed.max_distance = doc.value("max_distance", parsed.max_distance);
    parsed.cooldown_seconds = doc.value("cooldown_seconds", parsed.cooldown_seconds);
    parsed.spatial = doc.value("spatial", parsed.spatial);
    parsed.avoid_repeat = doc.value("avoid_repeat", parsed.avoid_repeat);

    if (const auto it = doc.find("entries"); it != doc.end()) {
        if (!it->is_array()) {
            error = "Sound cue 'entries' field must be an array.";
            return false;
        }
        if (it->size() > maximum_sound_cue_entries) {
            error = "Sound cue exceeds maximum clip entry count.";
            return false;
        }
        parsed.entries.reserve(it->size());
        for (const auto& item : *it) {
            if (!item.is_object() || !item.contains("asset_path") ||
                !item["asset_path"].is_string()) {
                error = "Sound cue entry must contain a string 'asset_path'.";
                return false;
            }
            SoundCueClipEntry entry{};
            entry.asset_path = item["asset_path"].get<std::string>();
            entry.weight = item.value("weight", 1.0F);
            entry.volume_scale = item.value("volume_scale", 1.0F);
            entry.pitch_scale = item.value("pitch_scale", 1.0F);
            parsed.entries.push_back(std::move(entry));
        }
    }

    if (!valid_cue_values(parsed, error)) {
        return false;
    }
    out_cue = std::move(parsed);
    return true;
}

std::optional<SoundCuePickResult> pick_sound_cue_variation(
    const SoundCueAsset& cue,
    std::uint64_t& rng_state,
    const std::optional<std::size_t> previous_index) noexcept {
    if (cue.entries.empty()) {
        return std::nullopt;
    }

    const bool exclude_previous =
        cue.avoid_repeat && cue.entries.size() > 1U &&
        previous_index.has_value() && *previous_index < cue.entries.size();

    float total_weight = 0.0F;
    for (std::size_t i = 0U; i < cue.entries.size(); ++i) {
        if (exclude_previous && i == *previous_index) {
            continue;
        }
        const float w = cue.entries[i].weight;
        if (std::isfinite(w) && w > 0.0F) {
            total_weight += w;
        }
    }

    std::size_t chosen = 0U;
    if (total_weight > 0.0F) {
        float ticket = next_unit_float(rng_state) * total_weight;
        for (std::size_t i = 0U; i < cue.entries.size(); ++i) {
            if (exclude_previous && i == *previous_index) {
                continue;
            }
            const float w = cue.entries[i].weight;
            if (!std::isfinite(w) || w <= 0.0F) {
                continue;
            }
            chosen = i;
            if (ticket <= w) {
                break;
            }
            ticket -= w;
        }
    } else if (exclude_previous) {
        chosen = (*previous_index + 1U) % cue.entries.size();
    }

    const float t_vol = next_unit_float(rng_state);
    const float t_pitch = next_unit_float(rng_state);
    const float raw_volume =
        (cue.volume_min + (cue.volume_max - cue.volume_min) * t_vol) *
        cue.entries[chosen].volume_scale;
    const float raw_pitch =
        (cue.pitch_min + (cue.pitch_max - cue.pitch_min) * t_pitch) *
        cue.entries[chosen].pitch_scale;

    return SoundCuePickResult{
        .entry_index = chosen,
        .volume = std::clamp(raw_volume, 0.0F, 4.0F),
        .pitch = std::clamp(raw_pitch, 0.25F, 4.0F),
    };
}

} // namespace vcr
